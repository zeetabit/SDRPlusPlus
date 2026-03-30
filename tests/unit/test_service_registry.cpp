#include <catch.hpp>
#include <utils/service_registry.h>

namespace {
    class ITestService {
    public:
        virtual ~ITestService() = default;
        virtual int getValue() = 0;
    };

    class IOtherService {
    public:
        virtual ~IOtherService() = default;
        virtual std::string getName() = 0;
    };

    class TestImpl : public ITestService {
    public:
        TestImpl(int v) : val(v) {}
        int getValue() override { return val; }
    private:
        int val;
    };

    class OtherImpl : public IOtherService {
    public:
        OtherImpl(std::string n) : name(std::move(n)) {}
        std::string getName() override { return name; }
    private:
        std::string name;
    };
}

TEST_CASE("ServiceRegistry provide and query", "[service_registry]") {
    auto& reg = ServiceRegistry::get();

    TestImpl impl(42);

    SECTION("provide then query returns same pointer") {
        REQUIRE(reg.provide<ITestService>("test1", &impl));
        auto* result = reg.query<ITestService>("test1");
        REQUIRE(result != nullptr);
        REQUIRE(result->getValue() == 42);
        reg.remove<ITestService>("test1");
    }

    SECTION("query nonexistent returns nullptr") {
        REQUIRE(reg.query<ITestService>("nonexistent") == nullptr);
    }

    SECTION("duplicate provide returns false") {
        REQUIRE(reg.provide<ITestService>("dup", &impl));
        REQUIRE_FALSE(reg.provide<ITestService>("dup", &impl));
        reg.remove<ITestService>("dup");
    }
}

TEST_CASE("ServiceRegistry remove", "[service_registry]") {
    auto& reg = ServiceRegistry::get();
    TestImpl impl(10);

    reg.provide<ITestService>("rm_test", &impl);
    REQUIRE(reg.exists<ITestService>("rm_test"));

    REQUIRE(reg.remove<ITestService>("rm_test"));
    REQUIRE_FALSE(reg.exists<ITestService>("rm_test"));
    REQUIRE(reg.query<ITestService>("rm_test") == nullptr);

    SECTION("remove nonexistent returns false") {
        REQUIRE_FALSE(reg.remove<ITestService>("doesnt_exist"));
    }
}

TEST_CASE("ServiceRegistry type isolation", "[service_registry]") {
    auto& reg = ServiceRegistry::get();
    TestImpl testImpl(7);
    OtherImpl otherImpl("hello");

    reg.provide<ITestService>("shared_name", &testImpl);
    reg.provide<IOtherService>("shared_name", &otherImpl);

    auto* t = reg.query<ITestService>("shared_name");
    auto* o = reg.query<IOtherService>("shared_name");

    REQUIRE(t != nullptr);
    REQUIRE(o != nullptr);
    REQUIRE(t->getValue() == 7);
    REQUIRE(o->getName() == "hello");

    reg.remove<ITestService>("shared_name");
    reg.remove<IOtherService>("shared_name");
}

TEST_CASE("ServiceRegistry listNames", "[service_registry]") {
    auto& reg = ServiceRegistry::get();
    TestImpl a(1), b(2), c(3);

    reg.provide<ITestService>("list_a", &a);
    reg.provide<ITestService>("list_b", &b);
    reg.provide<ITestService>("list_c", &c);

    auto names = reg.listNames<ITestService>();
    REQUIRE(names.size() >= 3);

    // Check all three are present (order not guaranteed)
    std::sort(names.begin(), names.end());
    REQUIRE(std::find(names.begin(), names.end(), "list_a") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "list_b") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "list_c") != names.end());

    reg.remove<ITestService>("list_a");
    reg.remove<ITestService>("list_b");
    reg.remove<ITestService>("list_c");
}

TEST_CASE("ServiceRegistry exists", "[service_registry]") {
    auto& reg = ServiceRegistry::get();
    TestImpl impl(0);

    REQUIRE_FALSE(reg.exists<ITestService>("ex_test"));
    reg.provide<ITestService>("ex_test", &impl);
    REQUIRE(reg.exists<ITestService>("ex_test"));
    reg.remove<ITestService>("ex_test");
    REQUIRE_FALSE(reg.exists<ITestService>("ex_test"));
}
