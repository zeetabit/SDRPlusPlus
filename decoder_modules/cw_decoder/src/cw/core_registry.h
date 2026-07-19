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

    inline constexpr const char* DEFAULT_CORE = "legacy";
}
