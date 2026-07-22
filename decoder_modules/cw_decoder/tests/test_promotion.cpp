#include <catch.hpp>
#include "cw_matrix.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace cw_test;

// ============================================================
// Promotion adjudication — ON DEMAND ONLY.
//
// The matrix (test_matrix.cpp) is a survey at n=24: broad, cheap, and not
// capable of deciding anything. This is the decision, and it spends seeds
// accordingly.
//
// Two defects in the method it replaces, both measured 2026-07-20:
//
//   n=24 gives wrong verdicts in both directions. A noise3.0 regression read
//   t=1.91 (not significant) at 24 seeds and t=5.43 at 96; two hand-keyed
//   improvements read t=-0.47 and -1.09 at 24 seeds and -3.29 and -3.63 at 96.
//
//   Counting better/worse off mean differences with a 1e-6 float epsilon makes
//   a one-character shift across the whole seed set a regression. The qrn
//   result that blocked legacy+edge was 3 differing seeds in 96.
//
// Both cores see the identical seed list, so seed difficulty cancels in the
// per-seed difference (comparePaired, cw_bench_stats.h). The promotion rule is
// unchanged — worse on any profile blocks — but "worse" now means a
// significant paired regression rather than any difference at all.
//
//   ./cw_decoder_tests "[promotion]"
// ============================================================

namespace {
    constexpr int PSEEDS = 96;

    // Sub-character: one character of MSG_FULL is 1/71 = 0.0141.
    constexpr float NON_INFERIORITY_TOL = 0.005f;

    struct Verdict { int better = 0; int worse = 0; int ns = 0; int harmful = 0; };

    Verdict adjudicate(const std::string& candidate,
                       const std::vector<MatrixCell>& baselines) {
        const auto profiles = standardProfiles();
        Verdict v;

        printf("\n=== %s vs legacy (paired, n=%d) ===\n", candidate.c_str(), PSEEDS);
        printf("%-14s %9s %9s %10s %10s %7s %10s %6s  %s\n",
               "profile", "legacy", "cand", "delta", "stderr", "t",
               "worstcase", "ndiff", "verdict");

        for (size_t i = 0; i < profiles.size(); i++) {
            const auto& pr = profiles[i];
            auto cand = runCell(candidate, pr.name, pr.message, pr.params, PSEEDS);
            auto d = comparePaired(baselines[i].cerSamples, cand.cerSamples);

            const bool harm = d.harmful(NON_INFERIORITY_TOL);
            printf("%-14s %9.4f %9.4f %+10.4f %10.4f %7.2f %+10.4f %6d  %s%s\n",
                   pr.name, baselines[i].cerMean, cand.cerMean,
                   d.meanDelta, d.stderrDelta, d.t, d.worstCaseDelta(),
                   d.nDiffer, d.verdict(), harm ? " HARM" : "");

            INFO("candidate " << candidate << " profile " << pr.name);
            CHECK(d.nSeeds == PSEEDS);
            if (harm) { v.harmful++; }
            if (!d.significant())        { v.ns++; }
            else if (d.meanDelta < 0)    { v.better++; }
            else                         { v.worse++; }
        }

        // Promotion needs zero harmful profiles, not merely zero significant
        // regressions: a profile whose upper 95% bound exceeds the tolerance
        // has not shown it is safe, whatever its t.
        printf("  %d better, %d worse, %d ns, %d harmful  ->  %s\n",
               v.better, v.worse, v.ns, v.harmful,
               (v.harmful == 0 && v.better > 0) ? "PROMOTABLE" : "blocked");
        return v;
    }
}

