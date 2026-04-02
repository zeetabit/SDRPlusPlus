#pragma once
#include "../block.h"
#include "thread_pool.h"
#include <utils/flog.h>
#include <typeinfo>

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
    public:
        void setBlockName(const std::string& name) { blockName = name; }

    protected:
        void doStart() override {
            // Build a diagnostic name: explicit name + RTTI class name
            std::string taskName = blockName.empty() ? typeid(*this).name() : blockName;

            workerDone = engine::getPool().submit([this]() -> int {
                workerLoop();
                return 0;
            }, taskName);
        }

        void doStop() override {
            std::string taskName = blockName.empty() ? typeid(*this).name() : blockName;

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
                    flog::warn("scheduled_block::doStop: worker '{0}' did not finish within 2s, forcing stop", taskName);
                    for (auto& in : inputs) { in->stopReader(); }
                    for (auto& out : outputs) { out->stopWriter(); }
                    auto retry = workerDone.wait_for(std::chrono::milliseconds(500));
                    if (retry == std::future_status::ready) {
                        workerDone.get();
                    }
                    else {
                        flog::error("scheduled_block::doStop: worker '{0}' stuck, abandoning", taskName);
                    }
                }
            }

            // Clear stream stop flags for potential restart
            for (auto& in : inputs) { in->clearReadStop(); }
            for (auto& out : outputs) { out->clearWriteStop(); }
        }

    private:
        std::string blockName;
        std::future<int> workerDone;
    };

} // namespace dsp
