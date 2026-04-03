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
        TIMING_KALMAN,      // Kalman filter with Bayesian gap classification
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
    class KalmanTiming {
    public:
        void init() { reset(); }

        TimingEvent classifyOn(float durationMs) {
            TimingEvent evt;
            evt.type = TimingEvent::KEY_ELEMENT;
            evt.durationMs = durationMs;
            if (durationMs < 1.0f) durationMs = 1.0f;

            elementCount++;

            // Bootstrap: collect first few elements to get initial estimate
            if (elementCount <= seedCount) {
                seedBuf[elementCount - 1] = durationMs;
                if (elementCount == seedCount) {
                    float minD = seedBuf[0], maxD = seedBuf[0];
                    for (int i = 1; i < seedCount; i++) {
                        if (seedBuf[i] < minD) minD = seedBuf[i];
                        if (seedBuf[i] > maxD) maxD = seedBuf[i];
                    }
                    if (maxD / minD > 1.8f) {
                        ditEst = minD;
                    } else {
                        ditEst = minD;
                    }
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

            if (ditLik >= dahLik) {
                evt.element = DIT;
                float K = P / (P + R);
                ditEst += K * (durationMs - ditEst);
                P *= (1.0f - K);
                // Adapt R slowly — prevents QSB-induced outliers from blowing it up
                float residual = durationMs - ditEst;
                R = R * 0.95f + 0.05f * residual * residual;
            } else {
                evt.element = DAH;
                float impliedDit = durationMs / 3.0f;
                float dahR = 9.0f * R;
                float K = P / (P + dahR);
                ditEst += K * (impliedDit - ditEst);
                P *= (1.0f - K);
            }

            // Clamp to practical WPM range (8-35 WPM)
            ditEst = std::clamp(ditEst, 34.0f, 150.0f);
            P = std::clamp(P, 1.0f, ditEst * ditEst * 0.25f);
            R = std::clamp(R, std::max(ditEst * 0.05f, 4.0f), ditEst * ditEst * 0.1f);

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
            reset();
        }

        TimingEvent classifyOn(float durationMs) {
            TimingEvent result;
            switch (_strategy) {
                case TIMING_KMEANS:  result = kmeans.classifyOn(durationMs); break;
                case TIMING_MEDIAN:  result = median.classifyOn(durationMs); break;
                case TIMING_BIMODAL: result = bimodal.classifyOn(durationMs); break;
                case TIMING_KALMAN:  result = kalman.classifyOn(durationMs); break;
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
                case TIMING_KALMAN:  return kalman.getWPM();
            }
            return 0;
        }

        float getDitDuration() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.getDitDuration();
                case TIMING_MEDIAN:  return median.getDitDuration();
                case TIMING_BIMODAL: return bimodal.getDitDuration();
                case TIMING_KALMAN:  return kalman.getDitDuration();
            }
            return 80.0f;
        }

        bool isLocked() const {
            switch (_strategy) {
                case TIMING_KMEANS:  return kmeans.isLocked();
                case TIMING_MEDIAN:  return median.isLocked();
                case TIMING_BIMODAL: return bimodal.isLocked();
                case TIMING_KALMAN:  return kalman.isLocked();
            }
            return false;
        }

        void reset() {
            kmeans.reset();
            median.reset();
            bimodal.reset();
            kalman.reset();
            elementDurations.clear();
            gapDurations.clear();
        }

        TimingStrategy getStrategy() const { return _strategy; }
        void setStrategy(TimingStrategy s) { _strategy = s; reset(); }

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
        void estimateGapCenters(float dit, float& elemMean, float& charMean, float& wordMean) const {
            if ((int)gapDurations.size() < 10) return;  // not enough data

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

            if (nElem >= 3) elemMean = sumElem / nElem;
            if (longGaps.size() < 2) return;  // can't split char/word

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
            if (charMean <= elemMean * 1.5f) charMean = dit * 3.0f;
            if (wordMean <= charMean * 1.5f) wordMean = charMean * 2.5f;
        }

        float _sampleRate = 1000;
        TimingStrategy _strategy = TIMING_KALMAN;
        KMeansTiming kmeans;
        MedianTiming median;
        BimodalTiming bimodal;
        KalmanTiming kalman;

        std::vector<float> elementDurations;  // recent ON-durations for sigma estimation
        std::vector<float> gapDurations;      // recent OFF-durations for gap center estimation
    };

}
