#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"

using namespace cw_test;

TEST_CASE("Weak: noise=0.5", "[cw][weak]") {
    auto s = decodeAndScore("EEETTT SOS", profileMildNoise(80.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Weak: noise=1.5", "[cw][weak]") {
    auto s = decodeAndScore("EEETTT SOS", profileModerateNoise(80.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.8f);
}

TEST_CASE("Weak: QSB fading", "[cw][weak]") {
    auto s = decodeAndScore("EEETTT SOS", profileQSB(80.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.8f);
}

TEST_CASE("Weak: hand-keyed", "[cw][weak]") {
    auto s = decodeAndScore("EEETTT SOS", profileHandKeyed(80.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.6f);
}

TEST_CASE("Weak: QRM", "[cw][weak]") {
    auto s = decodeAndScore("EEETTT SOS", profileQRM(80.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Weak: QRN impulse", "[cw][weak]") {
    auto s = decodeAndScore("EEETTT SOS", profileQRN(80.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}
