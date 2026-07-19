#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"
using namespace cw_test;

TEST_CASE("Registry: cores are selectable and distinct", "[cw][registry]") {
    REQUIRE(cw::coreRegistry().size() >= 7);
    for (const auto& s : cw::coreRegistry()) {
        INFO("core=" << s.name);
        auto c = s.make();
        REQUIRE(c != nullptr);
    }
    // Unknown name must fall back, not crash
    cw::Channel ch; ch.init(0, 700.0f, "does-not-exist");
    REQUIRE(ch.coreName() == cw::DEFAULT_CORE);
}
