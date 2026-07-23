#pragma once
#include "core.h"
#include "staged_core.h"
#include "regime_route.h"

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
                PeakTracker peak = PEAK_INSTANT_ATTACK,
                bool adaptiveBpf = false,
                bool bpfGarbageRevert = false,
                bool bpfReeval = false) {
            return std::make_unique<StagedCore>(
                std::make_unique<EnvelopeFrontEnd>(bpfCutoff, bpfTrans),
                std::make_unique<SchmittDetector>(edgeBias, peak),
                std::make_unique<AdaptiveTimingStage>(timing),
                std::make_unique<BeamSymbolDecoder>(),
                mfResize, 1.0f, adaptiveBpf, bpfGarbageRevert, bpfReeval);
        }

        // Streaming forward-backward detector (docs §52 #42). adaptiveBpf pairs it
        // with the narrow speed-matched pre-detection filter (#31) that was
        // variance-blocked on the Schmitt trigger — the robust detector is the
        // missing partner. bpfCutoff defaults wide; the adaptive path narrows it.
        inline std::unique_ptr<IDecodeCore> makeFB(
                TimingStrategy timing, bool fbBpf = true,
                float bpfCutoff = 140.0f, float bpfTrans = 140.0f,
                float smoothCutoff = 88.0f, float smoothTrans = 100.0f) {
            // Start WIDE: the forward-only detector no longer runs away on a wide
            // warmup (§52 step 4), so starting wide avoids the narrow->wide switch
            // transient on clean AND keeps high-SNR non-AWGN (hand-keyed) wide. The fb
            // adaptive path (fbBpf) narrows only for confirmed broadband noise
            // (inputSnr < 8 at the fast gate). matchedFilter=false: the fb detector
            // does its own smoothing, so the boxcar is bypassed (§52 step 4).
            return std::make_unique<StagedCore>(
                std::make_unique<EnvelopeFrontEnd>(bpfCutoff, bpfTrans, smoothCutoff, smoothTrans),
                std::make_unique<ForwardBackwardDetector>(),
                std::make_unique<AdaptiveTimingStage>(timing),
                std::make_unique<BeamSymbolDecoder>(),
                MF_RESET, 1.0f, /*adaptiveBpf=*/false, false, false,
                /*matchedFilter=*/false, /*fbBpf=*/fbBpf);
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

            // §39: V1 + asymmetric dah-absorption guard. Targets the exact
            // +38.9% dit runaway §38b traced to V1 learning from noise-shortened
            // dahs, narrower than V2's blanket confidence gate. Tests whether the
            // one-directional guard clears the -10 dB wall that blocks V2/bpfauto/LR.
            {"legacy+ditguard", "Legacy pipeline + asymmetric dah-absorption guard",
             []{ return detail::makeStaged(TIMING_KALMAN_GUARD); }},

            // §41: V2 with the confidence gate applied only below 27 WPM. Keeps
            // V2's slow/moderate/worstcase wins, reverts to V1 learning at fast CW
            // where §40 found V2 regresses (noise2.0-30wpm t=4.79). Tests whether
            // the slow-vs-fast tradeoff separates on the orthogonal speed axis.
            {"legacy+kalman2s", "Legacy pipeline, speed-gated V2 Kalman timing",
             []{ return detail::makeStaged(TIMING_KALMAN_V2S); }},

            // §48: regime-adaptive timing selector. Routes to log on jittered
            // good-SNR signals (hand-keyed, where log wins §46) and to kalman2s
            // otherwise, gated on jitter AND getSNR so weak hand-keyed stays on
            // kalman2s (§46b). Targets the +0.17 good-SNR hand-keyed ceiling (§47).
            {"legacy+select", "Legacy pipeline, jitter+SNR timing selector (log/kalman2s)",
             []{ return detail::makeStaged(TIMING_SELECT); }},

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
            // ── Streaming forward-backward soft detector (docs §52 #42). The
            //    HMM posterior + fixed-lag smoothing that copies fast+heavy where
            //    LR failed. +fb pairs it with the adaptive narrow BPF; +fb+fixed
            //    is a plain wide front end for the batch-reproduction probe. ──
            {"legacy+fb", "Forward-backward soft detector + SNR-adaptive BPF/smoothing + Kalman timing",
             []{ return detail::makeFB(TIMING_KALMAN, /*fbBpf=*/true); }},

            // Regime router (§52 step 4B): select by default, fb in heavy broadband
            // noise. Captures fb's fast+heavy-AWGN wins without its weak-regime harm.
            {"legacy+route", "Regime router: select, or fb when buried in broadband noise",
             []{ return std::make_unique<RegimeRouteCore>(
                     detail::makeStaged(TIMING_SELECT),
                     detail::makeFB(TIMING_KALMAN, /*fbBpf=*/true)); }},

            {"legacy+fb+wide", "Forward-backward soft detector, fixed wide BPF (no adaptation)",
             []{ return detail::makeFB(TIMING_KALMAN, /*fbBpf=*/false, 140.0f, 140.0f, 88.0f, 100.0f); }},

            {"legacy+fb+narrow", "Forward-backward soft detector, fixed narrow 32Hz BPF",
             []{ return detail::makeFB(TIMING_KALMAN, /*fbBpf=*/false, 32.0f, 32.0f, 20.0f, 25.0f); }},

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

            // ── WPM-locked, noise-aware pre-detection BPF (docs §29–31). At
            //    timing lock the BPF is retuned to a bandwidth matched to the
            //    locked WPM (ENBW ≈ 2/T_dit) but only as far as the detector's
            //    SNR warrants — wide at high SNR (jitter-limited signals keep
            //    their edges), narrow at low SNR (noise rejection). The ground-
            //    truth ceiling: 6 significant wins, 0 regressions (§29), noise3.0
            //    0.82 → 0.07; this core is the runtime approximation. ──
            {"legacy+bpfauto", "Legacy pipeline, WPM-locked noise-aware BPF (§30)",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET,
                                           EDGE_RAW, PEAK_INSTANT_ATTACK, true); }},

            // ── Robustness variants of bpfauto (docs §37). The one-shot narrow
            //    fails two gates: it rings noise into garbage at −10 dB and it
            //    commits to one fade phase on QSB. A = revert-on-garbage (event
            //    rate vs WPM), B = periodic re-evaluation on a converged smoothed
            //    getSNR with hysteresis. Benchmarked A vs B vs A+B ([bpf-ab]). ──
            {"legacy+bpfauto+a", "bpfauto + garbage-revert safety (§37 A)",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET,
                                           EDGE_RAW, PEAK_INSTANT_ATTACK, true, true, false); }},
            {"legacy+bpfauto+b", "bpfauto + re-evaluating decision (§37 B)",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET,
                                           EDGE_RAW, PEAK_INSTANT_ATTACK, true, false, true); }},
            {"legacy+bpfauto+ab", "bpfauto + garbage-revert + re-eval (§37 A+B)",
             []{ return detail::makeStaged(TIMING_KALMAN, 100.0f, 100.0f, MF_RESET,
                                           EDGE_RAW, PEAK_INSTANT_ATTACK, true, true, true); }},

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
    //
    // legacy+select PROMOTED 2026-07-22 (§48–51). The regime timing selector routes
    // each signal to log (jittered good-SNR hand-keyed) or a kalman timing (else),
    // gated on dit-CV AND getSNR, with a re-armable dit-drift latch (§49) so a
    // borderline/changing operator decodes coherently. Paired n=384: 13 better, 0
    // significant worse. Two initial ratchet failures were resolved (§50–51): the
    // moderate-noise 0.0 ratchet was a LUCKY n=24 legacy draw (legacy scores 0.0136
    // at n=96; select 0.0026 — better), recalibrated to a robust bound; the contest
    // regression was V2's cold-start dah-absorption under QRM (leading C→F), fixed
    // by SNR-grading the non-log branch — light-noise machine signals route to V1
    // (which handles them), V2 only below getSNR 6.5 where its heavy-noise advantage
    // is real. Unlike the reverted LR promotion, the gate coverage is complete
    // (fast×heavy §43, weak-hand-keyed §48, and the recalibrated artifacts §51).
    //
    // legacy+route PROMOTED 2026-07-23 (§53). The regime router runs legacy+select and
    // the forward-only online-EM fb detector in parallel and, at a 12s commit, hands off
    // to fb where the DECODE-PLAUSIBILITY signal says fb copies a buried AWGN signal that
    // select cannot: select's valid/total token RATIO is low (garbage), fb's ratio beats
    // it by a margin, fb's keying CONTRAST is low (buried, not strong-hand-keyed), and the
    // pre-BPF inputSnr is broadband. Paired n=96 vs select: 6 better / 0 worse / 0 harm
    // (noise3-25wpm 0.98→0.41, noise3-30wpm 0.92→0.53, +4 more cells); full default gate
    // suite green. This SUPERSEDES the earlier deferral: the deferred version used a raw
    // token-COUNT gate (selGood<=0 && fbGood>=3) that stalled on select's 1-2 garbage
    // tokens and only reached 0.84 with 3 improved cells. Two §53 fixes unlocked it: the
    // staged_core idle-flush no longer misclassifies stretched gaps (farnsworth 0.155→
    // 0.014, so fb's router branch is clean), and the ratio+contrast arbitration replaced
    // the count. Cost: ~2x decode for the 12s commit window, 1x after (winner only).
    // fb detector: src/cw/fb_detector.h; router+arbitration: src/cw/regime_route.h.
    inline constexpr const char* DEFAULT_CORE = "legacy+route";
}
