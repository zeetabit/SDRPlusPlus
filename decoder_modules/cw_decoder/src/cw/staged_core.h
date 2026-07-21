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
                   MatchedFilterResize mfResize = MF_RESET,
                   float minElemScale = 1.0f,
                   bool adaptiveBpf = false,
                   bool bpfGarbageRevert = false,
                   bool bpfReeval = false)
            : frontEnd(std::move(fe)), detector(std::move(det)),
              timing(std::move(tim)), symbols(std::move(sym)),
              mfResizePolicy(mfResize), minElementScale(minElemScale),
              adaptiveBpf(adaptiveBpf), bpfGarbageRevert(bpfGarbageRevert),
              bpfReeval(bpfReeval) {}

        // Runtime noise-aware BPF geometry (docs §29–33), from the locked WPM
        // (dit, ms) and the INPUT-referred SNR (dB, pre-BPF; frontEnd
        // getInputSnrDb). Matched-to-WPM narrowing (ENBW ≈ 2/T_dit, via the §4.1
        // cut↔ENBW fit) is a floor reached only at low SNR; at high SNR the
        // geometry widens to legacy (100, 100) so a jitter-limited signal is not
        // over-narrowed and smeared (§29).
        //
        // snrDb is the pre-BPF inputSnr, NOT the detector's post-BPF getSNR: the
        // latter reads high on real audio because the wide filter already removed
        // the noise, so it never fired the narrowing on real signals (§32). The
        // pre-BPF estimate transfers across signal type — it reads ~6 dB at
        // noiseAmp ≥ 1 on both synthetic and real audio, ~9+ for light noise, ~23
        // clean (§33 table). Thresholds: wide above 9 dB (keeps synthetic
        // hand-keyed, inputSnr 9.2, wide), matched below 6.5.
        static std::pair<float, float> bpfGeom(float ditMs, float inputSnrDb, float postBpfSnrDb) {
            const float matched = std::min(std::max((2000.0f / ditMs + 2.0f) / 1.65f, 20.0f), 40.0f);
            // Narrow only in a mid-SNR band [2.8, 9] dB post-BPF getSNR (§33):
            //  - Above 9: either clean, or a high-getSNR/low-inputSnr split that
            //    means out-of-band interference (QRM) the wide BPF already rejects
            //    — narrowing buys nothing and only adds a retune transient. Also
            //    keeps light-noise hand-keyed (getSNR ~11) wide, resolving the §29
            //    over-narrowing concern directly.
            //  - Below 2.8: the signal is too deep in noise to recover, and a
            //    narrow filter there makes noise LOOK like signal — it emits
            //    garbage rather than going silent (§4/§32 floor), a regression on
            //    an already-failed decode. Stay wide.
            if (postBpfSnrDb > 9.0f || postBpfSnrDb < 3.5f) { return {100.0f, 100.0f}; }
            // Only narrow when the input-referred estimate also indicates
            // broadband noise (transfers across signal type; a high inputSnr means
            // clean/light and the narrow filter would only smear).
            if (inputSnrDb > 8.0f) { return {100.0f, 100.0f}; }
            // GRADED narrowing on the post-BPF getSNR (§36). inputSnr saturates at
            // ~6 dB once the signal is buried, so keying narrowFrac off it makes the
            // rule near-binary: moderate noise gets the same aggressive 20 Hz as
            // heavy noise, which over-narrows a still-readable signal (moderate-CER
            // regression). getSNR keeps resolution here (5.4 at noiseAmp 1.5, 4.5 at
            // 2.0, 3.7 at 3.0), so grading on it narrows gently at the margin — where
            // a mis-estimate costs little — and hard only when noise is unambiguous.
            constexpr float SNR_HI = 5.5f, SNR_LO = 3.7f;
            const float nf = std::min(std::max((SNR_HI - postBpfSnrDb) / (SNR_HI - SNR_LO), 0.0f), 1.0f);
            const float cut   = 100.0f * (1.0f - nf) + matched * nf;
            const float trans = 100.0f * (1.0f - nf) + (matched + 10.0f) * nf;
            return {cut, trans};
        }

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
            // B: smoothed getSNR, started only AFTER the estimate has converged.
            // The §36 EMA failed because it averaged from t=0 through the
            // acquisition transient; gating the start on inputSnrReady keeps it
            // clean, so the re-evaluated decision reflects sustained conditions
            // (QSB, drift) rather than a single fade phase.
            if (bpfReeval && frontEnd->inputSnrReady()) {
                if (!snrSmoothStarted) { _snrSmooth = _snr; snrSmoothStarted = true; }
                else {
                    const float a = 1.0f - expf(-(float)envCount / (2.0f * _internalRate));
                    _snrSmooth += a * (_snr - _snrSmooth);
                }
            }
            float sqFactor = std::clamp((_snr - 3.0f) / 7.0f, 0.0f, 1.0f);

            for (auto& evt : events) {
                // A: count key-downs for the garbage-rate detector. Counted before
                // the squelch gate so a narrow filter's spurious flood is seen even
                // when each event is low-confidence.
                if (evt.keyDown) { bpfKeyDowns++; }
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
                                // evt.confidence is the detector's element
                                // plausibility (1.0 for all cores except soft
                                // LR): a marginally-detected element down-weights
                                // the beam commitment, widening the search.
                                const float elemConf = te.confidence * sqFactor * evt.confidence;
                                symbols->addElement(te.element, elemConf);
                                _confidence = elemConf;
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

            // WPM-locked, noise-aware BPF (docs §30/§33). Fires once, when timing
            // has locked (WPM known) AND the input-SNR estimate has converged. At
            // the lock instant the wide noise window is still filling and reads a
            // huge transient (§33), so gating on lock alone never narrows on real
            // audio — the readiness gate is what makes the trigger fire.
            //
            // Firing earlier (at inputSnr-ready, before lock) to close the
            // acquisition gap was tried and refuted (§34): the getSNR garbage-floor
            // guard already blocks narrowing at the heavy-noise SNRs where the gap
            // matters (noiseAmp 2.0, getSNR 2.3 < 3.5), so earlier narrowing has
            // nothing to act on there and only perturbs the lighter-noise decodes.
            if (adaptiveBpf && timingWasLocked && frontEnd->inputSnrReady() && !bpfGaveUp) {
                const bool firstEval = !bpfRetuned;
                bool doEval = firstEval;
                // B: re-evaluate periodically so the bandwidth tracks changing
                // conditions (QSB fades, drift) and a bad one-shot call can
                // self-correct, instead of committing forever at lock (§37).
                if (bpfReeval && bpfRetuned) {
                    const float since = (float)(totalSamples - lastEvalSample) / _internalRate;
                    if (since >= 3.0f) { doEval = true; }
                }
                if (doEval) {
                    const float snrDec = (bpfReeval && snrSmoothStarted) ? _snrSmooth : _snr;
                    auto g = bpfGeom(timing->getDitDuration(), frontEnd->getInputSnrDb(), snrDec);
                    // Apply on the first eval, or when the target moves enough to be
                    // worth the retune transient (hysteresis) — avoids hunting.
                    if (firstEval || std::fabs(g.first - currentCut) > 15.0f) {
                        if (g.first < 99.0f || currentCut < 99.0f) {
                            frontEnd->setBandwidth(g.first, g.second);
                            currentCut = g.first;
                            bpfNarrowed = (g.first < 99.0f);
                            if (bpfNarrowed) {
                                narrowStartSample = totalSamples;
                                keyDownsAtNarrow = bpfKeyDowns;
                                // Capture the WPM at narrow time — BEFORE any
                                // garbage flood inflates it — as the stable
                                // baseline for the rate check (§37).
                                wpmAtNarrow = std::max(timing->getWPM(), 5.0f);
                            }
                            if (debugLog) fprintf(stderr, "[CW ch%d] BPF RETUNE cut=%.1f snr=%.1f\n", id, g.first, snrDec);
                        }
                    }
                    bpfRetuned = true;
                    lastEvalSample = totalSamples;
                }
            }

            // A: garbage detector. A narrow filter ringing on a signal too dead to
            // recover emits far more key events than any real signal at the locked
            // WPM. If the post-narrow event rate exceeds a multiple of the expected
            // element rate, the narrowing backfired — revert to wide. Without B this
            // is a one-shot give-up; with B, just widen and let the next re-eval
            // decide again once conditions are re-read (§37).
            if (adaptiveBpf && bpfGarbageRevert && bpfNarrowed) {
                const float elapsed = (float)(totalSamples - narrowStartSample) / _internalRate;
                if (elapsed > 1.5f) {
                    const float rate = (float)(bpfKeyDowns - keyDownsAtNarrow) / elapsed;
                    // Expected element rate from the WPM at narrow time (~WPM/6,
                    // PARIS). Narrowing that HELPS produces fewer events (cleaner);
                    // only a backfiring narrow floods above the baseline, so 2x is
                    // a wide margin that clears real heavy-noise decodes (§37).
                    const float expected = wpmAtNarrow / 6.0f;
                    if (rate > 2.0f * expected) {
                        frontEnd->setBandwidth(100.0f, 100.0f);
                        currentCut = 100.0f;
                        bpfNarrowed = false;
                        if (!bpfReeval) { bpfGaveUp = true; }
                        if (debugLog) fprintf(stderr, "[CW ch%d] BPF REVERT rate=%.1f exp=%.1f\n", id, rate, expected);
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
            bpfRetuned = false;
            bpfNarrowed = false;
            bpfGaveUp = false;
            narrowStartSample = 0;
            keyDownsAtNarrow = 0;
            bpfKeyDowns = 0;
            wpmAtNarrow = 0.0f;
            currentCut = 100.0f;
            _snrSmooth = 0;
            snrSmoothStarted = false;
            lastEvalSample = 0;
        }

        CoreStats stats() const override {
            return { _snr, _wpm, _confidence, timingWasLocked, frontEnd->getInputSnrDb() };
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
            // The filter rejects short elements to drop noise spikes, but on
            // hand-keyed signals real short dits are exactly what it rejects, so
            // it also biases the dit estimate up (§17.3.4). minElementScale
            // relaxes it: 0 leaves only the 5 ms floor, 1.0 is the historical
            // 0.3/0.15 factor. The LR detector already rejects spikes by
            // duration, so under it the filter may be redundant (§24).
            float factor = (_snr > 6.0f) ? 0.3f : 0.15f;
            return std::max(5.0f, timing->getDitDuration() * factor * minElementScale);
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
        float minElementScale = 1.0f;
        bool adaptiveBpf = false;
        bool bpfRetuned = false;
        // A (garbage-revert) and B (re-eval) state, docs §37.
        bool bpfGarbageRevert = false;
        bool bpfReeval = false;
        bool bpfNarrowed = false;
        bool bpfGaveUp = false;
        long long narrowStartSample = 0;
        long long keyDownsAtNarrow = 0;
        long long bpfKeyDowns = 0;
        float wpmAtNarrow = 0.0f;
        float currentCut = 100.0f;
        float _snrSmooth = 0.0f;
        bool snrSmoothStarted = false;
        long long lastEvalSample = 0;

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
