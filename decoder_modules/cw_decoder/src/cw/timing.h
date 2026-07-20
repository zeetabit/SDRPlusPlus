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
        TIMING_LOG,         // log-duration Kalman: multiplicative jitter model (Mills 1977)
        TIMING_LOG_ROBUST,  // + Huberised state update: outliers teach R but barely move x
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
                float minL = seedBuf[0];
                for (int i = 1; i < elementCount; i++) {
                    if (seedBuf[i] < minL) minL = seedBuf[i];
                }
                if (elementCount == seedCount) {
                    x = minL;
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
        }

    private:
        static constexpr float LN3 = 1.0986123f;          // ln 3
        static constexpr float LN_DIT_MIN = 3.526361f;    // ln 34 ms
        static constexpr float LN_DIT_MAX = 5.010635f;    // ln 150 ms
        static constexpr int seedCount = 3;
        static constexpr float processNoise = 0.001f;
        static constexpr float learnThreshold = 0.60f;
        static constexpr float ditPrior = 1.2f;
        static constexpr float huberK = 2.0f;   // innovations beyond 2 sigma are downweighted
        bool robust = false;

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
                    // KNOWN DEFECT: when maxD/minD <= 1.8 the seed set is all
                    // one element type and minD may be a DAH, seeding ditEst 3x
                    // too high (messages opening O, MM, TT). Deferring the seed
                    // until the set is unambiguous was tried and regresses clean
                    // decoding badly (WPM sweep CER 0.01 -> 0.364) because it
                    // also defers timing lock and retroDecode. The correct fix
                    // uses gap durations to disambiguate -- gaps within a
                    // character are element gaps ~= 1 dit -- which classifyOn()
                    // cannot see. See docs/decoder-investigation-2026-07.md 2.1(c).
                    ditEst = minD;
                    P = ditEst * ditEst * 0.25f;  // initial uncertainty: 50% of dit
                    R = ditEst * ditEst * 0.04f;   // measurement noise: 20% of dit
                }
                // During seed: rough classification
                float minSoFar = seedBuf[0];
                for (int i = 1; i < elementCount; i++)
                    if (seedBuf[i] < minSoFar) minSoFar = seedBuf[i];
                evt.element = (durationMs < minSoFar * 2.0f) ? DIT : DAH;
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
            bool learn = (variant == KALMAN_V1) || (confPre >= learnThreshold);

            if (ditLik >= dahLik) {
                evt.element = DIT;
                if (learn) {
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
                    float dahR = (variant == KALMAN_V1) ? (9.0f * R) : (R / 9.0f);
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
            float rFloor = (variant == KALMAN_V1) ? std::max(ditEst * 0.05f, 4.0f)
                                                  : std::max(0.05f * ditEst * 0.05f * ditEst, 4.0f);
            R = std::clamp(R, rFloor, ditEst * ditEst * 0.1f);

            float totalLik = ditLik + dahLik;
            evt.confidence = (totalLik > 0) ? std::max(ditLik, dahLik) / totalLik : 0.5f;

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
        }

    private:
        static float gaussLikelihood(float x, float mean, float var) {
            if (var < 1.0f) var = 1.0f;
            float diff = x - mean;
            return expf(-0.5f * diff * diff / var) / sqrtf(var);
        }

        static constexpr int seedCount = 3;
        static constexpr float learnThreshold = 0.60f;  // V2: skip state update below this classification confidence
        KalmanVariant variant = KALMAN_V1;
        static constexpr float processNoise = 0.001f;  // slow drift allowed

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
            kalman.setVariant(strategy == TIMING_KALMAN_V2 ? KALMAN_V2 : KALMAN_V1);
            logTiming.setRobust(strategy == TIMING_LOG_ROBUST);
            reset();
        }

        TimingEvent classifyOn(float durationMs) {
            TimingEvent result;
            switch (_strategy) {
                case TIMING_KMEANS:  result = kmeans.classifyOn(durationMs); break;
                case TIMING_MEDIAN:  result = median.classifyOn(durationMs); break;
                case TIMING_BIMODAL: result = bimodal.classifyOn(durationMs); break;
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2: result = kalman.classifyOn(durationMs); break;
                case TIMING_LOG:
                case TIMING_LOG_ROBUST: result = logTiming.classifyOn(durationMs); break;
                default:             result = kalman.classifyOn(durationMs); break;
            }
            // Track element durations for gap sigma estimation
            elementDurations.push_back(durationMs);
            if ((int)elementDurations.size() > 30) elementDurations.erase(elementDurations.begin());
            return result;
        }

        TimingEvent classifyOff(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_GAP;
            evt.durationMs = durationMs;

            // Track gap durations for adaptive center estimation
            gapDurations.push_back(durationMs);
            if ((int)gapDurations.size() > 40) gapDurations.erase(gapDurations.begin());

            float dit = getDitDuration();
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
            }

            return evt;
        }

        float getWPM() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.getWPM();
                case TIMING_MEDIAN:  return median.getWPM();
                case TIMING_BIMODAL: return bimodal.getWPM();
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2: return kalman.getWPM();
                case TIMING_LOG:
                case TIMING_LOG_ROBUST: return logTiming.getWPM();
            }
            return 0;
        }

        float getDitDuration() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.getDitDuration();
                case TIMING_MEDIAN:  return median.getDitDuration();
                case TIMING_BIMODAL: return bimodal.getDitDuration();
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2: return kalman.getDitDuration();
                case TIMING_LOG:
                case TIMING_LOG_ROBUST: return logTiming.getDitDuration();
            }
            return 80.0f;
        }

        bool isLocked() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.isLocked();
                case TIMING_MEDIAN:  return median.isLocked();
                case TIMING_BIMODAL: return bimodal.isLocked();
                case TIMING_KALMAN:
                case TIMING_KALMAN_V2: return kalman.isLocked();
                case TIMING_LOG:
                case TIMING_LOG_ROBUST: return logTiming.isLocked();
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
        }

        TimingStrategy getStrategy() const { return _strategy; }
        void setStrategy(TimingStrategy s) { _strategy = s; reset(); }

        // Cold-start gap bootstrap (docs §16). Settable so the A/B against the
        // hardcoded 1:3:7 cold start is a paired measurement on identical seeds
        // rather than a rebuild, and so the losing arm stays reproducible.
        void setGapBootstrap(bool on) { gapBootstrap = on; }

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
            std::vector<float> longGaps;

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
            if ((int)gapDurations.size() < 10) {
                if (longGaps.empty() || !gapBootstrap) return GAPC_COLD;
                float minLong = *std::min_element(longGaps.begin(), longGaps.end());
                // Only override the default when the observation contradicts it.
                // The default model flips char->word just above the 5*dit midpoint
                // of its own 3:7 centres, so a shortest long gap below that is
                // already classified correctly and overriding can only add error —
                // which is what an ungated bootstrap did on every noisy profile,
                // where an early spurious gap became the char centre for the rest
                // of the cold window. 5.5 rather than 5.0 keeps standard-timing
                // signals untouched; ratio 1.5 (4.5*dit) already decodes clean.
                // Gating this on isLocked() was tried and is strictly worse
                // (docs §16): the first char gap arrives after four elements,
                // before the filter locks, so requiring lock disables the fix
                // exactly where it is needed while still firing later in the
                // cold window — losing the win and keeping the perturbation.
                if (minLong < dit * 5.5f) return GAPC_COLD;
                charMean = minLong;
                wordMean = charMean * (7.0f / 3.0f);
                return GAPC_BOOTSTRAP;
            }

            if (nElem >= 3) elemMean = sumElem / nElem;
            if (longGaps.size() < 2) return GAPC_NOSPLIT;  // can't split char/word

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
        bool gapBootstrap = true;             // config, not state: survives reset()
    };

}
