#include <catch.hpp>
#include <cw/vocabulary.h>
#include <cw/corrector.h>

using namespace cw;

// ============================================================
// Vocabulary Tests
// ============================================================

TEST_CASE("Vocabulary: common CW words recognized", "[cw][vocab]") {
    REQUIRE(vocabulary::isKnownWord("CQ"));
    REQUIRE(vocabulary::isKnownWord("DE"));
    REQUIRE(vocabulary::isKnownWord("RST"));
    REQUIRE(vocabulary::isKnownWord("QTH"));
    REQUIRE(vocabulary::isKnownWord("73"));
    REQUIRE(vocabulary::isKnownWord("TEST"));
    REQUIRE(vocabulary::isKnownWord("K"));
    REQUIRE(vocabulary::isKnownWord("BK"));
}

TEST_CASE("Vocabulary: unknown words not recognized", "[cw][vocab]") {
    REQUIRE_FALSE(vocabulary::isKnownWord("XYZ"));
    REQUIRE_FALSE(vocabulary::isKnownWord("QQQQQ"));
    REQUIRE_FALSE(vocabulary::isKnownWord(""));
}

TEST_CASE("Vocabulary: callsign patterns recognized", "[cw][vocab]") {
    REQUIRE(vocabulary::isCallsign("W1AW"));
    REQUIRE(vocabulary::isCallsign("K1ABC"));
    REQUIRE(vocabulary::isCallsign("VE3NEA"));
    REQUIRE(vocabulary::isCallsign("JA1XYZ"));
    REQUIRE(vocabulary::isCallsign("DL1ABC"));
    REQUIRE_FALSE(vocabulary::isCallsign("CQ"));
    REQUIRE_FALSE(vocabulary::isCallsign("123"));
    REQUIRE_FALSE(vocabulary::isCallsign("A"));
}

TEST_CASE("Vocabulary: RST pattern recognized", "[cw][vocab]") {
    REQUIRE(vocabulary::isRSTReport("599"));
    REQUIRE(vocabulary::isRSTReport("579"));
    REQUIRE(vocabulary::isRSTReport("339"));
    REQUIRE_FALSE(vocabulary::isRSTReport("099"));
    REQUIRE_FALSE(vocabulary::isRSTReport("59"));
    REQUIRE_FALSE(vocabulary::isRSTReport("ABC"));
}

TEST_CASE("Vocabulary: Q-codes recognized", "[cw][vocab]") {
    REQUIRE(vocabulary::isQCode("QTH"));
    REQUIRE(vocabulary::isQCode("QSO"));
    REQUIRE(vocabulary::isQCode("QRM"));
    REQUIRE(vocabulary::isQCode("QSB"));
    REQUIRE_FALSE(vocabulary::isQCode("QT"));
    REQUIRE_FALSE(vocabulary::isQCode("ABC"));
}

// ============================================================
// Corrector Tests
// ============================================================

TEST_CASE("Corrector: exact match not changed", "[cw][corrector]") {
    REQUIRE(corrector::correctWord("CQ", 0.9f) == "CQ");
    REQUIRE(corrector::correctWord("DE", 0.9f) == "DE");
    REQUIRE(corrector::correctWord("TEST", 0.9f) == "TEST");
    REQUIRE(corrector::correctWord("W1AW", 0.9f) == "W1AW");
}

TEST_CASE("Corrector: near-miss dictionary correction", "[cw][corrector]") {
    // edit distance 1 from dictionary words, low confidence → correct
    REQUIRE(corrector::correctWord("9Q", 0.4f) == "CQ");    // 9→C
    REQUIRE(corrector::correctWord("TIST", 0.4f) == "TEST"); // I→E
    // "DT" is ambiguous (could be BT or DE) — not tested
}

TEST_CASE("Corrector: high confidence not corrected", "[cw][corrector]") {
    // Even near-miss, if confidence is high, trust the decode
    REQUIRE(corrector::correctWord("9Q", 0.95f) == "9Q");
}

TEST_CASE("Corrector: distant words not corrected", "[cw][corrector]") {
    REQUIRE(corrector::correctWord("XYZ", 0.4f) == "XYZ");
    REQUIRE(corrector::correctWord("ABCD", 0.4f) == "ABCD");
}

TEST_CASE("Corrector: short words not false-corrected", "[cw][corrector]") {
    // 2-letter words without prosigns should NOT be corrected
    // even if distance 1 from a dictionary word (MY↔BK, etc.)
    REQUIRE(corrector::correctWord("MY", 0.4f) == "MY");
    REQUIRE(corrector::correctWord("BY", 0.4f) == "BY");
    REQUIRE(corrector::correctWord("MK", 0.4f) == "MK");
    // But prosign chars in 2-letter words ARE corrected
    REQUIRE(corrector::correctWord("+Q", 0.4f) == "CQ");
    REQUIRE(corrector::correctWord("*K", 0.4f) == "SK");
}

TEST_CASE("Corrector: callsign digit/letter fix", "[cw][corrector]") {
    // K1A6C → K1ABC: 6→B (edit distance 1, looks like callsign)
    REQUIRE(corrector::correctWord("K1A6C", 0.4f) == "K1ABC");
}

TEST_CASE("Corrector: correct word preserved", "[cw][corrector]") {
    // "TEST" should not become "BEST" even though edit distance = 1
    REQUIRE(corrector::correctWord("TEST", 0.5f) == "TEST");
    REQUIRE(corrector::correctWord("599", 0.5f) == "599");
}

TEST_CASE("Corrector: RST report correction", "[cw][corrector]") {
    // 5NN → 599 (N looks like 9 in CW: -. vs ----.)
    // Actually 5NN is a valid contest shorthand, keep it
    REQUIRE(corrector::correctWord("5NN", 0.5f) == "5NN");
    // But +99 should become 599 (+ is not a valid RST digit)
    REQUIRE(corrector::correctWord("+99", 0.4f) == "599");
}
