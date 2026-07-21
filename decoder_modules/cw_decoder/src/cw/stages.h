#pragma once
#include "core.h"
#include "dsp.h"
#include "tone_detector.h"
#include "timing.h"
#include "morse_tree.h"

// Adapters binding the existing concrete components to the stage interfaces in
// core.h. These add no behaviour — every method forwards. They exist so
// StagedCore can hold stages polymorphically and so alternative
// implementations can be dropped in beside them.

namespace cw {

    // ── Stage 1: front end ──────────────────────────────────────

    class EnvelopeFrontEnd : public IFrontEnd {
    public:
        // Filter geometry is a constructor parameter: pre-detection bandwidth
        // is a decoding design choice, and the benchmark matrix varies it.
        EnvelopeFrontEnd(float bpfCutoff = 100.0f, float bpfTrans = 100.0f,
                         float smoothCutoff = 80.0f, float smoothTrans = 100.0f)
            : _bpfCutoff(bpfCutoff), _bpfTrans(bpfTrans),
              _smoothCutoff(smoothCutoff), _smoothTrans(smoothTrans) {}

        void init(float toneFreq, float sampleRate, float internalRate) override {
            dsp.init(toneFreq, sampleRate, internalRate,
                     _bpfCutoff, _bpfTrans, _smoothCutoff, _smoothTrans);
        }
        void setToneFreq(float freq) override { dsp.setToneFreq(freq); }
        int process(int count, const dsp::complex_t* in, float* out) override {
            return dsp.process(count, in, out);
        }
        void setBandwidth(float cutoff, float trans) override { dsp.setBandwidth(cutoff, trans); }
        float getInputSnrDb() const override { return dsp.getInputSnrDb(); }
        bool inputSnrReady() const override { return dsp.inputSnrReady(); }
        const char* name() const override { return "envelope"; }

    private:
        EnvelopeDSP dsp;
        float _bpfCutoff, _bpfTrans, _smoothCutoff, _smoothTrans;
    };

    // ── Stage 2: detector ───────────────────────────────────────

    class SchmittDetector : public IDetector {
    public:
        explicit SchmittDetector(EdgeBias bias = EDGE_RAW,
                                 PeakTracker peak = PEAK_INSTANT_ATTACK,
                                 float peakAttackMs = 300.0f,
                                 float peakWindowMs = 2000.0f,
                                 float peakDualThreshold = 0.15f,
                                 int peakDualPersist = 1)
            : _bias(bias), _peak(peak), _attackMs(peakAttackMs),
              _windowMs(peakWindowMs), _dualThresh(peakDualThreshold),
              _dualPersist(peakDualPersist) {}

        void init(float internalRate) override {
            det.init(internalRate);
            det.setEdgeBias(_bias);
            det.setPeakTracker(_peak);
            det.setPeakAttackMs(_attackMs);
            det.setPeakWindowMs(_windowMs);
            det.setPeakDualThreshold(_dualThresh);
            det.setPeakDualPersist(_dualPersist);
            det.setGuardThreshold(_guard);
        }
        void reset() override { det.reset(); }
        std::vector<KeyEvent> process(const float* env, int count) override {
            return det.process(env, count);
        }
        float getSNR() const override { return det.getSNR(); }
        bool isKeyDown() const override { return det.isKeyDown(); }
        void preseed(float level, int count) override { det.preseed(level, count); }

        const char* name() const override {
            switch (_bias) {
                case EDGE_RAW:        return "schmitt";
                case EDGE_SYMMETRIC:  return "schmitt-sym";
                case EDGE_COMPENSATE: return "schmitt-comp";
            }
            return "schmitt";
        }

        // Guard instrumentation, forwarded for the §13.9 probe. Held here rather
        // than on IDetector: no other detector has a dynamic-range guard, so
        // widening the interface would describe this implementation, not the role.
        void setGuardTrace(bool on) { det.setGuardTrace(on); }
        // Stored rather than forwarded: init() runs after construction and would
        // otherwise overwrite it with the default.
        void setGuardThreshold(float t) { _guard = t; det.setGuardThreshold(t); }
        long long guardEvaluated() const { return det.getGuardEvaluated(); }
        long long guardRejected() const { return det.getGuardRejected(); }
        const std::vector<std::pair<long long, int>>& guardRuns() const { return det.getGuardRuns(); }

