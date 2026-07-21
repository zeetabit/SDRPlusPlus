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
TEST_CASE("kalman2 recheck: paired n=96 vs legacy (§40)", "[cw][.][kalman2-recheck]") {
    constexpr int PSEEDS = 96;
    constexpr float TOL = 0.005f;   // non-inferiority tolerance, as [promotion]
    const auto profiles = standardProfiles();

    printf("\n=== legacy+kalman2 vs legacy (paired, n=%d) ===\n", PSEEDS);
    printf("%-16s %9s %9s %10s %8s %6s %10s %6s  %s\n",
           "profile", "legacy", "kalman2", "delta", "t", "ndiff",
           "worstcase", "", "verdict");

    int better = 0, worse = 0, ns = 0, harmful = 0;
    for (const auto& pr : profiles) {
        auto base = runCell("legacy",         pr.name, pr.message, pr.params, PSEEDS);
        auto cand = runCell("legacy+kalman2", pr.name, pr.message, pr.params, PSEEDS);
        auto d = comparePaired(base.cerSamples, cand.cerSamples);

        const bool harm = d.harmful(TOL);
        const char* v = !d.significant() ? "ns" : (d.meanDelta < 0 ? "BETTER" : "WORSE");
        printf("%-16s %9.4f %9.4f %+10.4f %6.2f %6d %+10.4f %6s  %s%s\n",
               pr.name, base.cerMean, cand.cerMean, d.meanDelta, d.t,
               d.nDiffer, d.worstCaseDelta(), "", v, harm ? " HARM" : "");

        if (harm) { harmful++; }
        if (!d.significant())     { ns++; }
        else if (d.meanDelta < 0) { better++; }
        else                      { worse++; }
    }
    printf("  %d better, %d worse, %d ns, %d harmful  ->  %s\n",
           better, worse, ns, harmful,
           (harmful == 0 && better > 0) ? "PROMOTABLE" : "blocked");
}
