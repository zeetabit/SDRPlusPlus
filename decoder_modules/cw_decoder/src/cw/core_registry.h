#pragma once
#include "core.h"
#include "staged_core.h"

// The single list of benchmarkable decoder configurations.
//
// The module config UI lists these by name; the benchmark matrix iterates them.
// Adding a core, or a new combination of stages, means adding one entry here
// and nothing else.
//
// Naming: "<core>" for a default configuration, "<core>+<variant>" for a stage
// substitution. Names are persisted in module config, so keep them stable.

namespace cw {

    namespace detail {
        inline std::unique_ptr<IDecodeCore> makeStaged(
                TimingStrategy timing,
                float bpfCutoff = 100.0f, float bpfTrans = 100.0f,
                MatchedFilterResize mfResize = MF_RESET,
                EdgeBias edgeBias = EDGE_RAW,
                PeakTracker peak = PEAK_INSTANT_ATTACK) {
            return std::make_unique<StagedCore>(
                std::make_unique<EnvelopeFrontEnd>(bpfCutoff, bpfTrans),
                std::make_unique<SchmittDetector>(edgeBias, peak),
                std::make_unique<AdaptiveTimingStage>(timing),
                std::make_unique<BeamSymbolDecoder>(),
                mfResize);
        }

        // Likelihood-ratio detector (docs §24). Its own factory: the CUSUM
        // parameters are specific to this detector. Timing is a parameter so the
        // headline test (does a better detector rescue log?) is one line.
        inline std::unique_ptr<IDecodeCore> makeLR(
                TimingStrategy timing, bool soft = false,
                float theta = 0.5f, float boundHi = 3.0f, float boundLo = -3.0f,
                float minElemScale = 1.0f, bool adaptive = false) {
            return std::make_unique<StagedCore>(
                std::make_unique<EnvelopeFrontEnd>(),
                std::make_unique<LikelihoodRatioDetector>(theta, boundHi, boundLo, soft, adaptive),
                std::make_unique<AdaptiveTimingStage>(timing),
                std::make_unique<BeamSymbolDecoder>(),
                MF_RESET, minElemScale);
        }

        // Dual-window peak reference has its own factory: the parameters are
        // specific to that estimator and would bloat makeStaged for every
        // other variant.
        inline std::unique_ptr<IDecodeCore> makeDualPeak(float shortMs, float thresh, int persist) {
            return std::make_unique<StagedCore>(
                std::make_unique<EnvelopeFrontEnd>(),
                std::make_unique<SchmittDetector>(EDGE_RAW, PEAK_DUAL_WINDOW,
                                                  300.0f, shortMs, thresh, persist),
                std::make_unique<AdaptiveTimingStage>(TIMING_KALMAN),
                std::make_unique<BeamSymbolDecoder>());
        }
    }

