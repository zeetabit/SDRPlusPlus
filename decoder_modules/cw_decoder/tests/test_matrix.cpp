#include <catch.hpp>
#include "cw_matrix.h"
#include <cstdio>

using namespace cw_test;

// ============================================================
// Core × profile benchmark matrix — ON DEMAND ONLY.
//
// Tagged [.] so Catch2 hides it from the default run: the always-on suite keeps
// the legacy-only regression gates in test_benchmark_multiseed.cpp, and this
// exploratory sweep is opt-in.
//
//   ./cw_decoder_tests "[matrix]" -s          full sweep
//   ./cw_decoder_tests "[matrix-timing]" -s   timing strategies only
// ============================================================

namespace {
    constexpr int SEEDS = 24;

    void header() {
        printf("\n%-16s %-14s %7s %7s %7s %7s %7s %7s %8s %8s %8s\n",
               "core", "profile", "CER", "p95", "WER", "ins", "del", "sub",
               "rt(x)", "ttfo_ms", "wpmRMS");
        printf("%s\n", std::string(118, '-').c_str());
    }

    void row(const MatrixCell& c) {
        printf("%-16s %-14s %7.4f %7.4f %7.4f %7.4f %7.4f %7.4f %8.0f %8.0f %8.2f\n",
               c.core.c_str(), c.profile.c_str(),
               c.cerMean, c.cerP95, c.werMean,
               c.insRate, c.delRate, c.subRate,
               c.realtimeX, c.ttfoMs, c.wpmRmsErr);
    }

    void sweep(const std::vector<std::string>& cores) {
        header();
        for (const auto& core : cores) {
            for (auto& pr : standardProfiles()) {
                row(runCell(core, pr.name, pr.message, pr.params, SEEDS));
            }
            printf("\n");
        }
    }
}

TEST_CASE("Matrix: timing strategies", "[cw][.][matrix][matrix-timing]") {
    sweep({"legacy", "legacy+kmeans", "legacy+median", "legacy+bimodal"});
    SUCCEED();
}

TEST_CASE("Matrix: front-end bandwidth", "[cw][.][matrix][matrix-bpf]") {
    sweep({"legacy", "legacy+bpf40", "legacy+bpf30", "legacy+bpf20"});
    SUCCEED();
}

TEST_CASE("Matrix: all registered cores", "[cw][.][matrix][matrix-all]") {
    std::vector<std::string> all;
    for (const auto& s : cw::coreRegistry()) { all.push_back(s.name); }
    sweep(all);
    SUCCEED();
}

// Speed and noise vary independently here, which the named profiles do not
// allow: comparing handkeyed* against noise* also varies jitter and weight
// bias. Reports paired t against legacy, so a cell is a verdict rather than a
// difference of two means.
TEST_CASE("Factorial: speed x noise, jitter held", "[cw][.][factorial]") {
    constexpr int FSEEDS = 48;
    const char* CAND = "legacy+edge+log";

    const struct { const char* name; float ditMs; } speeds[] = {
        {"15wpm", 80.0f}, {"25wpm", 48.0f}, {"35wpm", 34.3f}, {"40wpm", 30.0f},
    };
    const float noises[] = {0.0f, 1.0f, 2.0f, 3.0f};

    for (float jitter : {0.0f, 0.15f}) {
        const float bias = jitter > 0 ? 0.1f : 0.0f;
        printf("\n=== jitter %.2f bias %.2f — %s vs legacy, paired t (n=%d) ===\n",
               jitter, bias, CAND, FSEEDS);
        printf("%-8s %10s %10s %10s %10s\n", "speed", "noise0.0", "noise1.0",
               "noise2.0", "noise3.0");

        for (const auto& sp : speeds) {
            printf("%-8s", sp.name);
            for (float n : noises) {
                const SignalParams p = profileFactorial(sp.ditMs, n, jitter, bias);
                auto base = runCell("legacy", sp.name, MSG_FULL(), p, FSEEDS);
                auto cand = runCell(CAND,     sp.name, MSG_FULL(), p, FSEEDS);
                auto d = comparePaired(base.cerSamples, cand.cerSamples);
                printf(" %+9.2f%c", d.t, d.significant() ? '*' : ' ');

                INFO("jitter " << jitter << " " << sp.name << " noise " << n);
                CHECK(base.cerSamples.size() == (size_t)FSEEDS);
                CHECK(d.nSeeds == FSEEDS);
                // Noiseless and unjittered must decode exactly at every speed,
                // for both cores — a variant that breaks that is not a trade.
                if (n == 0.0f && jitter == 0.0f) {
                    CHECK(base.cerMean == Approx(0.0f).margin(1e-6));
                    CHECK(cand.cerMean == Approx(0.0f).margin(1e-6));
                }
            }
            printf("\n");
        }
    }

    // Instrument self-check: a core compared against itself must be
    // indistinguishable on every seed, or the pairing is not aligned by seed.
    const SignalParams p = profileFactorial(80.0f, 2.0f, 0.15f, 0.1f);
    auto a = runCell("legacy", "self", MSG_FULL(), p, FSEEDS);
    auto b = runCell("legacy", "self", MSG_FULL(), p, FSEEDS);
    auto self = comparePaired(a.cerSamples, b.cerSamples);
    CHECK(self.nDiffer == 0);
    CHECK_FALSE(self.significant());
    printf("\nself-check: legacy vs legacy nDiffer=%d verdict=%s\n\n",
           self.nDiffer, self.verdict());
}
