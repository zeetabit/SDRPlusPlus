#include <catch.hpp>
#include "cw_test_signals.h"
#include "cw_matrix.h"

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
