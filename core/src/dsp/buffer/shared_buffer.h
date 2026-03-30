#pragma once
#include <atomic>
#include <vector>
#include <mutex>
#include "buffer.h"

namespace dsp::buffer {

    // Reference-counted buffer for zero-copy fan-out.
    // Acquired from a pool, returned when all consumers release it.
    template <class T>
    class SharedBuffer {
    public:
        SharedBuffer(int capacity) : _capacity(capacity), _refCount(0) {
            _data = alloc<T>(capacity);
        }

        ~SharedBuffer() {
            if (_data) { free(_data); }
        }

        SharedBuffer(const SharedBuffer&) = delete;
        SharedBuffer& operator=(const SharedBuffer&) = delete;

        T* data() { return _data; }
        int capacity() const { return _capacity; }

        void addRef() { _refCount.fetch_add(1, std::memory_order_relaxed); }

        // Returns true if this was the last reference (buffer can be recycled).
        bool release() { return _refCount.fetch_sub(1, std::memory_order_acq_rel) == 1; }

        int refCount() const { return _refCount.load(std::memory_order_relaxed); }

    private:
        T* _data;
        int _capacity;
        std::atomic<int> _refCount;
    };

    // Pool of shared buffers to avoid allocation churn.
    template <class T>
    class SharedBufferPool {
    public:
        SharedBufferPool(int bufferCapacity, int initialCount = 4)
            : _bufferCapacity(bufferCapacity) {
            for (int i = 0; i < initialCount; i++) {
                _free.push_back(new SharedBuffer<T>(bufferCapacity));
            }
        }

        ~SharedBufferPool() {
            for (auto* buf : _free) { delete buf; }
        }

        SharedBufferPool(const SharedBufferPool&) = delete;
        SharedBufferPool& operator=(const SharedBufferPool&) = delete;

        // Acquire a buffer from the pool (creates one if pool is empty).
        SharedBuffer<T>* acquire() {
            std::lock_guard<std::mutex> lck(_mtx);
            if (_free.empty()) {
                return new SharedBuffer<T>(_bufferCapacity);
            }
            auto* buf = _free.back();
            _free.pop_back();
            return buf;
        }

        // Return a buffer to the pool.
        void recycle(SharedBuffer<T>* buf) {
            std::lock_guard<std::mutex> lck(_mtx);
            _free.push_back(buf);
        }

    private:
        int _bufferCapacity;
        std::vector<SharedBuffer<T>*> _free;
        std::mutex _mtx;
    };

} // namespace dsp::buffer
