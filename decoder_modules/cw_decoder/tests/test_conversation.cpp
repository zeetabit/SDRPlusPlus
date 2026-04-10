#include <catch.hpp>
#include <cw/conversation.h>

using namespace cw;

// ============================================================
// Conversation State Machine Tests
// ============================================================

TEST_CASE("Conversation: starts in IDLE", "[cw][conversation]") {
    ConversationTracker conv;
    REQUIRE(conv.getState() == ConversationTracker::IDLE);
}

TEST_CASE("Conversation: CQ triggers CQ_CALL state", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);
    // Multiple CQ's stay in CQ_CALL
    conv.feedWord("CQ");
    conv.feedWord("CQ");
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);
}

TEST_CASE("Conversation: DE after CQ triggers EXCHANGE", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("CQ");
    conv.feedWord("DE");
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);
}

TEST_CASE("Conversation: callsign in EXCHANGE stays in EXCHANGE", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);
}

TEST_CASE("Conversation: RST triggers RST state", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("RST");
    REQUIRE(conv.getState() == ConversationTracker::RST_EXCHANGE);
}

TEST_CASE("Conversation: 599 in EXCHANGE triggers RST state", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("599");
    REQUIRE(conv.getState() == ConversationTracker::RST_EXCHANGE);
}

TEST_CASE("Conversation: 73 triggers CLOSING", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("73");
    REQUIRE(conv.getState() == ConversationTracker::CLOSING);
}

TEST_CASE("Conversation: SK triggers CLOSING", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("SK");
    REQUIRE(conv.getState() == ConversationTracker::CLOSING);
}

TEST_CASE("Conversation: reset returns to IDLE", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.reset();
    REQUIRE(conv.getState() == ConversationTracker::IDLE);
}

// ============================================================
// Context-Aware Correction Boost Tests
// ============================================================

TEST_CASE("Conversation: CQ_CALL boosts CQ/DE corrections", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");

    // In CQ_CALL state, "CQ" and "DE" should be boosted
    REQUIRE(conv.isExpectedWord("CQ"));
    REQUIRE(conv.isExpectedWord("DE"));
    REQUIRE_FALSE(conv.isExpectedWord("73"));
    REQUIRE_FALSE(conv.isExpectedWord("RST"));
}

TEST_CASE("Conversation: EXCHANGE boosts callsign/RST", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");

    REQUIRE(conv.isExpectedWord("RST"));
    REQUIRE(conv.isExpectedWord("UR"));
    REQUIRE(conv.isExpectedWord("599"));
    REQUIRE_FALSE(conv.isExpectedWord("CQ"));
}

TEST_CASE("Conversation: RST_EXCHANGE boosts numbers", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("RST");

    REQUIRE(conv.isExpectedWord("599"));
    REQUIRE(conv.isExpectedWord("579"));
    REQUIRE(conv.isExpectedWord("QTH"));
    REQUIRE_FALSE(conv.isExpectedWord("CQ"));
}

TEST_CASE("Conversation: CLOSING boosts 73/SK/TU/CQ", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("73");

    REQUIRE(conv.isExpectedWord("73"));
    REQUIRE(conv.isExpectedWord("SK"));
    REQUIRE(conv.isExpectedWord("TU"));
    REQUIRE(conv.isExpectedWord("CQ"));  // new CQ call may follow
    REQUIRE_FALSE(conv.isExpectedWord("RST"));
}

// ============================================================
// New Conversation Detection (missed ending)
// ============================================================

TEST_CASE("Conversation: high-confidence CQ in EXCHANGE resets to CQ_CALL", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);

    // High-confidence CQ mid-exchange = new station calling, missed previous ending
    conv.feedWord("CQ", 0.9f);
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);
}

TEST_CASE("Conversation: high-confidence CQ in RST_EXCHANGE resets", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("RST");
    REQUIRE(conv.getState() == ConversationTracker::RST_EXCHANGE);

    conv.feedWord("CQ", 0.95f);
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);
}

TEST_CASE("Conversation: low-confidence CQ in EXCHANGE does NOT reset", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);

    // Low-confidence CQ is probably a decode error, not a new conversation
    conv.feedWord("CQ", 0.4f);
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);
}

TEST_CASE("Conversation: high-confidence CQ at different WPM does NOT reset", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ", 0.9f, 15.0f);
    conv.feedWord("DE", 0.9f, 15.0f);
    conv.feedWord("W1AW", 0.9f, 15.0f);
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);

    // CQ at 25 WPM — 67% faster than locked 15 WPM. That's a different operator.
    conv.feedWord("CQ", 0.95f, 25.0f);
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);  // NOT reset
}

TEST_CASE("Conversation: high-confidence CQ at similar WPM DOES reset", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ", 0.9f, 15.0f);
    conv.feedWord("DE", 0.9f, 15.0f);
    conv.feedWord("W1AW", 0.9f, 15.0f);
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);

    // CQ at 17 WPM — within 30% of locked 15 WPM. Same operator, new conversation.
    conv.feedWord("CQ", 0.95f, 17.0f);
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);
}

TEST_CASE("Conversation: CQ in CLOSING resets normally (any confidence)", "[cw][conversation]") {
    ConversationTracker conv;
    conv.feedWord("CQ");
    conv.feedWord("DE");
    conv.feedWord("W1AW");
    conv.feedWord("73");
    REQUIRE(conv.getState() == ConversationTracker::CLOSING);

    // CQ after closing is expected — handled by normal state machine, not the override
    conv.feedWord("CQ", 0.5f);
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);
}

// ============================================================
// Full QSO Sequence Test
// ============================================================

TEST_CASE("Conversation: full QSO sequence", "[cw][conversation]") {
    ConversationTracker conv;

    // Typical QSO flow
    conv.feedWord("CQ");
    conv.feedWord("CQ");
    conv.feedWord("CQ");
    REQUIRE(conv.getState() == ConversationTracker::CQ_CALL);

    conv.feedWord("DE");
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);

    conv.feedWord("W1AW");
    conv.feedWord("W1AW");
    conv.feedWord("K");
    REQUIRE(conv.getState() == ConversationTracker::EXCHANGE);

    conv.feedWord("UR");
    conv.feedWord("RST");
    REQUIRE(conv.getState() == ConversationTracker::RST_EXCHANGE);

    conv.feedWord("599");
    conv.feedWord("599");
    conv.feedWord("BK");
    REQUIRE(conv.getState() == ConversationTracker::RST_EXCHANGE);

    conv.feedWord("TNX");
    conv.feedWord("FER");
    conv.feedWord("QSO");
    conv.feedWord("73");
    REQUIRE(conv.getState() == ConversationTracker::CLOSING);

    conv.feedWord("SK");
    REQUIRE(conv.getState() == ConversationTracker::CLOSING);
}
