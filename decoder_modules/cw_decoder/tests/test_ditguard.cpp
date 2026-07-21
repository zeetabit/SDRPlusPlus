#include <catch.hpp>
#include "cw_test_signals.h"
#include "cw_matrix.h"
#include "cw_bench_stats.h"

using namespace cw_test;

// §39 decode-level verdict for the asymmetric dah-guard. The dit-source probe
// (§38b) showed the guard collapses ditEst to -57% at noise-3.0 regardless of
// factor. This confirms whether that estimator collapse produces garbage decode
// vs legacy and V2, across the regimes that matter.
TEST_CASE("ditguard decode verdict (§39)", "[cw][.][ditguard]") {
    constexpr int SEEDS = 48;

    struct Prof { const char* name; SignalParams params; };
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
    SignalParams n4 = profileClean(80.0f); n4.noiseAmp = 4.0f;
    const Prof profs[] = {
        {"clean",     profileClean(80.0f)},
        {"noise-2.0", [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }()},
        {"noise-3.0", n3},
        {"noise-4.0", n4},
        {"handkeyed", profileHandKeyed(80.0f)},
        {"worstcase", profileWorstCase(80.0f)},
    };
    const char* cores[] = {"legacy", "legacy+ditguard", "legacy+kalman2"};

    printf("\n=== §39 dah-guard decode CER (mean, %d seeds) ===\n", SEEDS);
    printf("%-11s", "profile");
    for (const char* c : cores) { printf(" %18s", c); }
    printf("\n");

    for (const auto& p : profs) {
        printf("%-11s", p.name);
        for (const char* c : cores) {
            auto cell = runCell(c, p.name, MSG_FULL(), p.params, SEEDS);
            printf(" %18.4f", cell.cerMean);
        }
        printf("\n");
    }
}

// §40 — is +kalman2's noise-4.0 regression load-bearing or a variance artifact?
//
// kalman2 (V2 confidence gate) is the symmetric fix for the dit runaway (§38b)
// and dominates legacy on 5 of 6 profiles, blocked only by noise-4.0. §35 found
// bpfauto's block was a variance/tail miss, not a mean regression. This applies
// the SAME paired n=96 gate the promotion test uses to decide whether kalman2's
// noise-4.0 delta is a significant paired regression (real block) or ns
// (underpowered — then kalman2 is the campaign's strongest promotion candidate).
namespace {
    void adjudicateVsLegacy(const char* candidate, int PSEEDS = 96) {
        constexpr float TOL = 0.005f;   // non-inferiority tolerance, as [promotion]
        const auto profiles = standardProfiles();

        printf("\n=== legacy+%s vs legacy (paired, n=%d) ===\n", candidate, PSEEDS);
        printf("%-16s %9s %9s %10s %8s %6s %10s  %s\n",
               "profile", "legacy", "cand", "delta", "t", "ndiff", "worstcase", "verdict");

        int better = 0, worse = 0, ns = 0, harmful = 0;
        for (const auto& pr : profiles) {
            auto base = runCell("legacy", pr.name, pr.message, pr.params, PSEEDS);
            auto cand = runCell(std::string("legacy+") + candidate,
                                pr.name, pr.message, pr.params, PSEEDS);
            auto d = comparePaired(base.cerSamples, cand.cerSamples);

            const bool harm = d.harmful(TOL);
            const char* v = !d.significant() ? "ns" : (d.meanDelta < 0 ? "BETTER" : "WORSE");
            printf("%-16s %9.4f %9.4f %+10.4f %6.2f %6d %+10.4f  %s%s\n",
                   pr.name, base.cerMean, cand.cerMean, d.meanDelta, d.t,
                   d.nDiffer, d.worstCaseDelta(), v, harm ? " HARM" : "");

            if (harm) { harmful++; }
            if (!d.significant())     { ns++; }
            else if (d.meanDelta < 0) { better++; }
            else                      { worse++; }
        }
        printf("  %d better, %d worse, %d ns, %d harmful  ->  %s\n",
               better, worse, ns, harmful,
               (harmful == 0 && better > 0) ? "PROMOTABLE" : "blocked");
    }
}

