#include <catch.hpp>
#include <dsp/buffer/shared_buffer.h>

TEST_CASE("SharedBuffer reference counting", "[shared_buffer]") {
    dsp::buffer::SharedBuffer<float> buf(1024);

    SECTION("initial refcount is 0") {
        REQUIRE(buf.refCount() == 0);
    }

    SECTION("addRef increments") {
        buf.addRef();
        REQUIRE(buf.refCount() == 1);
        buf.addRef();
        REQUIRE(buf.refCount() == 2);
        buf.release();
        buf.release();
    }

    SECTION("release returns true on last ref") {
        buf.addRef();
        buf.addRef();
        REQUIRE_FALSE(buf.release());  // refcount 2 -> 1
        REQUIRE(buf.release());         // refcount 1 -> 0, last ref
    }

    SECTION("data pointer is valid") {
        REQUIRE(buf.data() != nullptr);
        REQUIRE(buf.capacity() == 1024);
    }

    SECTION("data is writable") {
        buf.data()[0] = 3.14f;
        buf.data()[1023] = 2.71f;
        REQUIRE(buf.data()[0] == Approx(3.14f));
        REQUIRE(buf.data()[1023] == Approx(2.71f));
    }
}

TEST_CASE("SharedBufferPool acquire and recycle", "[shared_buffer]") {
    dsp::buffer::SharedBufferPool<float> pool(512, 2);

    SECTION("acquire returns valid buffer") {
        auto* buf = pool.acquire();
        REQUIRE(buf != nullptr);
        REQUIRE(buf->capacity() == 512);
        pool.recycle(buf);
    }

    SECTION("recycled buffer is reused") {
        auto* buf1 = pool.acquire();
        auto* buf2 = pool.acquire();
        pool.recycle(buf1);
        auto* buf3 = pool.acquire();
        REQUIRE(buf3 == buf1);  // Reused from pool
        pool.recycle(buf2);
        pool.recycle(buf3);
    }

    SECTION("pool grows beyond initial count") {
        std::vector<dsp::buffer::SharedBuffer<float>*> bufs;
        for (int i = 0; i < 10; i++) {
            bufs.push_back(pool.acquire());
            REQUIRE(bufs.back() != nullptr);
        }
        for (auto* b : bufs) { pool.recycle(b); }
    }
}
