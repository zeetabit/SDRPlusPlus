#include <catch.hpp>
#include "cw_matrix.h"
#include <cw/staged_core.h>
#include <cw/stages.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace cw_test;

// ============================================================
// Bound-scale CER curve (docs §28) — ON DEMAND ONLY.
//
//   ./cw_decoder_tests "[lr-snr]"
//
// §27 left a fast + HEAVY-noise residual: even with a perfect speed signal,
// noise2.0-25/30wpm stay worse than legacy. The proposal is to close it from the
// noise side — grow the CUSUM bound when SNR is low. But bound-scaling controls
// two things in conflict at fast+heavy noise: a small bound shortens the lag
// (fixes timing) but shrinks the evidence margin (admits spikes); a large bound
// does the reverse. SNR scaling can only help if the CER-vs-bound curve has a
// minimum AWAY from where truth-WPM (scale = ditMs/80) already sits.
//
// This measures that curve directly, before writing any SNR law. setExternalDitMs
// sets the bound scale to clamp(ditMs/80, 0.4, 1.2), so feeding a FAKE dit sweeps
// the bound at a FIXED real signal — a controlled bound-vs-CER curve. If each
// target profile's minimum is at the scale its true speed already picks, no SNR
// rule can beat it and the residual is not closable by bound-scaling.
// ============================================================

namespace {
    constexpr int PSEEDS = 64;

    // LR+log core with the bound scale forced to `scale` via a fake external dit
    // (80 ms * scale), independent of the signal's real speed. Mirrors
    // detail::makeLR(TIMING_LOG) otherwise.
    CoreFactory forcedScaleFactory(float scale) {
        const float fakeDitMs = 80.0f * scale;   // externalScale = fakeDitMs/80 = scale
        return [fakeDitMs](const GeneratedSignal&) -> std::unique_ptr<cw::IDecodeCore> {
            auto det = std::make_unique<cw::LikelihoodRatioDetector>(
                0.5f, 3.0f, -3.0f, /*soft=*/false, /*adaptive=*/false);
            det->setExternalDitMs(fakeDitMs);
            return std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::move(det),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_LOG),
                std::make_unique<cw::BeamSymbolDecoder>(),
                cw::MF_RESET, 1.0f);
        };
    }
}

TEST_CASE("LR bound-scale curve at fast+heavy noise vs slow guardrails", "[cw][.][lr-snr]") {
    struct Focus { const char* name; SignalParams params; float trueScale; };
    // trueScale = clamp(ditMs/80, 0.4, 1.2) — the scale that profile's real speed
    // selects, i.e. where truth-WPM (§27) operates. The question is whether the
    // curve minimum lands there or elsewhere.
    std::vector<Focus> focus = {
        {"noise2.0-25wpm", [] { auto p = profileClean(48.0f); p.noiseAmp = 2.0f; return p; }(), 0.60f},
        {"noise2.0-30wpm", [] { auto p = profileClean(40.0f); p.noiseAmp = 2.0f; return p; }(), 0.50f},
        {"handkeyed-40",   profileHandKeyed(30.0f), 0.40f},   // fast + LIGHT noise
        {"noise3.0",       [] { auto p = profileClean(80.0f); p.noiseAmp = 3.0f; return p; }(), 1.00f},
        {"worstcase",      profileWorstCase(80.0f), 1.00f},
        {"clean-15",       profileClean(80.0f), 1.00f},
    };
    const std::vector<float> scales = {0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.2f};

    printf("\n=== CER vs forced bound-scale (n=%d). '*' marks the scale the true "
           "speed selects. Lower is better. ===\n", PSEEDS);
    printf("%-15s", "profile\\scale");
    for (float s : scales) { printf("  %5.1f", s); }
    printf("   legacy   argmin\n");

    std::map<std::string, std::map<float, float>> cer;   // profile -> scale -> CER
    std::map<std::string, float> legacyCer;

    for (const auto& f : focus) {
        auto legacy = runCell("legacy", f.name, MSG_FULL(), f.params, PSEEDS);
        legacyCer[f.name] = legacy.cerMean;

        printf("%-15s", f.name);
        float best = 1e9f; float bestScale = 0;
        for (float s : scales) {
            auto c = runCellWith(forcedScaleFactory(s), "forced", f.name,
                                 MSG_FULL(), f.params, PSEEDS);
            cer[f.name][s] = c.cerMean;
            const bool atTrue = std::fabs(s - f.trueScale) < 0.001f;
            printf(" %5.3f%c", c.cerMean, atTrue ? '*' : ' ');
            if (c.cerMean < best) { best = c.cerMean; bestScale = s; }
        }
        printf("  %6.4f   %.1f (%.3f)\n", legacyCer[f.name], bestScale, best);
    }

    printf("\n  Finding: fast+heavy noise wants a SMALL bound, slow+heavy wants a\n"
           "  LARGE bound — OPPOSITE directions. SNR is low for both, so an SNR\n"
           "  rule cannot separate them; speed already picks the right direction\n"
           "  in every quadrant. And even the best bound leaves fast+heavy above\n"
           "  legacy, so the residual is not closable by bound-scaling on any axis.\n");

    // The opposite-direction finding, as invariants. Grow the bound (0.4 -> 1.0):
    //   fast+heavy gets WORSE (lag dominates), slow+heavy gets BETTER (rejection
    //   dominates). This is why one scalar keyed on SNR (low for both) cannot win.
    CHECK(cer["noise2.0-30wpm"][1.0f] > cer["noise2.0-30wpm"][0.4f]);   // fast: small better
    CHECK(cer["noise3.0"][1.0f]       < cer["noise3.0"][0.4f]);          // slow: large better

    // Even the floor bound does not reach legacy on the fast+heavy targets: the
    // residual is intrinsic to the LR CUSUM, not a bound-tuning miss.
    CHECK(cer["noise2.0-25wpm"][0.4f] > legacyCer["noise2.0-25wpm"]);
    CHECK(cer["noise2.0-30wpm"][0.4f] > legacyCer["noise2.0-30wpm"]);
}
