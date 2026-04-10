#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"

using namespace cw_test;

// ============================================================
// Farnsworth Spacing Tests
//
// Farnsworth sends elements at normal speed but stretches
// inter-character and inter-word gaps. Common in CW training.
// The decoder must adapt gap classification to handle stretched gaps.
// ============================================================

TEST_CASE("Farnsworth: ratio 1.5 CQ at 15 WPM", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(80.0f, 1.5f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.15f);
}

TEST_CASE("Farnsworth: ratio 2.0 CQ at 15 WPM", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(80.0f, 2.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.25f);
}

TEST_CASE("Farnsworth: ratio 1.5 SOS", "[cw][farnsworth]") {
    auto s = decodeAndScore("EEETTT SOS", profileFarnsworth(80.0f, 1.5f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.15f);
}

TEST_CASE("Farnsworth: ratio 1.5 at 20 WPM", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(60.0f, 1.5f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.2f);
}

TEST_CASE("Farnsworth: ratio 1.5 with mild noise", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworthNoisy(80.0f, 1.5f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.2f);
}

TEST_CASE("Farnsworth: ratio 2.0 long message", "[cw][farnsworth]") {
    auto s = decodeAndScore(
        "CQ CQ CQ DE W1AW W1AW QTH NEWINGTON CT K",
        profileFarnsworth(80.0f, 2.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.25f);
}

TEST_CASE("Farnsworth: ratio 1.0 is standard (regression check)", "[cw][farnsworth]") {
    // ratio=1.0 should be identical to standard timing
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(80.0f, 1.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer == 0.0f);
}

TEST_CASE("Farnsworth: word gaps preserved at ratio 2.0", "[cw][farnsworth]") {
    // At ratio=2.0, word gaps are 14 dit lengths (vs 7 standard).
    // The decoder must still identify word boundaries correctly.
    std::string decoded = decode(MSG_CQ(), profileFarnsworth(80.0f, 2.0f));
    std::string norm = normalize(decoded);
    INFO("decoded='" << norm << "'");
    // Should have multiple words (spaces between them)
    auto words = splitWords(norm);
    INFO("word count=" << words.size());
    REQUIRE(words.size() >= 4);  // CQ CQ CQ DE ... at least 4 words
}
