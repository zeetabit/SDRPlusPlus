#include <catch.hpp>
#include <cw/channel.h>
#include <cw/dsp.h>
#include "cw_test_signals.h"
#include <cstring>

using namespace cw_test;

// ============================================================
// DSP Chain Tests
// ============================================================

TEST_CASE("EnvelopeDSP extracts envelope from tone", "[cw][dsp]") {
    cw::EnvelopeDSP dsp;
    dsp.init(700.0f, 8000.0f, 1000.0f);
    const int N = 4000;
    dsp::complex_t iq[N];
    float phase = 0, omega = 2.0f * M_PI * 700.0f / 8000.0f;
    for (int i = 0; i < N; i++) { iq[i].re = cosf(phase); iq[i].im = sinf(phase); phase += omega; }
    float env[N / 8 + 100];
    int outCount = dsp.process(N, iq, env);
    REQUIRE(outCount == N / 8);
    float maxEnv = 0;
    for (int i = outCount / 2; i < outCount; i++) if (env[i] > maxEnv) maxEnv = env[i];
    REQUIRE(maxEnv > 0.1f);
}

TEST_CASE("EnvelopeDSP rejects off-frequency tone", "[cw][dsp]") {
    cw::EnvelopeDSP dsp;
    dsp.init(700.0f, 8000.0f, 1000.0f);
    dsp::complex_t iq[8000];
    float phase = 0, omega = 2.0f * M_PI * 300.0f / 8000.0f;
    for (int i = 0; i < 8000; i++) { iq[i].re = cosf(phase); iq[i].im = sinf(phase); phase += omega; }
    float env[2000];
    int outCount = dsp.process(8000, iq, env);
    float maxEnv = 0;
    for (int i = outCount / 2; i < outCount; i++) if (env[i] > maxEnv) maxEnv = env[i];
    REQUIRE(maxEnv < 0.05f);
}

TEST_CASE("EnvelopeDSP silence produces near-zero", "[cw][dsp]") {
    cw::EnvelopeDSP dsp;
    dsp.init(700.0f, 8000.0f, 1000.0f);
    dsp::complex_t iq[8000];
    memset(iq, 0, sizeof(iq));
    float env[2000];
    int outCount = dsp.process(8000, iq, env);
    float maxEnv = 0;
    for (int i = 0; i < outCount; i++) if (fabsf(env[i]) > maxEnv) maxEnv = fabsf(env[i]);
    REQUIRE(maxEnv < 0.001f);
}

// ============================================================
// Channel Integration Tests (full IQ path)
// ============================================================

TEST_CASE("Channel decodes SOS", "[cw][integration]") {
    auto s = decodeAndScore("EEETTT SOS", profileClean(80.0f));
    INFO("SOS CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Channel decodes CQ at 20 WPM", "[cw][integration]") {
    auto s = decodeAndScore("EEETTT CQ", profileClean(60.0f));
    INFO("CQ CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Channel decodes at 25 WPM", "[cw][integration]") {
    auto s = decodeAndScore("EEETTT HI", profileClean(48.0f));
    INFO("25WPM CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

// ============================================================
// Morse Ambiguity Tests
// ============================================================

TEST_CASE("Morse: I vs E E", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT I", profileClean());
    INFO("I CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Morse: A vs E T", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT A", profileClean());
    INFO("A CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Morse: N vs T E", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT N", profileClean());
    INFO("N CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Morse: M vs T T", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT M", profileClean());
    INFO("M CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Morse: K", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT K", profileClean());
    INFO("K CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("Morse: distinct E E vs I", "[cw][morse][ambiguity]") {
    std::string d = decode("EEETTT E E", profileClean());
    int eCount = 0;
    for (char c : d) if (c == 'E') eCount++;
    REQUIRE(eCount >= 2);
}

TEST_CASE("Morse: distinct T T vs M", "[cw][morse][ambiguity]") {
    std::string d = decode("EEETTT T T", profileClean());
    int tCount = 0;
    for (char c : d) if (c == 'T') tCount++;
    REQUIRE(tCount >= 2);
}

TEST_CASE("Morse: PARIS word", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT PARIS", profileClean());
    INFO("PARIS CER=" << s.cer);
    REQUIRE(s.cer < 0.3f);
}

TEST_CASE("Morse: 5 and H", "[cw][morse][ambiguity]") {
    auto s = decodeAndScore("EEETTT 5 H", profileClean());
    INFO("5H CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

// ============================================================
// WPM Range Tests (10-35 WPM)
// ============================================================

TEST_CASE("WPM: 10 (dit=120ms)", "[cw][morse][wpm]") {
    auto s = decodeAndScore("EEETTT SOS", profileClean(120.0f));
    INFO("10WPM CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("WPM: 15 (dit=80ms)", "[cw][morse][wpm]") {
    auto s = decodeAndScore("EEETTT SOS", profileClean(80.0f));
    INFO("15WPM CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("WPM: 20 (dit=60ms)", "[cw][morse][wpm]") {
    auto s = decodeAndScore("EEETTT SOS", profileClean(60.0f));
    INFO("20WPM CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("WPM: 30 (dit=40ms)", "[cw][morse][wpm]") {
    auto s = decodeAndScore("EEETTT SOS", profileClean(40.0f));
    INFO("30WPM CER=" << s.cer);
    REQUIRE(s.cer < 0.5f);
}

TEST_CASE("WPM: 35 (dit=34ms)", "[cw][morse][wpm]") {
    auto s = decodeAndScore("EEETTT SOS", profileClean(34.0f));
    INFO("35WPM CER=" << s.cer);
    REQUIRE(s.cer < 0.7f);
}
