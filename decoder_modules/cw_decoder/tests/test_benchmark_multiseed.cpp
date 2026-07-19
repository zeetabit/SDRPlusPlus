#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"
#include "cw_bench_stats.h"
#include <cstdio>

using namespace cw_test;

// ============================================================
// Multi-seed regression gates.
//
// The single-seed benchmarks in test_benchmark.cpp remain the correctness
// tests. These add a statistical gate: each profile is run over N independent
// noise realizations and asserted on the MEAN.
//
// Rules for these thresholds:
//   * Set from the measured mean of the CURRENT implementation.
//   * A change that raises a mean is a regression, even if every single-seed
//     test still passes.
//   * Never widen a bound to admit a change. Lower them when a change earns it.
// ============================================================

namespace {
    constexpr int SEEDS = 24;

    struct Profile {
        const char* name;
        const char* message;
        SignalParams params;
    };

    std::vector<Profile> allProfiles() {
        std::vector<Profile> v;
        v.push_back({"clean-15wpm",        MSG_FULL(), profileClean(80.0f)});
        v.push_back({"clean-20wpm",        MSG_FULL(), profileClean(60.0f)});
        v.push_back({"clean-25wpm",        MSG_FULL(), profileClean(48.0f)});
        v.push_back({"mild-noise",         MSG_FULL(), profileMildNoise(80.0f)});
        v.push_back({"moderate-noise",     MSG_FULL(), profileModerateNoise(80.0f)});
        v.push_back({"handkeyed-15wpm",    MSG_FULL(), profileHandKeyed(80.0f)});
        v.push_back({"handkeyed-20wpm",    MSG_FULL(), profileHandKeyed(60.0f)});
        v.push_back({"handkeyed-25wpm",    MSG_FULL(), profileHandKeyed(48.0f)});
        v.push_back({"qsb",                MSG_FULL(), profileQSB(80.0f)});
        v.push_back({"qrm",                MSG_FULL(), profileQRM(80.0f)});
        v.push_back({"qrn",                MSG_FULL(), profileQRN(80.0f)});
        v.push_back({"contest-20wpm",      MSG_CONTEST(), profileContest(60.0f)});
        v.push_back({"farnsworth-1.5",     MSG_FULL(), profileFarnsworth(80.0f, 1.5f)});
        v.push_back({"farnsworth-2.0",     MSG_FULL(), profileFarnsworth(80.0f, 2.0f)});
        v.push_back({"worstcase",          MSG_FULL(), profileWorstCase(80.0f)});

        // Explicit SNR ladder on the moderate-noise profile. This is the axis
        // that filter bandwidth acts on, so it is the axis a DSP change must
        // be judged against.
        for (float amp : {1.0f, 2.0f, 3.0f, 4.0f}) {
            Profile p;
            p.name = amp == 1.0f ? "snr-noise1.0" :
                     amp == 2.0f ? "snr-noise2.0" :
                     amp == 3.0f ? "snr-noise3.0" : "snr-noise4.0";
            p.message = MSG_FULL();
            p.params = profileClean(80.0f);
            p.params.noiseAmp = amp;
            v.push_back(p);
        }
        return v;
    }
}

// Prints the full distribution table. Always passes — this is the measuring
// instrument, run it with `./cw_decoder_tests "[characterize]" -s` to see it.
TEST_CASE("Characterize: multi-seed CER distribution per profile", "[cw][characterize]") {
    printf("\n%-20s %8s %9s %8s %8s %8s %8s\n",
           "profile", "mean", "stderr", "median", "p95", "worst", "wer");
    printf("%s\n", std::string(74, '-').c_str());
    for (auto& prof : allProfiles()) {
        auto s = decodeAndScoreMulti(prof.message, prof.params, SEEDS);
        printf("%-20s %8.4f %9.4f %8.4f %8.4f %8.4f %8.4f\n",
               prof.name, s.mean, s.stderrMean, s.median, s.p95, s.worst, s.meanWER);
    }
    printf("\n");
    SUCCEED();
}

