#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"
#include <cstring>

using namespace cw_test;

static DecodeScore runBenchmark(const std::string& message, const SignalParams& params) {
    return decodeAndScore(message, params);
}

// ============================================================
// CER/WER Scoring Tests — verify the metric itself
// ============================================================

TEST_CASE("Score: perfect decode", "[cw][benchmark][score]") {
    auto s = score("SOS", "SOS");
    REQUIRE(s.cer == 0.0f);
    REQUIRE(s.wer == 0.0f);
}

TEST_CASE("Score: one char error", "[cw][benchmark][score]") {
    auto s = score("SOS", "SOT");
    REQUIRE(s.charErrors == 1);
    REQUIRE(s.cer == Approx(1.0f / 3.0f));
}

TEST_CASE("Score: missing word", "[cw][benchmark][score]") {
    auto s = score("CQ CQ DE W1AW", "CQ DE W1AW");
    REQUIRE(s.wordErrors == 1);
    REQUIRE(s.wer == Approx(1.0f / 4.0f));
}

TEST_CASE("Score: extra characters", "[cw][benchmark][score]") {
    auto s = score("SOS", "XSOS");
    REQUIRE(s.charErrors == 1);  // one insertion
}

TEST_CASE("Score: empty decode", "[cw][benchmark][score]") {
    auto s = score("SOS", "");
    REQUIRE(s.charErrors == 3);
    REQUIRE(s.cer == 1.0f);
}

TEST_CASE("Score: normalization", "[cw][benchmark][score]") {
    auto s = score("CQ  CQ", "CQ CQ");
    REQUIRE(s.cer == 0.0f);  // collapsed whitespace matches
}

// ============================================================
// Signal Generator Tests — verify the IQ generation
// ============================================================

TEST_CASE("Signal generator: clean signal has correct duration", "[cw][benchmark][gen]") {
    auto p = profileClean();
    auto sig = generateMessage("E", p);  // E = single dit
    // 200ms warmup + 1 dit (80ms) + 10 dit trailing (800ms) ≈ 1.08s
    float durationS = sig.samples.size() / p.sampleRate;
    REQUIRE(durationS > 0.9f);
    REQUIRE(durationS < 1.5f);
}

TEST_CASE("Signal generator: jitter varies durations", "[cw][benchmark][gen]") {
    auto p = profileHandKeyed();
    auto sig1 = generateMessage("EEEEEE", p);
    p.seed = 99;
    auto sig2 = generateMessage("EEEEEE", p);
    // Different seeds should produce different sample counts
    REQUIRE(sig1.samples.size() != sig2.samples.size());
}

TEST_CASE("Signal generator: QRM adds interferer energy", "[cw][benchmark][gen]") {
    auto p = profileClean();
    p.noiseAmp = 0;
    auto clean = generateMessage("E", p);

    p.qrmFreq = 900.0f;
    p.qrmAmp = 1.0f;
    auto withQRM = generateMessage("E", p);

    // QRM signal should have higher total energy during silence
    // Check a window in the warmup silence (first 1000 samples)
    float cleanEnergy = 0, qrmEnergy = 0;
    for (int i = 0; i < 1000; i++) {
        cleanEnergy += clean.samples[i].re * clean.samples[i].re + clean.samples[i].im * clean.samples[i].im;
        qrmEnergy += withQRM.samples[i].re * withQRM.samples[i].re + withQRM.samples[i].im * withQRM.samples[i].im;
    }
    REQUIRE(qrmEnergy > cleanEnergy * 10.0f);
}

// ============================================================
// Baseline Benchmarks — clean signal at various WPM
//
// These establish the decoder's best-case performance.
// CER targets: <0.1 for clean signals (allows warmup garble).
// ============================================================

