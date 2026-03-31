#pragma once
#include "../block.h"
#include "thread_pool.h"
#include <utils/flog.h>

namespace dsp {

    // Drop-in replacement for dsp::block that runs its worker loop on
    // the global thread pool instead of spawning a dedicated thread.
    //
    // Blocks inherit from this (via ScheduledProcessor/ScheduledSink) to opt in.
    // The run() method, tempStop/tempStart, and all stream semantics are unchanged.
    //
    // The worker loop (blocking read → process → swap) is identical to the
    // original block. The only difference is WHERE the thread comes from:
    // a shared pool instead of std::thread per block.
    class scheduled_block : public block {
    protected:
        void doStart() override {
            // Submit the worker loop to the thread pool instead of spawning
            // a dedicated thread. The pool thread runs workerLoop() exactly
            // like a dedicated thread would, blocking on stream reads.
            //
            // We still need a joinable handle for doStop(). Use std::future
            // to track completion.
            workerDone = engine::getPool().submit([this]() -> int {
                workerLoop();
                return 0;
            });
        }

        void doStop() override {
            // Signal all streams to stop (unblocks read/swap)
            for (auto& in : inputs) { in->stopReader(); }
            for (auto& out : outputs) { out->stopWriter(); }

            // Wait for the pool-based worker to finish (with timeout to avoid deadlock)
            if (workerDone.valid()) {
                auto status = workerDone.wait_for(std::chrono::milliseconds(2000));
                if (status == std::future_status::ready) {
                    workerDone.get();
                }
                else {
                    flog::warn("scheduled_block::doStop: worker did not finish within 2s, forcing stop");
                    // Re-signal in case of a race where the worker started after the first signal
                    for (auto& in : inputs) { in->stopReader(); }
                    for (auto& out : outputs) { out->stopWriter(); }
                    // Give it one more chance
                    auto retry = workerDone.wait_for(std::chrono::milliseconds(500));
                    if (retry == std::future_status::ready) {
                        workerDone.get();
                    }
                    else {
                        flog::error("scheduled_block::doStop: worker stuck, abandoning");
                    }
                }
            }

            // Clear stream stop flags for potential restart
            for (auto& in : inputs) { in->clearReadStop(); }
            for (auto& out : outputs) { out->clearWriteStop(); }
        }

    private:
        std::future<int> workerDone;
    };

} // namespace dsp
