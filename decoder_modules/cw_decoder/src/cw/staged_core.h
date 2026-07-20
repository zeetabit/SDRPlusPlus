#pragma once
#include "core.h"
#include "stages.h"
#include <algorithm>
#include <cstdio>

// StagedCore — the classic pipeline as a composition of swappable stages:
//
//     front end → matched filter → detector → timing → symbol decoder
//
// The sequencing around those stages (pre-lock event capture, retro-decode on
// timing lock, silence flush, timing freeze) is inline here rather than being a
// fifth stage. A jointly-estimating core (Bell 1977) replaces this logic
// wholesale, so there is no second implementation to validate an ISequencer
// interface against yet; extract it when one exists.

namespace cw {

    // What happens to the matched filter when the window size changes.
    //
    // MF_RESET zeroes the ring buffer, so the output collapses to ~0 and takes
    // W samples to refill — a transient in the middle of a live signal. The
    // window is int(dit x 0.4), so ordinary drift of ditEst across an integer
    // boundary triggers it. Measured on a noiseless 25 WPM signal: 356 detected
    // transitions against 342 keyed, every spurious pair landing 3-13 ms into
    // an element onset. See docs/decoder-investigation-2026-07.md §12.
    //
    // MF_PRESERVE refills with the running mean instead, so the output is
    // continuous across the resize.
    enum MatchedFilterResize {
        MF_RESET,      // historical behaviour
        MF_PRESERVE,
    };

    class StagedCore : public IDecodeCore {
    public:
        StagedCore(std::unique_ptr<IFrontEnd> fe,
                   std::unique_ptr<IDetector> det,
                   std::unique_ptr<ITiming> tim,
                   std::unique_ptr<ISymbolDecoder> sym,
                   MatchedFilterResize mfResize = MF_RESET)
            : frontEnd(std::move(fe)), detector(std::move(det)),
              timing(std::move(tim)), symbols(std::move(sym)),
              mfResizePolicy(mfResize) {}

        int id = 0;
        bool debugLog = false;

        void init(float sampleRate, float internalRate, float toneFreq) override {
            _internalRate = internalRate;
            frontEnd->init(toneFreq, sampleRate, internalRate);
            detector->init(internalRate);
            timing->init(internalRate);
            symbols->init();
            envBuf = dsp::buffer::alloc<float>(CORE_MAX_ENVELOPE);
            mfBuf  = dsp::buffer::alloc<float>(CORE_MAX_ENVELOPE);
        }

        ~StagedCore() override {
            if (envBuf) { dsp::buffer::free(envBuf); }
            if (mfBuf)  { dsp::buffer::free(mfBuf); }
        }

        void setToneFreq(float freq) override { frontEnd->setToneFreq(freq); }

