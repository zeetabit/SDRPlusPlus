#pragma once
#include <cstdio>
#include <dsp/types.h>
#include "dsp.h"
#include "tone_detector.h"
#include "timing.h"
#include "morse_tree.h"
#include "text_buffer.h"
#include "corrector.h"
#include "conversation.h"

#define CW_SAMPLERATE    8000.0f
#define CW_INTERNAL_RATE 1000.0f
#define CW_MAX_ENVELOPE  65536

namespace cw {

    class Channel {
    public:
        int id = 0;
        float toneFreq = 700.0f;
        bool active = true;
        bool debugLog = false;  // TEMP: enable verbose decode logging

        float snr = 0;
        float wpm = 0;
        float confidence = 0;
        TextBuffer text;
        ConversationTracker conversation;

        void init(int channelId, float tone) {
            id = channelId;
            toneFreq = tone;
            dsp.init(toneFreq, CW_SAMPLERATE, CW_INTERNAL_RATE);
            detector.init(CW_INTERNAL_RATE);
            timing.init(CW_INTERNAL_RATE);
            morseDecoder.init();
            envBuf = dsp::buffer::alloc<float>(CW_MAX_ENVELOPE);
            mfBuf = dsp::buffer::alloc<float>(CW_MAX_ENVELOPE);
        }

        ~Channel() {
            if (envBuf) { dsp::buffer::free(envBuf); envBuf = nullptr; }
            if (mfBuf) { dsp::buffer::free(mfBuf); mfBuf = nullptr; }
        }

