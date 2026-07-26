#pragma once
#include <cmath>
#include <algorithm>
#include <vector>
#include <numeric>

namespace cw {

    enum Element { DIT, DAH };
    enum Gap { ELEMENT_GAP, CHAR_GAP, WORD_GAP };

    struct TimingEvent {
        enum Type { KEY_ELEMENT, KEY_GAP } type;
        union { Element element; Gap gap; };
        float confidence;
        float durationMs;
    };

    enum TimingStrategy {
        TIMING_KMEANS,
        TIMING_MEDIAN,
        TIMING_BIMODAL,
        TIMING_KALMAN,      // Kalman filter with Bayesian gap classification (V1, historical)
        TIMING_KALMAN_V2,   // + corrected dah gain, confidence-gated learning, fixed R floor
        TIMING_KALMAN_GUARD,// V1 + asymmetric dah-absorption guard (docs §39): blocks the
                            // one-directional ditEst runaway §38b traced to V1 learning
                            // from noise-shortened dahs, without V2's blanket gate
        TIMING_KALMAN_V2S,  // V2 with the confidence gate applied only below a WPM
                            // threshold (docs §41): keeps V2's slow/moderate wins,
                            // reverts to V1 learning at fast CW where V2 regresses (§40)
        TIMING_SELECT,      // §48 regime selector: runs kalman2s + log, routes output to
                            // log on jittered good-SNR signals (hand-keyed, where log wins
                            // §46) and to kalman2s otherwise. Gated on jitter AND SNR so a
                            // weak hand-keyed signal stays on kalman2s (log loses there §46b)
        TIMING_LOG,         // log-duration Kalman: multiplicative jitter model (Mills 1977)
        TIMING_LOG_ROBUST,  // + Huberised state update: outliers teach R but barely move x
        TIMING_LOG_GUARDED, // + hard x-freeze beyond guardK sigma; R still learns full.
                            // Targets the diagnosed runaway (docs §20.7): a spike
                            // flood pulls x down, shrinking minElementMs, admitting
                            // more spikes. LOG_ROBUST only downweights x (never 0)
                            // and its full-R learning saturates R, widening the gate
                            // and disabling its own Huber weight. Freezing x breaks
                            // the drift; full-R keeps the qrm/qrn tolerance.
    };

    // ════════════════════════════════════════════════════════════
    // Strategy 1: K-Means EMA with auto-seed
    // ════════════════════════════════════════════════════════════
    class KMeansTiming {
    public:
        void init() { reset(); }

        TimingEvent classifyOn(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_ELEMENT;
            evt.durationMs = durationMs;

            if (durationMs < 1.0f) { durationMs = 1.0f; }
            if (elementCount < seedCount) {
                seedBuf[elementCount] = durationMs;
                elementCount++;

                if (elementCount == seedCount) {
                    float minD = seedBuf[0], maxD = seedBuf[0];
                    for (int i = 1; i < seedCount; i++) {
                        if (seedBuf[i] < minD) minD = seedBuf[i];
                        if (seedBuf[i] > maxD) maxD = seedBuf[i];
                    }
                    if (maxD / minD > 1.8f) {
                        ditCenter = minD; dahCenter = maxD;
                    } else {
                        ditCenter = minD; dahCenter = minD * 3.0f;
                    }
                    enforceRatio();
                }

                float minSoFar = seedBuf[0];
                for (int i = 1; i < elementCount; i++) {
                    if (seedBuf[i] < minSoFar) minSoFar = seedBuf[i];
                }
                evt.element = (durationMs < minSoFar * 2.0f) ? DIT : DAH;
                evt.confidence = 0.3f;
                return evt;
            }

            float boundary = (ditCenter + dahCenter) / 2.0f;
            float separation = (dahCenter - ditCenter) / 2.0f;
            if (separation < 1.0f) { separation = 1.0f; }

            if (durationMs < boundary) {
                evt.element = DIT;
                ditCenter += alpha * (durationMs - ditCenter);
            } else {
                evt.element = DAH;
                dahCenter += alpha * (durationMs - dahCenter);
            }

            evt.confidence = std::clamp(fabsf(durationMs - boundary) / separation, 0.0f, 1.0f);
            enforceRatio();
            elementCount++;
            if (elementCount > seedCount + 10) { alpha = 0.1f; }
            return evt;
        }

        float getWPM() const { return ditCenter < 1.0f ? 0 : 1200.0f / ditCenter; }
        float getDitDuration() const { return ditCenter; }
        bool isLocked() const { return elementCount >= seedCount + 6; }
        void reset() { ditCenter = 80.0f; dahCenter = 240.0f; alpha = 0.3f; elementCount = 0; }

    private:
        void enforceRatio() {
            float ratio = dahCenter / ditCenter;
            if (ratio < 2.0f) { dahCenter = ditCenter * 2.5f; }
            else if (ratio > 4.5f) { dahCenter = ditCenter * 4.0f; }
        }
        static constexpr int seedCount = 4;
        float seedBuf[8] = {};
        float ditCenter = 80.0f;
        float dahCenter = 240.0f;
        float alpha = 0.3f;
        int elementCount = 0;
    };

    // ════════════════════════════════════════════════════════════
    // Strategy 2: Median Split
    // ════════════════════════════════════════════════════════════
    class MedianTiming {
    public:
        void init() { reset(); }
        TimingEvent classifyOn(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_ELEMENT;
            evt.durationMs = durationMs;
            durations.push_back(durationMs);
            if ((int)durations.size() > windowSize) durations.erase(durations.begin());
            if ((int)durations.size() < minSamples) {
                float avg = 0;
                for (float d : durations) avg += d;
                avg /= durations.size();
                evt.element = (durationMs < avg) ? DIT : DAH;
                evt.confidence = 0.3f;
            } else {
                recalcClusters();
                float boundary = (ditCenter + dahCenter) / 2.0f;
                float separation = std::max(1.0f, (dahCenter - ditCenter) / 2.0f);
                evt.element = (durationMs < boundary) ? DIT : DAH;
                evt.confidence = std::clamp(fabsf(durationMs - boundary) / separation, 0.0f, 1.0f);
            }
            elementCount++;
            return evt;
        }
        float getWPM() const { return ditCenter < 1.0f ? 0 : 1200.0f / ditCenter; }
        float getDitDuration() const { return ditCenter; }
        bool isLocked() const { return elementCount >= minSamples; }
        void reset() { durations.clear(); ditCenter = 80.0f; dahCenter = 240.0f; elementCount = 0; }
    private:
        void recalcClusters() {
            std::vector<float> s = durations;
            std::sort(s.begin(), s.end());
            float maxGap = 0; int splitIdx = s.size() / 2;
            for (int i = 1; i < (int)s.size(); i++) {
                float gap = s[i] - s[i-1];
                if (gap > maxGap) { maxGap = gap; splitIdx = i; }
            }
            float sumLo = 0, sumHi = 0;
            for (int i = 0; i < splitIdx; i++) sumLo += s[i];
            for (int i = splitIdx; i < (int)s.size(); i++) sumHi += s[i];
            if (splitIdx > 0) ditCenter = sumLo / splitIdx;
            if ((int)s.size() - splitIdx > 0) dahCenter = sumHi / ((int)s.size() - splitIdx);
            if (ditCenter > 0 && dahCenter / ditCenter < 1.5f) dahCenter = ditCenter * 3.0f;
        }
        static constexpr int windowSize = 40;
        static constexpr int minSamples = 6;
        std::vector<float> durations;
        float ditCenter = 80.0f; float dahCenter = 240.0f; int elementCount = 0;
    };