TEST_CASE("Benchmark: clean SOS at 15 WPM", "[cw][benchmark][baseline]") {
    // SOS alone is only 9 elements — too short for timing to lock.
    // Real CW always has preamble. Use with warmup prefix.
    auto s = runBenchmark("EEE SOS", profileClean(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer << " chars=" << s.refChars << " errors=" << s.charErrors);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: clean CQ at 15 WPM", "[cw][benchmark][baseline]") {
    auto s = runBenchmark(MSG_CQ(), profileClean(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: clean CQ at 20 WPM", "[cw][benchmark][baseline]") {
    auto s = runBenchmark(MSG_CQ(), profileClean(60.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: clean CQ at 25 WPM", "[cw][benchmark][baseline]") {
    auto s = runBenchmark(MSG_CQ(), profileClean(48.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: clean RST at 15 WPM", "[cw][benchmark][baseline]") {
    auto s = runBenchmark(MSG_RST(), profileClean(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

// ============================================================
// Degradation Benchmarks — regression gates (not aspirational).
// These thresholds are set just above measured CER values.
// ============================================================

TEST_CASE("Benchmark: mild noise CQ at 15 WPM", "[cw][benchmark][noise]") {
    auto s = runBenchmark(MSG_CQ(), profileMildNoise(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: moderate noise CQ at 15 WPM", "[cw][benchmark][noise]") {
    auto s = runBenchmark(MSG_CQ(), profileModerateNoise(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: hand-keyed CQ at 15 WPM", "[cw][benchmark][jitter]") {
    auto s = runBenchmark(MSG_CQ(), profileHandKeyed(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: hand-keyed CQ at 20 WPM", "[cw][benchmark][jitter]") {
    auto s = runBenchmark(MSG_CQ(), profileHandKeyed(60.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    CHECK(s.cer < 0.1f);
}

TEST_CASE("Benchmark: hand-keyed CQ at 25 WPM", "[cw][benchmark][jitter]") {
    auto s = runBenchmark(MSG_CQ(), profileHandKeyed(48.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.07f);
}

TEST_CASE("Benchmark: hand-keyed with QRN at 20 WPM", "[cw][benchmark][jitter]") {
    auto p = profileHandKeyed(60.0f);
    p.qrnRate = 0.001f; p.qrnAmp = 3.0f;
    auto s = runBenchmark(MSG_CQ(), p);
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.12f);
}

TEST_CASE("Benchmark: QSB fading CQ at 15 WPM", "[cw][benchmark][qsb]") {
    auto s = runBenchmark(MSG_CQ(), profileQSB(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: QRM interference CQ at 15 WPM", "[cw][benchmark][qrm]") {
    auto s = runBenchmark(MSG_CQ(), profileQRM(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: QRN impulse noise CQ at 15 WPM", "[cw][benchmark][qrn]") {
    auto s = runBenchmark(MSG_CQ(), profileQRN(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: QRN mid-message integrity — easy chars", "[cw][benchmark][qrn]") {
    // W1AW: W(.--) 1(.----) A(.-) W(.--) — mostly dah-heavy, less confusable
    auto qrnStd = profileQRN(80.0f);

    SECTION("standard QRN") {
        auto s = decodeAndScore(MSG_CQ(), qrnStd);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.1f);
    }
    SECTION("heavy rate 5x") {
        auto p = qrnStd; p.qrnRate = 0.005f;
        std::string norm = normalize(decode(MSG_CQ(), p));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("W1AW") != std::string::npos);
    }
    SECTION("strong impulse amp=5") {
        auto p = qrnStd; p.qrnAmp = 5.0f;
        std::string norm = normalize(decode(MSG_CQ(), p));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("W1AW") != std::string::npos);
    }
    SECTION("QRN + noise 0.8") {
        auto p = qrnStd; p.noiseAmp = 0.8f;
        std::string norm = normalize(decode(MSG_CQ(), p));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("W1AW") != std::string::npos);
    }
    SECTION("20 WPM") {
        auto p = profileQRN(60.0f);
        std::string norm = normalize(decode(MSG_CQ(), p));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("W1AW") != std::string::npos);
    }
    SECTION("long message") {
        std::string norm = normalize(decode(
            "CQ CQ CQ DE W1AW W1AW QTH NEWINGTON CT K", qrnStd));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("W1AW") != std::string::npos);
    }
}

TEST_CASE("Benchmark: QRN mid-message integrity — hard chars", "[cw][benchmark][qrn]") {
    // Hard characters under QRN: dit-heavy chars where one extra/missing dit
    // changes the letter. B(-...) vs 6(-....), H(....) vs 5(.....), S(...) vs I(..)
    // These are the most confusable under impulse noise.

    // Use MSG_RST: "UR RST 599 599 BK" — contains R(.-.), S(...), T(-), 5(.....), 9(----.), B(-...)
    SECTION("RST standard QRN") {
        auto s = decodeAndScore(MSG_RST(), profileQRN(80.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("RST 599 survives standard QRN") {
        std::string norm = normalize(decode(MSG_RST(), profileQRN(80.0f)));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("599") != std::string::npos);
    }
    SECTION("RST heavy rate QRN") {
        auto p = profileQRN(80.0f); p.qrnRate = 0.005f;
        auto s = decodeAndScore(MSG_RST(), p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.4f);
    }
    SECTION("RST strong QRN amp=5") {
        auto p = profileQRN(80.0f); p.qrnAmp = 5.0f;
        auto s = decodeAndScore(MSG_RST(), p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("RST QRN + noise 0.8") {
        auto p = profileQRN(80.0f); p.noiseAmp = 0.8f;
        auto s = decodeAndScore(MSG_RST(), p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("RST QRN at 20 WPM") {
        auto s = decodeAndScore(MSG_RST(), profileQRN(60.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }

    // MSG_MIXED: "CQ TEST DE K1ABC 5NN" — B(-...) and 5(.....) are QRN targets
    SECTION("contest ABC survives standard QRN") {
        std::string norm = normalize(decode(MSG_MIXED(), profileQRN(80.0f)));
        INFO("decoded='" << norm << "'");
        REQUIRE(norm.find("ABC") != std::string::npos);
    }
    SECTION("contest heavy rate QRN") {
        auto p = profileQRN(80.0f); p.qrnRate = 0.005f;
        auto s = decodeAndScore(MSG_MIXED(), p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.4f);
    }
    SECTION("contest strong QRN amp=5") {
        auto p = profileQRN(80.0f); p.qrnAmp = 5.0f;
        auto s = decodeAndScore(MSG_MIXED(), p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("contest QRN + noise 0.8") {
        auto p = profileQRN(80.0f); p.noiseAmp = 0.8f;
        auto s = decodeAndScore(MSG_MIXED(), p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("contest QRN at 20 WPM") {
        auto s = decodeAndScore(MSG_MIXED(), profileQRN(60.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }

    // Dit-heavy message: H(....)  5(.....)  B(-...)  6(-....) — one spurious dit flips each
    SECTION("dit-heavy standard QRN") {
        auto s = decodeAndScore("CQ CQ DE TEST H5B6 K", profileQRN(80.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.4f);
    }
    SECTION("dit-heavy strong QRN amp=5") {
        auto p = profileQRN(80.0f); p.qrnAmp = 5.0f;
        auto s = decodeAndScore("CQ CQ DE TEST H5B6 K", p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.5f);
    }
    SECTION("dit-heavy heavy rate QRN") {
        auto p = profileQRN(80.0f); p.qrnRate = 0.005f;
        auto s = decodeAndScore("CQ CQ DE TEST H5B6 K", p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.5f);
    }
    SECTION("dit-heavy QRN + noise 0.8") {
        auto p = profileQRN(80.0f); p.noiseAmp = 0.8f;
        auto s = decodeAndScore("CQ CQ DE TEST H5B6 K", p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.5f);
    }
    SECTION("dit-heavy QRN at 20 WPM") {
        auto s = decodeAndScore("CQ CQ DE TEST H5B6 K", profileQRN(60.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.4f);
    }

    // Number-heavy message: all digits use 5 elements
    SECTION("numbers standard QRN") {
        auto s = decodeAndScore("CQ CQ 599 1234567890 K", profileQRN(80.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("numbers heavy rate QRN") {
        auto p = profileQRN(80.0f); p.qrnRate = 0.005f;
        auto s = decodeAndScore("CQ CQ 599 1234567890 K", p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.5f);
    }
    SECTION("numbers strong QRN amp=5") {
        auto p = profileQRN(80.0f); p.qrnAmp = 5.0f;
        auto s = decodeAndScore("CQ CQ 599 1234567890 K", p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("numbers QRN + noise 0.8") {
        auto p = profileQRN(80.0f); p.noiseAmp = 0.8f;
        auto s = decodeAndScore("CQ CQ 599 1234567890 K", p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("numbers QRN at 20 WPM") {
        auto s = decodeAndScore("CQ CQ 599 1234567890 K", profileQRN(60.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
}

TEST_CASE("Benchmark: contest conditions at 20 WPM", "[cw][benchmark][contest]") {
    auto s = runBenchmark(MSG_MIXED(), profileContest(60.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: contest B not decoded as 6", "[cw][benchmark][contest]") {
    // B(-...) vs 6(-....) — noise can add a phantom dit after B.
    // The message K1ABC contains B which is the prime confusable.
    std::string norm = normalize(decode(MSG_MIXED(), profileContest(60.0f)));
    INFO("decoded='" << norm << "'");
    // B should survive — not become 6
    REQUIRE(norm.find("K1ABC") != std::string::npos);
}

TEST_CASE("Benchmark: contest confusable chars across profiles", "[cw][benchmark][contest]") {
    // Message with all dit-heavy confusable pairs:
    // B(-...) vs 6(-....), D(-..) vs B(-...), H(....) vs 5(.....)
    std::string msg = "CQ TEST DE K1ABC 599 BK";

    SECTION("contest profile") {
        auto s = decodeAndScore(msg, profileContest(60.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.15f);
    }
    SECTION("contest + extra noise") {
        auto p = profileContest(60.0f);
        p.noiseAmp = 1.2f;
        auto s = decodeAndScore(msg, p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
    SECTION("contest at 25 WPM") {
        auto s = decodeAndScore(msg, profileContest(48.0f));
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.2f);
    }
    SECTION("contest + QRN standard") {
        auto p = profileContest(60.0f);
        p.qrnRate = 0.001f; p.qrnAmp = 3.0f;
        auto s = decodeAndScore(msg, p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.25f);
    }
    SECTION("contest + QRN heavy rate") {
        auto p = profileContest(60.0f);
        p.qrnRate = 0.005f; p.qrnAmp = 3.0f;
        auto s = decodeAndScore(msg, p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.35f);
    }
    SECTION("contest + QRN strong amp") {
        auto p = profileContest(60.0f);
        p.qrnRate = 0.001f; p.qrnAmp = 5.0f;
        auto s = decodeAndScore(msg, p);
        INFO("CER=" << s.cer);
        CHECK(s.cer < 0.3f);
    }
}

TEST_CASE("Benchmark: worst case at 15 WPM", "[cw][benchmark][worst]") {
    auto s = runBenchmark(MSG_CQ(), profileWorstCase(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    // All degradations combined. During QSB dips, signal at -9.5 dB SNR.
    CHECK(s.cer < 0.6f);
}

TEST_CASE("Benchmark: worst case long message", "[cw][benchmark][worst]") {
    auto s = runBenchmark(MSG_FULL(), profileWorstCase(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer << " refChars=" << s.refChars);
    // Longer message gives timing more data to lock — should be slightly better
    CHECK(s.cer < 0.5f);
}

TEST_CASE("Benchmark: worst case preserves some structure", "[cw][benchmark][worst]") {
    std::string norm = normalize(decode(MSG_CQ(), profileWorstCase(80.0f)));
    INFO("decoded='" << norm << "'");
    // Even under worst conditions, at least one "W1" fragment should survive
    // (W is .-- and 1 is .----, both dah-heavy = less confusable)
    CHECK(norm.find("W1") != std::string::npos);
}

// ============================================================
// Cross-WPM Benchmark Matrix — same message, all speeds
// ============================================================

TEST_CASE("Benchmark matrix: clean signal across WPM range", "[cw][benchmark][matrix]") {
    struct WPMCase { float ditMs; float wpm; float maxCER; };
    // Practical WPM range: 8-35 WPM covers nearly all real CW operating
    WPMCase cases[] = {
        {150.0f,  8.0f, 0.01f},
        {100.0f, 12.0f, 0.01f},
        {80.0f,  15.0f, 0.01f},
        {60.0f,  20.0f, 0.01f},
        {48.0f,  25.0f, 0.01f},
        {40.0f,  30.0f, 0.01f},
        {34.0f,  35.0f, 0.01f},
    };

    for (auto& tc : cases) {
        auto s = runBenchmark("SOS SOS SOS", profileClean(tc.ditMs));
        INFO("WPM=" << tc.wpm << " dit=" << tc.ditMs << "ms CER=" << s.cer
             << " WER=" << s.wer << " errors=" << s.charErrors << "/" << s.refChars);
        CHECK(s.cer < tc.maxCER);
    }
}

TEST_CASE("Benchmark matrix: hand-keyed across WPM range", "[cw][benchmark][matrix]") {
    struct WPMCase { float ditMs; float wpm; float maxCER; };
    // Baseline: severely broken (CER > 1.0 = hallucinated output)
    WPMCase cases[] = {
        {120.0f, 10.0f, 0.01f},
        {80.0f,  15.0f, 0.01f},
        {60.0f,  20.0f, 0.12f},
        {48.0f,  25.0f, 0.01f},
    };

    for (auto& tc : cases) {
        auto s = runBenchmark("CQ DE W1AW", profileHandKeyed(tc.ditMs));
        INFO("WPM=" << tc.wpm << " CER=" << s.cer << " WER=" << s.wer);
        CHECK(s.cer < tc.maxCER);
    }
}

// ============================================================
// Longer message benchmarks — more data for timing to lock
// ============================================================

TEST_CASE("Benchmark: long message clean 15 WPM", "[cw][benchmark][long]") {
    auto s = runBenchmark("CQ CQ CQ DE W1AW W1AW QTH NEWINGTON CT K", profileClean(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer << " refChars=" << s.refChars);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: long message hand-keyed 15 WPM", "[cw][benchmark][long]") {
    auto s = runBenchmark("CQ CQ CQ DE W1AW W1AW QTH NEWINGTON CT K", profileHandKeyed(80.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}

TEST_CASE("Benchmark: long message contest 20 WPM", "[cw][benchmark][long]") {
    auto s = runBenchmark("CQ TEST K1ABC CQ TEST K1ABC", profileContest(60.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.01f);
}
