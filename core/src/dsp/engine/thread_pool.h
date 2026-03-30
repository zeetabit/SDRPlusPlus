#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <future>

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

        // Submit work and get a future. If all workers are busy,
        // a new worker thread is spawned to prevent deadlock.
        std::future<int> submit(std::function<int()> work) {
            auto task = std::make_shared<std::packaged_task<int()>>(std::move(work));
            std::future<int> result = task->get_future();
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (stopping) { return result; }
                tasks.push([task]() { (*task)(); });

                // If all workers are busy, grow the pool
                if (idleCount == 0) {
                    spawnWorker();
                }
            }
            cv.notify_one();
            return result;
        }

        void submitAsync(std::function<void()> work) {
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (stopping) { return; }
                tasks.push(std::move(work));

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
            {
                std::lock_guard<std::mutex> lck(mtx);
                if (stopping) { return; }
                stopping = true;
            }
            cv.notify_all();

            std::atomic<bool> allJoined{false};
            std::thread joiner([this, &allJoined]() {
                std::lock_guard<std::mutex> lck(workersMtx);
                for (auto& w : workers) {
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
                std::lock_guard<std::mutex> lck(workersMtx);
                int stuck = 0;
                for (auto& w : workers) {
                    if (!w.joinable()) { continue; }
                    stuck++;
#ifndef _WIN32
                    pthread_cancel(w.native_handle());
#endif
                    w.detach();
                }
                if (stuck > 0) {
                    fprintf(stderr, "[ThreadPool] Shutdown timeout: %d worker(s) force-cancelled (buggy module?)\n", stuck);
                }
            }
            {
                std::lock_guard<std::mutex> lck(workersMtx);
                workers.clear();
            }
        }

    private:
        void spawnWorker() {
            totalCount++;
            std::lock_guard<std::mutex> lck(workersMtx);
            workers.emplace_back([this]() { workerFunc(); });
        }

        void workerFunc() {
            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lck(mtx);
                    idleCount++;
                    cv.wait(lck, [this] { return stopping || !tasks.empty(); });
                    idleCount--;

                    if (stopping && tasks.empty()) {
                        totalCount--;
                        return;
                    }
                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            }
        }

        int baseSize;
        std::atomic<int> totalCount{0};
        std::atomic<int> idleCount{0};

        std::vector<std::thread> workers;
        std::mutex workersMtx;  // protects workers vector

        std::queue<std::function<void()>> tasks;
        std::mutex mtx;  // protects tasks queue + stopping flag
        std::condition_variable cv;
        bool stopping = false;
    };

    inline ThreadPool& getPool() {
        static ThreadPool pool;
        return pool;
    }

} // namespace dsp::engine
