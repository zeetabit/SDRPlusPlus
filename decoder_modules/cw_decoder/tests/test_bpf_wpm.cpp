#include <catch.hpp>
#include "cw_matrix.h"
#include <cw/staged_core.h>
#include <cw/stages.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace cw_test;

// ============================================================
// WPM-matched BPF ceiling (#31, docs §29) — ON DEMAND ONLY.
//
//   ./cw_decoder_tests "[bpf-wpm]"
//
// §4 measured that a narrow pre-detection BPF is a large, monotone SNR win at
// heavy noise (noise3.0/15WPM 0.76 → 0.02, 38×) but smears fast keying edges, so
// no single fixed bandwidth serves both slow+noisy and fast. The +bpf* variants
// pick one bandwidth for all speeds; #31 proposes matching bandwidth to the
// locked WPM (target ENBW ≈ 2/T_dit) to dissolve that tradeoff.
//
// This is the CEILING experiment, before any runtime-retuning machinery (whose
// filter-transition glitches are a real risk, cf. the §12 MF-resize defect). A
// legacy core (Schmitt + Kalman) is built per-signal with its BPF cutoff derived
// from the generator's GROUND-TRUTH dit — the best a perfect WPM lock could do —
// and measured against plain legacy across the speed×noise grid. Front-end
// mechanism only: the detector and timing are unchanged, so this is orthogonal
// to the LR bound-scaling that §28 exhausted.
//
//   If WPM-matched BPF beats legacy on slow+heavy noise WITHOUT regressing fast,
//     the tradeoff is real and dissolvable — build the runtime lock.
//   If fast regresses too, the smear is not speed-separable and #31 is a dead end.
// ============================================================

namespace {
    constexpr int PSEEDS = 96;
    constexpr float NON_INFERIORITY_TOL = 0.005f;

    // Target ENBW ≈ 2/T_dit (the matched-filter bandwidth for CW), inverted to a
    // BPF cutoff via the linear ENBW≈1.65·cut−2 fit measured in §4.1 for the
    // trans=cut+10 geometry. Clamped to the range §4 actually characterised
    // (20/30 … 40/50) so this is interpolation, not extrapolation.
    float wpmBpfCutoff(float ditMs) {
        const float targetEnbw = 2000.0f / ditMs;          // 2/T_dit in Hz
        const float cut = (targetEnbw + 2.0f) / 1.65f;
        return std::min(std::max(cut, 20.0f), 40.0f);
    }

    CoreFactory wpmBpfFactory() {
        return [](const GeneratedSignal& sig) -> std::unique_ptr<cw::IDecodeCore> {
            const float cut = wpmBpfCutoff(sig.model.nominalDitMs);
            const float trans = cut + 10.0f;
            return std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(cut, trans),   // matched BPF
                std::make_unique<cw::SchmittDetector>(),             // legacy detector
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>(),
                cw::MF_RESET, 1.0f);
        };
    }
}

TEST_CASE("WPM-matched BPF ceiling vs legacy", "[cw][.][bpf-wpm]") {
    const auto profiles = standardProfiles();
    const auto factory = wpmBpfFactory();

    printf("\n=== legacy + WPM-matched BPF (ground-truth cutoff) vs legacy "
           "(paired, n=%d) ===\n", PSEEDS);
    printf("%-15s %8s %8s %8s %8s  %7s %6s  %s\n",
           "profile", "cut(Hz)", "legacy", "matched", "delta", "t", "ndiff", "verdict");

    int better = 0, worse = 0, harmful = 0;

    for (const auto& pr : profiles) {
        auto base    = runCell("legacy", pr.name, pr.message, pr.params, PSEEDS);
        auto matched = runCellWith(factory, "legacy+bpfwpm",
                                   pr.name, pr.message, pr.params, PSEEDS);
        auto d = comparePaired(base.cerSamples, matched.cerSamples);
        const bool harm = d.harmful(NON_INFERIORITY_TOL);

        printf("%-15s %8.1f %8.4f %8.4f %+8.4f  %7.2f %6d  %s%s\n",
               pr.name, wpmBpfCutoff(pr.params.ditMs), base.cerMean, matched.cerMean,
               d.meanDelta, d.t, d.nDiffer, d.verdict(), harm ? " HARM" : "");

        if (d.significant() && d.meanDelta < 0) { better++; }
        if (d.significant() && d.meanDelta > 0) { worse++; }
        if (harm) { harmful++; }
        CHECK(d.nSeeds == PSEEDS);
    }

    printf("\n  %d significant better, %d significant worse, %d harmful\n",
           better, worse, harmful);
    printf("  verdict: %s\n",
           (worse == 0 && better > 0)
               ? "TRADEOFF DISSOLVED — matched BPF helps with no speed regression"
               : "smear is not speed-separable this way — inspect the worse rows");

    // The experiment is only meaningful if the matched filter actually changed
    // the result somewhere — otherwise the cutoffs collapsed to the baseline.
    CHECK(better + worse > 0);
}