// §40/§41 — is +kalman2's noise-4.0 regression load-bearing? And does speed-gating
// the confidence gate (§41, kalman2s) separate the slow/moderate wins from the
// fast-CW regression §40 exposed?
TEST_CASE("kalman2 recheck: paired n=96 vs legacy (§40/§41)", "[cw][.][kalman2-recheck]") {
    adjudicateVsLegacy("kalman2");
    adjudicateVsLegacy("kalman2s");
}

// §42: both residual HARM are better/neutral-mean, so more power should clear the
// non-inferiority bound (confirmed for noise4.0 at n=768). This re-adjudicates
// kalman2s at n=192 to check whether the whole gate goes 0-harmful with power.
TEST_CASE("kalman2s adjudication at higher power (§42)", "[cw][.][kalman2s-power]") {
    adjudicateVsLegacy("kalman2s", 192);
}

// §43 — the noise×speed grid, including the fast×heavy-noise cells the standard
// gate lacks. kalman2s switches on the self-estimated WPM, which the dit runaway
// inflates under heavy noise (§38b) — so a genuinely-fast signal could read slow
// and flip into V2 exactly where V2 regresses fast CW. This tests every cell.
TEST_CASE("kalman2s noise x speed grid (§43)", "[cw][.][kalman2s-grid]") {
    constexpr int SEEDS = 96;
    const float dits[]  = {80.0f, 48.0f, 40.0f};   // 15, 25, 30 WPM
    const int   wpms[]  = {15, 25, 30};
    const float amps[]  = {2.0f, 3.0f, 4.0f};
    const char* cores[] = {"legacy", "legacy+kalman2s"};

    (void)cores;
    printf("\n=== §43 noise x speed grid: CER mean + paired t vs legacy ===\n");
    printf("kalman2 = pure V2 (attributes intrinsic vs switch); kalman2s = speed-switched\n");
    printf("%5s %5s %9s %9s %7s %9s %7s\n",
           "wpm", "n", "legacy", "kal2", "t2", "kal2s", "t2s");

    for (size_t s = 0; s < 3; s++) {
        for (float amp : amps) {
            SignalParams p = profileClean(dits[s]);
            p.noiseAmp = amp;
            char name[32]; snprintf(name, sizeof(name), "%dwpm-n%.0f", wpms[s], amp);
            auto base = runCell("legacy",          name, MSG_FULL(), p, SEEDS);
            auto k2   = runCell("legacy+kalman2",  name, MSG_FULL(), p, SEEDS);
            auto k2s  = runCell("legacy+kalman2s", name, MSG_FULL(), p, SEEDS);
            auto d2  = comparePaired(base.cerSamples, k2.cerSamples);
            auto d2s = comparePaired(base.cerSamples, k2s.cerSamples);
            auto tag = [](const PairedDelta& d) {
                return !d.significant() ? "  " : (d.meanDelta < 0 ? "<<" : ">>");  // >> = WORSE
            };
            printf("%5d %5.1f %9.4f %9.4f %+6.2f%s %9.4f %+6.2f%s\n",
                   wpms[s], amp, base.cerMean, k2.cerMean, d2.t, tag(d2),
                   k2s.cerMean, d2s.t, tag(d2s));
        }
    }
    printf("\n>> = significant WORSE. 30wpm x n3/n4: runaway-flips-switch-to-V2 hole.\n");
    printf("If kal2 also >> at a cell, it is V2-intrinsic, not the switch.\n");
}