    private:
        ToneDetector det;
        EdgeBias _bias;
        PeakTracker _peak;
        float _attackMs;
        float _windowMs;
        float _dualThresh;
        int _dualPersist;
        float _guard = 1.8f;
    };

    // Sequential likelihood-ratio detector (docs §24). Same IDetector role as
    // SchmittDetector, different decision rule: sustained evidence, not
    // magnitude. Constructor exposes the CUSUM knobs so a sweep can vary them.
    class LikelihoodRatioDetector : public IDetector {
    public:
        explicit LikelihoodRatioDetector(float theta = 0.5f,
                                         float boundHi = 3.0f, float boundLo = -3.0f,
                                         bool soft = false, bool adaptive = false)
            : _theta(theta), _boundHi(boundHi), _boundLo(boundLo),
              _soft(soft), _adaptive(adaptive) {}

        void init(float internalRate) override {
            det.init(internalRate);
            det.setTheta(_theta);
            det.setBounds(_boundHi, _boundLo);
            det.setSoft(_soft);
            det.setAdaptive(_adaptive);
            det.setExternalDitMs(_externalDitMs);
        }
        void reset() override { det.reset(); }
        std::vector<KeyEvent> process(const float* env, int count) override {
            return det.process(env, count);
        }
        float getSNR() const override { return det.getSNR(); }
        bool isKeyDown() const override { return det.isKeyDown(); }
        void preseed(float level, int count) override { det.preseed(level, count); }
        const char* name() const override { return "lr"; }

        // Stored, not forwarded live: init() runs after construction and would
        // otherwise overwrite it with the default (docs §27). Set by the test
        // factory from the generator's ground-truth dit.
        void setExternalDitMs(float ms) { _externalDitMs = ms; }

    private:
        LRDetector det;
        float _theta, _boundHi, _boundLo;
        bool _soft, _adaptive;
        float _externalDitMs = 0.0f;
    };

    // ── Stage 3: timing ─────────────────────────────────────────

    class AdaptiveTimingStage : public ITiming {
    public:
        explicit AdaptiveTimingStage(TimingStrategy s = TIMING_KALMAN) : _strategy(s) {}

        void init(float internalRate) override {
            t.init(internalRate, _strategy);
            if (_speedGate >= 0.0f) { t.setSpeedGateWpm(_speedGate); }
        }
        // §41 sweep hook: override the strategy's default speed-gate threshold.
        void setSpeedGateWpm(float w) { _speedGate = w; }
        void reset() override { t.reset(); }
        TimingEvent classifyOn(float ms) override { return t.classifyOn(ms); }
        TimingEvent classifyOff(float ms) override { return t.classifyOff(ms); }
        float getDitDuration() const override { return t.getDitDuration(); }
        float getWPM() const override { return t.getWPM(); }
        bool isLocked() const override { return t.isLocked(); }

        const char* name() const override {
            switch (_strategy) {
                case TIMING_KMEANS:  return "kmeans";
                case TIMING_MEDIAN:  return "median";
                case TIMING_BIMODAL: return "bimodal";
                case TIMING_KALMAN:    return "kalman";
                case TIMING_KALMAN_V2: return "kalman2";
                case TIMING_KALMAN_GUARD: return "ditguard";
                case TIMING_KALMAN_V2S: return "kalman2s";
                case TIMING_LOG:        return "log";
                case TIMING_LOG_ROBUST: return "logrobust";
                case TIMING_LOG_GUARDED: return "logguard";
            }
            return "unknown";
        }

        std::unique_ptr<ITiming> makeFresh() const override {
            return std::make_unique<AdaptiveTimingStage>(_strategy);
        }

        void setRetroMode(bool on) override {
            t.setMinGapSamples(on ? 4 : 10);
            t.setGapBootstrap(on);
        }

    private:
        AdaptiveTiming t;
        TimingStrategy _strategy;
        float _speedGate = -1.0f;   // §41: <0 keeps the strategy default
    };

    // ── Stage 4: symbol decoder ─────────────────────────────────

    class BeamSymbolDecoder : public ISymbolDecoder {
    public:
        void init() override { d.init(); }
        void reset() override { d.reset(); }
        void addElement(Element e, float conf) override { d.addElement(e, conf); }
        char characterBreak() override { return d.characterBreak(); }
        const char* name() const override { return "beam"; }

        std::unique_ptr<ISymbolDecoder> makeFresh() const override {
            auto p = std::make_unique<BeamSymbolDecoder>();
            p->init();
            return p;
        }

    private:
        MorseDecoder d;
    };
}
