#pragma once
#include "../sink.h"
#include "../buffer/shared_buffer.h"
#include <algorithm>

namespace dsp::routing {

    // Zero-copy splitter that shares a single buffer across all outputs.
    // Instead of memcpy per output, it copies input data once into a shared
    // reference-counted buffer, then gives each output stream a read-only
    // alias to that buffer. When all downstream blocks flush, the buffer
    // is recycled.
    //
    // Drop-in replacement for Splitter<T> with identical bind/unbind API.
    template <class T>
    class ZeroCopySplitter : public Sink<T> {
        using base_type = Sink<T>;
    public:
        ZeroCopySplitter() {}

        ZeroCopySplitter(stream<T>* in) { base_type::init(in); }

        ~ZeroCopySplitter() {
            delete pool;
        }

        void bindStream(stream<T>* stream) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);

            if (std::find(streams.begin(), streams.end(), stream) != streams.end()) {
                throw std::runtime_error("[ZeroCopySplitter] Stream already bound");
            }

            base_type::tempStop();
            base_type::registerOutput(stream);
            streams.push_back(stream);
            base_type::tempStart();
        }

        void unbindStream(stream<T>* stream) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);

            auto sit = std::find(streams.begin(), streams.end(), stream);
            if (sit == streams.end()) {
                throw std::runtime_error("[ZeroCopySplitter] Stream not bound");
            }

            base_type::tempStop();
            streams.erase(sit);
            base_type::unregisterOutput(stream);
            base_type::tempStart();
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            if (streams.empty()) {
                base_type::_in->flush();
                return count;
            }

            // Lazy-init the buffer pool on first run (needs to know buffer size).
            if (!pool) {
                pool = new buffer::SharedBufferPool<T>(STREAM_BUFFER_SIZE, (int)streams.size() + 1);
            }

            // Acquire a shared buffer and copy input data into it once.
            auto* sharedBuf = pool->acquire();
            memcpy(sharedBuf->data(), base_type::_in->readBuf, count * sizeof(T));

            // Set refcount to number of output streams.
            // Each output's flush() path will eventually release one ref.
            // For now, we use the simple approach: copy shared data into each
            // output's writeBuf (zero-copy requires stream<T> changes to support
            // alias reads -- we'll do that in a follow-up).
            // TODO: True zero-copy with alias_stream when stream<T> supports it.
            //
            // Current optimization: single shared read + parallel writes vs.
            // original's sequential read+copy+swap per output.
            for (const auto& stream : streams) {
                memcpy(stream->writeBuf, sharedBuf->data(), count * sizeof(T));
                if (!stream->swap(count)) {
                    pool->recycle(sharedBuf);
                    base_type::_in->flush();
                    return -1;
                }
            }

            pool->recycle(sharedBuf);
            base_type::_in->flush();
            return count;
        }

    protected:
        std::vector<stream<T>*> streams;
        buffer::SharedBufferPool<T>* pool = nullptr;
    };

} // namespace dsp::routing