// Always-on: comparePaired decides promotions, so its edge cases are gated
// here rather than only exercised through a hidden sweep.
TEST_CASE("comparePaired: verdicts and edge cases", "[cw][stats]") {
    SECTION("identical inputs are ns with no differing seeds") {
        std::vector<float> a = {0.1f, 0.2f, 0.3f, 0.4f};
        auto d = comparePaired(a, a);
        CHECK(d.nDiffer == 0);
        CHECK(d.meanDelta == Approx(0.0f));
        CHECK_FALSE(d.significant());
        CHECK(std::string(d.verdict()) == "ns");
    }

    SECTION("constant offset has zero variance and is decisive") {
        // A deterministic profile (noiseAmp and jitterPct both 0) yields the
        // same signal every seed, so a real difference reproduces exactly.
        // Returning t=0 here would discard the regression entirely.
        // Dyadic values so the deltas are bit-identical and the variance is
        // exactly zero, as it is for a real deterministic profile. Decimal
        // literals would leave last-bit residue and exercise a different path.
        std::vector<float> base = {0.125f, 0.250f, 0.375f, 0.500f};
        std::vector<float> worse, better;
        for (float v : base) { worse.push_back(v + 0.0625f); better.push_back(v - 0.0625f); }

        auto w = comparePaired(base, worse);
        CHECK(w.nDiffer == 4);
        CHECK(w.stderrDelta == Approx(0.0f));
        CHECK(w.significant());
        CHECK(std::string(w.verdict()) == "WORSE");

        auto b = comparePaired(base, better);
        CHECK(b.significant());
        CHECK(std::string(b.verdict()) == "BETTER");
    }

    SECTION("a difference inside the noise is ns") {
        // Alternating signs: mean delta near zero, spread large.
        std::vector<float> base = {0.10f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f};
        std::vector<float> cand = {0.20f, 0.10f, 0.40f, 0.30f, 0.60f, 0.50f};
        auto d = comparePaired(base, cand);
        CHECK(d.nDiffer == 6);
        CHECK(d.meanDelta == Approx(0.0f).margin(1e-6));
        CHECK_FALSE(d.significant());
    }

    SECTION("mismatched or degenerate input yields no verdict") {
        std::vector<float> a = {0.1f, 0.2f, 0.3f};
        std::vector<float> b = {0.1f, 0.2f};
        CHECK(comparePaired(a, b).nSeeds == 0);
        CHECK_FALSE(comparePaired(a, b).significant());

        std::vector<float> one = {0.5f};
        CHECK(comparePaired(one, one).nSeeds == 0);
    }
}

// §52 step 4 — the fb core through the full paired adjudication (n=96 here for
// speed; the promotion rule is zero harmful profiles + some better). Runs the
// shipped default (select) alongside so fb can be compared per-profile against
// BOTH legacy and the current default.
TEST_CASE("Promotion: fb core adjudication (§52 step 4)", "[cw][.][fb-adjudicate]") {
    const auto profiles = standardProfiles();
    std::vector<MatrixCell> baselines;
    for (const auto& pr : profiles) {
        baselines.push_back(runCell("legacy", pr.name, pr.message, pr.params, PSEEDS));
    }
    adjudicate("legacy+select", baselines);   // current shipped default
    adjudicate("legacy+fb",     baselines);   // candidate
}

TEST_CASE("Promotion: paired adjudication vs legacy", "[cw][.][promotion]") {
    const auto profiles = standardProfiles();

    // Computed once and reused across candidates: the baseline is the same
    // seed list every time, and it must be, or the pairing is invalid.
    std::vector<MatrixCell> baselines;
    for (const auto& pr : profiles) {
        baselines.push_back(runCell("legacy", pr.name, pr.message, pr.params, PSEEDS));
    }

    adjudicate("legacy+edge",       baselines);
    adjudicate("legacy+mf",         baselines);
    adjudicate("legacy+edge+mf",    baselines);
    adjudicate("legacy+logguard",   baselines);
    adjudicate("legacy+edge+logguard", baselines);
    adjudicate("legacy+lr",         baselines);
    adjudicate("legacy+lr+log",     baselines);
    const Verdict edgeLog = adjudicate("legacy+edge+log", baselines);

    // ── Instrument self-checks ────────────────────────────────
    //
    // Three sweeps in this suite shipped with assertions that could not fail
    // (architecture.md working practices). These can.

    // legacy against its own baseline must be identical on every seed of every
    // profile. Any difference means the pairing is not seed-aligned.
    Verdict self = adjudicate("legacy", baselines);
    CHECK(self.worse == 0);
    CHECK(self.better == 0);
    CHECK(self.ns == (int)profiles.size());
    // A core against itself has delta and stderr both exactly zero, so it must
    // clear non-inferiority on every profile at any tolerance.
    CHECK(self.harmful == 0);

    // The harness must be able to report a regression, not only agreement.
    // legacy+edge+log is the canary because it is measured to fail here — CER
    // 1.79 vs 0.82 on noise3.0 at t=18 — not because it is assumed to.
    //
    // legacy+peakdual16 was tried first and is wrong for this: its catastrophe
    // is real-audio 5 WPM (CER 0.5087) and standardProfiles() has no profile
    // below 15 WPM, so on synthetic signals it is 5 better / 1 worse.
    CHECK(edgeLog.worse > 0);
    CHECK(edgeLog.better > 0);
}
