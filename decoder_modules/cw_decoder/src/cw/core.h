#pragma once
#include <dsp/types.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "timing.h"

namespace cw {

    // ════════════════════════════════════════════════════════════
    // Pluggable decoding architecture.
    //
    // Two levels, because two kinds of decoder must coexist:
    //
    //   IDecodeCore  — IQ in, characters out. The outer contract.
    //                  Jointly-estimating decoders (Bell 1977 trellis, HMM/EM)
    //                  implement this DIRECTLY: their detection, timing and
    //                  symbol decoding are one inseparable computation and
    //                  cannot be expressed as a pipeline of stages.
    //
    //   StagedCore   — one implementation of IDecodeCore that composes
    //                  independently swappable stages (front end, detector,
    //                  timing, symbol decoder). The current decoder is a
    //                  configuration of this. Mixing stages happens here.
    //
    // Both appear in the same registry, so the module config UI and the
    // benchmark matrix enumerate cores identically and neither needs to know
    // which kind it is holding.
    //
    // Post-processing (corrector, conversation, TextBuffer) is owned by Channel
    // and is identical for every core — a core never touches decoded text
    // directly, only the CharSink. That keeps the benchmark measuring decoding.
    // ════════════════════════════════════════════════════════════

    struct CoreStats {
        float snr        = 0;
        float wpm        = 0;
        float confidence = 0;
        bool  locked     = false;
        float inputSnr   = 0;   // pre-BPF, input-referred SNR (docs §32/§33)
        // DR-4b: count of timing locks refused because the estimated speed was
        // non-physical (outside ~5-60 WPM). Tracked ALWAYS. A raw estimator that
        // produces an unreal speed is an implementation smell — tests/benchmarks
        // assert this is 0 so the backstop never launders a bug (docs §14).
        int   unrealWpmRejections = 0;
    };

    // Where decoded characters go.
    class CharSink {
    public:
        virtual ~CharSink() = default;
        virtual void emitChar(char c, float confidence) = 0;
        // Word boundary reached: run spelling correction on the pending word,
        // but do NOT append a space. emitWordGap() = flushWord() + space.
        virtual void flushWord() = 0;
        virtual void emitWordGap() = 0;
        // Discard everything emitted so far — used when a core retro-decodes
        // and replaces provisional output with a better-timed reconstruction.
        virtual void clearEmitted() = 0;
    };

    // ── Outer contract ──────────────────────────────────────────

    class IDecodeCore {
    public:
        virtual ~IDecodeCore() = default;

        virtual void init(float sampleRate, float internalRate, float toneFreq) = 0;
        virtual void setToneFreq(float freq) = 0;
        virtual void reset() = 0;

        virtual void process(int count, const dsp::complex_t* iq, CharSink& sink) = 0;

        virtual CoreStats stats() const = 0;

        // Envelope for the UI waveform display. Empty = nothing to show.
        virtual const float* diagnostic(int& countOut) const { countOut = 0; return nullptr; }

        // Optional: pre-charge noise estimation. No-op for cores without one.
        virtual void preseed(float level, int count) { (void)level; (void)count; }
    };

    // ── Swappable stages (used by StagedCore only) ──────────────

    struct KeyEvent;   // from tone_detector.h

    // Stage 1: IQ → envelope at the internal rate.
    class IFrontEnd {
    public:
        virtual ~IFrontEnd() = default;
        virtual void init(float toneFreq, float sampleRate, float internalRate) = 0;
        virtual void setToneFreq(float freq) = 0;
        // Returns the number of envelope samples written to `out`.
        virtual int process(int count, const dsp::complex_t* in, float* out) = 0;
        virtual const char* name() const = 0;
        // Optional: retune pre-detection bandwidth at runtime (docs §30). No-op
        // for front ends without a settable filter, so a fixed-geometry core is
        // unaffected.
        virtual void setBandwidth(float cutoff, float trans) { (void)cutoff; (void)trans; }
        // Post-detection envelope-smoothing cutoff (docs §52 step 2). Default no-op.
        virtual void setSmoothing(float cutoff, float trans) { (void)cutoff; (void)trans; }
        // Optional: input-referred SNR (dB), measured before the narrow filter
        // (docs §32/§33) — the trigger for adaptive bandwidth. Default 99 (very
        // high ⇒ never narrow) for front ends without the estimator.
        virtual float getInputSnrDb() const { return 99.0f; }
        // Whether getInputSnrDb has converged (its noise window has filled).
        // Default true so cores without the estimator are unaffected.
        virtual bool inputSnrReady() const { return true; }
        // Provisional (~0.5s) readiness for the fb adaptive filter (docs §52 step 2).
        virtual bool inputSnrReadyFast() const { return inputSnrReady(); }
    };

