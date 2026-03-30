#include <catch.hpp>
#include <dsp/engine/thread_pool.h>
#include <atomic>
#include <chrono>

TEST_CASE("ThreadPool basic execution", "[thread_pool]") {
    dsp::engine::ThreadPool pool(2);

    SECTION("submit returns correct result via future") {
        auto f = pool.submit([]() -> int { return 42; });
        REQUIRE(f.get() == 42);
    }

    SECTION("submitAsync executes work") {
        std::atomic<bool> done{false};
        pool.submitAsync([&]() { done = true; });

        // Wait up to 1s for completion
        for (int i = 0; i < 100 && !done; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        REQUIRE(done);
    }

    SECTION("pool has at least base thread count") {
        REQUIRE(pool.size() >= 2);
    }
}

TEST_CASE("ThreadPool concurrent execution", "[thread_pool]") {
    dsp::engine::ThreadPool pool(4);
    std::atomic<int> counter{0};

    std::vector<std::future<int>> futures;
    for (int i = 0; i < 100; i++) {
        futures.push_back(pool.submit([&]() -> int {
            counter.fetch_add(1);
            return 1;
        }));
    }

    int total = 0;
    for (auto& f : futures) {
        total += f.get();
    }

    REQUIRE(total == 100);
    REQUIRE(counter.load() == 100);
}

TEST_CASE("ThreadPool handles heavy load", "[thread_pool]") {
    dsp::engine::ThreadPool pool(2);
    std::atomic<int> sum{0};

    for (int i = 0; i < 1000; i++) {
        pool.submitAsync([&, i]() {
            sum.fetch_add(i);
        });
    }

    // Submit a barrier task and wait for it
    auto barrier = pool.submit([]() -> int { return 0; });
    barrier.get();

    // Sum of 0..999 = 499500
    REQUIRE(sum.load() == 499500);
}

TEST_CASE("ThreadPool default size", "[thread_pool]") {
    dsp::engine::ThreadPool pool(0);
    int expected = (int)std::thread::hardware_concurrency();
    if (expected < 1) expected = 4;
    REQUIRE(pool.size() == expected);
}

TEST_CASE("ThreadPool global singleton", "[thread_pool]") {
    auto& p1 = dsp::engine::getPool();
    auto& p2 = dsp::engine::getPool();
    REQUIRE(&p1 == &p2);
}