    // ════════════════════════════════════════════════════════════
    // Strategy 3: Bimodal Detector (histogram valley)
    // ════════════════════════════════════════════════════════════
    class BimodalTiming {
    public:
        void init() { reset(); }
        TimingEvent classifyOn(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_ELEMENT;
            evt.durationMs = durationMs;
            durations.push_back(durationMs);
            if ((int)durations.size() > windowSize) durations.erase(durations.begin());
            elementCount++;
            if ((int)durations.size() < minSamples) {
                if (durations.size() >= 2) {
                    float minD = *std::min_element(durations.begin(), durations.end());
                    float maxD = *std::max_element(durations.begin(), durations.end());
                    if (maxD / minD > 1.5f) {
                        evt.element = (durationMs < (minD + maxD) / 2.0f) ? DIT : DAH;
                        evt.confidence = 0.4f;
                        return evt;
                    }
                }
                evt.element = DIT; evt.confidence = 0.3f; return evt;
            }
            float minD = *std::min_element(durations.begin(), durations.end());
            float maxD = *std::max_element(durations.begin(), durations.end());
            if (maxD - minD < 5.0f) {
                ditCenter = minD; dahCenter = minD * 3.0f;
                evt.element = DIT; evt.confidence = 0.5f; return evt;
            }
            float range = maxD - minD;
            int hist[20] = {};
            for (float d : durations) {
                int bin = std::clamp((int)((d - minD) / range * 19), 0, 19);
                hist[bin]++;
            }
            float smooth[20];
            for (int i = 0; i < 20; i++) {
                float sum = (float)hist[i]; int cnt = 1;
                if (i > 0) { sum += hist[i-1]; cnt++; }
                if (i < 19) { sum += hist[i+1]; cnt++; }
                smooth[i] = sum / cnt;
            }
            int peak1 = 0;
            for (int i = 1; i < 20; i++) if (smooth[i] > smooth[peak1]) peak1 = i;
            int peak2 = 19; float best2 = -1;
            for (int i = 0; i < 20; i++) if (abs(i-peak1) >= 3 && smooth[i] > best2) { best2 = smooth[i]; peak2 = i; }
            if (peak1 > peak2) std::swap(peak1, peak2);
            int valley = peak1; float valleyVal = smooth[peak1];
            for (int i = peak1+1; i < peak2; i++) if (smooth[i] < valleyVal) { valleyVal = smooth[i]; valley = i; }
            float boundary = minD + (valley + 0.5f) / 20.0f * range;
            float sumLo = 0, sumHi = 0; int countLo = 0, countHi = 0;
            for (float d : durations) { if (d < boundary) { sumLo += d; countLo++; } else { sumHi += d; countHi++; } }
            if (countLo > 0) ditCenter = sumLo / countLo;
            if (countHi > 0) dahCenter = sumHi / countHi;
            if (ditCenter < 1.0f) ditCenter = 1.0f;
            if (dahCenter < ditCenter * 1.5f) dahCenter = ditCenter * 3.0f;
            float separation = std::max(1.0f, (dahCenter - ditCenter) / 2.0f);
            evt.element = (durationMs < boundary) ? DIT : DAH;
            evt.confidence = std::clamp(fabsf(durationMs - boundary) / separation, 0.0f, 1.0f);
            return evt;
        }
        float getWPM() const { return ditCenter < 1.0f ? 0 : 1200.0f / ditCenter; }
        float getDitDuration() const { return ditCenter; }
        bool isLocked() const { return elementCount >= minSamples; }
        void reset() { durations.clear(); ditCenter = 80.0f; dahCenter = 240.0f; elementCount = 0; }
    private:
        static constexpr int windowSize = 50;
        static constexpr int minSamples = 6;
        std::vector<float> durations;
        float ditCenter = 80.0f; float dahCenter = 240.0f; int elementCount = 0;
    };

    // ════════════════════════════════════════════════════════════
    // Strategy 4: Kalman Filter Timing
    //
    // State: dit duration (ms) — tracked with process + measurement noise.
    // The Kalman filter provides both the estimate AND its uncertainty (P).
    // This uncertainty feeds directly into Bayesian gap classification:
    // wider uncertainty = softer gap boundaries = more jitter tolerance.
    //
    // Element classification: Gaussian likelihood ratio.
    //   P(dit|d) ~ N(d; ditEst, P + R)
    //   P(dah|d) ~ N(d; 3*ditEst, 9*P + R)
    //   Pick whichever has higher likelihood, update accordingly.
    // ════════════════════════════════════════════════════════════
    // V1 is the historical implementation, preserved byte-for-byte as the
    // production default and as the comparison baseline. V2 carries the Stage-1
    // corrections. Both stay in the tree: a variant is only promoted once it is
    // better on every profile, never on an average.
    enum KalmanVariant { KALMAN_V1, KALMAN_V2 };

    // ════════════════════════════════════════════════════════════
    // Strategy 5: Log-Domain Kalman
    //
    // State: x = ln(dit duration in ms), NOT the duration itself.
    //
    // Hand-keying jitter is multiplicative, not additive — a 10% timing error
    // on a dah is three times as many milliseconds as the same error on a dit.
    // Mills (TR-554, 1977) models this directly, with observation variance
    // proportional to duration. Working in log space makes that variance
    // constant, which collapses three separate defects in the linear model:
    //
    //   * dit and dah differ by a fixed offset ln(3), so there is ONE state and
    //     one offset instead of two independently-drifting means.
    //   * measurement noise is the same R for dit and dah, so the Kalman gain
    //     is P/(P+R) in BOTH branches — the 81x dah-gain error of the linear
    //     model (2.1a) becomes inexpressible.
    //   * gap centres 1:3:7 become additive offsets 0 : ln3 : ln7, so
    //     Farnsworth stretching is a shift rather than a scale change.
    // ════════════════════════════════════════════════════════════
    class LogTiming {
    public:
        void init() { reset(); }
        void setRobust(bool r) { robust = r; }
        void setGuarded(bool g) { guarded = g; }
        // KNOWN-DEFECT fix: intra-character element gap (~1 dit), fed by AdaptiveTiming.
        void setGapDitHint(float ms) { _gapDitHint = ms; }