    inline const std::vector<CoreSpec>& coreRegistry() {
        static const std::vector<CoreSpec> reg = {
            // ── Production default ──
            {"legacy", "Schmitt detector + Kalman timing + beam search",
             []{ return detail::makeStaged(TIMING_KALMAN); }},

            // ── Timing-strategy variants (existing code, never benchmarked
            //    head-to-head across seeds until now) ──
            {"legacy+kmeans",  "Legacy pipeline, K-means timing",
             []{ return detail::makeStaged(TIMING_KMEANS); }},
            {"legacy+median",  "Legacy pipeline, median-split timing",
             []{ return detail::makeStaged(TIMING_MEDIAN); }},
            {"legacy+bimodal", "Legacy pipeline, bimodal-histogram timing",
             []{ return detail::makeStaged(TIMING_BIMODAL); }},

            // ── Stage-1 Kalman corrections (docs/decoder-investigation §9).
            //    Kept separate from `legacy`: better on 10 of 13 profiles but
            //    worse on 3, so not promoted. Both remain for comparison. ──
            {"legacy+kalman2", "Legacy pipeline, corrected Kalman timing (V2)",
             []{ return detail::makeStaged(TIMING_KALMAN_V2); }},

            // ── Log-duration timing (Stage 4). Multiplicative jitter model:
            //    dit/dah differ by a constant ln(3) offset and share one
            //    measurement variance, so the linear model's dah-gain error
            //    cannot be expressed. See docs/decoder-investigation §5.3b. ──
            {"legacy+log", "Legacy pipeline, log-duration Kalman timing",
             []{ return detail::makeStaged(TIMING_LOG); }},

            {"legacy+logrobust", "Log timing + Huberised update (outliers teach R, not x)",
             []{ return detail::makeStaged(TIMING_LOG_ROBUST); }},

            // ── Guarded log timing (docs §20.7). The +log runaway is a spike
            //    flood pulling dit down until minElementMs collapses; robust
            //    only downweights x and its full-R learning saturates the gate.
            //    Guarded freezes x beyond 3 sigma so a spike cannot move it. ──
            {"legacy+logguard", "Log timing + hard x-freeze for outliers (§20.7)",
             []{ return detail::makeStaged(TIMING_LOG_GUARDED); }},

            {"legacy+edge+logguard", "Edge correction + guarded log timing",
             []{ return detail::makeStaged(TIMING_LOG_GUARDED, 100.0f, 100.0f, MF_RESET, EDGE_COMPENSATE); }},

            // ── Likelihood-ratio detector (docs §24). Sequential CUSUM on the
            //    envelope energy statistic: rejects spikes by evidence duration,
            //    not magnitude — the discriminator §20.8 showed timing lacks.
            //    legacy+lr+log is the headline test: does a spike-resistant
            //    detector rescue log's runaway? ──
            {"legacy+lr", "Likelihood-ratio detector + Kalman timing",
             []{ return detail::makeLR(TIMING_KALMAN); }},

            {"legacy+lr+log", "Likelihood-ratio detector + log-duration timing",
             []{ return detail::makeLR(TIMING_LOG); }},

            // ── Soft LR (docs §22): the detector's per-element evidence margin
            //    feeds the beam confidence, so a marginally-detected element
            //    widens the beam instead of committing. Everything else as the
            //    promoted default. ──
            {"legacy+lr+soft", "Soft LR detector (evidence-weighted) + Kalman timing",
             []{ return detail::makeLR(TIMING_KALMAN, true); }},

            {"legacy+lr+soft+log", "Soft LR detector + log-duration timing",
             []{ return detail::makeLR(TIMING_LOG, true); }},

            // ── Min-element filter disabled (docs §24). Confirms §17.3.4: the
            //    filter biases fast hand-keyed (handkeyed-40 better without it)
            //    but is not redundant under the LR detector — it still catches
            //    short evidence-passing spikes, so worstcase/noise3.0 regress.
            //    A bounded tradeoff, not removable. Measured, not promoted. ──
            {"legacy+lr+log+nofilt", "Default chain, min-element filter off (§24)",
             []{ return detail::makeLR(TIMING_LOG, false, 0.5f, 3.0f, -3.0f, 0.0f); }},

            // ── Speed-adaptive LR bound (docs §26). Scales the CUSUM bound by
            //    the median recent element duration to cut the fixed lag on fast
            //    CW (§25). Helps fast (handkeyed-40, fast+moderate-noise) but the
            //    self-estimate is corrupted at slow + heavy noise, reviving the
            //    runaway there. Measured, not promoted — the attempted §25 fix. ──
            {"legacy+lr+log+adapt", "Speed-adaptive LR bound (§26)",
             []{ return detail::makeLR(TIMING_LOG, false, 0.5f, 3.0f, -3.0f, 1.0f, true); }},

            // ── Matched-filter resize transient (docs §12). Zeroing the ring
            //    buffer on a window change injects a dropout mid-element; at
            //    25 WPM this produced 14 spurious transitions on a *noiseless*
            //    signal. MF_PRESERVE refills with the running mean instead. ──
            {"legacy+mf", "Legacy pipeline, matched filter preserved across resize",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_PRESERVE); }},

            {"legacy+mf+log", "Preserved matched filter + log-duration timing",
             []{ return detail::makeStaged(TIMING_LOG, 100.0f, 100.0f, MF_PRESERVE); }},