        void process(int count, const dsp::complex_t* iq) {
            if (!active) { return; }
            if (totalSamples == 0) {
                fprintf(stderr, "[CW ch%d] FIRST PROCESS debugLog=%d tone=%.1f active=%d\n", id, (int)debugLog, toneFreq, (int)active);
                fflush(stderr);
            }
            if (totalSamples % 10000 == 0) {
                fprintf(stderr, "[CW ch%d] t=%.1fs snr=%.1f wpm=%.1f locked=%d frozen=%d dbg=%d\n",
                    id, (float)totalSamples / CW_INTERNAL_RATE, snr, wpm, (int)timingWasLocked, (int)timingFrozen, (int)debugLog);
                fflush(stderr);
            }

            int envCount = dsp.process(count, iq, envBuf);
            if (envCount <= 0) { return; }

            {
                std::lock_guard<std::mutex> lck(diagMtx);
                int toCopy = std::min(envCount, (int)diagBuf.size());
                diagBuf.erase(diagBuf.begin(), diagBuf.begin() + toCopy);
                diagBuf.insert(diagBuf.end(), envBuf, envBuf + envCount);
            }

            applyMatchedFilter(envBuf, mfBuf, envCount);

            auto events = detector.process(mfBuf, envCount);
            snr = detector.getSNR();
            float sqFactor = std::clamp((snr - 3.0f) / 7.0f, 0.0f, 1.0f);

            for (auto& evt : events) {
                if (sqFactor <= 0.0f) { continue; }

                // Unfreeze timing when a confident key-down arrives
                if (timingFrozen && evt.keyDown && sqFactor > 0.5f) {
                    if (debugLog) fprintf(stderr, "[CW ch%d] UNFREEZE sqF=%.2f\n", id, sqFactor);
                    timingFrozen = false;
                }

                float now = (float)(totalSamples + evt.sampleOffset) / CW_INTERNAL_RATE * 1000.0f;

                if (!timingWasLocked) {
                    // Pre-lock: save events for retroDecode, feed timing only
                    preLockEvents.push_back({evt.keyDown, now});
                    if (evt.keyDown) {
                        flushed = false;
                        if (lastKeyUp >= 0) {
                            float gapMs = now - lastKeyUp;
                            if (!timingFrozen) timing.classifyOff(gapMs);
                        }
                        lastKeyDown = now;
                        if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f PRE DOWN snr=%.1f sqF=%.2f\n", id, now, snr, sqFactor);
                    } else {
                        if (lastKeyDown >= 0) {
                            float elemMs = now - lastKeyDown;
                            if (elemMs >= 5.0f) {
                                if (!timingFrozen) {
                                    auto te = timing.classifyOn(elemMs);
                                    if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f PRE elem=%.1fms %s conf=%.2f dit=%.1f wpm=%.1f\n",
                                        id, now, elemMs, te.element == DIT ? "DIT" : "DAH", te.confidence,
                                        timing.getDitDuration(), timing.getWPM());
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
                            auto te = timing.classifyOff(gapMs);
                            if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f gap=%.1fms %s conf=%.2f dit=%.1f wpm=%.1f\n",
                                id, now, gapMs,
                                te.gap == ELEMENT_GAP ? "ELEM" : (te.gap == CHAR_GAP ? "CHAR" : "WORD"),
                                te.confidence, timing.getDitDuration(), timing.getWPM());
                            if (te.gap == CHAR_GAP || te.gap == WORD_GAP) {
                                char c = morseDecoder.characterBreak();
                                if (c) {
                                    if (debugLog) fprintf(stderr, "[CW ch%d] EMIT '%c' conf=%.2f\n", id, c, te.confidence * sqFactor);
                                    emitChar(c, te.confidence * sqFactor);
                                }
                                if (te.gap == WORD_GAP) { emitWordGap(); }
                            }
                        }
                        lastKeyDown = now;
                    } else {
                        if (lastKeyDown >= 0) {
                            float elemMs = now - lastKeyDown;
                            if (elemMs >= minElementMs()) {
                                auto te = timing.classifyOn(elemMs);
                                if (debugLog) fprintf(stderr, "[CW ch%d] t=%.0f elem=%.1fms %s conf=%.2f sqF=%.2f dit=%.1f wpm=%.1f\n",
                                    id, now, elemMs, te.element == DIT ? "DIT" : "DAH", te.confidence,
                                    sqFactor, timing.getDitDuration(), timing.getWPM());
                                morseDecoder.addElement(te.element, te.confidence * sqFactor);
                                confidence = te.confidence * sqFactor;
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
                if (timing.isLocked()) {
                    if (debugLog) fprintf(stderr, "[CW ch%d] TIMING LOCKED dit=%.1f wpm=%.1f\n", id, timing.getDitDuration(), timing.getWPM());
                    timingWasLocked = true;
                    retroDecode();
                } else if (lastKeyUp >= 0 && !detector.isKeyDown() && !preLockEvents.empty()) {
                    float nowMs = (float)totalSamples / CW_INTERNAL_RATE * 1000.0f;
                    float silenceMs = nowMs - lastKeyUp;
                    if (silenceMs > timing.getDitDuration() * 4.0f && !flushed) {
                        retroDecode();
                        char c = morseDecoder.characterBreak();
                        if (c) { emitChar(c, 0.5f); }
                        flushWord();
                        timingWasLocked = true;
                        flushed = true;
                    }
                }
            }

            if (timingWasLocked && lastKeyUp >= 0 && !detector.isKeyDown()) {
                float nowMs = (float)totalSamples / CW_INTERNAL_RATE * 1000.0f;
                float silenceMs = nowMs - lastKeyUp;
                float dit = timing.getDitDuration();
                float flushMs = dit * 4.0f;
                if (silenceMs > flushMs && !flushed) {
                    char c = morseDecoder.characterBreak();
                    if (c) { emitChar(c, confidence); }
                    if (silenceMs > dit * 6.0f) {
                        emitWordGap();
                    } else {
                        flushWord();
                    }
                    flushed = true;
                    if (debugLog) fprintf(stderr, "[CW ch%d] FLUSH silence=%.0fms\n", id, silenceMs);
                }
                // Freeze timing after extended silence to prevent noise-induced drift.
                // 30×dit ≈ ~1.3s at 28 WPM, ~2.4s at 15 WPM — beyond any normal gap.
                if (!timingFrozen && silenceMs > dit * 30.0f) {
                    timingFrozen = true;
                    frozenDitEst = dit;
                    // Ensure word gap is emitted before freeze — the flush at 4×dit
                    // may have only emitted a char break, not a word space
                    emitWordGap();
                    if (debugLog) fprintf(stderr, "[CW ch%d] FREEZE dit=%.1f wpm=%.1f silence=%.0fms\n",
                        id, dit, 1200.0f / dit, silenceMs);
                }
            }

            wpm = timingFrozen ? (frozenDitEst > 0 ? 1200.0f / frozenDitEst : 0) : timing.getWPM();
        }

        // Process pre-computed envelope at internal rate (1kHz).
        // Skips the IQ→envelope DSP chain. Used for fast tests.
        void processEnvelope(const float* envelope, int count) {
            if (!active || count <= 0) { return; }

            applyMatchedFilter(envelope, mfBuf, count);

            auto events = detector.process(mfBuf, count);
            snr = detector.getSNR();
            float sqFactor = std::clamp((snr - 3.0f) / 7.0f, 0.0f, 1.0f);

            for (auto& evt : events) {
                if (sqFactor <= 0.0f) { continue; }
                if (timingFrozen && evt.keyDown && sqFactor > 0.5f) {
                    timingFrozen = false;
                }
                float now = (float)(totalSamples + evt.sampleOffset) / CW_INTERNAL_RATE * 1000.0f;
                if (!timingWasLocked) {
                    preLockEvents.push_back({evt.keyDown, now});
                    if (evt.keyDown) {
                        flushed = false;
                        if (lastKeyUp >= 0 && !timingFrozen) timing.classifyOff(now - lastKeyUp);
                        lastKeyDown = now;
                    } else {
                        if (lastKeyDown >= 0) {
                            float elemMs = now - lastKeyDown;
                            if (elemMs >= 5.0f && !timingFrozen) timing.classifyOn(elemMs);
                        }
                        lastKeyUp = now;
                    }
                } else {
                    if (evt.keyDown) {
                        flushed = false;
                        if (lastKeyUp >= 0 && !timingFrozen) {
                            float gapMs = now - lastKeyUp;
                            auto te = timing.classifyOff(gapMs);
                            if (te.gap == CHAR_GAP || te.gap == WORD_GAP) {
                                char c = morseDecoder.characterBreak();
                                if (c) { emitChar(c, te.confidence * sqFactor); }
                                if (te.gap == WORD_GAP) { emitWordGap(); }
                            }
                        }
                        lastKeyDown = now;
                    } else {
                        if (lastKeyDown >= 0) {
                            float elemMs = now - lastKeyDown;
                            if (elemMs >= minElementMs() && !timingFrozen) {
                                auto te = timing.classifyOn(elemMs);
                                morseDecoder.addElement(te.element, te.confidence * sqFactor);
                                confidence = te.confidence * sqFactor;
                            }
                        }
                        lastKeyUp = now;
                    }
                }
            }

            totalSamples += count;

            if (!timingWasLocked) {
                if (timing.isLocked()) {
                    timingWasLocked = true;
                    retroDecode();
                } else if (lastKeyUp >= 0 && !detector.isKeyDown() && !preLockEvents.empty()) {
                    float nowMs = (float)totalSamples / CW_INTERNAL_RATE * 1000.0f;
                    float silenceMs = nowMs - lastKeyUp;
                    if (silenceMs > timing.getDitDuration() * 4.0f && !flushed) {
                        retroDecode();
                        char c = morseDecoder.characterBreak();
                        if (c) { emitChar(c, 0.5f); }
                        flushWord();
                        timingWasLocked = true;
                        flushed = true;
                    }
                }
            }

            if (timingWasLocked && lastKeyUp >= 0 && !detector.isKeyDown()) {
                float nowMs = (float)totalSamples / CW_INTERNAL_RATE * 1000.0f;
                float silenceMs = nowMs - lastKeyUp;
                float dit = timing.getDitDuration();
                float flushMs = dit * 4.0f;
                if (silenceMs > flushMs && !flushed) {
                    char c = morseDecoder.characterBreak();
                    if (c) { emitChar(c, confidence); }
                    if (silenceMs > dit * 6.0f) {
                        emitWordGap();
                    } else {
                        flushWord();
                    }
                    flushed = true;
                }
                if (!timingFrozen && silenceMs > dit * 30.0f) {
                    timingFrozen = true;
                    frozenDitEst = dit;
                    emitWordGap();
                }
            }

            wpm = timingFrozen ? (frozenDitEst > 0 ? 1200.0f / frozenDitEst : 0) : timing.getWPM();
        }

        void setToneFreq(float freq) {
            toneFreq = freq;
            dsp.setToneFreq(freq);
        }

        void preseed(float noiseLevel = 0.001f, float durationMs = 500.0f) {
            int samples = (int)(durationMs / 1000.0f * CW_INTERNAL_RATE);
            detector.preseed(noiseLevel, samples);
        }

        // Emit a decoded character immediately to text buffer, and track
        // the current word for post-correction on word boundary.
        void emitChar(char c, float conf) {
            if (c == ' ' || c == '\0') return;
            text.append(c, conf);
            currentWord += c;
            currentWordConfSum += conf;
            currentWordCharCount++;
        }

        // On word boundary: correct the current word in-place if needed,
        // then feed the (corrected) word to the conversation tracker.
        void flushWord() {
            if (currentWord.empty()) return;
            float avgConf = currentWordCharCount > 0 ? currentWordConfSum / currentWordCharCount : 0.5f;
            std::string corrected = corrector::correctWord(currentWord, avgConf, &conversation);
            if (corrected != currentWord) {
                text.replaceLastN(currentWord.size(), corrected, avgConf);
            }
            conversation.feedWord(corrected, avgConf, wpm);
            currentWord.clear();
            currentWordConfSum = 0;
            currentWordCharCount = 0;
        }

        // Emit a word gap (space). Corrects the current word first.
        void emitWordGap() {
            flushWord();
            text.appendSpace();
        }

        void reset() {
            detector.reset();
            timing.reset();
            morseDecoder.reset();
            conversation.reset();
            text.clear();
            totalSamples = 0;
            lastKeyDown = -1;
            lastKeyUp = -1;
            flushed = false;
            snr = 0;
            wpm = 0;
            confidence = 0;
            squelchHoldoff = 0;
            currentWord.clear();
            currentWordConfSum = 0;
            currentWordCharCount = 0;
            mfRingBuf.clear();
            mfRingIdx = 0;
            mfRingSum = 0;
            mfCurrentW = 0;
            preLockEvents.clear();
            envHistory.clear();
            timingWasLocked = false;
            timingFrozen = false;
            frozenDitEst = 0;
        }

        std::vector<float> getDiagramData(int maxSamples) {
            std::lock_guard<std::mutex> lck(diagMtx);
            if ((int)diagBuf.size() > maxSamples) {
                return std::vector<float>(diagBuf.end() - maxSamples, diagBuf.end());
            }
            return diagBuf;
        }

    private:
        void applyMatchedFilter(const float* in, float* out, int count) {
            int targetW = computeFilterWindow();
            if (targetW != mfCurrentW) {
                mfRingBuf.assign(targetW, 0.0f);
                mfRingIdx = 0;
                mfRingSum = 0;
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

        // Minimum element duration: rejects noise-induced phantom elements.
        // Pre-lock: hard 5ms floor. Post-lock: 30% of dit at good SNR.
        // At low SNR (< 6dB), relax to 15% of dit — edge accuracy is lower
        // and rejecting shortened elements causes more harm than phantom elements.
        float minElementMs() const {
            if (!timing.isLocked()) return 5.0f;
            float factor = (snr > 6.0f) ? 0.3f : 0.15f;
            return std::max(5.0f, timing.getDitDuration() * factor);
        }

        int computeFilterWindow() {
            if (!timing.isLocked()) { return 25; }
            float factor = 0.4f;
            int w = (int)(timing.getDitDuration() / 1000.0f * CW_INTERNAL_RATE * factor);
            return std::max(5, std::min(w, 100));
        }

        // Replay saved key events with locked timing.
        // Uses the SAME detector events that the main path already captured,
        // but re-classifies elements and gaps with the now-known dit duration.
        void retroDecode() {
            if (preLockEvents.empty()) { return; }

            float lockedDit = timing.getDitDuration();

            // Seed retro timing with known dit/dah
            AdaptiveTiming retroTiming;
            retroTiming.init(CW_INTERNAL_RATE);
            for (int i = 0; i < 8; i++) {
                retroTiming.classifyOn(lockedDit);
                retroTiming.classifyOn(lockedDit * 3.0f);
            }

            MorseDecoder retroMorse;
            retroMorse.init();
            TextBuffer retroText;
            float retroLastKeyDown = -1, retroLastKeyUp = -1;

            for (auto& evt : preLockEvents) {
                if (evt.keyDown) {
                    if (retroLastKeyUp >= 0) {
                        float gapMs = evt.timeMs - retroLastKeyUp;
                        auto te = retroTiming.classifyOff(gapMs);
                        if (te.gap == CHAR_GAP || te.gap == WORD_GAP) {
                            char c = retroMorse.characterBreak();
                            if (c) { retroText.append(c, te.confidence); }
                            if (te.gap == WORD_GAP) { retroText.appendSpace(); }
                        }
                    }
                    retroLastKeyDown = evt.timeMs;
                } else {
                    if (retroLastKeyDown >= 0) {
                        float elemMs = evt.timeMs - retroLastKeyDown;
                        float retroMinElem = std::max(5.0f, lockedDit * 0.3f);
                        if (elemMs >= retroMinElem) {
                            auto te = retroTiming.classifyOn(elemMs);
                            retroMorse.addElement(te.element, te.confidence);
                        }
                    }
                    retroLastKeyUp = evt.timeMs;
                }
            }

            // Don't flush the last character — it may be incomplete.
            // Transfer the in-progress morse state to the main decoder
            // so it can be completed by subsequent post-lock events.

            std::string retroStr = retroText.getText();
            if (!retroStr.empty()) {
                text.clear();
                currentWord.clear();
                currentWordConfSum = 0;
                currentWordCharCount = 0;
                // Emit retro text through correction pipeline
                for (char ch : retroStr) {
                    if (ch == ' ') { emitWordGap(); }
                    else { emitChar(ch, 0.5f); }
                }
                // Don't flush last word — it may be continued by post-lock events
            }

            // Transfer retro's morse state so post-lock continues the in-progress char
            morseDecoder = retroMorse;

            // Keep lastKeyDown/lastKeyUp from the pre-lock phase — they're
            // already in global time, so post-lock events compute correct gaps
            preLockEvents.clear();
            envHistory.clear();
        }

        EnvelopeDSP dsp;
        ToneDetector detector;
        AdaptiveTiming timing;
        MorseDecoder morseDecoder;
        float* envBuf = nullptr;
        float* mfBuf = nullptr;

        std::vector<float> mfRingBuf;
        int mfRingIdx = 0;
        float mfRingSum = 0;
        int mfCurrentW = 0;

        struct SavedEvent {
            bool keyDown;
            float timeMs;
        };
        std::vector<SavedEvent> preLockEvents;
        std::vector<float> envHistory;
        bool timingWasLocked = false;

        long long totalSamples = 0;
        int squelchHoldoff = 0;
        std::string currentWord;
        float currentWordConfSum = 0;
        int currentWordCharCount = 0;
        float lastKeyDown = -1;
        float lastKeyUp = -1;
        bool flushed = false;
        bool timingFrozen = false;
        float frozenDitEst = 0;

        std::mutex diagMtx;
        std::vector<float> diagBuf = std::vector<float>(512, 0.0f);
    };

}