// Ablation: separates the two factors by which the multi-seed baseline differs
// from the figures in decoding-improvement-plan.md (message length, seed count).
// Reproduces the plan's exact conditions first, then varies one factor at a time.
TEST_CASE("Characterize: single-seed vs multi-seed ablation", "[cw][characterize]") {
    struct Case { const char* label; const char* msg; float ditMs; };
    Case cases[] = {
        {"handkeyed-15wpm", MSG_CQ(),   80.0f},
        {"handkeyed-20wpm", MSG_CQ(),   60.0f},
        {"handkeyed-25wpm", MSG_CQ(),   48.0f},
    };
    printf("\n%-18s %-9s %10s %10s %10s\n",
           "case", "msg", "seed42", "CQ x24", "FULL x24");
    printf("%s\n", std::string(60, '-').c_str());
    for (auto& c : cases) {
        auto p = profileHandKeyed(c.ditMs);

        p.seed = 42;                                   // the plan's exact condition
        float single = decodeAndScore(c.msg, p).cer;

        auto multiCQ   = decodeAndScoreMulti(c.msg,      p, 24);  // vary seed only
        auto multiFULL = decodeAndScoreMulti(MSG_FULL(), p, 24);  // vary seed + length

        printf("%-18s %-9s %10.4f %10.4f %10.4f\n",
               c.label, "CQ(23ch)", single, multiCQ.mean, multiFULL.mean);
    }
    printf("\n");
    SUCCEED();
}

// ============================================================
// Regression gates.
//
// Bounds below are the 2026-07 measured mean + 2*stderr over SEEDS=24
// deterministic seeds. They are ratchets: lower them when a change earns it,
// never raise them to admit a change. A rise in mean CER is a regression even
// when every single-seed test in test_benchmark.cpp still passes.
// ============================================================

namespace {
    void gate(const char* name, const char* message, const SignalParams& p, float maxMean) {
        auto s = decodeAndScoreMulti(message, p, SEEDS);
        INFO(name << "  " << s.summary());
        CHECK(s.mean <= maxMean);
    }
}

TEST_CASE("Gate: clean signals decode exactly", "[cw][multiseed][gate]") {
    // Strongest possible assertion: zero errors on every seed.
    gate("clean-15wpm", MSG_FULL(), profileClean(80.0f), 0.0f);
    gate("clean-20wpm", MSG_FULL(), profileClean(60.0f), 0.0f);
    gate("clean-25wpm", MSG_FULL(), profileClean(48.0f), 0.0f);
    gate("farnsworth-1.5", MSG_FULL(), profileFarnsworth(80.0f, 1.5f), 0.0f);
}

TEST_CASE("Gate: additive noise", "[cw][multiseed][gate]") {
    gate("mild-noise",     MSG_FULL(), profileMildNoise(80.0f),     0.0f);
    gate("moderate-noise", MSG_FULL(), profileModerateNoise(80.0f), 0.0f);
}

TEST_CASE("Gate: hand-keyed jitter", "[cw][multiseed][gate]") {
    // Distribution is bimodal (median << mean): most seeds decode, a minority
    // fail catastrophically. Suspected cause is the Kalman seed ambiguity in
    // timing.h — see docs/decoder-investigation-2026-07.md 2.1(c).
    gate("handkeyed-15wpm", MSG_FULL(), profileHandKeyed(80.0f), 0.238f);
    gate("handkeyed-20wpm", MSG_FULL(), profileHandKeyed(60.0f), 0.311f);
    gate("handkeyed-25wpm", MSG_FULL(), profileHandKeyed(48.0f), 0.199f);
}

TEST_CASE("Gate: propagation and interference", "[cw][multiseed][gate]") {
    gate("qsb",           MSG_FULL(),     profileQSB(80.0f),           0.019f);
    gate("qrm",           MSG_FULL(),     profileQRM(80.0f),           0.021f);
    gate("qrn",           MSG_FULL(),     profileQRN(80.0f),           0.009f);
    gate("contest-20wpm", MSG_CONTEST(),  profileContest(60.0f),       0.008f);
    gate("farnsworth-2.0",MSG_FULL(),     profileFarnsworth(80.0f, 2.0f), 0.015f);
    gate("worstcase",     MSG_FULL(),     profileWorstCase(80.0f),     0.667f);
}

// SNR ladder. This is the axis pre-detection filter bandwidth acts on; the
// pre-existing suite tops out at noiseAmp=1.5 where CER is 0, so without these
// a bandwidth change is not observable by the test suite at all.
TEST_CASE("Gate: SNR ladder", "[cw][multiseed][gate][snr]") {
    struct { const char* name; float amp; float maxMean; } ladder[] = {
        {"snr-noise1.0", 1.0f, 0.000f},
        {"snr-noise2.0", 2.0f, 0.116f},
        {"snr-noise3.0", 3.0f, 0.886f},
        {"snr-noise4.0", 4.0f, 0.923f},
    };
    for (auto& l : ladder) {
        auto p = profileClean(80.0f);
        p.noiseAmp = l.amp;
        gate(l.name, MSG_FULL(), p, l.maxMean);
    }
}
