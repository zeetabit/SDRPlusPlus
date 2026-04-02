#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>
#include <map>
#include <string>

namespace dsp::engine {

    // Elastic thread pool for DSP block worker loops.
    //
    // DSP blocks run blocking worker loops (read → process → swap) that can't
    // be multiplexed on a fixed-size pool without deadlocking. This pool starts
    // with a base number of threads and grows elastically when all workers are
    // busy, ensuring every submitted task gets a thread.
    //
    // Benefits over raw std::thread per block:
    // - Centralized thread lifecycle management
    // - Controlled shutdown with timeout
    // - Thread reuse when blocks are stopped and restarted
    // - Observable: size() and activeCount() for diagnostics
    // - Named tasks: stuck workers report which block/module caused the hang
    class ThreadPool {
    public:
        ThreadPool(int baseThreads = 0) {
            int count = baseThreads > 0 ? baseThreads : (int)std::thread::hardware_concurrency();
            if (count < 4) { count = 4; }
            baseSize = count;
            for (int i = 0; i < count; i++) {
                spawnWorker();
            }
        }

        ~ThreadPool() {
            shutdown();
        }

        // Submit named work and get a future. The name is used for diagnostics
        // when a worker gets stuck during shutdown.
        std::future<int> submit(std::function<int()> work, const std::string& name = "") {
            auto task = std::make_shared<std::packaged_task<int()>>(std::move(work));
            std::future<int> result = task->get_future();
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (stopping) { return result; }
                std::string taskName = name;
                tasks.push({[task]() { (*task)(); }, std::move(taskName)});

                if (idleCount == 0) {
                    spawnWorker();
                }
            }
            cv.notify_one();
            return result;
        }

        void submitAsync(std::function<void()> work, const std::string& name = "") {
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (stopping) { return; }
                tasks.push({std::move(work), name});

                if (idleCount == 0) {
                    spawnWorker();
                }
            }
            cv.notify_one();
        }

        int size() const { return totalCount.load(); }
        int active() const { return totalCount.load() - idleCount.load(); }

        // Shutdown with timeout. Joins clean workers, force-cancels stuck ones.
        void shutdown(int timeoutMs = 3000) {
            std::vector<std::thread> snapshot;
            std::map<std::thread::id, std::string> nameSnapshot;
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (stopping) { return; }
                stopping = true;
            }
            cv.notify_all();
            {
                std::lock_guard<std::mutex> lck(workersMtx);
                snapshot = std::move(workers);
                nameSnapshot = workerNames;
            }

            std::atomic<bool> allJoined{false};
            std::thread joiner([&snapshot, &allJoined]() {
                for (auto& w : snapshot) {
                    if (w.joinable()) { w.join(); }
                }
                allJoined = true;
            });

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            while (!allJoined && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (allJoined) {
                joiner.join();
            }
            else {
                joiner.detach();
                int stuck = 0;
                for (auto& w : snapshot) {
                    if (!w.joinable()) { continue; }
                    stuck++;
                    auto it = nameSnapshot.find(w.get_id());
                    std::string taskName = (it != nameSnapshot.end() && !it->second.empty())
                        ? it->second : "(unnamed)";
                    fprintf(stderr, "[ThreadPool] Stuck worker: thread=%lu task='%s'\n",
                        (unsigned long)std::hash<std::thread::id>{}(w.get_id()), taskName.c_str());
#ifndef _WIN32
                    pthread_cancel(w.native_handle());
#endif
                    w.detach();
                }
                if (stuck > 0) {
                    fprintf(stderr, "[ThreadPool] Shutdown timeout: %d worker(s) force-cancelled\n", stuck);
                }
            }
        }

    private:
        struct NamedTask {
            std::function<void()> func;
            std::string name;
        };

        void spawnWorker() {
            totalCount++;
            std::lock_guard<std::mutex> lck(workersMtx);
            workers.emplace_back([this]() { workerFunc(); });
        }

        void workerFunc() {
            auto tid = std::this_thread::get_id();
            while (true) {
                NamedTask task;
                {
                    std::unique_lock<std::mutex> lck(mtx);
                    idleCount++;
                    // Clear task name while idle
                    {
                        std::lock_guard<std::mutex> nlck(workersMtx);
                        workerNames[tid] = "";
                    }
                    cv.wait(lck, [this] { return stopping || !tasks.empty(); });
                    idleCount--;

                    if (stopping && tasks.empty()) {
                        totalCount--;
                        std::lock_guard<std::mutex> nlck(workersMtx);
                        workerNames.erase(tid);
                        return;
                    }
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                // Record which task this worker is running
                {
                    std::lock_guard<std::mutex> nlck(workersMtx);
                    workerNames[tid] = task.name;
                }
                task.func();
            }
        }

        int baseSize;
        std::atomic<int> totalCount{0};
        std::atomic<int> idleCount{0};

        std::vector<std::thread> workers;
        std::map<std::thread::id, std::string> workerNames; // current task per worker thread
        std::mutex workersMtx;  // protects workers vector + workerNames

        std::queue<NamedTask> tasks;
        std::mutex mtx;  // protects tasks queue + stopping flag
        std::condition_variable cv;
        bool stopping = false;
    };

    inline ThreadPool& getPool() {
        static ThreadPool pool;
        return pool;
    }

} // namespace dsp::engine