        TimingEvent classifyOn(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_ELEMENT;
            evt.durationMs = durationMs;
            if (durationMs < 1.0f) durationMs = 1.0f;

            elementCount++;
            float ld = logf(durationMs);

            // Bootstrap: shortest seed element is taken as a dit.
            if (elementCount <= seedCount) {
                seedBuf[elementCount - 1] = ld;
                float minL = seedBuf[0], maxL = seedBuf[0];
                for (int i = 1; i < elementCount; i++) {
                    if (seedBuf[i] < minL) minL = seedBuf[i];
                    if (seedBuf[i] > maxL) maxL = seedBuf[i];
                }
                if (elementCount == seedCount) {
                    // KNOWN-DEFECT FIX (was: x = minL): an all-one-type seed set
                    // (maxL-minL <= ln 1.8) may be all DAHs -> x seeded 3x high. If the
                    // element gap-dit hint says minD is ~3x a dit, minD is a DAH -> seed
                    // from the gap. Mixed sets (e.g. CQ) keep minL, so clean is unchanged.
                    float seedX = minL;
                    // See KalmanTiming seed: fire only on a tight all-one-type run whose
                    // minD is ~3x the element-gap dit (physical dah:dit = 3:1), so a noise
                    // gap (off-ratio) cannot pull the seed down. Mixed sets keep minL.
                    const bool ambiguous = (maxL - minL <= 0.405f);   // ln(1.5)
                    const float md = expf(minL);
                    const float ratio = _gapDitHint > 1.0f ? md / _gapDitHint : 0.0f;
                    if (ambiguous && ratio > 2.2f && ratio < 4.0f) {
                        seedX = logf(_gapDitHint);
                    }
                    x = seedX;
                    P = 0.25f;   // ln-space variance: ~50% duration uncertainty
                    R = 0.04f;   // ~20% duration measurement noise
                }
                evt.element = (ld < minL + LN3 * 0.5f) ? DIT : DAH;
                evt.confidence = 0.3f;
                return evt;
            }

            P += processNoise;

            // Homoscedastic: both hypotheses share variance V, so the Gaussian
            // normalisers cancel and this is a pure distance comparison.
            float V = P + R;
            float dDit = ld - x;
            float dDah = ld - (x + LN3);
            float ditLik = expf(-0.5f * dDit * dDit / V) * ditPrior;
            float dahLik = expf(-0.5f * dDah * dDah / V);

            float total = ditLik + dahLik;
            float conf = (total > 0) ? std::max(ditLik, dahLik) / total : 0.5f;

            // Same gain for both branches — the offset differs, the noise does not.
            float innovation;
            if (ditLik >= dahLik) { evt.element = DIT; innovation = dDit; }
            else                  { evt.element = DAH; innovation = dDah; }

            // Two independent gates, measuring different things:
            //
            //   conf      — can we tell dit from dah? Low when the duration
            //               falls between the two hypotheses.
            //   mahalanobis — is this a plausible element AT ALL? Distance to
            //               the NEAREST hypothesis in sigma. A noise spike far
            //               below both means scores conf ~ 1.0 (it is obviously
            //               "more dit than dah") while being 14 sigma from any
            //               real element. Without this test the filter learns
            //               from spikes with full confidence.
            // A hard outlier gate was tried here and rejected: it fixes the
            // heavy-noise garbage flood (noise3.0 1.084 -> 0.703) but wrecks
            // interference profiles at every threshold (qrm 0.002 -> 0.197 at
            // 3 sigma, -> 0.393 at ln(2)). The reason is that rejecting an
            // outlier suppresses TWO effects at once, and only one is harmful:
            //
            //   moving x  — harmful: a spike must not redefine the dit length
            //   raising R — helpful: it widens acceptance and keeps the filter
            //               tolerant, which is what qrm/qrn depend on
            //
            // ROBUST mode separates them with a Huber weight: the state update
            // is downweighted for outliers, while R always learns from the full
            // innovation. PLAIN mode keeps the unweighted update for comparison.
            if (conf >= learnThreshold) {
                float K = P / (P + R);
                float w = 1.0f;
                if (robust) {
                    float z = fabsf(innovation) / sqrtf(V);
                    if (z > huberK) { w = huberK / z; }
                }
                // GUARDED: freeze the state entirely beyond guardK sigma (a real
                // element sits within ~1 sigma of its hypothesis, so a large
                // innovation is a noise spike, not a speed change). Unlike ROBUST
                // (w = huberK/z, never 0) this stops the spike moving x at all.
                //
                // R still learns full here, deliberately: also freezing R fixes
                // the runaway further but reintroduces the qrm/qrn catastrophe
                // (qrm 0.002 -> 0.197), because interference outliers and noise
                // spikes are the same magnitude and R-learning is what rides
                // through interference (docs §20.7). Keeping R means this only
                // partially blunts the runaway — measured, not promoted.
                if (guarded) {
                    float z = fabsf(innovation) / sqrtf(V);
                    if (z > guardK) { w = 0.0f; }
                }
                x += w * K * innovation;
                P *= (1.0f - w * K);
                R = R * 0.95f + 0.05f * innovation * innovation;
            }

            x = std::clamp(x, LN_DIT_MIN, LN_DIT_MAX);   // 8-35 WPM
            P = std::clamp(P, 1e-4f, 0.25f);
            R = std::clamp(R, 1e-3f, 0.25f);

            evt.confidence = conf;
            return evt;
        }

        float getDitDuration() const { return expf(x); }
        float getWPM() const { float d = expf(x); return d < 1.0f ? 0 : 1200.0f / d; }
        bool isLocked() const { return elementCount >= seedCount + 4; }

        void reset() {
            x = logf(80.0f);
            P = 0.25f;
            R = 0.04f;
            elementCount = 0;
            _gapDitHint = 0.0f;
        }

    private:
        float _gapDitHint = 0.0f;   // shortest acquisition gap (~1 dit), fed by AdaptiveTiming
        static constexpr float LN3 = 1.0986123f;          // ln 3
        static constexpr float LN_DIT_MIN = 3.526361f;    // ln 34 ms
        static constexpr float LN_DIT_MAX = 5.010635f;    // ln 150 ms
        static constexpr int seedCount = 3;
        static constexpr float processNoise = 0.001f;
        static constexpr float learnThreshold = 0.60f;
        static constexpr float ditPrior = 1.2f;
        static constexpr float huberK = 2.0f;   // innovations beyond 2 sigma are downweighted
        static constexpr float guardK = 3.0f;    // innovations beyond 3 sigma freeze x entirely
        bool robust = false;
        bool guarded = false;

        float seedBuf[8] = {};
        float x = 4.382027f;   // ln 80 ms
        float P = 0.25f;
        float R = 0.04f;
        int elementCount = 0;
    };

    class KalmanTiming {
    public:
        void init() { reset(); }
        void setVariant(KalmanVariant v) { variant = v; }
        // Asymmetric dah-absorption guard (docs §38b/§39). V1 learns from every
        // element; under noise a shortened dah can fall below the 2*ditEst
        // boundary, be classified DIT, and inflate ditEst in a positive-feedback
        // runaway. This blocks learning from DIT-classified elements too long to
        // be a dit, in the one direction the runaway occurs.
        void setGuardDah(bool on) { guardDah = on; }
        void setGuardDahFactor(float f) { guardDahFactor = f; }
        // §41 speed-gated V2. The confidence gate (§38b fix) wins at slow/moderate
        // but regresses fast CW (§40): fast elements are noisier, so more updates
        // are gated and the responsive V2 filter is starved. Applying the gate
        // only below this WPM keeps the runaway fix where the runaway lives (slow)
        // and reverts to V1's always-learn where it hurts (fast). 0 = always gate.
        void setSpeedGateWpm(float wpm) { speedGateWpm = wpm; }
        // §51 SNR-graded V1/V2 (selector only). Reverts to V1 at good SNR where
        // V2's cold-start sensitivity regresses light-noise machine signals (§50).
        void setSnrGraded(bool g) { snrGraded = g; }
        void setKalmanSnr(float s) { _snrForGate = s; }
        // KNOWN-DEFECT fix (timing.h seed): the intra-character element gap ~= 1 dit is
        // an independent dit estimate that classifyOn cannot see; AdaptiveTiming feeds
        // the shortest acquisition gap here so the seed can reject an all-DAH seed set.
        void setGapDitHint(float ms) { _gapDitHint = ms; }

