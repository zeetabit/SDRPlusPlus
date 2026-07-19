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
                MatchedFilterResize mfResize = MF_RESET) {
            return std::make_unique<StagedCore>(
                std::make_unique<EnvelopeFrontEnd>(bpfCutoff, bpfTrans),
                std::make_unique<SchmittDetector>(),
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
