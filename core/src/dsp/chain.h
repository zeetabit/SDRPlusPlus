#pragma once
#include <vector>
#include <map>
#include "processor.h"

namespace dsp {

    // Type-erased wrapper for any block that has an output stream<T> and
    // setInput(stream<T>*). Works with both Processor<T,T> and ScheduledProcessor<T,T>.
    template<class T>
    struct ChainLink {
        block* blk;
        stream<T>* outStream;
        void (*setInputFn)(void* blk, stream<T>* in);

        void setInput(stream<T>* in) { setInputFn(blk, in); }
        void start() { blk->start(); }
        void stop() { blk->stop(); }
    };

    // Helper to create a ChainLink from any block type with `out` and `setInput()`.
    template<class T, class B>
    ChainLink<T> makeChainLink(B* block) {
        ChainLink<T> link;
        link.blk = static_cast<dsp::block*>(block);
        link.outStream = &block->out;
        link.setInputFn = [](void* b, stream<T>* in) {
            static_cast<B*>(b)->setInput(in);
        };
        return link;
    }

    template<class T>
    class chain {
    public:
        chain() {}

        chain(stream<T>* in) { init(in); }

        void init(stream<T>* in) {
            _in = in;
            out = _in;
        }

        template<typename Func>
        void setInput(stream<T>* in, Func onOutputChange) {
            _in = in;
            for (auto& ln : links) {
                if (states[ln.blk]) {
                    ln.setInput(_in);
                    return;
                }
            }
            out = _in;
            onOutputChange(out);
        }

        // Accept any block type with `out` stream and `setInput()`.
        template<class B>
        void addBlock(B* block, bool enabled) {
            auto link = makeChainLink<T>(block);
            if (blockExists(link.blk)) {
                throw std::runtime_error("[chain] Tried to add a block that is already part of the chain");
            }
            links.push_back(link);
            states[link.blk] = false;
            if (enabled) { enableBlock(block, [](stream<T>* out){}); }
        }

        template<class B, typename Func>
        void removeBlock(B* block, Func onOutputChange) {
            auto* blk = static_cast<dsp::block*>(block);
            if (!blockExists(blk)) {
                throw std::runtime_error("[chain] Tried to remove a block that is not part of the chain");
            }
            disableBlock(block, onOutputChange);
            states.erase(blk);
            links.erase(std::find_if(links.begin(), links.end(),
                [blk](const ChainLink<T>& l) { return l.blk == blk; }));
        }

        template<class B, typename Func>
        void enableBlock(B* block, Func onOutputChange) {
            auto* blk = static_cast<dsp::block*>(block);
            if (!blockExists(blk)) {
                throw std::runtime_error("[chain] Tried to enable a block that isn't part of the chain");
            }
            if (states[blk]) { return; }

            ChainLink<T>* before = linkBefore(blk);
            ChainLink<T>* after = linkAfter(blk);
            ChainLink<T>* cur = findLink(blk);

            if (after) {
                after->setInput(cur->outStream);
            }
            else {
                out = cur->outStream;
                onOutputChange(out);
            }

            cur->setInput(before ? before->outStream : _in);

            if (running) { cur->start(); }
            states[blk] = true;
        }

        template<class B, typename Func>
        void disableBlock(B* block, Func onOutputChange) {
            auto* blk = static_cast<dsp::block*>(block);
            if (!blockExists(blk)) {
                throw std::runtime_error("[chain] Tried to disable a block that isn't part of the chain");
            }
            if (!states[blk]) { return; }

            findLink(blk)->stop();
            states[blk] = false;

            ChainLink<T>* before = linkBefore(blk);
            ChainLink<T>* after = linkAfter(blk);

            if (after) {
                after->setInput(before ? before->outStream : _in);
            }
            else {
                out = before ? before->outStream : _in;
                onOutputChange(out);
            }
        }

        template<class B, typename Func>
        void setBlockEnabled(B* block, bool enabled, Func onOutputChange) {
            if (enabled) {
                enableBlock(block, onOutputChange);
            }
            else {
                disableBlock(block, onOutputChange);
            }
        }

        template<typename Func>
        void enableAllBlocks(Func onOutputChange) {
            for (auto& ln : links) {
                if (!states[ln.blk]) {
                    states[ln.blk] = false; // ensure entry exists
                    enableBlockByLink(&ln, onOutputChange);
                }
            }
        }

        template<typename Func>
        void disableAllBlocks(Func onOutputChange) {
            for (auto& ln : links) {
                if (states[ln.blk]) {
                    disableBlockByLink(&ln, onOutputChange);
                }
            }
        }

        void start() {
            if (running) { return; }
            for (auto& ln : links) {
                if (!states[ln.blk]) { continue; }
                ln.start();
            }
            running = true;
        }

        void stop() {
            if (!running) { return; }
            for (auto& ln : links) {
                if (!states[ln.blk]) { continue; }
                ln.stop();
            }
            running = false;
        }

        stream<T>* out;

    private:
        template<typename Func>
        void enableBlockByLink(ChainLink<T>* cur, Func onOutputChange) {
            if (states[cur->blk]) { return; }
            ChainLink<T>* before = linkBefore(cur->blk);
            ChainLink<T>* after = linkAfter(cur->blk);
            if (after) {
                after->setInput(cur->outStream);
            } else {
                out = cur->outStream;
                onOutputChange(out);
            }
            cur->setInput(before ? before->outStream : _in);
            if (running) { cur->start(); }
            states[cur->blk] = true;
        }

        template<typename Func>
        void disableBlockByLink(ChainLink<T>* cur, Func onOutputChange) {
            if (!states[cur->blk]) { return; }
            cur->stop();
            states[cur->blk] = false;
            ChainLink<T>* before = linkBefore(cur->blk);
            ChainLink<T>* after = linkAfter(cur->blk);
            if (after) {
                after->setInput(before ? before->outStream : _in);
            } else {
                out = before ? before->outStream : _in;
                onOutputChange(out);
            }
        }

        ChainLink<T>* findLink(block* blk) {
            for (auto& ln : links) {
                if (ln.blk == blk) { return &ln; }
            }
            return nullptr;
        }

        ChainLink<T>* linkBefore(block* blk) {
            ChainLink<T>* prev = nullptr;
            for (auto& ln : links) {
                if (ln.blk == blk) { return prev; }
                if (states[ln.blk]) { prev = &ln; }
            }
            return nullptr;
        }

        ChainLink<T>* linkAfter(block* blk) {
            bool found = false;
            for (auto& ln : links) {
                if (ln.blk == blk) { found = true; continue; }
                if (states[ln.blk] && found) { return &ln; }
            }
            return nullptr;
        }

        bool blockExists(block* blk) {
            return states.find(blk) != states.end();
        }

        stream<T>* _in;
        std::vector<ChainLink<T>> links;
        std::map<block*, bool> states;
        bool running = false;
    };
}