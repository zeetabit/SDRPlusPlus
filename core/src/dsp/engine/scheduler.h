#pragma once
#include <mutex>
#include <map>
#include <atomic>
#include <functional>
#include "thread_pool.h"
#include "../block.h"

namespace dsp::engine {

    // Per-block scheduling state.
    struct BlockState {
        std::atomic<bool> inFlight{false};   // True while run() is executing on a pool thread
        std::atomic<bool> registered{false}; // True while block is active in the scheduler
        std::function<void()> onDataReady;   // Callback installed on the block's input stream
    };

    // Data-driven scheduler. When a stream has data ready, the downstream block's
    // run() is submitted to the thread pool. Guarantees single-flight: a block's
    // run() is never called concurrently from two pool threads.
    class Scheduler {
    public:
        void registerBlock(block* blk) {
            std::lock_guard<std::mutex> lck(mtx);
            auto& state = blocks[blk];
            state.registered = true;
            state.inFlight = false;

            // Create the data-ready callback that will be installed on the input stream.
            // This captures a raw pointer to the state -- safe because unregisterBlock
            // ensures the state outlives any in-flight callbacks.
            state.onDataReady = [this, blk, &state]() {
                scheduleBlock(blk, state);
            };
        }

        void unregisterBlock(block* blk) {
            BlockState* state = nullptr;
            {
                std::lock_guard<std::mutex> lck(mtx);
                auto it = blocks.find(blk);
                if (it == blocks.end()) { return; }
                state = &it->second;
                state->registered = false;
            }

            // Spin-wait for any in-flight run() to complete.
            // This is called from doStop() which already holds ctrlMtx,
            // so run() cannot be resubmitted.
            while (state->inFlight.load()) {
                std::this_thread::yield();
            }

            std::lock_guard<std::mutex> lck(mtx);
            blocks.erase(blk);
        }

        // Get the onDataReady callback for a block (to install on its input stream).
        std::function<void()>* getCallback(block* blk) {
            std::lock_guard<std::mutex> lck(mtx);
            auto it = blocks.find(blk);
            if (it == blocks.end()) { return nullptr; }
            return &it->second.onDataReady;
        }

    private:
        void scheduleBlock(block* blk, BlockState& state) {
            // Single-flight guard: if already in-flight, skip.
            // The next onDataReady from upstream swap() will reschedule.
            bool expected = false;
            if (!state.inFlight.compare_exchange_strong(expected, true)) { return; }

            getPool().submitAsync([blk, &state]() {
                if (!state.registered.load()) {
                    state.inFlight = false;
                    return;
                }

                // Run the block's processing exactly once.
                // stream::read() will block until data is available or stopped.
                // After run() completes, the upstream's next swap() triggers
                // another onDataReady callback, which reschedules this block.
                blk->run();

                state.inFlight = false;
            });
        }

        std::mutex mtx;
        std::map<block*, BlockState> blocks;
    };

    // Global scheduler singleton.
    inline Scheduler& getScheduler() {
        static Scheduler scheduler;
        return scheduler;
    }

} // namespace dsp::engine