    // Stage 2: envelope → key up/down events.
    class IDetector {
    public:
        virtual ~IDetector() = default;
        virtual void init(float internalRate) = 0;
        virtual void reset() = 0;
        virtual std::vector<KeyEvent> process(const float* envelope, int count) = 0;
        virtual float getSNR() const = 0;
        virtual bool isKeyDown() const = 0;
        virtual const char* name() const = 0;
        // Pre-charge the noise estimator so detection can start immediately.
        virtual void preseed(float level, int count) { (void)level; (void)count; }
        // DR-4: re-detect a buffered envelope window under the detector's CURRENT
        // parameters, without touching live state — the basis of the continuous
        // re-decode. Returns events with buffer-relative offsets. Detectors that do
        // not support re-detection return empty (the core then skips re-decode).
        virtual std::vector<KeyEvent> reDetect(const float* env, int count) {
            (void)env; (void)count; return {};
        }
        // Whether the detector's parameter model is calibrated enough that a
        // re-decode is worth trusting (both element classes observed).
        virtual bool paramsReady() const { return true; }
    };

    // Stage 3: durations → DIT/DAH and gap classes.
    // AdaptiveTiming already implements four strategies behind one type; this
    // interface exists so a log-domain or discrete-speed model can replace it.
    class ITiming {
    public:
        virtual ~ITiming() = default;
        virtual void init(float internalRate) = 0;
        virtual void reset() = 0;
        virtual TimingEvent classifyOn(float durationMs) = 0;
        virtual TimingEvent classifyOff(float durationMs) = 0;
        virtual float getDitDuration() const = 0;
        virtual float getWPM() const = 0;
        virtual bool isLocked() const = 0;
        virtual const char* name() const = 0;
        // A fresh instance of the same implementation and configuration.
        // Retro-decoding replays saved events through a second, seeded model.
        virtual std::unique_ptr<ITiming> makeFresh() const = 0;

        // Opt-in, defaulted to a no-op: a timing stage that keeps no learned gap
        // model has nothing to relax, and should not be forced to describe this.
        // Retro replay sets it because it holds the complete pre-lock gap set and
        // will never receive more, so the live sample floor does not apply.
        virtual void setRetroMode(bool) {}

        // Opt-in, defaulted to a no-op: the post-BPF getSNR, pushed each block so
        // a regime-selecting timing (§48) can gate its strategy on SNR. Most
        // timings ignore it.
        virtual void setSnr(float) {}
    };

    // Stage 4: element sequence → character.
    class ISymbolDecoder {
    public:
        virtual ~ISymbolDecoder() = default;
        virtual void init() = 0;
        virtual void reset() = 0;
        virtual void addElement(Element e, float confidence) = 0;
        virtual char characterBreak() = 0;
        virtual const char* name() const = 0;
        virtual std::unique_ptr<ISymbolDecoder> makeFresh() const = 0;
    };

    // ── Registry ────────────────────────────────────────────────
    //
    // One named entry per benchmarkable configuration. The config UI lists
    // these; the benchmark matrix iterates them. Adding a core or a stage
    // combination means adding a registry entry and nothing else.

    struct CoreSpec {
        std::string name;         // stable id, e.g. "legacy", "legacy+median"
        std::string description;
        std::function<std::unique_ptr<IDecodeCore>()> make;
    };

    // Defined in core_registry.h once concrete cores exist.
    const std::vector<CoreSpec>& coreRegistry();

    inline const CoreSpec* findCore(const std::string& name) {
        for (const auto& s : coreRegistry()) {
            if (s.name == name) { return &s; }
        }
        return nullptr;
    }
}