        TimingEvent classifyOn(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_ELEMENT;
            evt.durationMs = durationMs;
            if (durationMs < 1.0f) durationMs = 1.0f;

            elementCount++;

            // Bootstrap: collect elements until the seed set contains BOTH
            // dits and dahs, so that min(seed) is known to be a dit.
            //
            // A seed set whose elements are all within 1.8x of each other is
            // ambiguous: they could be all dits or all dahs, and assuming dits
            // seeds ditEst at a dah for messages opening with O, MM or TT — a
            // 3x speed error that retroDecode then replays across the whole
            // buffer. Collect more elements instead of guessing.
            if (elementCount <= seedCount) {
                seedBuf[elementCount - 1] = durationMs;
                if (elementCount == seedCount) {
                    float minD = seedBuf[0], maxD = seedBuf[0];
                    for (int i = 1; i < elementCount; i++) {
                        if (seedBuf[i] < minD) minD = seedBuf[i];
                        if (seedBuf[i] > maxD) maxD = seedBuf[i];
                    }
                    // KNOWN-DEFECT FIX (was: ditEst = minD): when maxD/minD <= 1.8 the
                    // seed set is all ONE element type, so minD may be a DAH, seeding
                    // ditEst 3x too high (openings O=---, MM, TT). Deferring the seed
                    // regresses clean decode (WPM sweep 0.01->0.364, it also defers the
                    // lock + retroDecode). Instead DISAMBIGUATE with the intra-character
                    // element gap ~= 1 dit (setGapDitHint, fed by AdaptiveTiming): if the
                    // set is ambiguous AND minD is ~3x the gap-dit, minD is a DAH -> seed
                    // from the gap. The mixed case (maxD/minD > 1.8, e.g. CQ) keeps minD
                    // unchanged, so clean decoding is byte-identical there.
                    float seedDit = minD;
                    // Fire only on a TIGHT all-one-type run whose minD is ~3x the
                    // element-gap dit (a DAH). The ratio band is the physical dah:dit =
                    // 3:1; a NOISE gap gives an off-ratio (e.g. 8x) and is rejected, so a
                    // spurious short gap cannot pull the seed down (min(gap) alone is as
                    // fragile as min(element)). Mixed sets (CQ) fail `ambiguous` and are
                    // byte-identical.
                    const bool ambiguous = (maxD <= minD * 1.5f);
                    const float ratio = _gapDitHint > 1.0f ? minD / _gapDitHint : 0.0f;
                    if (ambiguous && ratio > 2.2f && ratio < 4.0f) {
                        seedDit = _gapDitHint;
                    }
                    ditEst = seedDit;
                    P = ditEst * ditEst * 0.25f;  // initial uncertainty: 50% of dit
                    R = ditEst * ditEst * 0.04f;   // measurement noise: 20% of dit
                }
                // During seed: rough classification. At the seed-completing element,
                // classify against the just-set (possibly gap-corrected) ditEst, NOT
                // minSoFar*2 -- otherwise an all-DAH seed's 3rd element (== minSoFar)
                // is mislabeled DIT even though ditEst is now correct (O -> W bug).
                float minSoFar = seedBuf[0];
                for (int i = 1; i < elementCount; i++)
                    if (seedBuf[i] < minSoFar) minSoFar = seedBuf[i];
                const float boundary = (elementCount == seedCount) ? ditEst * 2.0f
                                                                    : minSoFar * 2.0f;
                evt.element = (durationMs < boundary) ? DIT : DAH;
                evt.confidence = 0.3f;
                return evt;
            }

            // Predict: dit duration may drift slowly
            float Q = ditEst * ditEst * processNoise;  // process noise scales with dit
            P += Q;

            // Classify: Gaussian likelihood for dit vs dah
            float ditMean = ditEst;
            float dahMean = ditEst * 3.0f;
            float ditVar = P + R;
            float dahVar = 9.0f * P + R;  // dah variance = 3^2 * dit variance + R

            float ditLik = gaussLikelihood(durationMs, ditMean, ditVar);
            float dahLik = gaussLikelihood(durationMs, dahMean, dahVar);

            // Prior: dits are slightly more common than dahs in English Morse
            ditLik *= 1.2f;

            // Classification confidence, computed BEFORE any state update so it
            // can gate learning. An element whose dit/dah likelihoods are close
            // carries almost no information about the true dit duration;
            // updating from it injects noise into the estimate. This matters
            // because the dah gain below is now correct (~0.9), so the filter
            // tracks fast and a corrupted measurement moves it a long way.
            float totalLikPre = ditLik + dahLik;
            float confPre = (totalLikPre > 0) ? std::max(ditLik, dahLik) / totalLikPre : 0.5f;
            // §41: V2 is a package — the dah-gain fix (R/9) drives both the win
            // and the fast-CW regression, and the confidence gate protects that
            // responsiveness (§41 decomposition). Speed-gating only the gate did
            // nothing (§41); the whole V1/V2 behaviour must switch on speed. Above
            // speedGateWpm the estimator reverts to V1 (9R dah-gain + always-learn)
            // where V2 regresses (§40); below it (and during the runaway, where
            // inflated ditEst reads slow) it is full V2. speedGateWpm==0 keeps
            // pure V2.
            // §41: switch V1/V2 on a smoothed WPM with HYSTERESIS. A single EMA +
            // one threshold cannot serve both sides — slow signals (qsb/qrm) need
            // stability to stay in V2 through fade bursts, fast signals need to
            // stay in V1 through downward jitter dips. A latched mode with a
            // ±2 WPM dead-band gives bidirectional stability: leave V2 only above
            // 29, return only below 25, so jitter/fade excursions inside the band
            // never flip the mode. Seeded from the first estimate, so steady-state
            // speed is right immediately. speedGateWpm==0 keeps pure V2.
            const float curWpm    = ditEst > 1.0f ? 1200.0f / ditEst : 0.0f;
            const float switchWpm = switchWpmEma > 0.0f ? switchWpmEma : curWpm;
            // §43: latch-at-lock was refuted — under heavy noise the estimate is
            // already corrupted by the lock point, so freezing traps 30wpm-n3/n4
            // in V2 (3 regressions vs the per-element switch's 1). The per-element
            // hysteresis switch below is the better form.
            if (speedGateWpm > 0.0f) {
                if (switchInV2 && switchWpm > speedGateWpm + switchHyst)      { switchInV2 = false; }
                else if (!switchInV2 && switchWpm < speedGateWpm - switchHyst) { switchInV2 = true; }
            }
            // §51: when SNR-graded (selector only), also revert to V1 at GOOD SNR.
            // V2's cold-start dah-absorption sensitivity regresses light-noise
            // machine signals (contest: leading C→F, §50); V1 handles them. V2's
            // advantage is only heavy noise, so V2 only below the SNR threshold.
            // This is a per-element update-rule change over ONE ditEst, so it adds
            // no dit-discontinuity (unlike the log/kalman switch). Not applied to
            // standalone kalman2s (its high-SNR hand-keyed win needs V2).
            const bool snrLowEnough = !snrGraded || _snrForGate < SNR_V2_THRESH;
            const bool effV2 = (variant == KALMAN_V2)
                            && (speedGateWpm <= 0.0f || switchInV2) && snrLowEnough;
            bool learn = !effV2 || (confPre >= learnThreshold);

            if (ditLik >= dahLik) {
                evt.element = DIT;
                // §39: a DIT-classified element longer than guardDahFactor*ditEst
                // is a noise-shortened dah absorbed across the 2*ditEst boundary.
                // Learning from it is the sole driver of the +38.9% runaway
                // (§38b), and the runaway is one-directional, so the guard is too:
                // classification is unchanged, only the upward learning is blocked.
                const bool dahAbsorption = guardDah && durationMs > guardDahFactor * ditEst;
                if (learn && !dahAbsorption) {
                    float K = P / (P + R);
                    ditEst += K * (durationMs - ditEst);
                    P *= (1.0f - K);
                }
                // NOTE: this residual is POSTERIOR (after the update), not the
                // innovation, so it is shrunk by the gain and biases R low.
                // Formally wrong, but correcting it regresses CER both alone
                // (hand-keyed 0.158 -> 0.223) and combined with the dah-gain
                // fix (worstcase 0.383 -> 0.397). The low R keeps the gain high
                // and compensates for this model being in the wrong coordinate:
                // linear ms with additive noise, when jitter is multiplicative
                // (Mills 1977). Fix with the move to log-duration, not before.
                // See docs/decoder-investigation-2026-07.md 2.1(b).
                float residual = durationMs - ditEst;
                R = R * 0.95f + 0.05f * residual * residual;
            } else {
                evt.element = DAH;
                // Measurement is d = 3*dit + v. Expressed against
                // impliedDit = d/3 the noise variance is R/9, not 9R — the
                // classifier above already assumes R_dah = R (dahVar = 9P + R).
                // The old 9R made dah observations move the estimate ~9x less
                // than they should.
                if (learn) {
                    float impliedDit = durationMs / 3.0f;
                    float dahR = effV2 ? (R / 9.0f) : (9.0f * R);   // §41: V1 gain above speedGateWpm
                    float K = P / (P + dahR);
                    ditEst += K * (impliedDit - ditEst);
                    P *= (1.0f - K);
                }
            }

            // Clamp to practical WPM range (8-35 WPM)
            ditEst = std::clamp(ditEst, 34.0f, 150.0f);
            P = std::clamp(P, 1.0f, ditEst * ditEst * 0.25f);
            // R is a variance (ms^2), so the floor must be quadratic in dit.
            // The old floor was linear (ditEst * 0.05), which is dimensionally
            // meaningless and evaluated to ~4 ms^2 at every speed.
            // §41: rFloor is the third V1/V2 difference; switch it on effV2 too so
            // above the threshold the estimator is exactly V1 (legacy), below it
            // exactly V2. (effV2 already governs the learn gate and the dah gain.)
            float rFloor = !effV2 ? std::max(ditEst * 0.05f, 4.0f)
                                  : std::max(0.05f * ditEst * 0.05f * ditEst, 4.0f);
            R = std::clamp(R, rFloor, ditEst * ditEst * 0.1f);

            float totalLik = ditLik + dahLik;
            evt.confidence = (totalLik > 0) ? std::max(ditLik, dahLik) / totalLik : 0.5f;

            // §41: smoothed WPM feeding the hysteresis latch above. Moderate
            // 0.9 retention — the dead-band, not the EMA speed, provides the
            // stability now, so this only needs to reject per-element spikes.
            const float wpmNow = ditEst > 1.0f ? 1200.0f / ditEst : 0.0f;
            switchWpmEma = switchWpmEma > 0.0f ? (0.9f * switchWpmEma + 0.1f * wpmNow) : wpmNow;

            return evt;
        }