// §42 — does noise4.0's non-inferiority bound converge under 0.005 with more seeds?
// The mean is ~zero (better/neutral), so the HARM is a wide CI from -10 dB
// per-seed variance. worstCaseDelta = meanDelta + 2*stderrDelta; stderr ~ 1/sqrt(n).
// This measures the bound's actual trajectory to decide whether more power clears
// it or the variance floor makes it infeasible.
TEST_CASE("noise4.0 bound vs seed count (§42)", "[cw][.][noise4-power]") {
    SignalParams n4 = profileClean(80.0f); n4.noiseAmp = 4.0f;
    const int Ns[] = {96, 192, 384, 768};

    printf("\n=== noise4.0: legacy+kalman2s vs legacy, bound vs n ===\n");
    printf("%6s %10s %10s %8s %8s %10s  %s\n",
           "n", "legacy", "cand", "mean", "2*stderr", "bound", "harmful?");
    for (int n : Ns) {
        auto base = runCell("legacy",         "noise4.0", MSG_FULL(), n4, n);
        auto cand = runCell("legacy+kalman2s","noise4.0", MSG_FULL(), n4, n);
        auto d = comparePaired(base.cerSamples, cand.cerSamples);
        printf("%6d %10.4f %10.4f %+8.4f %8.4f %+10.4f  %s\n",
               n, base.cerMean, cand.cerMean, d.meanDelta,
               2.0f * d.stderrDelta, d.worstCaseDelta(),
               d.harmful(0.005f) ? "HARM" : "clean");
    }
    printf("\nNote: at -10 dB both cores fail (CER ~0.9 = garbage); the bound\n");
    printf("compares garbage-vs-garbage variance, not usable decode.\n");
}

// §41 decomposition: the speed gate at 27 WPM did not fix the fast-CW regression.
// Two causes: (1) the gate is still active at 30 WPM (self-WPM reads low under the
// runaway), or (2) the regression is from V2's dah-gain fix, not the confidence
// gate. Sweeping the threshold separates them: gateWpm=1 disables the gate almost
// entirely (V1 learning + V2 dah-gain), so if noise2.0-30wpm STILL regresses
// there, the dah-gain is the culprit and speed-gating the gate cannot help.
TEST_CASE("kalman2s speed-gate threshold sweep (§41)", "[cw][.][kalman2s-sweep]") {
    constexpr int SEEDS = 96;
    auto gatedFactory = [](float gateWpm) {
        return [gateWpm](const GeneratedSignal&) -> std::unique_ptr<cw::IDecodeCore> {
            auto timing = std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN_V2S);
            timing->setSpeedGateWpm(gateWpm);
            return std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::make_unique<cw::SchmittDetector>(),
                std::move(timing),
                std::make_unique<cw::BeamSymbolDecoder>());
        };
    };

    struct Prof { const char* name; SignalParams params; };
    const Prof profs[] = {
        {"noise2.0-30wpm", [] { auto p = profileClean(40.0f); p.noiseAmp = 2.0f; return p; }()},
        {"handkeyed-30",   profileHandKeyed(40.0f)},
        {"noise3.0",       [] { auto p = profileClean(80.0f); p.noiseAmp = 3.0f; return p; }()},
        {"worstcase",      profileWorstCase(80.0f)},
    };
    const float gates[] = {1.0f, 20.0f, 24.0f, 27.0f, 30.0f, 999.0f};  // 1=gate off, 999=pure V2

    printf("\n=== §41 speed-gate threshold sweep (CER mean, %d seeds) ===\n", SEEDS);
    printf("gateWpm=1 -> gate off (V1 learn + V2 dah-gain); 999 -> pure V2\n");
    printf("%-16s %8s", "profile", "legacy");
    for (float g : gates) { printf("  g=%-5.0f", g); }
    printf("\n");

    for (const auto& p : profs) {
        auto base = runCell("legacy", p.name, MSG_FULL(), p.params, SEEDS);
        printf("%-16s %8.4f", p.name, base.cerMean);
        for (float g : gates) {
            auto cell = runCellWith(gatedFactory(g), "kalman2s", p.name, MSG_FULL(), p.params, SEEDS);
            printf("  %7.4f", cell.cerMean);
        }
        printf("\n");
    }
    printf("\nRead: g=1 collapses to legacy (pure V1), g=999 is pure V2. A mid\n");
    printf("threshold keeps the slow-win rows near V2 and the 30wpm rows near legacy.\n");
    printf("(§41 history: with only the confidence gate switched, 30wpm was flat\n");
    printf("across gates => the fast regression is V2's dah-gain, so the whole\n");
    printf("V1/V2 behaviour must switch, not just the gate.)\n");
}