        void process(int count, const dsp::complex_t* iq, CharSink& sink) override {
            int envCount = frontEnd->process(count, iq, envBuf);
            if (envCount <= 0) { return; }

            diagCount = envCount;

            applyMatchedFilter(envBuf, mfBuf, envCount);

            auto events = detector->process(mfBuf, envCount);
            _snr = detector->getSNR();
            float sqFactor = std::clamp((_snr - 3.0f) / 7.0f, 0.0f, 1.0f);

            for (auto& evt : events) {
                if (sqFactor <= 0.0f) { continue; }

                // Unfreeze timing when a confident key-down arrives
                if (timingFrozen && evt.keyDown && sqFactor > 0.5f) {
                    if (debugLog) fprintf(stderr, "[CW ch%d] UNFREEZE sqF=%.2f\n", id, sqFactor);
                    timingFrozen = false;
                }

                float now = (float)(totalSamples + evt.sampleOffset) / _internalRate * 1000.0f;

                if (!timingWasLocked) {
                    // Pre-lock: save events for retroDecode, feed timing only
                    preLockEvents.push_back({evt.keyDown, now});
                    if (evt.keyDown) {
                        flushed = false;
                        if (lastKeyUp >= 0) {
                            float gapMs = now - lastKeyUp;
                            if (!timingFrozen) timing->classifyOff(gapMs);
                        }
                        lastKeyDown = now;
                        if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f PRE DOWN snr=%.1f sqF=%.2f\n", id, now, _snr, sqFactor);
                    } else {
                        if (lastKeyDown >= 0) {
                            float elemMs = now - lastKeyDown;
                            if (elemMs >= 5.0f) {
                                if (!timingFrozen) {
                                    auto te = timing->classifyOn(elemMs);
                                    if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f PRE elem=%.1fms %s conf=%.2f dit=%.1f wpm=%.1f\n",
                                        id, now, elemMs, te.element == DIT ? "DIT" : "DAH", te.confidence,
                                        timing->getDitDuration(), timing->getWPM());
                                }
                            }
                        }
                        lastKeyUp = now;
                    }
                } else if (!timingFrozen) {
                    // Post-lock, not frozen: full decode
                    if (evt.keyDown) {
                        flushed = false;
                        if (lastKeyUp >= 0) {
                            float gapMs = now - lastKeyUp;
                            auto te = timing->classifyOff(gapMs);
                            if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f gap=%.1fms %s conf=%.2f dit=%.1f wpm=%.1f\n",
                                id, now, gapMs,
                                te.gap == ELEMENT_GAP ? "ELEM" : (te.gap == CHAR_GAP ? "CHAR" : "WORD"),
                                te.confidence, timing->getDitDuration(), timing->getWPM());
                            if (te.gap == CHAR_GAP || te.gap == WORD_GAP) {
                                char c = symbols->characterBreak();
                                if (c) {
                                    if (debugLog) fprintf(stderr, "[CW ch%d] EMIT '%c' conf=%.2f\n", id, c, te.confidence * sqFactor);
                                    sink.emitChar(c, te.confidence * sqFactor);
                                }
                                if (te.gap == WORD_GAP) { sink.emitWordGap(); }
                            }
                        }
                        lastKeyDown = now;
                    } else {
                        if (lastKeyDown >= 0) {
                            float elemMs = now - lastKeyDown;
                            if (elemMs >= minElementMs()) {
                                auto te = timing->classifyOn(elemMs);
                                if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f elem=%.1fms %s conf=%.2f sqF=%.2f dit=%.1f wpm=%.1f\n",
                                    id, now, elemMs, te.element == DIT ? "DIT" : "DAH", te.confidence,
                                    sqFactor, timing->getDitDuration(), timing->getWPM());
                                symbols->addElement(te.element, te.confidence * sqFactor);
                                _confidence = te.confidence * sqFactor;
                            } else if (debugLog) {
                                fprintf(stderr, "[CW ch%d] t=%.0f elem=%.1fms REJECTED min=%.1f\n", id, now, elemMs, minElementMs());
                            }
                        }
                        lastKeyUp = now;
                    }
                } else {
                    // Frozen: skip timing updates and decoding, only track key times
                    if (evt.keyDown) { lastKeyDown = now; }
                    else { lastKeyUp = now; }
                }
            }

            totalSamples += envCount;

            if (!timingWasLocked) {
                if (timing->isLocked()) {
                    if (debugLog) fprintf(stderr, "[CW ch%d] TIMING LOCKED dit=%.1f wpm=%.1f\n", id, timing->getDitDuration(), timing->getWPM());
                    timingWasLocked = true;
                    retroDecode(sink);
                } else if (lastKeyUp >= 0 && !detector->isKeyDown() && !preLockEvents.empty()) {
                    float nowMs = (float)totalSamples / _internalRate * 1000.0f;
                    float silenceMs = nowMs - lastKeyUp;
                    if (silenceMs > timing->getDitDuration() * 4.0f && !flushed) {
                        retroDecode(sink);
                        char c = symbols->characterBreak();
                        if (c) { sink.emitChar(c, 0.5f); }
                        sink.flushWord();
                        timingWasLocked = true;
                        flushed = true;
                    }
                }
            }

            if (timingWasLocked && lastKeyUp >= 0 && !detector->isKeyDown()) {
                float nowMs = (float)totalSamples / _internalRate * 1000.0f;
                float silenceMs = nowMs - lastKeyUp;
                float dit = timing->getDitDuration();
                float flushMs = dit * 4.0f;
                if (silenceMs > flushMs && !flushed) {
                    char c = symbols->characterBreak();
                    if (c) { sink.emitChar(c, _confidence); }
                    if (silenceMs > dit * 6.0f) {
                        sink.emitWordGap();
                    } else {
                        sink.flushWord();
                    }
                    flushed = true;
                    if (debugLog) fprintf(stderr, "[CW ch%d] FLUSH silence=%.0fms\n", id, silenceMs);
                }
                // Freeze timing after extended silence to prevent noise-induced drift.
                if (!timingFrozen && silenceMs > dit * 30.0f) {
                    timingFrozen = true;
                    frozenDitEst = dit;
                    sink.emitWordGap();
                    if (debugLog) fprintf(stderr, "[CW ch%d] FREEZE dit=%.1f wpm=%.1f silence=%.0fms\n",
                        id, dit, 1200.0f / dit, silenceMs);
                }
            }

            _wpm = timingFrozen ? (frozenDitEst > 0 ? 1200.0f / frozenDitEst : 0) : timing->getWPM();
        }

        void reset() override {
            detector->reset();
            timing->reset();
            symbols->reset();
            totalSamples = 0;
            lastKeyDown = -1;
            lastKeyUp = -1;
            flushed = false;
            _snr = 0;
            _wpm = 0;
            _confidence = 0;
            mfRingBuf.clear();
            mfRingIdx = 0;
            mfRingSum = 0;
            mfCurrentW = 0;
            preLockEvents.clear();
            timingWasLocked = false;
            timingFrozen = false;
            frozenDitEst = 0;
            diagCount = 0;
        }

        CoreStats stats() const override {
            return { _snr, _wpm, _confidence, timingWasLocked };
        }

        const float* diagnostic(int& countOut) const override {
            countOut = diagCount;
            return envBuf;
        }

        void preseed(float level, int count) override { detector->preseed(level, count); }

    private:
        static constexpr int CORE_MAX_ENVELOPE = 65536;

        void applyMatchedFilter(const float* in, float* out, int count) {
            int targetW = computeFilterWindow();
            if (targetW != mfCurrentW) {
                float fill = 0.0f;
                if (mfResizePolicy == MF_PRESERVE && mfCurrentW > 0) {
                    fill = mfRingSum / mfCurrentW;   // current output level
                }
                mfRingBuf.assign(targetW, fill);
                mfRingIdx = 0;
                mfRingSum = fill * targetW;
                mfCurrentW = targetW;
            }
            for (int i = 0; i < count; i++) {
                mfRingSum -= mfRingBuf[mfRingIdx];
                mfRingBuf[mfRingIdx] = in[i];
                mfRingSum += in[i];
                mfRingIdx = (mfRingIdx + 1) % mfCurrentW;
                out[i] = mfRingSum / mfCurrentW;
            }
        }

        float minElementMs() const {
            if (!timing->isLocked()) return 5.0f;
            float factor = (_snr > 6.0f) ? 0.3f : 0.15f;
            return std::max(5.0f, timing->getDitDuration() * factor);
        }

        int computeFilterWindow() {
            if (!timing->isLocked()) { return 25; }
            float factor = 0.4f;
            int w = (int)(timing->getDitDuration() / 1000.0f * _internalRate * factor);
            return std::max(5, std::min(w, 100));
        }

        void retroDecode(CharSink& sink) {
            if (preLockEvents.empty()) { return; }

            float lockedDit = timing->getDitDuration();
            if (debugLog) fprintf(stderr, "[CW ch%d] RETRO events=%d dit=%.1f\n",
                                  id, (int)preLockEvents.size(), lockedDit);

            auto retroTiming = timing->makeFresh();
            retroTiming->init(_internalRate);
            retroTiming->setRetroMode(true);
            for (int i = 0; i < 8; i++) {
                retroTiming->classifyOn(lockedDit);
                retroTiming->classifyOn(lockedDit * 3.0f);
            }

            // Seed the gap-centre window before classifying anything. makeFresh()
            // returns a blank estimator and the loop above restores only the
            // element model, so without this the replay re-enters the same
            // cold-start it exists to repair: the first char gap is classified
            // against the hardcoded 1:3:7 defaults, which are a Farnsworth
            // ratio-1.0 assumption (docs §16). Every gap is already in hand
            // here, so there is no reason to classify any of them cold.
            if (retroGapSeed) {
                float seedLastKeyUp = -1;
                for (auto& evt : preLockEvents) {
                    if (evt.keyDown) {
                        if (seedLastKeyUp >= 0) { retroTiming->classifyOff(evt.timeMs - seedLastKeyUp); }
                    } else {
                        seedLastKeyUp = evt.timeMs;
                    }
                }
            }

            auto retroSymbols = symbols->makeFresh();
            std::string retroStr;
            float retroLastKeyDown = -1, retroLastKeyUp = -1;

            for (auto& evt : preLockEvents) {
                if (evt.keyDown) {
                    if (retroLastKeyUp >= 0) {
                        float gapMs = evt.timeMs - retroLastKeyUp;
                        auto te = retroTiming->classifyOff(gapMs);
                        if (te.gap == CHAR_GAP || te.gap == WORD_GAP) {
                            char c = retroSymbols->characterBreak();
                            if (c) { retroStr += c; }
                            if (te.gap == WORD_GAP) { retroStr += ' '; }
                        }
                    }
                    retroLastKeyDown = evt.timeMs;
                } else {
                    if (retroLastKeyDown >= 0) {
                        float elemMs = evt.timeMs - retroLastKeyDown;
                        float retroMinElem = std::max(5.0f, lockedDit * 0.3f);
                        if (elemMs >= retroMinElem) {
                            auto te = retroTiming->classifyOn(elemMs);
                            retroSymbols->addElement(te.element, te.confidence);
                        }
                    }
                    retroLastKeyUp = evt.timeMs;
                }
            }

            if (!retroStr.empty()) {
                sink.clearEmitted();
                for (char ch : retroStr) {
                    if (ch == ' ') { sink.emitWordGap(); }
                    else { sink.emitChar(ch, 0.5f); }
                }
            }

            // Transfer retro's in-progress character to the live decoder
            symbols = std::move(retroSymbols);

            preLockEvents.clear();
        }

        std::unique_ptr<IFrontEnd> frontEnd;
        std::unique_ptr<IDetector> detector;
        std::unique_ptr<ITiming> timing;
        std::unique_ptr<ISymbolDecoder> symbols;

        float _internalRate = 1000.0f;
        bool retroGapSeed = true;   // docs §16.4; settable for the paired A/B
        float* envBuf = nullptr;
        float* mfBuf = nullptr;
        int diagCount = 0;

        std::vector<float> mfRingBuf;
        int mfRingIdx = 0;
        float mfRingSum = 0;
        int mfCurrentW = 0;
        MatchedFilterResize mfResizePolicy = MF_RESET;

        struct SavedEvent { bool keyDown; float timeMs; };
        std::vector<SavedEvent> preLockEvents;

        bool timingWasLocked = false;
        long long totalSamples = 0;
        float lastKeyDown = -1;
        float lastKeyUp = -1;
        bool flushed = false;
        bool timingFrozen = false;
        float frozenDitEst = 0;

        float _snr = 0;
        float _wpm = 0;
        float _confidence = 0;
    };
}