        float getWPM() const { return ditEst < 1.0f ? 0 : 1200.0f / ditEst; }
        float getDitDuration() const { return ditEst; }
        float getUncertainty() const { return sqrtf(P); }
        bool isLocked() const { return elementCount >= seedCount + 4; }

        void reset() {
            ditEst = 80.0f;
            P = 1600.0f;     // initial uncertainty (40ms std)
            R = 64.0f;       // measurement noise (8ms std)
            elementCount = 0;
            switchWpmEma = 0.0f;
            switchInV2 = true;
            _gapDitHint = 0.0f;
        }

    private:
        float _gapDitHint = 0.0f;   // shortest acquisition gap (~1 dit), fed by AdaptiveTiming
        static float gaussLikelihood(float x, float mean, float var) {
            if (var < 1.0f) var = 1.0f;
            float diff = x - mean;
            return expf(-0.5f * diff * diff / var) / sqrtf(var);
        }

        static constexpr int seedCount = 3;
        static constexpr float learnThreshold = 0.60f;  // V2: skip state update below this classification confidence
        KalmanVariant variant = KALMAN_V1;
        static constexpr float processNoise = 0.001f;  // slow drift allowed
        float switchWpmEma = 0.0f;                     // §41 smoothed WPM for the V1/V2 speed switch
        bool  switchInV2 = true;                       // §41 latched mode (hysteresis), V2 by default
        static constexpr float switchHyst = 1.5f;      // §41 dead-band half-width: band [26.5,29.5]
                                                       // brackets the 25wpm (V2) and 30wpm (V1) profiles,
                                                       // whose 15% jitter ranges otherwise overlap
        bool guardDah = false;                          // §39 asymmetric dah-guard
        float guardDahFactor = 1.5f;                    // DIT updates above this*ditEst are dah-absorption
        float speedGateWpm = 0.0f;                      // §41: gate confidence only below this WPM (0 = always)
        bool  snrGraded = false;                        // §51 selector: revert to V1 at good SNR
        float _snrForGate = 20.0f;                      // §51 current getSNR (pushed each block)
        static constexpr float SNR_V2_THRESH = 6.5f;    // §51 V2 only below this getSNR (mean: contest 7.85 -> V1, moderate 5.43 / noise2.0 4.49 -> V2)

        float seedBuf[8] = {};
        float ditEst = 80.0f;
        float P = 6400.0f;    // state covariance (uncertainty squared)
        float R = 256.0f;     // measurement noise variance
        int elementCount = 0;
    };

    // ════════════════════════════════════════════════════════════
    // Unified AdaptiveTiming
    //
    // Gap classification: Bayesian with Gaussian mixture model.
    // Three gap types modeled as Gaussians:
    //   element gap ~ N(1*dit, sigma_e^2)
    //   char gap    ~ N(3*dit, sigma_c^2)
    //   word gap    ~ N(7*dit, sigma_w^2)
    //
    // sigma scales with timing uncertainty (from Kalman P, or from
    // observed variance for other strategies).
    // ════════════════════════════════════════════════════════════
    class AdaptiveTiming {
    public:
        void init(float sampleRate, TimingStrategy strategy = TIMING_KALMAN) {
            _sampleRate = sampleRate;
            _strategy = strategy;
            // §48: TIMING_SELECT routes between a kalman2s (V2 + speed gate) and a
            // log model; both are configured and fed so the non-selected one stays
            // warm for an instant switch.
            const bool v2s = (strategy == TIMING_KALMAN_V2S || strategy == TIMING_SELECT);
            kalman.setVariant((strategy == TIMING_KALMAN_V2 || v2s) ? KALMAN_V2 : KALMAN_V1);
            kalman.setGuardDah(strategy == TIMING_KALMAN_GUARD);
            kalman.setSpeedGateWpm(v2s ? defaultSpeedGateWpm : 0.0f);
            kalman.setSnrGraded(strategy == TIMING_SELECT);   // §51: SNR-graded V1/V2 in the selector only
            logTiming.setRobust(strategy == TIMING_LOG_ROBUST);
            logTiming.setGuarded(strategy == TIMING_LOG_GUARDED);
            reset();
        }

        void setSnr(float s) { _snr = s; kalman.setKalmanSnr(s); }

        TimingEvent classifyOn(float durationMs) {
            // §48: SELECT feeds BOTH models (keeping the idle one warm) and routes
            // the output by the latched jitter+SNR gate.
            if (_strategy == TIMING_SELECT) {
                TimingEvent kEvt = kalman.classifyOn(durationMs);
                TimingEvent lEvt = logTiming.classifyOn(durationMs);
                TimingEvent bEvt = bimodal.classifyOn(durationMs);   // §52.1 #40b: kept warm
                elementDurations.push_back(durationMs);
                if ((int)elementDurations.size() > 30) elementDurations.erase(elementDurations.begin());
                // Decide/settle the mode until frozen; after that it re-opens only
                // at a word gap on a robust dit-drift trigger (reEvalSelectGate).
                updateSelectGate();
                TimingEvent result = selectUseLog ? (selectUseBimodal ? bEvt : lEvt) : kEvt;
                applyProportionClassifier(result, durationMs);
                return result;
            }

            TimingEvent result;
            switch (_strategy) {
                case TIMING_KMEANS:  result = kmeans.classifyOn(durationMs); break;
                case TIMING_MEDIAN:  result = median.classifyOn(durationMs); break;
                case TIMING_BIMODAL: result = bimodal.classifyOn(durationMs); break;
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2S:
                case TIMING_KALMAN_GUARD:
                case TIMING_KALMAN_V2: result = kalman.classifyOn(durationMs); break;
                case TIMING_LOG:
                case TIMING_LOG_ROBUST:
                case TIMING_LOG_GUARDED: result = logTiming.classifyOn(durationMs); break;
                default:             result = kalman.classifyOn(durationMs); break;
            }
            // Track element durations for gap sigma estimation
            elementDurations.push_back(durationMs);
            if ((int)elementDurations.size() > 30) elementDurations.erase(elementDurations.begin());
            applyProportionClassifier(result, durationMs);
            return result;
        }

