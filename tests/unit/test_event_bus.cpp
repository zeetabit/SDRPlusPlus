#include <catch.hpp>
#include <utils/event_bus.h>
#include <thread>
#include <atomic>
#include <vector>

namespace {
    struct TestEvent { int value; };
    struct OtherEvent { std::string msg; };
}

TEST_CASE("EventBus subscribe and publish", "[event_bus]") {
    auto& bus = EventBus::get();

    SECTION("handler receives published event") {
        int received = -1;
        auto sub = bus.subscribe<TestEvent>([&](const TestEvent& e) {
            received = e.value;
        });

        bus.publish(TestEvent{42});
        REQUIRE(received == 42);
    }

    SECTION("multiple subscribers all receive event") {
        int count = 0;
        auto s1 = bus.subscribe<TestEvent>([&](const TestEvent&) { count++; });
        auto s2 = bus.subscribe<TestEvent>([&](const TestEvent&) { count++; });
        auto s3 = bus.subscribe<TestEvent>([&](const TestEvent&) { count++; });

        bus.publish(TestEvent{1});
        REQUIRE(count == 3);
    }

    SECTION("different event types are isolated") {
        int testCount = 0;
        int otherCount = 0;
        auto s1 = bus.subscribe<TestEvent>([&](const TestEvent&) { testCount++; });
        auto s2 = bus.subscribe<OtherEvent>([&](const OtherEvent&) { otherCount++; });

        bus.publish(TestEvent{1});
        REQUIRE(testCount == 1);
        REQUIRE(otherCount == 0);

        bus.publish(OtherEvent{"hello"});
        REQUIRE(testCount == 1);
        REQUIRE(otherCount == 1);
    }

    SECTION("publish with no subscribers does not crash") {
        struct UnusedEvent { int x; };
        REQUIRE_NOTHROW(bus.publish(UnusedEvent{99}));
    }
}

TEST_CASE("EventBus unsubscribe", "[event_bus]") {
    auto& bus = EventBus::get();

    SECTION("RAII unsubscribe on Subscription destruction") {
        int count = 0;
        {
            auto sub = bus.subscribe<TestEvent>([&](const TestEvent&) { count++; });
            bus.publish(TestEvent{1});
            REQUIRE(count == 1);
        }
        // sub destroyed, handler removed
        bus.publish(TestEvent{2});
        REQUIRE(count == 1);
    }

    SECTION("manual unsubscribe") {
        int count = 0;
        auto sub = bus.subscribe<TestEvent>([&](const TestEvent&) { count++; });

        bus.publish(TestEvent{1});
        REQUIRE(count == 1);

        sub.unsubscribe();
        bus.publish(TestEvent{2});
        REQUIRE(count == 1);
    }

    SECTION("double unsubscribe is safe") {
        auto sub = bus.subscribe<TestEvent>([](const TestEvent&) {});
        REQUIRE_NOTHROW(sub.unsubscribe());
        REQUIRE_NOTHROW(sub.unsubscribe());
    }
}

TEST_CASE("EventBus move semantics", "[event_bus]") {
    auto& bus = EventBus::get();
    int count = 0;

    Subscription moved;
    {
        auto sub = bus.subscribe<TestEvent>([&](const TestEvent&) { count++; });
        moved = std::move(sub);
    }
    // Original destroyed, but moved-to still active
    bus.publish(TestEvent{1});
    REQUIRE(count == 1);

    moved.unsubscribe();
    bus.publish(TestEvent{2});
    REQUIRE(count == 1);
}

TEST_CASE("EventBus subscriber count", "[event_bus]") {
    auto& bus = EventBus::get();

    struct CountTestEvent { int x; };
    REQUIRE(bus.subscriberCount<CountTestEvent>() == 0);

    auto s1 = bus.subscribe<CountTestEvent>([](const CountTestEvent&) {});
    REQUIRE(bus.subscriberCount<CountTestEvent>() == 1);

    auto s2 = bus.subscribe<CountTestEvent>([](const CountTestEvent&) {});
    REQUIRE(bus.subscriberCount<CountTestEvent>() == 2);

    s1.unsubscribe();
    REQUIRE(bus.subscriberCount<CountTestEvent>() == 1);
}

TEST_CASE("EventBus thread safety", "[event_bus]") {
    auto& bus = EventBus::get();
    std::atomic<int> total{0};

    struct ThreadEvent { int val; };

    std::vector<Subscription> subs;
    for (int i = 0; i < 10; i++) {
        subs.push_back(bus.subscribe<ThreadEvent>([&](const ThreadEvent& e) {
            total.fetch_add(e.val, std::memory_order_relaxed);
        }));
    }

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < 100; i++) {
                bus.publish(ThreadEvent{1});
            }
        });
    }

    for (auto& t : threads) { t.join(); }

    // 4 threads * 100 publishes * 10 subscribers * 1 value = 4000
    REQUIRE(total.load() == 4000);
}

TEST_CASE("EventBus publishWithTimeout completes normally", "[event_bus]") {
    auto& bus = EventBus::get();

    struct TimeoutTestEvent { int x; };

    int received = 0;
    auto sub = bus.subscribe<TimeoutTestEvent>([&](const TimeoutTestEvent& e) {
        received = e.x;
    });

    bus.publishWithTimeout(TimeoutTestEvent{42}, 1000);
    REQUIRE(received == 42);
}

TEST_CASE("EventBus publishWithTimeout does not hang on slow handler", "[event_bus]") {
    auto& bus = EventBus::get();

    struct SlowEvent {};

    std::atomic<bool> handlerStarted{false};
    auto sub = bus.subscribe<SlowEvent>([&](const SlowEvent&) {
        handlerStarted = true;
        std::this_thread::sleep_for(std::chrono::seconds(10)); // intentionally slow
    });

    auto start = std::chrono::steady_clock::now();
    bus.publishWithTimeout(SlowEvent{}, 200); // 200ms timeout
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    REQUIRE(handlerStarted);
    REQUIRE(elapsed < 2000); // should complete well under 2s, not wait 10s
}
