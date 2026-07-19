#pragma once
#include <cw/channel.h>
#include <cw/staged_core.h>
#include <cw/stages.h>
#include "cw_test_signals.h"

// Oracle ablation — replace one pipeline stage with a perfect one and measure
// how much CER that stage is costing.
//
// The existing matrix ranks cores against each other; it cannot express how
// close any of them is to what is achievable on a given signal. A core at CER
// 0.70 is near-optimal if the ceiling is 0.65 and is wasting 0.68 if the
// ceiling is 0.02 — the same number, opposite decisions.
//
// These stages live in tests/ and deliberately NOT in cw::coreRegistry():
// they need ground truth that only the signal generator has, so they cannot
// run on a real signal and must never reach the module's config UI.
//
// Note this is an upper bound, not a design: knowing a stage is costing 0.7
// does not say how to recover it, and a perfect stage is not buildable.

namespace cw_test {

    // ── Ground truth → internal-rate key transitions ─────────────

    struct TruthTransition {
        long long sample;   // at the decoder's internal rate
        bool keyDown;
    };

    inline std::vector<TruthTransition> truthTransitions(const GeneratedSignal& sig,
                                                         float sampleRate,
                                                         float internalRate) {
        const double decim = sampleRate / internalRate;
        std::vector<TruthTransition> out;
        for (const auto& seg : sig.segments) {
            if (!seg.tone) { continue; }
            out.push_back({(long long)(seg.startSample / decim), true});
            out.push_back({(long long)(seg.endSample   / decim), false});
        }
        return out;
    }

    // ── Oracle detector ─────────────────────────────────────────
    //
    // Emits the true key transitions and ignores the envelope entirely.
    //
    // No group-delay compensation is needed: everything downstream consumes
    // only differences between event times, so the front end's constant
    // latency cancels. (Detector *scoring* is a different matter — there the
    // latency does not cancel, so bias must be reported differentially.)
    //
    // CONFOUND: StagedCore derives its soft squelch from getSNR() and drops
    // events when sqFactor reaches zero. Reporting a high SNR therefore
    // removes detection error and squelch suppression together, so this
    // bounds the pair. Construct with the measured SNR instead to split them.
    class OracleDetector : public cw::IDetector {
    public:
        explicit OracleDetector(std::vector<TruthTransition> t, float snrDb = 20.0f)
            : trans(std::move(t)), _snr(snrDb) {}

        void init(float) override {}
        void reset() override { pos = 0; counter = 0; keyDown = false; }

        std::vector<cw::KeyEvent> process(const float*, int count) override {
            std::vector<cw::KeyEvent> out;
            const long long blockEnd = counter + count;
            while (pos < trans.size() && trans[pos].sample < blockEnd) {
                long long t = std::max(trans[pos].sample, counter);
                out.push_back({trans[pos].keyDown, (int)(t - counter)});
                keyDown = trans[pos].keyDown;
                pos++;
            }
            counter = blockEnd;
            return out;
        }

        float getSNR() const override { return _snr; }
        bool isKeyDown() const override { return keyDown; }
        const char* name() const override { return "oracle"; }

    private:
        std::vector<TruthTransition> trans;
        float _snr;
        size_t pos = 0;
        long long counter = 0;
        bool keyDown = false;
    };

    // ── Clairvoyant timing ──────────────────────────────────────
    //
    // Perfect knowledge of speed, but still classifies the *actual* noisy
    // durations. That separates "the speed estimate is wrong" from "the
    // durations themselves are unclassifiable" — a distinction the three
    // failed timing interventions could not make.
    class ClairvoyantTiming : public cw::ITiming {
    public:
        explicit ClairvoyantTiming(const ElementModel& m) : model(m) {}

        void init(float) override {}
        void reset() override {}

        // Boundaries are geometric, not arithmetic means. The generator's
        // jitter is multiplicative (duration × (1 + jitterPct·N)), so relative
        // spread is equal across classes and the equal-prior Bayes boundary
        // sits at the geometric mean — the same log-domain argument as
        // Mills 1977 and LogTiming.
        cw::TimingEvent classifyOn(float ms) override {
            cw::TimingEvent e;
            e.type = cw::TimingEvent::KEY_ELEMENT;
            e.durationMs = ms;
            e.element = (ms < geoMean(model.ditMs, model.dahMs)) ? cw::DIT : cw::DAH;
            e.confidence = 1.0f;
            return e;
        }

        cw::TimingEvent classifyOff(float ms) override {
            cw::TimingEvent e;
            e.type = cw::TimingEvent::KEY_GAP;
            e.durationMs = ms;
            e.gap = (ms < geoMean(model.elemGapMs, model.charGapMs)) ? cw::ELEMENT_GAP
                  : (ms < geoMean(model.charGapMs, model.wordGapMs)) ? cw::CHAR_GAP
                                                                      : cw::WORD_GAP;
            e.confidence = 1.0f;
            return e;
        }

        // The element scale StagedCore sizes its filter and flush timers from.
        float getDitDuration() const override { return model.ditMs; }
        // Speed is the nominal dit; weighting shifts elements without changing WPM.
        float getWPM() const override { return 1200.0f / model.nominalDitMs; }
        bool isLocked() const override { return true; }
        const char* name() const override { return "clairvoyant"; }

        std::unique_ptr<cw::ITiming> makeFresh() const override {
            return std::make_unique<ClairvoyantTiming>(model);
        }

    private:
        static float geoMean(float a, float b) { return std::sqrt(a * b); }
        ElementModel model;
    };

    // ── Core factories ──────────────────────────────────────────

    struct OracleConfig {
        bool detector = false;
        bool timing   = false;
    };

    inline std::unique_ptr<cw::IDecodeCore> makeOracleCore(const GeneratedSignal& sig,
                                                           const SignalParams& p,
                                                           OracleConfig cfg,
                                                           float internalRate = 1000.0f) {
        std::unique_ptr<cw::IDetector> det;
        if (cfg.detector) {
            det = std::make_unique<OracleDetector>(
                      truthTransitions(sig, p.sampleRate, internalRate));
        } else {
            det = std::make_unique<cw::SchmittDetector>();
        }

        std::unique_ptr<cw::ITiming> tim;
        if (cfg.timing) {
            tim = std::make_unique<ClairvoyantTiming>(sig.model);
        } else {
            tim = std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN);
        }

        return std::make_unique<cw::StagedCore>(
            std::make_unique<cw::EnvelopeFrontEnd>(),
            std::move(det), std::move(tim),
            std::make_unique<cw::BeamSymbolDecoder>());
    }
}
