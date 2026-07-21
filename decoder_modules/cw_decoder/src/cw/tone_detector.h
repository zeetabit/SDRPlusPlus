#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cw {

    struct KeyEvent {
        bool keyDown;
        int sampleOffset;
        // Detector's confidence that this transition closed a real element, in
        // [0,1]. Only the LR detector in soft mode sets it (from accumulated
        // evidence); everything else leaves it 1.0, so the beam weighting is
        // unchanged for those cores. Carried on the key-UP event, which is where
        // StagedCore closes the element (docs §22).
        float confidence = 1.0f;
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
    // Both known extremes fail, for opposite reasons: instant attack chases a
    // ~32 ms keying edge, a ~2 s percentile window cannot follow a ~3.3 s QSB
    // cycle. PEAK_SLOW_ATTACK sits between them — an asymmetric tracker whose
    // attack constant is the swept parameter. See docs §12.6, §13.
    enum PeakTracker {
        PEAK_INSTANT_ATTACK,   // historical behaviour
        PEAK_PERCENTILE,       // 90th percentile of the same subsampled window
        PEAK_SLOW_ATTACK,      // asymmetric EMA, attack constant settable
        PEAK_DUAL_WINDOW,      // short + long percentile; disagreement = fade detector
        PEAK_GATED,            // instant attack, gated on confirmed key-down
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
            setPeakAttackMs(peakAttackMs);
            setPeakWindowMs(peakWindowMs);
            setPeakLongWindowMs(2000.0f);
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

                    }

                    if (peakTracker == PEAK_DUAL_WINDOW) {
                        peakLongWin[peakLongWinPos] = v;
                        peakLongWinPos = (peakLongWinPos + 1) % peakLongWinSize;
                        if (peakLongWinCount < peakLongWinSize) peakLongWinCount++;
                        if (peakLongWinCount >= 10) {
                            int ln = peakLongWinCount;
                            peakLongSorted.resize(ln);
                            memcpy(peakLongSorted.data(), peakLongWin.data(), ln * sizeof(float));
                            int lidx = std::min(ln - 1, (ln * 9) / 10);
                            std::nth_element(peakLongSorted.begin(), peakLongSorted.begin() + lidx,
                                             peakLongSorted.begin() + ln);
                            peakPercentileLong = peakLongSorted[lidx];
                        }
                    }

                    if (peakTracker == PEAK_DUAL_WINDOW && peakPercentileLong > 1e-9f
                        && peakWinCount >= 10 && peakLongWinCount >= 10) {
                        float rel = fabsf(peakPercentile - peakPercentileLong) / peakPercentileLong;
                        if (rel > peakDualThreshold) { peakDualRun++; }
                        else                         { peakDualRun = 0; }
                    }

                    if (peakTracker == PEAK_PERCENTILE || peakTracker == PEAK_DUAL_WINDOW) {
                        // Element-independent by construction: the window spans
                        // many key cycles, so the estimate does not depend on
                        // where within an element the current sample sits.
                        peakWin[peakWinPos] = v;
                        peakWinPos = (peakWinPos + 1) % peakWinSize;
                        if (peakWinCount < peakWinSize) peakWinCount++;
                        if (peakWinCount >= 10) {
                            int pn = peakWinCount;
                            peakSorted.resize(pn);
                            memcpy(peakSorted.data(), peakWin.data(), pn * sizeof(float));
                            int pidx = std::min(pn - 1, (pn * 9) / 10);
                            std::nth_element(peakSorted.begin(), peakSorted.begin() + pidx,
                                             peakSorted.begin() + pn);
                            peakPercentile = peakSorted[pidx];
                        }
                    }
                }

                // Peak tracker
                if (v > signalPeak) {
                    signalPeak = v;
                } else {
                    signalPeak -= decayAlpha * (signalPeak - v);
                }

                // Threshold reference, tracked separately so signalPeak keeps
                // driving the impulse blanker and getSNR unchanged.
                if (v > slowPeak) { slowPeak += attackAlpha * (v - slowPeak); }
                else              { slowPeak -= decayAlpha * (slowPeak - v); }

                // Gated instant attack. Attack is enabled only once the key is
                // confirmed down, so a rising edge is measured against the level
                // the previous element left behind rather than against itself.
                // Decay runs unconditionally, which is what preserves the fade
                // tracking that every Phase 16 estimator gave up.
                //
                // Before the first confirmed key-down there is no previous
                // element to reference, so the gate opens and this degenerates
                // to instant attack — the same bootstrap the impulse blanker
                // uses (estimatedDitSamples > 0).
                if ((currentState || !gatedPeakArmed) && v > gatedPeak) { gatedPeak = v; }
                else if (gatedPeak > v) { gatedPeak -= decayAlpha * (gatedPeak - v); }

                // Wait for noise floor to converge before detecting.
                // Subsampled percentile needs noiseWinCount >= 10 to be valid.
                if (noiseWinCount < 10) { continue; }

                // Only the threshold reference changes; signalPeak still drives
                // the impulse blanker and getSNR, so the variant isolates one
                // mechanism.
                float peakRef = signalPeak;
                if (peakTracker == PEAK_PERCENTILE && peakPercentile > noiseFloor) {
                    peakRef = peakPercentile;
                } else if (peakTracker == PEAK_SLOW_ATTACK && slowPeak > noiseFloor) {
                    peakRef = slowPeak;
                } else if (peakTracker == PEAK_DUAL_WINDOW && peakPercentileLong > noiseFloor) {
                    // Agreement means the level is stationary: prefer the
                    // low-variance long estimate. Sustained disagreement means
                    // it is moving, so the lagging estimate is the wrong one.
                    peakRef = (peakDualRun >= peakDualPersist && peakPercentile > noiseFloor)
                            ? peakPercentile : peakPercentileLong;
                } else if (peakTracker == PEAK_GATED && gatedPeak > noiseFloor) {
                    peakRef = gatedPeak;
                }

                float dynamicRange = peakRef / noiseFloor;
                guardEvaluated++;
                if (dynamicRange < guardThreshold) {
                    guardRejected++;
                    if (guardTrace) {
                        long long at = totalProcessed + i;
                        if (!guardRuns.empty() &&
                            guardRuns.back().first + guardRuns.back().second == at) {
                            guardRuns.back().second++;
                        } else {
                            guardRuns.push_back({at, 1});
                        }
                    }
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
                        if (rawState) { gatedPeakArmed = true; }
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

        // Guard instrumentation (docs §13.9). The dynamicRange < 1.8 test skips
        // the sample entirely, so while it holds the detector is not merely
        // biased — it is off. Counting is unconditional and cheap; the
        // run-length trace is opt-in because its cost scales with rejections.
        void setGuardTrace(bool on) { guardTrace = on; }
        // peakRef >= noiseFloor by construction, so 1.0 disables the guard.
        void setGuardThreshold(float t) { guardThreshold = t; }
        long long getGuardEvaluated() const { return guardEvaluated; }
        long long getGuardRejected() const { return guardRejected; }
        const std::vector<std::pair<long long, int>>& getGuardRuns() const { return guardRuns; }
        bool isKeyDown() const { return currentState; }

        void setEdgeBias(EdgeBias b) { edgeBias = b; }
        void setPeakTracker(PeakTracker t) { peakTracker = t; }

        // Separate from the noise window so the peak estimator's length can be
        // varied without also changing the noise-floor estimator. At the default
        // 2000 ms this buffer holds exactly the same samples as noiseWin, so the
        // historical PEAK_PERCENTILE behaviour is reproduced.
        void setPeakWindowMs(float ms) {
            peakWindowMs = ms;
            peakWinSize = std::max(10, (int)(ms / 1000.0f * _sampleRate / noiseSubsample));
            peakWin.assign(peakWinSize, 0.0f);
            peakSorted.resize(peakWinSize);
            peakWinPos = 0;
            peakWinCount = 0;
        }

        // Window length trades two things the sweep showed are distinct: short
        // windows follow amplitude change, long windows estimate it precisely
        // (variance ~ 1/N). No single length serves both a stationary and a
        // fading signal. Running both and switching on their disagreement makes
        // the disagreement itself the fade detector — no new signal model.
        void setPeakLongWindowMs(float ms) {
            peakLongWinSize = std::max(10, (int)(ms / 1000.0f * _sampleRate / noiseSubsample));
            peakLongWin.assign(peakLongWinSize, 0.0f);
            peakLongSorted.resize(peakLongWinSize);
            peakLongWinPos = 0;
            peakLongWinCount = 0;
        }

        void setPeakDualThreshold(float t) { peakDualThreshold = t; }

        // Estimator noise is uncorrelated between subsamples; a fade is not.
        // Requiring N consecutive disagreements filters the former without
        // raising the threshold, which the sweep showed also blocks real fades.
        void setPeakDualPersist(int n) { peakDualPersist = std::max(1, n); }

        void setPeakAttackMs(float ms) {
            peakAttackMs = ms;
            attackAlpha = ms > 0 ? 1.0f - expf(-1000.0f / (ms * _sampleRate)) : 1.0f;
        }

        void preseed(float level, int count) {
            sampleCount = count;
            noiseFloor = std::max(level, 1e-12f);
            signalPeak = level;
            slowPeak = level;
            gatedPeak = level;
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
            slowPeak = 0.001f;
            std::fill(peakWin.begin(), peakWin.end(), 0.0f);
            peakWinPos = 0;
            peakWinCount = 0;
            std::fill(peakLongWin.begin(), peakLongWin.end(), 0.0f);
            peakLongWinPos = 0;
            peakLongWinCount = 0;
            peakPercentileLong = 0.0f;
            peakDualRun = 0;
            gatedPeak = 0.001f;
            gatedPeakArmed = false;
            guardEvaluated = 0;
            guardRejected = 0;
            guardRuns.clear();
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
        std::vector<float> peakWin, peakSorted;
        int peakWinSize = 250, peakWinPos = 0, peakWinCount = 0;
        float peakWindowMs = 2000.0f;

        std::vector<float> peakLongWin, peakLongSorted;
        int peakLongWinSize = 250, peakLongWinPos = 0, peakLongWinCount = 0;
        float peakPercentileLong = 0.0f;
        float peakDualThreshold = 0.15f;
        int peakDualPersist = 1;
        int peakDualRun = 0;
        float slowPeak = 0.001f;
        float gatedPeak = 0.001f;
        bool gatedPeakArmed = false;
        float peakAttackMs = 300.0f;
        float attackAlpha = 1.0f;
        // Effective edge width as a fraction of the dit. Must track
        // StagedCore::computeFilterWindow(), whose matched filter dominates the
        // ramp the thresholds are crossing.
        static constexpr float edgeWidthFactor = 0.4f;

        // Dynamic-range guard instrumentation
        long long guardEvaluated = 0;
        long long guardRejected = 0;
        float guardThreshold = 1.8f;
        bool guardTrace = false;
        std::vector<std::pair<long long, int>> guardRuns;

        // Impulse blanker state
        static constexpr float impulseThreshold = 2.5f;  // relative to signalPeak
        int impulseHoldCount = 0;
        float impulseHoldValue = 0.0f;
    };

    // ════════════════════════════════════════════════════════════
    // Likelihood-ratio detector (docs §24, motivated by §20.8).
    //
    // The Schmitt detector keys on envelope magnitude (v > threshold), so a
    // noise excursion that momentarily clears the threshold becomes a spurious
    // key event. Under heavy noise this floods the timing stage with short
    // elements, which log-duration timing cannot survive (§20.7). Magnitude
    // alone cannot separate a spike from a real element (§20.8).
    //
    // The envelope out of EnvelopeFrontEnd is |LPF(IQ)|: Rayleigh under noise,
    // Rician under signal. Per sample it forms a range-normalized evidence
    // u = (v - noiseFloor) / (signalPeak - noiseFloor) — 0 at the noise level,
    // 1 at the signal level — and accumulates (u - theta) into a clamped CUSUM,
    // keying on the accumulator crossing a bound. A brief spike contributes only
    // a few samples of evidence and never crosses, so rejection is by DURATION
    // of sustained evidence, not amplitude — the discriminator §20.8 showed the
    // timing layer cannot provide.
    //
    // The increment is a robust affine proxy for the true Rician/Rayleigh
    // log-likelihood ratio, not the exact form: v^2/(2 sigma^2) is the exact
    // energy statistic but explodes as the noise estimate -> 0 on a clean
    // signal (the ringing tail alone then reads as signal), and the exact Rician
    // LLR grows only linearly in v. Range normalization tracks the signal level
    // the way the Schmitt threshold does, so u stays bounded at every SNR.
    // theta = 0.5 makes both edges lag symmetrically, preserving element and gap
    // durations. This is a sequential probability ratio test in the SPRT sense.
    //
    // Output is hard KeyEvents, identical in kind to ToneDetector — timing and
    // the beam are unchanged. The soft accumulator margin is available for the
    // Phase 27 experiment but not exported here.
    class LRDetector {
    public:
        void init(float sampleRate) {
            _sampleRate = sampleRate;
            noiseSubsample = std::max(4, (int)(sampleRate / 125.0f));
            noiseWinSize = 250;
            noiseWin.assign(noiseWinSize, 0.0f);
            noiseSorted.assign(noiseWinSize, 0.0f);
            reset();
        }

        void setTheta(float t) { theta = t; }
        void setBounds(float hi, float lo) { boundHi = hi; boundLo = lo; }
        void setSoft(bool s) { soft = s; }
        void setAdaptive(bool a) { adaptive = a; }

        std::vector<KeyEvent> process(const float* envelope, int count) {
            std::vector<KeyEvent> events;
            for (int i = 0; i < count; i++) {
                const float v = envelope[i];

                if (v > signalPeak) { signalPeak = v; }
                else { signalPeak += (v - signalPeak) * decayAlpha; }

                if (++subsampleCounter >= noiseSubsample) {
                    subsampleCounter = 0;
                    noiseWin[noiseWinPos] = v;
                    noiseWinPos = (noiseWinPos + 1) % noiseWinSize;
                    if (noiseWinCount < noiseWinSize) { noiseWinCount++; }
                    if (noiseWinCount >= 10) {
                        const int n = noiseWinCount;
                        noiseSorted.resize(n);
                        memcpy(noiseSorted.data(), noiseWin.data(), n * sizeof(float));
                        const int idx = n / 4;   // 25th percentile ~ noise level
                        std::nth_element(noiseSorted.begin(), noiseSorted.begin() + idx,
                                         noiseSorted.begin() + n);
                        noiseFloor = std::max(noiseSorted[idx], 1e-12f);
                    }
                }

                // Warmup: no noise estimate yet, hold key up.
                if (noiseWinCount < 10) { totalProcessed++; continue; }

                // Range-normalized evidence: 0 at the noise level, 1 at the
                // signal level. Accumulate its excess over theta into a clamped
                // CUSUM. Sustained signal (u > theta) drives S to boundHi;
                // sustained noise (u < theta) drives it to boundLo. The clamp
                // gives hysteresis on both edges: a momentary excursion cannot
                // cross a full bound, so a spike is rejected by duration.
                const float range = std::max(signalPeak - noiseFloor, 1e-6f);
                const float u = (v - noiseFloor) / range;

                // Soft evidence: mean u over the key-down is how far the element
                // sat above the decision level — 1 for a clean element, near
                // theta for a marginal one. Accumulated while keyed down, mapped
                // to a confidence on the key-up that closes the element.
                if (currentState) { elemUSum += u; elemUCount++; }

                // Speed-adaptive bound: the fixed CUSUM lag is calibrated in
                // absolute time, so at fast speed it is a large fraction of a
                // short element and smears event timing (§25). Scaling the bound
                // by the recent element duration keeps the lag a constant
                // fraction. adaptScale is (median ON duration / 237 ms), 237 ms
                // being the 15 WPM median where the fixed bound was tuned. A
                // median, not a mean: at heavy noise the spurious SHORT elements
                // are a sporadic low tail a median ignores, where a mean would be
                // dragged down, shrinking the bound and reviving the runaway
                // (§26). Fast dits are a consistent stream the median tracks.
                const float bHi = boundHi * (adaptive ? adaptScale : 1.0f);
                const float bLo = boundLo * (adaptive ? adaptScale : 1.0f);

                S += u - theta;
                if (S > bHi) { S = bHi; }
                if (S < bLo) { S = bLo; }

                if (!currentState && S >= bHi) {
                    currentState = true;
                    elemUSum = 0.0f; elemUCount = 0;   // new element begins
                    events.push_back({true, i});
                } else if (currentState && S <= bLo) {
                    currentState = false;
                    if (adaptive && elemUCount > 0) {
                        onWin[onPos] = (float)elemUCount;
                        onPos = (onPos + 1) % ON_WIN;
                        if (onCount < ON_WIN) { onCount++; }
                        float tmp[ON_WIN];
                        memcpy(tmp, onWin, onCount * sizeof(float));
                        std::sort(tmp, tmp + onCount);
                        const float med = tmp[onCount / 2];
                        const float plo = tmp[onCount / 5];   // 20th percentile
                        // Only trust the speed estimate when the window is free
                        // of sub-dit elements: the shortest real element is a
                        // dit, ~1/3 of the median, so plo well below that means
                        // noise spikes contaminate the window and the median is
                        // unreliable. Then hold the full (safe) bound. This is
                        // what keeps slow + heavy noise from reviving the runaway
                        // while still shrinking the lag on clean fast CW (§26).
                        adaptScale = (onCount >= 6 && plo >= 0.2f * med)
                            ? std::min(std::max(med / 237.0f, 0.4f), 1.2f)
                            : 1.0f;
                    }
                    float conf = 1.0f;
                    if (soft && elemUCount > 0) {
                        const float meanU = elemUSum / elemUCount;
                        conf = std::min(std::max((meanU - theta) / (1.0f - theta), 0.1f), 0.95f);
                    }
                    events.push_back({false, i, conf});
                }
                totalProcessed++;
            }
            return events;
        }

        float getSNR() const {
            return (noiseFloor < 1e-12f) ? 0 : 10.0f * log10f(signalPeak / noiseFloor);
        }
        bool isKeyDown() const { return currentState; }

        void preseed(float level, int count) {
            (void)count;
            const float lv = std::max(level, 1e-12f);
            signalPeak = lv;
            noiseFloor = lv;
            noiseWin.assign(noiseWinSize, lv);
            noiseWinCount = noiseWinSize;
        }

        void reset() {
            decayAlpha = 1.0f - expf(-1.0f / (0.5f * _sampleRate));
            signalPeak = 0.001f;
            noiseFloor = 0.001f;
            S = 0.0f;
            currentState = false;
            elemUSum = 0.0f;
            elemUCount = 0;
            onPos = 0;
            onCount = 0;
            adaptScale = 1.0f;
            std::fill(noiseWin.begin(), noiseWin.end(), 0.0f);
            noiseWinPos = 0;
            noiseWinCount = 0;
            subsampleCounter = 0;
            totalProcessed = 0;
        }

    private:
        float _sampleRate = 1000.0f;
        float decayAlpha = 0.002f;

        // theta in [0,1] is the decision level on the normalized evidence; 0.5
        // makes the two edges lag symmetrically so durations are preserved. The
        // bound magnitude sets required evidence: ~2*bound/(1-theta) samples of
        // sustained signal to key down. Operating point theta 0.5 / bound 3.0
        // chosen by sweep (docs §21): bound 2 leaves a heavy-noise regression,
        // bound 4+ costs qsb, theta 0.6 breaks the symmetric-lag property.
        float theta = 0.5f;
        float boundHi = 3.0f;
        float boundLo = -3.0f;
        bool soft = false;
        bool adaptive = false;

        float S = 0.0f;
        bool currentState = false;
        float signalPeak = 0.001f;
        float noiseFloor = 0.001f;
        float elemUSum = 0.0f;
        int elemUCount = 0;
        static constexpr int ON_WIN = 20;   // recent ON durations for the median
        float onWin[ON_WIN] = {};
        int onPos = 0;
        int onCount = 0;
        float adaptScale = 1.0f;

        int noiseSubsample = 8;
        int noiseWinSize = 250;
        int noiseWinPos = 0;
        int noiseWinCount = 0;
        int subsampleCounter = 0;
        long long totalProcessed = 0;
        std::vector<float> noiseWin;
        std::vector<float> noiseSorted;
    };

}
