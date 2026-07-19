#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cw {

    struct KeyEvent {
        bool keyDown;
        int sampleOffset;
    };

    // How to handle the Schmitt trigger's on/off threshold asymmetry.
    //
    // Key-down is declared on rising through onRatio but released only on
    // falling through the lower offRatio, so on a ramp of width W the ON
    // duration is stretched by (onRatio - offRatio) x W and the following gap
    // shortened by the same amount. Measured on a noiseless signal: +9.8% of a
    // dit, which makes the observed dah:dit ratio 2.82 instead of 3.0. Every
    // model in timing.h assumes exact 1:3 and 1:3:7 ratios.
    // See docs/decoder-investigation-2026-07.md §12.1.
    enum EdgeBias {
        EDGE_RAW,          // historical behaviour
        EDGE_SYMMETRIC,    // on == off == 0.45: no bias, and no hysteresis
        EDGE_COMPENSATE,   // keep hysteresis, correct the event time
    };

    // Where the threshold's reference level comes from.
    //
    // PEAK_INSTANT_ATTACK sets signalPeak = v the moment v exceeds it, so on a
    // rising edge the reference tracks the signal upward and the threshold
    // (noiseFloor + ratio x range) chases it. On the falling edge the reference
    // holds, because decay is a 0.5 s exponential. That is an on/off asymmetry
    // independent of the threshold ratios.
    //
    // Hypothesis under test: this, not the ratio gap, is what stretches ON.
    // EDGE_SYMMETRIC removed the ratio gap entirely and cut the stretch by only
    // 14% of what the ratio-gap model predicted (docs §12.1, §13).
    enum PeakTracker {
        PEAK_INSTANT_ATTACK,   // historical behaviour
        PEAK_PERCENTILE,       // 90th percentile of the same subsampled window
    };

    // Adaptive CW tone detector.
    //
    // Noise floor: subsampled percentile (every Mth sample into a small buffer,
    // nth_element on the small buffer). O(1) amortized per sample.
    // Peak: instant attack / exponential decay.
    // Detection: Schmitt trigger with SNR-adaptive thresholds.
    class ToneDetector {
    public:
        void init(float sampleRate) {
            _sampleRate = sampleRate;
            decayAlpha = 1.0f - expf(-1.0f / (0.5f * sampleRate));
            minDebounce = (int)(0.005f * sampleRate);

            // Subsampled noise window: keep every 8th sample, ~250 entries for 2s
            noiseSubsample = std::max(4, (int)(sampleRate / 125.0f));
            noiseWinSize = 250;
            noiseWin.resize(noiseWinSize, 0.0f);
            noiseSorted.resize(noiseWinSize, 0.0f);
            noiseWinPos = 0;
            noiseWinCount = 0;
            subsampleCounter = 0;
        }

        std::vector<KeyEvent> process(const float* envelope, int count) {
            std::vector<KeyEvent> events;

            for (int i = 0; i < count; i++) {
                float v = envelope[i];
                sampleCount++;

                // Impulse blanker: reject QRN spikes during key-up.
                // Only activates after at least one key cycle (estimatedDitSamples > 0),
                // so it doesn't interfere with initial signal detection.
                // An impulse is a spike that far exceeds the known signal peak.
                if (!currentState && estimatedDitSamples > 0 && v > signalPeak * impulseThreshold) {
                    impulseHoldCount++;
                    if (impulseHoldCount <= minDebounce) {
                        v = impulseHoldValue;
                    } else {
                        impulseHoldCount = 0;
                    }
                } else {
                    impulseHoldCount = 0;
                    impulseHoldValue = v;
                }

                // Subsample into noise window (every Nth sample)
                if (++subsampleCounter >= noiseSubsample) {
                    subsampleCounter = 0;
                    noiseWin[noiseWinPos] = v;
                    noiseWinPos = (noiseWinPos + 1) % noiseWinSize;
                    if (noiseWinCount < noiseWinSize) noiseWinCount++;

                    // Recompute percentile on the small buffer
                    if (noiseWinCount >= 10) {
                        int n = noiseWinCount;
                        noiseSorted.resize(n);
                        memcpy(noiseSorted.data(), noiseWin.data(), n * sizeof(float));
                        int idx = n / 4;
                        std::nth_element(noiseSorted.begin(), noiseSorted.begin() + idx, noiseSorted.begin() + n);
                        noiseFloor = noiseSorted[idx];
                        if (noiseFloor < 1e-12f) noiseFloor = 1e-12f;

                        if (peakTracker == PEAK_PERCENTILE) {
                            // Lagged and stable by construction: cannot follow
                            // the sample currently being thresholded.
                            int pidx = std::min(n - 1, (n * 9) / 10);
                            std::nth_element(noiseSorted.begin(), noiseSorted.begin() + pidx,
                                             noiseSorted.begin() + n);
                            peakPercentile = noiseSorted[pidx];
                        }
                    }
                }

                // Peak tracker
                if (v > signalPeak) {
                    signalPeak = v;
                } else {
                    signalPeak -= decayAlpha * (signalPeak - v);
                }

                // Wait for noise floor to converge before detecting.
                // Subsampled percentile needs noiseWinCount >= 10 to be valid.
                if (noiseWinCount < 10) { continue; }

                // Only the threshold reference changes; signalPeak still drives
                // the impulse blanker and getSNR, so the variant isolates one
                // mechanism.
                float peakRef = signalPeak;
                if (peakTracker == PEAK_PERCENTILE && peakPercentile > noiseFloor) {
                    peakRef = peakPercentile;
                }

                float dynamicRange = peakRef / noiseFloor;
                if (dynamicRange < 1.8f) {
                    stableSamples = 0;
                    continue;
                }

                // All three historical pairs are centred on 0.45, so the
                // symmetric variant collapses to that midpoint at every SNR.
                float onRatio, offRatio;
                if (edgeBias == EDGE_SYMMETRIC) {
                    onRatio = 0.45f; offRatio = 0.45f;
                } else if (dynamicRange > 10.0f) {
                    onRatio = 0.55f; offRatio = 0.35f;
                } else if (dynamicRange > 4.0f) {
                    onRatio = 0.60f; offRatio = 0.30f;
                } else {
                    onRatio = 0.65f; offRatio = 0.25f;
                }

                float range = peakRef - noiseFloor;
                float onThresh = noiseFloor + onRatio * range;
                float offThresh = noiseFloor + offRatio * range;

                bool rawState = currentState;
                if (!currentState && v > onThresh) { rawState = true; }
                else if (currentState && v < offThresh) { rawState = false; }

                int debounceLen = minDebounce;
                if (estimatedDitSamples > 0) {
                    debounceLen = std::max(minDebounce, (int)(estimatedDitSamples * 0.08f));
                    debounceLen = std::min(debounceLen, (int)(0.025f * _sampleRate));
                }

                if (rawState != currentState) {
                    stableSamples++;
                    if (stableSamples >= debounceLen) {
                        // Pull the release edge earlier by the modelled stretch.
                        // StagedCore may hand this detector an offset outside
                        // [0,count); it only ever adds it to a running sample
                        // counter, so an out-of-range value is well defined.
                        int off = i;
                        if (edgeBias == EDGE_COMPENSATE && !rawState && estimatedDitSamples > 0) {
                            off -= (int)((onRatio - offRatio) * edgeWidthFactor * estimatedDitSamples);
                        }
                        events.push_back({rawState, off});
                        currentState = rawState;
                        stableSamples = 0;
                        if (rawState) {
                            if (lastTransition > 0) updateDitEstimate((float)(totalProcessed + i - lastTransition), false);
                            lastTransition = totalProcessed + i;
                        } else {
                            if (lastTransition > 0) updateDitEstimate((float)(totalProcessed + i - lastTransition), true);
                            lastTransition = totalProcessed + i;
                        }
                    }
                } else {
                    stableSamples = 0;
                }
            }

            totalProcessed += count;
            return events;
        }

        float getSNR() const {
            if (noiseFloor < 1e-12f) { return 0; }
            return 10.0f * log10f(signalPeak / noiseFloor);
        }

        float getNoiseFloor() const { return noiseFloor; }
        float getSignalPeak() const { return signalPeak; }
        bool isKeyDown() const { return currentState; }

        void setEdgeBias(EdgeBias b) { edgeBias = b; }
        void setPeakTracker(PeakTracker t) { peakTracker = t; }

        void preseed(float level, int count) {
            sampleCount = count;
            noiseFloor = std::max(level, 1e-12f);
            signalPeak = level;
            for (int i = 0; i < noiseWinSize; i++) noiseWin[i] = level;
            noiseWinCount = noiseWinSize;
        }

        void reset() {
            sampleCount = 0;
            noiseFloor = 0.001f;
            signalPeak = 0.001f;
            currentState = false;
            stableSamples = 0;
            estimatedDitSamples = 0;
            lastTransition = 0;
            totalProcessed = 0;
            onDurations.clear();
            std::fill(noiseWin.begin(), noiseWin.end(), 0.0f);
            noiseWinPos = 0;
            noiseWinCount = 0;
            subsampleCounter = 0;
            impulseHoldCount = 0;
            impulseHoldValue = 0.0f;
            peakPercentile = 0.0f;
        }

    private:
        void updateDitEstimate(float durationSamples, bool isOn) {
            if (!isOn || durationSamples < minDebounce * 2) return;
            onDurations.push_back(durationSamples);
            if ((int)onDurations.size() > 20) onDurations.erase(onDurations.begin());
            if (onDurations.size() >= 3) {
                std::vector<float> sorted = onDurations;
                std::sort(sorted.begin(), sorted.end());
                estimatedDitSamples = sorted[std::max(1, (int)sorted.size() / 2) / 2];
            }
        }

        float _sampleRate = 1000;
        int sampleCount = 0;

        // Subsampled noise percentile
        int noiseSubsample = 8;
        int noiseWinSize = 250;
        std::vector<float> noiseWin;
        std::vector<float> noiseSorted;
        int noiseWinPos = 0;
        int noiseWinCount = 0;
        int subsampleCounter = 0;

        float noiseFloor = 0.001f;
        float signalPeak = 0.001f;
        float decayAlpha = 0.002f;
        bool currentState = false;
        int stableSamples = 0;
        int minDebounce = 5;

        float estimatedDitSamples = 0;
        long long lastTransition = 0;
        long long totalProcessed = 0;
        std::vector<float> onDurations;

        EdgeBias edgeBias = EDGE_RAW;
        PeakTracker peakTracker = PEAK_INSTANT_ATTACK;
        float peakPercentile = 0.0f;
        // Effective edge width as a fraction of the dit. Must track
        // StagedCore::computeFilterWindow(), whose matched filter dominates the
        // ramp the thresholds are crossing.
        static constexpr float edgeWidthFactor = 0.4f;

        // Impulse blanker state
        static constexpr float impulseThreshold = 2.5f;  // relative to signalPeak
        int impulseHoldCount = 0;
        float impulseHoldValue = 0.0f;
    };

}