        // §48: route the gap classifier and all queries to the selected model.
        // classifyOff/getDitDuration read the selected model's dit, so a SELECT
        // signal decodes coherently through one model at a time.
    private:
        // §48: decide the model on the jitter CV (a second-order statistic) but
        // FREEZE it once settled, and re-open it only when a robust first-order
        // parameter — the dit duration — drifts (new operator / WPM change), tested
        // at a word gap. The CV is what noise corrupts, so switching on it flickers
        // on borderline signals and corrupts the decode via dit-discontinuity; the
        // dit is far more noise-robust, so it makes a clean re-evaluation trigger.
        // Result: a stable operator (incl. borderline contest) never re-switches
        // (coherent decode), while a genuine change still re-evaluates (no "latch
        // forever" smell). Runs per-element until settled, then only on re-arm.
        void updateSelectGate() {
            if (selectFrozen) { return; }
            const float dit = kalman.getDitDuration();
            if (dit < 1.0f) { return; }
            float sum = 0, sumSq = 0; int n = 0;
            for (float d : elementDurations) {
                if (d > 0.5f * dit && d < 1.5f * dit) { sum += d; sumSq += d * d; n++; }
            }
            if (n < 6) { return; }                    // too few dits for a stable jitter CV
            const float mean = sum / n;
            const float var  = std::max(0.0f, sumSq / n - mean * mean);
            const float cv   = mean > 1.0f ? sqrtf(var) / mean : 0.0f;
            if (!selectUseLog && cv > SELECT_CV_HI && _snr > SELECT_SNR_HI) { selectUseLog = true; }
            else if (selectUseLog && (cv < SELECT_CV_LO || _snr < SELECT_SNR_LO)) { selectUseLog = false; }
            // §52.1 #40b: within the hand-keyed (log) branch, a mid-speed operator
            // (dit band ~[35,57] ms, 22-30 wpm) decodes better on bimodal than log
            // (measured, [select-bimodal-sweep]). Slow keeps log; very fast (dit<LO)
            // keeps log (bimodal's cluster separation degrades on the shortest dits).
            // Decided here so it settles and freezes with the mode; the good-SNR gate
            // above keeps weak hand-keyed off bimodal (where it is far worse).
            selectUseBimodal = selectUseLog &&
                               dit >= SELECT_BIMODAL_DIT_LO && dit <= SELECT_BIMODAL_DIT_HI;
            if (kalman.isLocked()) { selectFrozen = true; selectFrozenDit = dit; }
        }

        // §48: at a word gap, re-open the frozen gate iff the operator's dit has
        // drifted materially (>25%) — a new operator or a non-automatic keyer.
        void reEvalSelectGate() {
            if (!selectFrozen) { return; }
            const float dit = kalman.getDitDuration();
            if (selectFrozenDit > 1.0f &&
                std::fabs(dit - selectFrozenDit) / selectFrozenDit > 0.25f) {
                selectFrozen = false;   // re-arm; classifyOn re-decides and re-settles
            }
        }
    public:

        TimingEvent classifyOff(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_GAP;
            evt.durationMs = durationMs;

            // Track gap durations for adaptive center estimation
            gapDurations.push_back(durationMs);
            if ((int)gapDurations.size() > 40) gapDurations.erase(gapDurations.begin());

            // KNOWN-DEFECT fix: feed the shortest ACQUISITION gap (~1 dit, the element
            // gap) to the seed estimators BEFORE they lock, so an all-DAH opening seed
            // set is corrected (the gap is an independent dit anchor classifyOn cannot
            // see). Only pre-lock and only the running min, so a clean signal's first
            // element gaps set it; after lock the seed is done and this is inert.
            if (!isLocked() && durationMs > 1.0f && durationMs < _seedGapDit) {
                _seedGapDit = durationMs;
                kalman.setGapDitHint(_seedGapDit);
                logTiming.setGapDitHint(_seedGapDit);
            }

            // _ditOverride (test-only, docs §38) substitutes a known dit into
            // the gap-centre and sigma computation ONLY, leaving getDitDuration()
            // and the element gate untouched. It isolates the gap misclassifi-
            // cation driven by a biased dit estimate from the part driven by
            // detector edge-jitter on the measured gap duration.
            float dit = _ditOverride > 0.0f ? _ditOverride : getDitDuration();
            if (dit < 1.0f) dit = 80.0f;

            float sigma = estimateJitterSigma(dit);

            // Gap centers: use observed data if enough samples, else default 1:3:7 ratio.
            // This adapts to Farnsworth spacing where char/word gaps are stretched.
            float elemMean = dit;
            float charMean = dit * 3.0f;
            float wordMean = dit * 7.0f;
            estimateGapCenters(dit, elemMean, charMean, wordMean);

            // Sigma for each gap type. Element gap sigma is kept tight
            // (element gaps are short and well-defined). Char/word sigmas are
            // wider to accommodate operator variability.
            float elemSigma = std::max(sigma * 0.5f, dit * 0.2f);
            float charSigma = std::max(sigma * 1.2f, dit * 0.5f);
            float wordSigma = std::max(sigma * 2.0f, dit * 1.0f);

            float pElem = gaussPdf(durationMs, elemMean, elemSigma);
            float pChar = gaussPdf(durationMs, charMean, charSigma);
            float pWord = gaussPdf(durationMs, wordMean, wordSigma);

            // Prior probabilities: element gaps are most common in Morse.
            // In typical text, ~65% of gaps are element gaps, ~25% char, ~10% word.
            // Strong element prior prevents short jittered gaps from being
            // promoted to char gaps.
            pElem *= 5.0f;
            pChar *= 2.0f;
            pWord *= 0.5f;

            float total = pElem + pChar + pWord;
            if (total < 1e-30f) {
                // All likelihoods near zero — use simple threshold fallback
                if (durationMs < dit * 2.0f) { evt.gap = ELEMENT_GAP; evt.confidence = 0.5f; }
                else if (durationMs < dit * 5.0f) { evt.gap = CHAR_GAP; evt.confidence = 0.5f; }
                else { evt.gap = WORD_GAP; evt.confidence = 0.5f; }
                return evt;
            }

            float postElem = pElem / total;
            float postChar = pChar / total;
            float postWord = pWord / total;

            if (postElem >= postChar && postElem >= postWord) {
                evt.gap = ELEMENT_GAP;
                evt.confidence = postElem;
            }
            else if (postChar >= postWord) {
                evt.gap = CHAR_GAP;
                evt.confidence = postChar;
            }
            else {
                evt.gap = WORD_GAP;
                evt.confidence = postWord;
                // §48: a word gap is the only place the frozen mode may re-open,
                // and only if the dit has drifted (operator change) — never a
                // mid-character switch on a stable signal.
                if (_strategy == TIMING_SELECT) { reEvalSelectGate(); }
            }

            // Track the recent element-gap reference (~1 dit) + its consistency, for the
            // proportion classifier: mark:element-gap ~= 3:1 dah / 1:1 dit is scale-
            // invariant, so it survives onset attenuation / amplitude drift where the
            // absolute mark-vs-2*dit test fails. _egMad/_egMean is the CV -> confidence.
            if (evt.gap == ELEMENT_GAP && evt.confidence >= 0.6f && durationMs > 1.0f) {
                if (_egMean <= 0.0f) { _egMean = durationMs; _egMad = 0.0f; }
                else {
                    _egMad  = 0.8f * _egMad + 0.2f * std::fabs(durationMs - _egMean);
                    _egMean = 0.8f * _egMean + 0.2f * durationMs;
                }
                _lastElemGap = durationMs;
            }

            return evt;
        }