            // ── Schmitt edge-bias corrections (docs §12.1, §13). The trigger
            //    stretches every ON by ~9.8% of a dit, so timing sees a
            //    dah:dit ratio of 2.82 rather than 3.0. Two ways to remove it,
            //    trading hysteresis against event-time accuracy. ──
            {"legacy+sym", "Symmetric Schmitt thresholds (0.45/0.45), no hysteresis",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET, EDGE_SYMMETRIC); }},

            {"legacy+edge", "Hysteresis kept, release edge corrected for the modelled stretch",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET, EDGE_COMPENSATE); }},

            {"legacy+edge+log", "Edge correction + log-duration timing",
             []{ return detail::makeStaged(TIMING_LOG, 100.0f, 100.0f, MF_RESET, EDGE_COMPENSATE); }},

            {"legacy+edge+mf", "Edge correction + preserved matched filter",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_PRESERVE, EDGE_COMPENSATE); }},

            // ── Threshold reference level (docs §13). Instant-attack peak
            //    tracking lets the rising threshold chase the signal while the
            //    falling threshold holds — an on/off asymmetry independent of
            //    the ratios, which is why +sym barely moved the ON stretch. ──
            {"legacy+peak", "Threshold referenced to a windowed 90th percentile, not instant peak",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET, EDGE_RAW, PEAK_PERCENTILE); }},

            {"legacy+peak+edge", "Percentile peak + release-edge correction",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET, EDGE_COMPENSATE, PEAK_PERCENTILE); }},

            // ── Dual-window peak reference (docs §13). Short and long
            //    percentile windows run concurrently; sustained disagreement
            //    between them is the fade detector. Two trade points kept:
            //    the quiet one is best overall, the eager one is best on
            //    hand-keyed. Neither dominates — see §13.5. ──
            {"legacy+peakdual", "Dual-window peak reference, 500 ms short window",
             []{ return detail::makeDualPeak(500.0f, 0.05f, 1); }},

            {"legacy+peakdual16", "Dual-window peak reference, 250 ms short + 16-sample persistence",
             []{ return detail::makeDualPeak(250.0f, 0.05f, 16); }},

            // ── Transition-gated peak (docs §13.7). Instant attack's fade
            //    tracking is the best figure in the Phase 16 sweep; its only
            //    defect is that the rising edge sets the reference it is then
            //    compared against. Gating attack on confirmed key-down removes
            //    the chase without replacing the estimator. ──
            {"legacy+peakgate", "Instant-attack peak, attack gated on confirmed key-down",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET, EDGE_RAW, PEAK_GATED); }},

            // ── Front-end bandwidth variants. ENBW figures measured in
            //    docs/decoder-investigation-2026-07.md §4.1. ──
            {"legacy+bpf40",  "Legacy pipeline, 40/50 BPF (64 Hz ENBW)",
             []{ return detail::makeStaged(TIMING_KALMAN, 40.0f, 50.0f); }},
            {"legacy+bpf30",  "Legacy pipeline, 30/40 BPF (48 Hz ENBW)",
             []{ return detail::makeStaged(TIMING_KALMAN, 30.0f, 40.0f); }},
            {"legacy+bpf20",  "Legacy pipeline, 20/30 BPF (31 Hz ENBW)",
             []{ return detail::makeStaged(TIMING_KALMAN, 20.0f, 30.0f); }},
        };
        return reg;
    }

    // legacy+lr+log was promoted 2026-07-21 (§21) then REVERTED 2026-07-21 (§25):
    // closing the gate's noise-axis coverage hole (all its noise profiles were
    // 15 WPM) exposed large regressions at fast CW under noise — 25 WPM / noise
    // 2.0 +0.198 (t=18), 30 WPM +0.332 (t=33), from the LR detector's fixed
    // evidence lag being too large a fraction of a short fast element. The
    // detector's big wins (hand-keyed, worstcase, the runaway fix) stand, so it
    // is kept as a variant while a speed-adaptive fix is developed (§26).
    inline constexpr const char* DEFAULT_CORE = "legacy";
}
