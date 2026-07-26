#pragma once
#include "core.h"
#include "stages.h"
#include "soft_seq_decoder.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
                   bool bpfReeval = false,
                   bool matchedFilter = true,
                   bool fbBpf = false,
                   bool unrealWpmGuard = false,
                   bool contRedecode = false,
                   bool softDecode = false)
            : frontEnd(std::move(fe)), detector(std::move(det)),
              timing(std::move(tim)), symbols(std::move(sym)),
              mfResizePolicy(mfResize), minElementScale(minElemScale),
              adaptiveBpf(adaptiveBpf), bpfGarbageRevert(bpfGarbageRevert),
              bpfReeval(bpfReeval), _mfEnabled(matchedFilter), _fbBpf(fbBpf),
              _unrealWpmGuard(unrealWpmGuard), _contRedecode(contRedecode),
              _softDecode(softDecode) {}

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
            if (_contRedecode) {
                _reCap = (int)(internalRate * 60.0f);
                _reEnv.assign(_reCap, 0.0f);
                // D: build the persistent re-decode chains ONCE. makeFresh()+init()
                // here == per-cycle makeFresh()+init() because AdaptiveTiming::init is
                // "configure variants from strategy (invariant), then reset()", and
                // MorseDecoder's tree is built once and invariant — so a per-cycle
                // reset() reproduces a fresh chain's state exactly (verified byte-
                // identical on the [cont] ladder). The strategy/rate never change, so
                // the invariant config is hoisted out of the 0.5 s hot loop.
                _reTimLive = timing->makeFresh(); _reTimLive->init(internalRate);
                _reTimCont = timing->makeFresh(); _reTimCont->init(internalRate);
                _reSymLive = symbols->makeFresh();
                _reSymCont = symbols->makeFresh();
                _liveSaved.reserve(1 << 16);   // bound: events in a 60 s window (+ noise)
                _reEvents.reserve(1 << 13);
                _reSaved.reserve(1 << 13);
                _reMarks.reserve(1 << 13);
                _reMarkSort.reserve(1 << 13);
                _reKeep.reserve(1 << 13);
                _reTextLive.reserve(1024);
                _reTextCont.reserve(1024);
                _reWinner.reserve(1024);
            }
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

            // §52 step 2: fb SNR-adaptive geometry. Unlike the Schmitt adaptiveBpf
            // (SNR-thresholded, lock-gated, BPF only), the fb detector wants BOTH the
            // pre-detection BPF and the post-detection smoothing narrowed together and
            // graded on the pre-detection input SNR (available before lock, so heavy
            // noise gets its narrow filter and can decode at all). Wide when clean
            // preserves keying edges (clean CER ~0); narrow when buried rejects noise.
            // inputSnr is bimodal (clean ~60dB vs any noise ~6dB, §52 step 2 cal), so
            // it supports one robust decision: clean -> WIDE (preserve keying edges,
            // clean CER ~0), any noise -> NARROW (reject noise). The front end STARTS
            // narrow so heavy noise never gets a wide warmup that floods the detector
            // into a runaway; clean widens the moment inputSnr reads high. Hysteresis
            // on the threshold band avoids hunting on a marginal estimate.
            // Decide ONCE at the fast gate, then commit — re-evaluating every block
            // makes a fading (QSB) signal hunt across the threshold, and per-seed jitter
            // straddles it. The narrow START already protects the [0, gate] window on
            // AWGN, so a single decision at ~0.5s is both stable and timely.
            if (_fbBpf && !_fbDecided && frontEnd->inputSnrReadyFast()) {
                const float in = frontEnd->getInputSnrDb();
                // Narrow ONLY for genuine broadband (AWGN) noise. Fast-gate calibration
                // (§52 step 4): AWGN reads ~6 dB, but hand-keyed/QRN read ~10 dB
                // (jitter/impulses are NOT broadband) — and for those narrowing HURTS
                // (fb+wide ~ legacy) while the detector copes wide. Widen above ~8 dB:
                // AWGN stays narrow (keeps the fast-noise wins), hand-keyed/QRN stay
                // wide (match legacy). QRM reads below AWGN (unfixable here); QSB/
                // Farnsworth are detector limits.
                // Widen for non-broadband signal (§52 step 4 calibration: AWGN reads
                // ~6 dB, hand-keyed/QRN ~10 dB). NOTE: this filter routing is an
                // interim — the real degradation on narrow smoothing is the online
                // debounce shifting mark/gap boundaries (see [fb-durs]); fixing that
                // in FBDetector is the non-workaround path and may remove the need to
                // route the filter at all.
                const float bpf = (in > 8.0f) ? 140.0f : 32.0f;
                if (bpf != _fbCurBpf) {
                    const float sm = 0.625f * bpf;
                    frontEnd->setBandwidth(bpf, bpf);
                    frontEnd->setSmoothing(sm, std::max(sm, 25.0f));
                    _fbCurBpf = bpf;
                }
                _fbDecided = true;
            }

            // DR-4: continuous re-decode buffers the envelope + live events and emits the
            // ratchet winner (re-decode vs live) itself; the normal streaming path is
            // skipped. The detector already ran above (θ updated), so live events + the
            // envelope are all the re-decode needs.
            if (_contRedecode) {
                contPost(mfBuf, envCount, events, totalSamples, sink);
                _wpm = _reWpm;
                return;
            }

            timing->setSnr(_snr);   // §48: SNR-gated regime selection (no-op for other timings)
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
                                if (elemMs < _plMinElem) { _plMinElem = elemMs; }  // flush-gate bimodality
                                if (elemMs > _plMaxElem) { _plMaxElem = elemMs; }
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
                // DR-4b: refuse a non-physical speed. Counted ALWAYS (an implementation
                // smell, not a normal event); refusing keeps the pre-lock classify loop
                // adapting the estimator toward a real dit before any retro-decode commits.
                const bool unrealLock = _unrealWpmGuard && ditUnreal(timing->getDitDuration());
                if (timing->isLocked() && !unrealLock) {
                    if (debugLog) fprintf(stderr, "[CW ch%d] TIMING LOCKED dit=%.1f wpm=%.1f\n", id, timing->getDitDuration(), timing->getWPM());
                    timingWasLocked = true;
                    retroDecode(sink);
                } else if (timing->isLocked() && unrealLock) {
                    _unrealWpmRejections++;
                    if (debugLog) fprintf(stderr, "[CW ch%d] UNREAL WPM refused dit=%.1f wpm=%.1f (n=%lld)\n",
                                          id, timing->getDitDuration(), timing->getWPM(), _unrealWpmRejections);
                } else if (lastKeyUp >= 0 && !detector->isKeyDown() && !preLockEvents.empty()) {
                    float nowMs = (float)totalSamples / _internalRate * 1000.0f;
                    float silenceMs = nowMs - lastKeyUp;
                    // Flush (force-commit the buffered opening) only when the signal has
                    // CLEARLY ended -- NOT on a mid-message word gap during acquisition.
                    // A word gap is ~7*dit; fb detector latency inflates the measured
                    // silence by ~3*dit more (~10*dit total). Firing at 4*dit committed
                    // the opening while the estimator was still SEEDING, so the next word
                    // decoded in seed phase and its leading dah mislabeled (T DE -> T SE).
                    // Waiting past a word gap lets the estimator seed across the early
                    // words and retro-decode the whole opening correctly.
                    // A/B toggle (test/investigation only, static -> read once): OLDFLUSH
                    // forces the pre-fix behaviour — a flat 4*dit silence-flush threshold
                    // regardless of seed state — to isolate the bimodality-gated fix below
                    // in a paired run.
                    static const bool oldFlush = getenv("OLDFLUSH") != nullptr;
                    // Raise the flush threshold only until the estimator has a RELIABLE dit
                    // = it has seen BOTH a dit and a dah (bimodal pre-lock elements). Until
                    // then a word gap (7*dit, ~10*dit with fb latency) would prematurely
                    // commit an opening whose dit is unknown -- a short opening (T DE) or an
                    // all-DAH run (0, OM) -- forcing the rest to decode in seed phase (T SE,
                    // 0->OTT). Once bimodal, retro decodes correctly, so revert to the
                    // original 4*dit and don't churn noisy multi-element acquisition
                    // (moderate CQ is bimodal by its 2nd element).
                    const float dit = timing->getDitDuration();
                    const bool haveReliableDit = _plMaxElem > _plMinElem * 1.8f;
                    const float flushThresh = (oldFlush || haveReliableDit)
                        ? (4.0f * dit)
                        : std::max(12.0f * dit, 900.0f);
                    if (silenceMs > flushThresh && !flushed) {
                        if (unrealLock) {
                            _unrealWpmRejections++;
                        } else {
                            retroDecode(sink);
                            char c = symbols->characterBreak();
                            if (c) { sink.emitChar(c, 0.5f); }
                            sink.flushWord();
                            timingWasLocked = true;
                            flushed = true;
                        }
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
                    // Bound latency only: flush the completed character so it appears
                    // promptly during a long gap. Do NOT decide word-vs-char here —
                    // this fires on PARTIAL silence at a block boundary, before the gap
                    // has ended, so a silenceMs > dit*6 test misreads a stretched
                    // (Farnsworth) char gap (6*dit) as a word gap and double-emits with
                    // the classifyOff below. The complete-gap classification at the next
                    // key-down owns the boundary; true end-of-transmission is handled by
                    // FREEZE (dit*30). flushWord ends the character without a space.
                    char c = symbols->characterBreak();
                    if (c) { sink.emitChar(c, _confidence); }
                    sink.flushWord();
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
            _fbDecided = false; _fbCurBpf = 1e9f;
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
            _plMinElem = 1e9f; _plMaxElem = 0.0f;
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
            _unrealWpmRejections = 0;
            _reFill = 0;
            _lastRedecodeSample = 0;
            _committedConf = -1.0f;
            _liveSaved.clear();
            _reCommittedText.clear();
        }

        CoreStats stats() const override {
            // Cont mode owns emission via reDecodeBuffer, which updates _committedConf
            // (the ratchet winner's mean element confidence) — NOT _confidence, which
            // only the streaming path touches. Report the committed value so the
            // channel/UI/router see a real confidence instead of a stale 0.
            const float conf = (_contRedecode && _committedConf >= 0.0f)
                               ? _committedConf : _confidence;
            return { _snr, _wpm, conf, timingWasLocked, frontEnd->getInputSnrDb(),
                     (int)_unrealWpmRejections };
        }

        const float* diagnostic(int& countOut) const override {
            countOut = diagCount;
            return envBuf;
        }

        void preseed(float level, int count) override { detector->preseed(level, count); }

    private:
        static constexpr int CORE_MAX_ENVELOPE = 65536;
        struct SavedEvent { bool keyDown; float timeMs; };

        void applyMatchedFilter(const float* in, float* out, int count) {
            // The fb detector does its own fixed-lag smoothing; a boxcar in front of
            // it corrupts its emission fit (§52 step 4). Pass through when disabled.
            if (!_mfEnabled) { std::memcpy(out, in, count * sizeof(float)); return; }
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

        struct Candidate { float conf; int n; float wpm; };

        // Decode a key-event stream (times in ms) through a caller-supplied timing+symbol
        // chain, writing the text into `outText`. D: the chain is a PERSISTENT member,
        // reset() here (not makeFresh()) so process() never allocates; mark scratch and
        // outText are reused member buffers (capacity kept across cycles). Live and
        // re-detected streams go through the SAME decoder so the confidence comparison
        // reflects only which DETECTION is cleaner.
        Candidate decodeStream(const std::vector<SavedEvent>& evs,
                               ITiming* tim, ISymbolDecoder* sym, std::string& outText) {
            tim->reset(); sym->reset();
            outText.clear();
            // Seed the estimator (docs §16.4): estimate dit from the mark durations, prime
            // the element model, then centre the gap window from every gap — so char/word
            // spacing classifies against real data, not the 1:3:7 cold defaults.
            {
                _reMarks.clear();
                float down = -1.0f;
                for (auto& ev : evs) {
                    if (ev.keyDown) { down = ev.timeMs; }
                    else if (down >= 0) { _reMarks.push_back(ev.timeMs - down); down = -1.0f; }
                }
                if (_reMarks.size() >= 2) {
                    _reMarkSort = _reMarks; std::sort(_reMarkSort.begin(), _reMarkSort.end());
                    float ditEst = std::max(5.0f, _reMarkSort[_reMarkSort.size() / 4]);   // lower quartile ~ dit
                    // ROBUST_DIT: seed from the fade-robust envelope-autocorr estimate
                    // instead of the fragment-poisoned mark quantile ([dit-cal]).
                    static const bool useRobust = getenv("ROBUST_DIT") != nullptr;
                    if (useRobust && _robustDitMs > 5.0f) { ditEst = _robustDitMs; }
                    for (int i = 0; i < 8; i++) { tim->classifyOn(ditEst); tim->classifyOn(ditEst * 3.0f); }
                }
                float seedUp = -1.0f;
                for (auto& ev : evs) {
                    if (ev.keyDown) { if (seedUp >= 0) { tim->classifyOff(ev.timeMs - seedUp); } }
                    else { seedUp = ev.timeMs; }
                }
            }
            float confSum = 0.0f; int confN = 0;
            float lastDown = -1.0f, lastUp = -1.0f;
            for (auto& ev : evs) {
                const float t = ev.timeMs;
                if (ev.keyDown) {
                    if (lastUp >= 0) {
                        auto te = tim->classifyOff(t - lastUp);
                        if (te.gap == CHAR_GAP || te.gap == WORD_GAP) {
                            char c = sym->characterBreak();
                            if (c) { outText += c; }
                            if (te.gap == WORD_GAP) { outText += ' '; }
                        }
                    }
                    lastDown = t;
                } else {
                    if (lastDown >= 0) {
                        const float elem = t - lastDown;
                        const float minEl = std::max(5.0f, tim->getDitDuration() * 0.3f);
                        if (elem >= minEl) {
                            auto te = tim->classifyOn(elem);
                            sym->addElement(te.element, te.confidence);
                            confSum += te.confidence; confN++;
                        }
                    }
                    lastUp = t;
                }
            }
            char c = sym->characterBreak();
            if (c) { outText += c; }
            return { confN > 0 ? confSum / (float)confN : 0.0f, confN, tim->getWPM() };
        }

        // DR-4 post-step: buffer the detection-input envelope + live events, then
        // re-decode periodically and emit the ratchet winner. Owns totalSamples (the
        // normal streaming path is skipped in continuous mode).
        void contPost(const float* env, int count, std::vector<KeyEvent>& events,
                      long long blockStart, CharSink& sink) {
            for (auto& ev : events) {
                const float ms = (float)(blockStart + ev.sampleOffset) / _internalRate * 1000.0f;
                _liveSaved.push_back({ ev.keyDown, ms });
            }
            // Sliding window: when the buffer would overflow, COMMIT the current decode
            // to the frozen prefix and slide to a fresh window (θ persists in the
            // detector — no re-acquisition). This handles messages longer than the
            // window and cuts only at a window boundary, never dropping the tail.
            if (_reFill + count > _reCap) {
                float c, w;
                _reCommittedText += winnerText(c, w);
                _reFill = 0;
                _liveSaved.clear();
            }
            std::memcpy(_reEnv.data() + _reFill, env, count * sizeof(float));
            _reFill += count;
            totalSamples += count;

            const long long period = (long long)(_internalRate * 0.5f);
            if (detector->paramsReady() &&
                (totalSamples - _lastRedecodeSample) >= period) {
                reDecodeBuffer(sink);
                _lastRedecodeSample = totalSamples;
            }
        }

        static float envF(const char* key, float def) {
            const char* e = getenv(key); return e ? (float)atof(e) : def;
        }

        // Robust, event-INDEPENDENT dit estimate from the envelope autocorrelation:
        // dit = K * (first zero-crossing lag). Calibrated K~0.41, stable across
        // speed / keying-style / noise ([dit-cal], stderr ~0.00-0.02). Fade-robust
        // (integrates over the window), so it does NOT collapse on the fragmented
        // short marks that poison a duration-quantile seed. Research direction: the
        // "robust speed estimate" for soft sequence decoding (docs §54, CW Skimmer).
        // Subsampled by 2 to bound cost. Returns 0 if no crossing (unusable).
        float robustDitMs(const float* env, int n) const {
            const int step = 2, m = n / step;
            if (m < 300) { return 0.0f; }
            double mean = 0; for (int i = 0; i < n; i += step) { mean += env[i]; }
            mean /= m;
            double r0 = 0; for (int i = 0; i < n; i += step) { double d = env[i]-mean; r0 += d*d; }
            if (r0 < 1e-12) { return 0.0f; }
            const int maxLag = std::min(m/2, (int)(0.6f * _internalRate / step));
            const float dtMs = 1000.0f / _internalRate * step;
            float prev = 1.0f;
            for (int lag = 1; lag < maxLag; lag++) {
                double s = 0; const int hi = n - lag*step;
                for (int i = 0; i < hi; i += step) { s += (double)(env[i]-mean)*(env[i+lag*step]-mean); }
                float r = (float)(s / r0);
                if (prev >= 0 && r < 0) {
                    float frac = prev / (prev - r);
                    return (lag - 1 + frac) * dtMs * ROBUST_DIT_K;
                }
                prev = r;
            }
            return 0.0f;
        }
        static constexpr float ROBUST_DIT_K = 0.41f;   // [dit-cal] calibrated

        // Mean envelope over [a,b).
        float meanEnv(int a, int b) const {
            if (b <= a) { return 0.0f; }
            double s = 0.0;
            for (int k = a; k < b; k++) { s += (double)_reEnv[k]; }
            return (float)(s / (double)(b - a));
        }
        // Fill-ratio of a gap RELATIVE TO ITS BRACKETING MARKS (fade-robust, local).
        // fill = (gapLevel - floor) / (localMark - floor), localMark = mean of the two
        // adjacent marks. ~0 => the gap dropped to the floor between two live marks (a
        // real key-up); ~1 => the gap stayed at the marks' own (faded) level (a notch
        // inside one element). A GLOBAL mark level would overestimate faded sections
        // and wash the discrimination out — the neighbours are the right reference.
        float gapFillLocal(int prevMarkA, int gA, int gB, int nextMarkB, float floor) const {
            const float gap  = meanEnv(gA, gB) - floor;
            const float mk   = 0.5f * ((meanEnv(prevMarkA, gA) - floor) + (meanEnv(gB, nextMarkB) - floor));
            if (mk <= 1e-9f) { return 0.0f; }
            return gap / mk;
        }

        // Option A — matched-filter element RE-GLUING (docs matched-filter-element-
        // integrity.md §3). Merge mark-gap-mark in the re-detected stream when the gap
        // is (1) sub-dit AND (2) never returned to the noise floor (high fill = a fade
        // notch inside a mark, not a real key-up). Condition 2 is the new physics: a
        // real gap reaches the floor (fill ~ 0) and is NEVER merged, so clean/handkeyed
        // stay byte-identical by construction; only weak-signal fade splits are glued.
        // Dit-relative window adapts to speed. DEFAULT OFF (MF_REGLUE unset) -> no-op.
        void applyReGlue() {
            static const bool  enable  = getenv("MF_REGLUE") != nullptr;
            if (!enable || _reEvents.size() < 4) { return; }
            static const float fillMin = envF("MF_FILL", 0.25f);
            static const float ditFrac = envF("MF_DITFRAC", 2.0f);

            const float floor = detector->noiseLevel();

            _reMarks.clear();
            for (size_t i = 1; i < _reEvents.size(); i++) {
                if (_reEvents[i-1].keyDown && !_reEvents[i].keyDown) {
                    _reMarks.push_back((float)(_reEvents[i].sampleOffset - _reEvents[i-1].sampleOffset));
                }
            }
            if (_reMarks.size() < 2) { return; }
            _reMarkSort = _reMarks; std::sort(_reMarkSort.begin(), _reMarkSort.end());
            const float ditSamp = std::max(5.0f, _reMarkSort[_reMarkSort.size() / 4]);
            const float maxGap  = ditFrac * ditSamp;

            _reKeep.assign(_reEvents.size(), 1);
            for (size_t i = 1; i + 2 < _reEvents.size(); i++) {
                // gap = up(i)..down(i+1), bracketed by mark down(i-1)..up(i) and
                // mark down(i+1)..up(i+2). Need both neighbours for the local ref.
                if (_reEvents[i].keyDown || !_reEvents[i+1].keyDown) { continue; }
                if (!_reEvents[i-1].keyDown || !_reEvents[i+2].keyDown) { continue; }
                const int gA = _reEvents[i].sampleOffset, gB = _reEvents[i+1].sampleOffset;
                const int pA = _reEvents[i-1].sampleOffset, nB = _reEvents[i+2].sampleOffset;
                if ((float)(gB - gA) >= maxGap) { continue; }        // real gap: never merge
                if (gapFillLocal(pA, gA, gB, nB, floor) <= fillMin) { continue; } // dropped to floor: real key-up
                _reKeep[i] = 0; _reKeep[i+1] = 0;                    // fade notch: glue the marks
            }
            size_t w = 0;
            for (size_t i = 0; i < _reEvents.size(); i++) {
                if (_reKeep[i]) { _reEvents[w++] = _reEvents[i]; }
            }
            static const bool dbg = getenv("MF_DBG") != nullptr;
            if (dbg) { fprintf(stderr, "[REGLUE] ev=%zu->%zu ditSamp=%.0f maxGap=%.0f floor=%.5f\n",
                               _reEvents.size(), w, ditSamp, maxGap, floor); }
            _reEvents.resize(w);
        }

        // Decide the winning decode of the CURRENT window: the re-detected stream vs the
        // live event stream, scored comparably (both via decodeStream), with a margin so
        // the re-decode only overrides when CLEARLY better — keeps Farnsworth / heavy
        // noise at the live decode while holding the stationary wins. Returns its text.
        const std::string& winnerText(float& outConf, float& outWpm) {
            detector->reDetect(_reEnv.data(), _reFill, _reEvents);
            applyReGlue();                       // Option A: matched-filter element re-gluing
            _robustDitMs = robustDitMs(_reEnv.data(), _reFill);   // envelope-autocorr speed
            _reSaved.clear();
            for (auto& ev : _reEvents) {
                _reSaved.push_back({ ev.keyDown, (float)ev.sampleOffset / _internalRate * 1000.0f });
            }
            const Candidate live = decodeStream(_liveSaved, _reTimLive.get(), _reSymLive.get(), _reTextLive);
            const Candidate cont = decodeStream(_reSaved,   _reTimCont.get(), _reSymCont.get(), _reTextCont);
            // §7b: replace the cont candidate's TEXT with the soft-segmentation decode
            // (same events + the same dit estimate); the ratchet still uses the hard
            // cont confidence/count, but a winning cont emits the un-fragmented text.
            if (_softDecode) {
                _softEvents.clear();
                for (auto& e : _reSaved) { _softEvents.push_back({e.keyDown, e.timeMs}); }
                _reTextCont = _soft.decode(_softEvents, _reTimCont->getDitDuration());
            }
            // Override live only when the re-decode is CLEARLY more confident (margin) —
            // at the noise floor both score ~equal on garbage, so a hair-thin lead is a
            // coin flip; the margin keeps live there while real wins (cont.conf ≫ live.conf)
            // are unaffected.
            constexpr float MARGIN = 0.04f;
            const bool useCont =
                (cont.n >= 3 && cont.conf > live.conf + MARGIN && cont.n >= (int)(0.6f * live.n));
            const Candidate& best = useCont ? cont : live;
            outConf = best.conf; outWpm = best.wpm;
            if (best.n < 3) { _reWinner.clear(); return _reWinner; }
            _reWinner = useCont ? _reTextCont : _reTextLive;
            return _reWinner;
        }

        void reDecodeBuffer(CharSink& sink) {
            float conf, wpm;
            const std::string& win = winnerText(conf, wpm);
            if (win.empty() && _reCommittedText.empty()) { return; }
            _committedConf = conf; _reWpm = wpm;
            sink.clearEmitted();
            for (char ch : _reCommittedText) {
                if (ch == ' ') { sink.emitWordGap(); } else { sink.emitChar(ch, conf); }
            }
            for (char ch : win) {
                if (ch == ' ') { sink.emitWordGap(); } else { sink.emitChar(ch, conf); }
            }
        }

        // A+B (all-dah acquisition fix): joint element+gap batch estimate of dit over the
        // buffered acquisition window, with a confidence. Elements cluster at {1,3}*dit,
        // gaps at {1,3,7}*dit; fitting a dit-multiple "comb" to BOTH breaks the harmonic
        // ambiguity a mark-only seed cannot -- the element gaps (~1 dit) anchor dit even
        // when every mark is a DAH (openings O/MM/0). Confidence = fit quality * evidence.
        void computeJointDit(const std::vector<SavedEvent>& events,
                             float& outDit, float& outConf) const {
            float lastDown = -1, lastUp = -1;
            float el[64]; int nEl = 0;
            float gp[64]; int nGp = 0;
            for (auto& e : events) {
                if (e.keyDown) {
                    if (lastUp >= 0 && nGp < 64) { gp[nGp++] = e.timeMs - lastUp; }
                    lastDown = e.timeMs;
                } else {
                    if (lastDown >= 0 && nEl < 64) { el[nEl++] = e.timeMs - lastDown; }
                    lastUp = e.timeMs;
                }
            }
            outDit = 0; outConf = 0;
            if (nEl < 2) { return; }
            const float EM[2] = { 1.0f, 3.0f };
            const float GM[3] = { 1.0f, 3.0f, 7.0f };
            float bestD = 0, bestErr = 1e30f;
            for (float D = 15.0f; D <= 240.0f; D += 1.0f) {
                float err = 0; int n = 0;
                for (int i = 0; i < nEl; i++) {
                    float be = 1e30f;
                    for (float m : EM) { float r = (el[i] - m*D)/(m*D); r*=r; if (r < be) be = r; }
                    err += be; n++;
                }
                for (int i = 0; i < nGp; i++) {
                    float bg = 1e30f;
                    for (float m : GM) { float r = (gp[i] - m*D)/(m*D); r*=r; if (r < bg) bg = r; }
                    err += bg; n++;
                }
                err /= (float)std::max(1, n);
                if (err < bestErr) { bestErr = err; bestD = D; }
            }
            outDit = bestD;
            const float fit = expf(-bestErr / (0.10f * 0.10f));   // ~10% rms jitter -> ~0.37
            const float eviN = (float)(nEl + nGp);
            outConf = fit * (eviN / (eviN + 6.0f));
        }

        void retroDecode(CharSink& sink) {
            if (preLockEvents.empty()) { return; }

            // A+B: prefer the joint gap-anchored dit over the streaming estimate, but only
            // when it is CONFIDENT (B) and MATERIALLY disagrees with the streaming dit --
            // i.e. the streaming seed is wrong (all-dah 3x error). When they agree, keep
            // the streaming dit so normal signals are byte-identical (no ladder regression).
            float lockedDit = timing->getDitDuration();
            float jointDit, jointConf;
            computeJointDit(preLockEvents, jointDit, jointConf);
            if (jointConf >= JOINT_CONF_MIN && jointDit > 1.0f && lockedDit > 1.0f) {
                const float ratio = jointDit / lockedDit;
                if (ratio < 0.6f || ratio > 1.6f) { lockedDit = jointDit; }
            }
            if (debugLog) fprintf(stderr, "[CW ch%d] RETRO events=%d dit=%.1f (stream=%.1f joint=%.1f conf=%.2f)\n",
                                  id, (int)preLockEvents.size(), lockedDit,
                                  timing->getDitDuration(), jointDit, jointConf);

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
                        if (debugLog) fprintf(stderr, "[CW ch%d]   gap=%.0fms -> %s (conf=%.2f)\n", id, gapMs,
                            te.gap==ELEMENT_GAP?"ELEM":te.gap==CHAR_GAP?"CHAR":"WORD", te.confidence);
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
                            if (debugLog) fprintf(stderr, "[CW ch%d] mark=%.0fms -> %s (conf=%.2f dit=%.0f)\n", id, elemMs,
                                te.element==DIT?"DIT":"DAH", te.confidence, retroTiming->getDitDuration());
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
        bool _mfEnabled = true;   // §52: fb core disables the boxcar matched filter
        bool _fbBpf = false;      // §52 step 2: SNR-adaptive BPF+smoothing for fb
        // DR-4b: refuse to accept a timing lock whose speed is non-physical.
        // 5-60 WPM => dit 240-20 ms (dit_ms = 1200/WPM, PARIS). Off by default so
        // every existing core is byte-identical.
        bool _unrealWpmGuard = false;
        long long _unrealWpmRejections = 0;
        static constexpr float DIT_MIN_MS = 20.0f;   // 60 WPM
        static constexpr float DIT_MAX_MS = 240.0f;  //  5 WPM
        static constexpr float JOINT_CONF_MIN = 0.5f;  // A+B: min joint-fit confidence to override dit
        bool ditUnreal(float ditMs) const { return ditMs < DIT_MIN_MS || ditMs > DIT_MAX_MS; }

        // DR-4: continuous confidence-ratcheted re-decode. Buffers the detection-input
        // envelope; while the buffer holds the whole message-so-far, periodically
        // RE-DETECTS it under the detector's current (maturing) θ, re-decodes with a
        // fresh timing+symbol chain, and COMMITS the result only when its confidence
        // strictly beats what is already shown. The first character is recovered as a
        // natural consequence once θ is good (re-detection un-merges the cold-start
        // blob); the ratchet makes it regression-safe (never commits a worse decode).
        // Off by default so every existing core is byte-identical.
        bool _contRedecode = false;
        bool _softDecode = false;                   // §7b: soft-segmentation cont decode
        SoftSeqDecoder _soft;
        std::vector<std::pair<bool, float>> _softEvents;   // (keyDown, ms) scratch
        std::vector<float> _reEnv;          // linear envelope history (detection input)
        int _reFill = 0;
        int _reCap = 60000;                 // 60 s window at 1 kHz; slides on overflow
        long long _lastRedecodeSample = 0;
        float _committedConf = -1.0f;       // ratchet: best committed mean element conf
        float _reWpm = 0.0f;
        std::string _reCommittedText;       // DR-4: text from windows that have slid out

        // D (RT-safety): pre-allocated scratch for the per-0.5 s re-decode so
        // process() never allocates ([[rt-thread-no-alloc-validate-live]]). Two
        // PERSISTENT decode chains (live + re-detected) built once and reset() per
        // cycle instead of makeFresh(); reusable event/mark/text buffers cleared
        // (capacity kept) per cycle. All reserved in init() under _contRedecode.
        std::unique_ptr<ITiming> _reTimLive, _reTimCont;
        std::unique_ptr<ISymbolDecoder> _reSymLive, _reSymCont;
        std::vector<KeyEvent> _reEvents;     // reDetect output
        std::vector<SavedEvent> _reSaved;    // reDetect events as SavedEvent (ms)
        std::vector<float> _reMarks, _reMarkSort;   // decodeStream mark scratch
        std::vector<char>  _reKeep;                 // applyReGlue keep-mask scratch
        float _robustDitMs = 0.0f;                  // envelope-autocorr dit (this window)
        std::string _reTextLive, _reTextCont;       // per-candidate decode text
        std::string _reWinner;                      // winnerText return buffer
        bool _fbDecided = false;  // fb geometry committed (decide-once, no hunting)
        float _fbCurBpf = 1e9f;   // last applied fb bpf cutoff (init "unset")
        bool bpfGaveUp = false;
        long long narrowStartSample = 0;
        long long keyDownsAtNarrow = 0;
        long long bpfKeyDowns = 0;
        float wpmAtNarrow = 0.0f;
        float currentCut = 100.0f;
        float _snrSmooth = 0.0f;
        bool snrSmoothStarted = false;
        long long lastEvalSample = 0;

        std::vector<SavedEvent> preLockEvents;
        float _plMinElem = 1e9f, _plMaxElem = 0.0f;  // pre-lock element min/max for flush bimodality gate
        std::vector<SavedEvent> _liveSaved;  // DR-4: live key events for live-vs-re-decode

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