        // Proportion classifier (user idea): correct an AMBIGUOUS absolute dit/dah
        // classification using the scale-invariant mark:element-gap ratio (~1 dit, ~3
        // dah), gated by element-gap consistency. Only touches low-confidence marks, so
        // a clean signal (confident absolute) is unchanged; it bites on degraded marks
        // (jitter/attenuation) where absolute duration fails but the ratio still holds.
        void applyProportionClassifier(TimingEvent& evt, float markMs) {
            // A/B toggles (test/investigation only, static -> read once):
            //   NOPROP     — disable this proportion classifier entirely, to isolate its
            //                effect from the rest of the pipeline in a paired run.
            //   PROP_TRACE — log every dit/dah flip this classifier makes (mark, egMean,
            //                ratio, old->new, conf, consistency) to stderr.
            static const bool disabled = getenv("NOPROP") != nullptr;
            static const bool trace = getenv("PROP_TRACE") != nullptr;
            if (disabled) { return; }
            if (evt.type != TimingEvent::KEY_ELEMENT) { return; }
            if (gapBootstrap) { return; }                       // retro replay (setRetroMode): don't churn the
                                                                // noisy opening; proportion's wins are live/mid-stream
            if (evt.confidence >= PROP_CONF_CEIL) { return; }   // absolute already confident
            if (_snr < PROP_SNR_MIN) { return; }                // heavy noise: gaps too corrupt to trust
            if (_lastElemGap < 1.0f || _egMean <= 0.0f) { return; }
            const float cv = _egMad / _egMean;                  // element-gap jitter
            const float consist = std::clamp(1.0f - cv / 0.35f, 0.0f, 1.0f);
            if (consist < PROP_CONSIST_MIN) { return; }         // gaps too jittery to trust
            const float ratio = markMs / _egMean;               // ~1 dit, ~3 dah
            const Element propClass = (ratio < 2.0f) ? DIT : DAH;
            const float margin = std::fabs(ratio - 2.0f);       // distance from boundary
            if (margin < 0.7f) { return; }                      // proportion itself ambiguous
            if (propClass != evt.element) {
                if (trace) fprintf(stderr, "  [PROP] mark=%.0f egMean=%.0f ratio=%.2f %s->%s conf=%.2f consist=%.2f\n",
                    markMs, _egMean, ratio, evt.element==DIT?"DIT":"DAH", propClass==DIT?"DIT":"DAH", evt.confidence, consist);
                evt.element = propClass;
                evt.confidence = std::min(0.95f, 0.5f + 0.25f * margin) * consist;
            }
        }

