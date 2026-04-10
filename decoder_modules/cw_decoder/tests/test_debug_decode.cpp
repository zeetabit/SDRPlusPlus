#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"

using namespace cw_test;

static void printDecode(const char* label, const std::string& msg, const SignalParams& params) {
    std::string decoded = decode(msg, params);
    auto s = score(normalize(msg), decoded);
    WARN(label << ": ref='" << normalize(msg) << "' dec='" << decoded << "' CER=" << s.cer);
}

TEST_CASE("Debug: all profiles", "[debug]") {
    printDecode("clean_15", MSG_CQ(), profileClean(80.0f));
    printDecode("mild_noise", MSG_CQ(), profileMildNoise(80.0f));
    printDecode("hand_keyed", MSG_CQ(), profileHandKeyed(80.0f));
    printDecode("qsb", MSG_CQ(), profileQSB(80.0f));
    printDecode("qrn", MSG_CQ(), profileQRN(80.0f));
    printDecode("contest", MSG_MIXED(), profileContest(60.0f));
    printDecode("worst", MSG_CQ(), profileWorstCase(80.0f));
}
