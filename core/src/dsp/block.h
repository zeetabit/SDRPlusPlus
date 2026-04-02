#pragma once
#include <assert.h>
#include <thread>
#include <vector>
#include <algorithm>
#include <chrono>
#include <string>
#include <typeinfo>
#include <atomic>
#include "stream.h"
#include "types.h"

namespace dsp {
    class generic_block {
    public:
        virtual ~generic_block() {}
        virtual void start() {}
        virtual void stop() {}
        virtual int run() { return -1; }
    };

    class block : public generic_block {
    public:
        virtual ~block() {
            if (!_block_init) { return; }
            stop();
            _block_init = false;
        }

        virtual void start() {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            if (running) {
                return;
            }
            running = true;
            doStart();
        }

        virtual void stop() noexcept {
            if (!_block_init) { return; }
            try {
                std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
                if (!running) { return; }
                doStop();
                running = false;
            }
            catch (...) {
                running = false;
            }
        }

        void tempStart() {
            assert(_block_init);
            if (!tempStopDepth || --tempStopDepth) { return; }
            if (tempStopped) {
                doStart();
                tempStopped = false;
            }
        }

        void tempStop() {
            assert(_block_init);
            if (tempStopDepth++) { return; }
            if (running && !tempStopped) {
                doStop();
                tempStopped = true;
            }
        }

        virtual int run() = 0;

    protected:
        void workerLoop() {
            while (run() >= 0) {}
        }

        virtual void doStart() {
            workerThread = std::thread(&block::workerLoop, this);
        }

        virtual void doStop() {
            for (auto& in : inputs) {
                in->stopReader();
            }
            for (auto& out : outputs) {
                out->stopWriter();
            }

            if (workerThread.joinable()) {
                // Timed join: detect stuck workers and log diagnostic
                auto start = std::chrono::steady_clock::now();
                std::atomic<bool> joined{false};
                std::thread joiner([this, &joined]() {
                    workerThread.join();
                    joined = true;
                });
                while (!joined) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 3000) {
                        fprintf(stderr, "[dsp::block] Worker thread stuck for >3s in block type '%s' — possible module bug\n",
                            typeid(*this).name());
                        joiner.detach();
                        break;
                    }
                }
                if (joined) { joiner.join(); }
            }

            for (auto& in : inputs) {
                in->clearReadStop();
            }
            for (auto& out : outputs) {
                out->clearWriteStop();
            }
        }
    
        void acquire() {
            ctrlMtx.lock();
        }

        void release() {
            ctrlMtx.unlock();
        }

        void registerInput(untyped_stream* inStream) {
            inputs.push_back(inStream);
        }

        void unregisterInput(untyped_stream* inStream) {
            inputs.erase(std::remove(inputs.begin(), inputs.end(), inStream), inputs.end());
        }

        void registerOutput(untyped_stream* outStream) {
            outputs.push_back(outStream);
        }

        void unregisterOutput(untyped_stream* outStream) {
            outputs.erase(std::remove(outputs.begin(), outputs.end(), outStream), outputs.end());
        }

        bool _block_init = false;

        std::recursive_mutex ctrlMtx;

        std::vector<untyped_stream*> inputs;
        std::vector<untyped_stream*> outputs;

        bool running = false;
        bool tempStopped = false;
        int tempStopDepth = 0;
        std::thread workerThread;
    };
}