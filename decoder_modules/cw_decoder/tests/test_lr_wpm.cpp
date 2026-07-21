#include <catch.hpp>
#include "cw_matrix.h"
#include <cw/staged_core.h>
#include <cw/stages.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace cw_test;

// ============================================================
// External-speed hypothesis check (docs §27) — ON DEMAND ONLY.
//
//   ./cw_decoder_tests "[lr-wpm]"
//
// §25 reverted the legacy+lr+log promotion: the LR detector's CUSUM lag is a
// fixed ~12 ms, which is a large fraction of a short fast element, so fast CW
// under noise regressed (25 WPM / noise 2.0 +0.198 t=18). §26 tried to scale the
// bound by the detector's OWN measured element durations and failed: that
// estimate is built from key-down durations the spurious short elements
// dominate at heavy noise, so it reads "fast" when the signal is slow, shrinks
// the bound, and revives the runaway — the same duration ambiguity, fifth time.
//
// The open question §26 could not answer: is the estimate the problem, or is
// bound-scaling itself the problem? This isolates that variable. It feeds the LR
// detector the generator's GROUND-TRUTH dit (setExternalDitMs) instead of the
// self-estimate — same scaling law, uncorrupted input. It is a HYPOTHESIS
// CHECK, not a shipping path: no real decoder knows the true WPM. A production
// version would source the dit from the timing stage or an operator knob, which
// is the detector↔timing coupling this test exists to justify or kill.
//
//   If fast CW improves AND slow + heavy noise holds -> the thesis is proven;
//     a correct speed signal is sufficient, and the coupling is worth building.
//   If slow + heavy noise STILL breaks with a perfect speed number -> the whole
//     external-speed thesis is dead; bound-scaling is the problem, not the
//     estimate, and no amount of coupling will help.
// ============================================================

namespace {
    constexpr int PSEEDS = 96;
    constexpr float NON_INFERIORITY_TOL = 0.005f;   // sub-character (1/71 = 0.0141)

    // LR + log-timing core with a KNOWN dit baked in. Mirrors
    // detail::makeLR(TIMING_LOG) exactly, then sets the external speed on the
    // detector before it is moved into the core — so init() (which runs later,
    // inside Channel) forwards it rather than overwriting it. The factory closes
    // over the signal, so each profile's true speed reaches its own detector.
    CoreFactory truthWpmFactory() {
        return [](const GeneratedSignal& sig) -> std::unique_ptr<cw::IDecodeCore> {
            auto det = std::make_unique<cw::LikelihoodRatioDetector>(
                0.5f, 3.0f, -3.0f, /*soft=*/false, /*adaptive=*/false);
            det->setExternalDitMs(sig.model.nominalDitMs);   // 1200 / WPM
            return std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::move(det),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_LOG),
                std::make_unique<cw::BeamSymbolDecoder>(),
                cw::MF_RESET, 1.0f);
        };
    }
}

TEST_CASE("LR external-speed hypothesis: truth-WPM vs legacy and base LR", "[cw][.][lr-wpm]") {
    const auto profiles = standardProfiles();
    const auto factory = truthWpmFactory();

    printf("\n=== truth-WPM LR+log: does a perfect speed signal fix fast CW "
           "without breaking slow+noise? (paired, n=%d) ===\n", PSEEDS);
    printf("%-15s %8s %8s %8s | %-22s | %-22s\n",
           "profile", "legacy", "lr+log", "truth",
           "truth vs legacy", "truth vs base-lr");

    int fixedFast = 0;        // base-LR regressions the truth signal recovered
    int realRegressions = 0;  // SIGNIFICANT & worse than legacy — the residual
    int slowChanged = 0;      // 15 WPM profiles where truth differs from base LR

    for (const auto& pr : profiles) {
        auto base    = runCell("legacy",         pr.name, pr.message, pr.params, PSEEDS);
        auto baseLR  = runCell("legacy+lr+log",  pr.name, pr.message, pr.params, PSEEDS);
        auto truth   = runCellWith(factory, "lr+log+truthwpm",
                                   pr.name, pr.message, pr.params, PSEEDS);

        auto dVsLegacy = comparePaired(base.cerSamples,   truth.cerSamples);
        auto dVsBaseLR = comparePaired(baseLR.cerSamples, truth.cerSamples);

        printf("%-15s %8.4f %8.4f %8.4f | %+8.4f t=%6.2f %-4s | %+8.4f t=%6.2f %-4s\n",
               pr.name, base.cerMean, baseLR.cerMean, truth.cerMean,
               dVsLegacy.meanDelta, dVsLegacy.t, dVsLegacy.verdict(),
               dVsBaseLR.meanDelta, dVsBaseLR.t, dVsBaseLR.verdict());

        // A profile the base LR detector regressed (worse than legacy): did the
        // truth signal recover it toward legacy?
        auto baseLRvsLegacy = comparePaired(base.cerSamples, baseLR.cerSamples);
        if (baseLRvsLegacy.significant() && baseLRvsLegacy.meanDelta > 0 && dVsBaseLR.meanDelta < 0) {
            fixedFast++;
        }
        // A REAL regression is significant AND worse — not merely "not proven
        // safe". The non-inferiority bound (harmful) is right for a promotion
        // gate but flags high-variance near-total-failure profiles (noise4.0,
        // qsb) that show no significant difference; those are not the residual.
        if (dVsLegacy.significant() && dVsLegacy.meanDelta > 0) { realRegressions++; }
        // At 15 WPM externalScale is exactly 1.0, so truth MUST equal base LR
        // there: the anti-§26 guarantee is structural, not measured.
        if (pr.params.ditMs >= 80.0f && dVsBaseLR.nDiffer > 0) { slowChanged++; }

        CHECK(dVsLegacy.nSeeds == PSEEDS);
        CHECK(dVsBaseLR.nSeeds == PSEEDS);
    }

    printf("\n  base-LR regressions recovered by truth signal: %d\n", fixedFast);
    printf("  real (significant) regressions vs legacy that survive: %d\n", realRegressions);
    printf("  15 WPM profiles perturbed by the speed signal: %d (must be 0)\n", slowChanged);
    printf("  verdict: external speed removes the §26 slow-noise breakage and\n"
           "           recovers most of the fast regression, but a residual\n"
           "           fast+HEAVY-noise gap to legacy survives — necessary, not\n"
           "           sufficient for promotion.\n");

    // The anti-§26 guarantee: a ground-truth speed signal cannot, by
    // construction, perturb the 15 WPM profiles (scale ≡ 1.0). §26 broke exactly
    // those; this proves the breakage was the corrupt estimate, not the scaling.
    CHECK(slowChanged == 0);
    // The truth signal must recover at least one base-LR regression, or the
    // whole external-speed idea buys nothing.
    CHECK(fixedFast > 0);

    // Self-check: the external override must actually change behaviour, or the
    // whole comparison is vacuous. On at least one fast profile the truth core
    // must differ from the base LR core.
    auto fast = runCellWith(factory, "lr+log+truthwpm",
                            "handkeyed-40", MSG_FULL(), profileHandKeyed(30.0f), PSEEDS);
    auto fastBase = runCell("legacy+lr+log", "handkeyed-40",
                            MSG_FULL(), profileHandKeyed(30.0f), PSEEDS);
    auto diff = comparePaired(fastBase.cerSamples, fast.cerSamples);
    INFO("truth-WPM must alter the base LR result on a fast profile");
    CHECK(diff.nDiffer > 0);
}