        float getWPM() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.getWPM();
                case TIMING_MEDIAN:  return median.getWPM();
                case TIMING_BIMODAL: return bimodal.getWPM();
                case TIMING_SELECT:  return selectUseLog ? (selectUseBimodal ? bimodal.getWPM() : logTiming.getWPM()) : kalman.getWPM();
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2S:
                case TIMING_KALMAN_GUARD:
                case TIMING_KALMAN_V2: return kalman.getWPM();
                case TIMING_LOG:
                case TIMING_LOG_ROBUST:
                case TIMING_LOG_GUARDED: return logTiming.getWPM();
            }
            return 0;
        }

        float getDitDuration() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.getDitDuration();
                case TIMING_MEDIAN:  return median.getDitDuration();
                case TIMING_BIMODAL: return bimodal.getDitDuration();
                case TIMING_SELECT:  return selectUseLog ? (selectUseBimodal ? bimodal.getDitDuration() : logTiming.getDitDuration()) : kalman.getDitDuration();
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2S:
                case TIMING_KALMAN_GUARD:
                case TIMING_KALMAN_V2: return kalman.getDitDuration();
                case TIMING_LOG:
                case TIMING_LOG_ROBUST:
                case TIMING_LOG_GUARDED: return logTiming.getDitDuration();
            }
            return 80.0f;
        }

        bool isLocked() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.isLocked();
                case TIMING_MEDIAN:  return median.isLocked();
                case TIMING_BIMODAL: return bimodal.isLocked();
                case TIMING_SELECT:  return selectUseLog ? (selectUseBimodal ? bimodal.isLocked() : logTiming.isLocked()) : kalman.isLocked();
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2S:
                case TIMING_KALMAN_GUARD:
                case TIMING_KALMAN_V2: return kalman.isLocked();
                case TIMING_LOG:
                case TIMING_LOG_ROBUST:
                case TIMING_LOG_GUARDED: return logTiming.isLocked();
            }
            return false;
        }

        void reset() {
            kmeans.reset();
            median.reset();
            bimodal.reset();
            kalman.reset();
            logTiming.reset();
            elementDurations.clear();
            gapDurations.clear();
            selectUseLog = false;   // §48: start on kalman2s until a jittered good-SNR run is seen
            selectUseBimodal = false;   // §52.1 #40b
            selectFrozen = false;
            selectFrozenDit = 0.0f;
            _seedGapDit = 1e9f;
            _lastElemGap = 0.0f; _egMean = 0.0f; _egMad = 0.0f;
        }

        TimingStrategy getStrategy() const { return _strategy; }
        void setStrategy(TimingStrategy s) { _strategy = s; reset(); }

        // Retro replay holds the complete pre-lock gap set and will never
        // receive more, so it can cluster from fewer samples than live
        // classification should trust, and can fall back to the single-sample
        // bootstrap when the split is still unavailable. Enabling either in the
        // live path perturbs the noisy profiles for no gain (docs §16.4).
        void setMinGapSamples(int n) { minGapSamples = n; }

        // Cold-start gap bootstrap (docs §16). Settable so the A/B against the
        // hardcoded 1:3:7 cold start is a paired measurement on identical seeds
        // rather than a rebuild, and so the losing arm stays reproducible.
        void setGapBootstrap(bool on) { gapBootstrap = on; }

        // Test-only (docs §38 confound probe). 0 disables. See classifyOff.
        void setDitOverride(float ms) { _ditOverride = ms; }

        // §39 dah-guard factor sweep (test-only). See KalmanTiming::guardDahFactor.
        void setKalmanGuardFactor(float f) { kalman.setGuardDahFactor(f); }

        // §41 speed-gate threshold. Applied by init() for TIMING_KALMAN_V2S; a
        // setter after init lets the probe sweep it. See KalmanTiming::speedGateWpm.
        void setSpeedGateWpm(float wpm) { defaultSpeedGateWpm = wpm; kalman.setSpeedGateWpm(wpm); }

        // Gap-centre instrumentation (docs §16). estimateGapCenters silently
        // substitutes hardcoded ratios both when it has too little data and when
        // its own output fails the sanity clamps, so a degenerate estimator is
        // indistinguishable from an adapting one at the classifyOff boundary.
        // This reports the centres actually in force plus which fallback fired.
        enum GapCenterSource {
            GAPC_COLD,      // < 10 gaps observed, none long yet; hardcoded 1:3:7
            GAPC_BOOTSTRAP, // < 10 gaps, char centre taken from smallest long gap
            GAPC_NOSPLIT,   // too few long gaps to separate char from word
            GAPC_CLAMPED,   // split ran, sanity clamp overwrote a centre
            GAPC_ADAPTED,   // observed centres used as-is
        };
        GapCenterSource gapCenters(float& elemMean, float& charMean, float& wordMean) const {
            float dit = getDitDuration();
            if (dit < 1.0f) dit = 80.0f;
            elemMean = dit;
            charMean = dit * 3.0f;
            wordMean = dit * 7.0f;
            return estimateGapCenters(dit, elemMean, charMean, wordMean);
        }

    private:
        static float gaussPdf(float x, float mean, float sigma) {
            if (sigma < 1.0f) sigma = 1.0f;
            float d = (x - mean) / sigma;
            return expf(-0.5f * d * d) / sigma;
        }

        // Estimate jitter sigma from observed element duration variance.
        // Uses the shorter cluster (dits) since they're more numerous and
        // represent the fundamental timing unit.
        float estimateJitterSigma(float dit) const {
            if (elementDurations.size() < 4) {
                return dit * 0.3f;  // default 30% jitter assumption
            }

            // Collect durations near dit length (within 2x)
            float sumSq = 0;
            int count = 0;
            for (float d : elementDurations) {
                if (d < dit * 2.0f && d > dit * 0.3f) {
                    float diff = d - dit;
                    sumSq += diff * diff;
                    count++;
                }
            }

            if (count < 3) {
                return dit * 0.3f;
            }

            float variance = sumSq / count;
            float sigma = sqrtf(variance);

            // Clamp: minimum 10% of dit (measurement noise floor),
            // maximum 50% of dit (beyond this, timing is unreliable)
            return std::clamp(sigma, dit * 0.15f, dit * 0.5f);
        }

        // Estimate gap cluster centers from observed gap durations.
        // Splits gaps into element (<2×dit) and non-element (>=2×dit), then
        // splits non-element into char and word by finding the largest gap
        // in the sorted non-element durations.
        GapCenterSource estimateGapCenters(float dit, float& elemMean, float& charMean, float& wordMean) const {
            float sumElem = 0; int nElem = 0;
            std::vector<float>& longGaps = _longGapsScratch;   // D: reused, not per-call alloc
            longGaps.clear();

            float boundary = dit * 2.0f;
            for (float g : gapDurations) {
                if (g < boundary) {
                    sumElem += g; nElem++;
                } else {
                    longGaps.push_back(g);
                }
            }

            // Cold start. The 1:3:7 defaults are not a neutral prior — they are
            // a Farnsworth-ratio-1.0 assumption, and at ratio >= 2 a true char
            // gap (6*dit) sits nearer the default word centre (7*dit) than the
            // default char centre (3*dit), so the first char gap is read as a
            // word gap and inserts a space. One observed long gap already
            // contradicts the ratio-1.0 assumption. Char gaps outnumber word
            // gaps roughly 2.5:1 and are the shorter class, so the smallest long
            // gap seen is the best single-sample estimate of the char centre.
            // Single-sample fallback, shared by both paths that cannot run the
            // split: too few gaps overall, and enough gaps but only one long one.
            // The pre-lock window routinely hits the second case — one char gap
            // and no word gap yet — so a bootstrap wired only into the first
            // never fires where Farnsworth needs it.
            //
            // Only override the default when the observation contradicts it. The
            // default model flips char->word just above the 5*dit midpoint of its
            // own 3:7 centres, so a shortest long gap below that is already
            // classified correctly and overriding can only add error — which is
            // what an ungated bootstrap did on every noisy profile, where an early
            // spurious gap became the char centre for the rest of the window. 5.5
            // rather than 5.0 keeps standard-timing signals untouched; ratio 1.5
            // (4.5*dit) already decodes clean.
            //
            // Gating on isLocked() was tried and is strictly worse (docs §16): the
            // first char gap arrives after four elements, before the filter locks,
            // so requiring lock disables the fix exactly where it is needed.
            auto bootstrap = [&]() -> bool {
                if (!gapBootstrap || longGaps.empty()) return false;
                float minLong = *std::min_element(longGaps.begin(), longGaps.end());
                if (minLong < dit * 5.5f) return false;
                charMean = minLong;
                wordMean = charMean * (7.0f / 3.0f);
                return true;
            };

            if ((int)gapDurations.size() < minGapSamples) {
                return bootstrap() ? GAPC_BOOTSTRAP : GAPC_COLD;
            }

            if (nElem >= 3) elemMean = sumElem / nElem;
            if (longGaps.size() < 2) {  // can't split char/word
                return bootstrap() ? GAPC_BOOTSTRAP : GAPC_NOSPLIT;
            }

            // Split long gaps into char and word by finding the largest gap
            std::sort(longGaps.begin(), longGaps.end());
            float maxGapDiff = 0; int splitIdx = (int)longGaps.size() / 2;
            for (int i = 1; i < (int)longGaps.size(); i++) {
                float diff = longGaps[i] - longGaps[i-1];
                if (diff > maxGapDiff) { maxGapDiff = diff; splitIdx = i; }
            }

            // Compute means for each cluster
            float sumChar = 0; int nChar = 0;
            float sumWord = 0; int nWord = 0;
            for (int i = 0; i < splitIdx; i++) { sumChar += longGaps[i]; nChar++; }
            for (int i = splitIdx; i < (int)longGaps.size(); i++) { sumWord += longGaps[i]; nWord++; }

            if (nChar >= 2) charMean = sumChar / nChar;
            if (nWord >= 1) wordMean = sumWord / nWord;

            // Sanity: char < word, elem < char
            bool clamped = false;
            if (charMean <= elemMean * 1.5f) { charMean = dit * 3.0f; clamped = true; }
            if (wordMean <= charMean * 1.5f) { wordMean = charMean * 2.5f; clamped = true; }
            return clamped ? GAPC_CLAMPED : GAPC_ADAPTED;
        }

        float _sampleRate = 1000;
        TimingStrategy _strategy = TIMING_KALMAN;
        KMeansTiming kmeans;
        MedianTiming median;
        BimodalTiming bimodal;
        KalmanTiming kalman;
        LogTiming logTiming;

        std::vector<float> elementDurations;  // recent ON-durations for sigma estimation
        std::vector<float> gapDurations;      // recent OFF-durations for gap center estimation
        mutable std::vector<float> _longGapsScratch;  // D: reused by estimateGapCenters (const)
        float _seedGapDit = 1e9f;             // KNOWN-DEFECT: shortest pre-lock gap (~1 dit) for seed disambiguation
        float _lastElemGap = 0.0f;            // proportion classifier: last confident element gap
        float _egMean = 0.0f, _egMad = 0.0f;  // element-gap EMA mean + mean-abs-dev (consistency)
        static constexpr float PROP_CONF_CEIL   = 0.75f;  // only correct classifications below this conf
        static constexpr float PROP_CONSIST_MIN = 0.5f;   // min element-gap consistency to trust ratio
        static constexpr float PROP_SNR_MIN     = 6.0f;   // skip under heavy noise (jittery gaps corrupt ratio)
        bool gapBootstrap = false;            // config, not state: survives reset()
        int  minGapSamples = 10;              // relaxed only for retro replay (docs §16.4)
        float _ditOverride = 0.0f;            // test-only confound probe (docs §38)
        float defaultSpeedGateWpm = 28.0f;    // §41 crossover centred between the 25wpm/30wpm profiles (§40)

        // §48 regime selector state. The gate routes to log on a JITTERED
        // (hand-keyed) signal at GOOD SNR — where log wins (§46) — and to
        // kalman2s otherwise. Both conditions are required: a weak hand-keyed
        // signal is jittered but log loses it (§46b), so low SNR forces kalman2s.
        float _snr = 20.0f;                   // pushed each block by StagedCore::setSnr
        bool  selectUseLog = false;           // §48: log/kalman2s choice
        bool  selectUseBimodal = false;       // §52.1 #40b: bimodal/log sub-choice within the log branch
        bool  selectFrozen = false;           // §48: mode settled; re-opens only on dit drift
        float selectFrozenDit = 0.0f;         // §48: dit at freeze — the operator-change reference
        static constexpr float SELECT_CV_HI  = 0.12f;  // dit-CV enter-log (hand-keyed jitter ~0.15)
        static constexpr float SELECT_CV_LO  = 0.09f;  // dit-CV leave-log (machine at good SNR ~0.05-0.08)
        static constexpr float SELECT_SNR_HI = 8.0f;   // getSNR enter-log (hand-keyed ~11+, noise2.0 ~4.5)
        static constexpr float SELECT_SNR_LO = 6.0f;   // getSNR leave-log
        // §52.1 #40b: dit band (ms) where bimodal beats log inside the hand-keyed
        // branch (22-30 wpm). Slow (dit>HI) and very fast (dit<LO) keep log.
        static constexpr float SELECT_BIMODAL_DIT_LO = 35.0f;
        static constexpr float SELECT_BIMODAL_DIT_HI = 57.0f;
    };

}
