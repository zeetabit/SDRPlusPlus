#include <catch.hpp>
#include "cw_detector_score.h"
#include "cw_matrix.h"
#include <cstdio>

// Which *kind* of detector error dominates on each profile.
//
// §11 measured that the detector holds all the AWGN-family error. CER cannot
// say whether it is hallucinating events, dropping them, or misplacing their
// edges — and those need different fixes.

using namespace cw_test;

namespace {

    struct DProfile { const char* name; const char* message; SignalParams params; };

    std::vector<DProfile> detectorProfiles() {
        std::vector<DProfile> v;
        v.push_back({"clean-15wpm",     MSG_FULL(), profileClean(80.0f)});
        v.push_back({"clean-25wpm",     MSG_FULL(), profileClean(48.0f)});
        v.push_back({"handkeyed-15wpm", MSG_FULL(), profileHandKeyed(80.0f)});
        v.push_back({"handkeyed-25wpm", MSG_FULL(), profileHandKeyed(48.0f)});
        v.push_back({"qsb",             MSG_FULL(), profileQSB(80.0f)});
        v.push_back({"qrm",             MSG_FULL(), profileQRM(80.0f)});
        v.push_back({"qrn",             MSG_FULL(), profileQRN(80.0f)});
        v.push_back({"worstcase",       MSG_FULL(), profileWorstCase(80.0f)});
        for (float amp : {1.0f, 2.0f, 3.0f, 4.0f}) {
            DProfile p;
            p.name = amp == 1.0f ? "snr-noise1.0" : amp == 2.0f ? "snr-noise2.0" :
                     amp == 3.0f ? "snr-noise3.0" : "snr-noise4.0";
            p.message = MSG_FULL();
            p.params = profileClean(80.0f);
            p.params.noiseAmp = amp;
            v.push_back(p);
        }
        return v;
    }

}

TEST_CASE("detector ground-truth metrics", "[cw][.][detector-metrics]") {
    constexpr int SEEDS = 24;

    printf("\n%-18s %8s %8s %10s %10s %10s\n",
           "profile", "false", "miss", "ONstretch", "edge sd", "delay");
    printf("%-18s %8s %8s %10s %10s %10s\n",
           "", "/elem", "/elem", "%dit", "%dit", "ms");
    printf("%s\n", std::string(76, '-').c_str());

    for (const auto& p : detectorProfiles()) {
        auto s = measureDetectorMulti(p.message, p.params, SEEDS).mean;
        printf("%-18s %8.3f %8.3f %+10.2f %10.2f %10.1f\n",
               p.name, s.falseRate, s.missRate,
               s.onStretchPct, s.edgeJitterPct, s.groupDelayMs);
        CHECK(s.trueTransitions > 0);
    }
    printf("\n");
}

// Diagnostic: is a reported false detection a real extra event, or an
// alignment artifact? Transition counts settle it — alignment cannot change
// how many events the detector emitted.
TEST_CASE("detector diagnostic: event counts and delay drift", "[cw][.][detector-diag]") {
    struct Case { const char* name; SignalParams p; };
    std::vector<Case> cases = {
        {"clean-15wpm", profileClean(80.0f)},
        {"clean-25wpm", profileClean(48.0f)},
        {"handkeyed-25wpm", profileHandKeyed(48.0f)},
    };

    for (auto& c : cases) {
        printf("\n=== %s ===\n", c.name);
        for (unsigned seed : {1000u, 8919u, 16838u}) {
            SignalParams p = c.p;
            p.seed = seed;

            auto sig = generateMessage(MSG_FULL(), p);
            auto truth = truthTransitions(sig, p.sampleRate, 1000.0f);

            auto rec = std::make_unique<RecordingDetector>(
                           std::make_unique<cw::SchmittDetector>());
            RecordingDetector* probe = rec.get();
            auto core = std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(), std::move(rec),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>());
            cw::Channel ch;
            ch.initWithCore(0, p.toneFreq, std::move(core), "probe");
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }

            const auto& det = probe->events();
            std::vector<TruthTransition> falses, misses;
            auto s = scoreDetector(truth, det, probe->processed(), 1000.0f,
                                   sig.model.ditMs, &falses, &misses);

            printf("seed %-6u truth=%3zu detected=%3zu  (delta %+d)   "
                   "false=%.3f miss=%.3f\n",
                   seed, truth.size(), det.size(),
                   (int)det.size() - (int)truth.size(), s.falseRate, s.missRate);

            // Locate each spurious event inside the true key pattern: an event
            // during a gap is a different defect from one splitting an element.
            std::sort(falses.begin(), falses.end(),
                      [](const TruthTransition& a, const TruthTransition& b) {
                          return a.sample < b.sample;
                      });
            const long long lagSamples = (long long)(s.groupDelayMs + 0.5f);
            for (const auto& f : falses) {
                // Detected times carry the front end's latency; remove it
                // before asking where in the true key pattern the event sits.
                const long long trueSample = (f.sample - lagSamples) * 8;
                const TruthSegment* host = nullptr;
                for (const auto& seg : sig.segments) {
                    if (trueSample >= seg.startSample && trueSample < seg.endSample) {
                        host = &seg; break;
                    }
                }
                const char* kindName = "?";
                if (host) {
                    switch (host->kind) {
                        case TRUTH_DIT: kindName = "DIT"; break;
                        case TRUTH_DAH: kindName = "DAH"; break;
                        case TRUTH_ELEMENT_GAP: kindName = "elem-gap"; break;
                        case TRUTH_CHAR_GAP: kindName = "char-gap"; break;
                        case TRUTH_WORD_GAP: kindName = "word-gap"; break;
                        case TRUTH_WARMUP: kindName = "warmup"; break;
                        case TRUTH_TRAILING: kindName = "trailing"; break;
                    }
                }
                float intoMs = host ? (trueSample - host->startSample) / 8.0f : 0;
                float hostMs = host ? (host->endSample - host->startSample) / 8.0f : 0;
                printf("             FALSE %-4s t=%6lldms  inside %-9s "
                       "(%.0fms long, %.0fms in = %.0f%%)\n",
                       f.keyDown ? "DOWN" : "UP", f.sample - lagSamples, kindName,
                       hostMs, intoMs, hostMs > 0 ? 100.0f * intoMs / hostMs : 0);
            }

            // Per-edge delay over time: if the constant-lag model is wrong the
            // residual drifts, and drift beyond the match window shows up as
            // paired false+miss.
            if (truth.size() == det.size()) {
                float mn = 1e9f, mx = -1e9f;
                size_t argmn = 0, argmx = 0;
                for (size_t i = 0; i < truth.size(); i++) {
                    float d = (float)(det[i].sample - truth[i].sample);
                    if (d < mn) { mn = d; argmn = i; }
                    if (d > mx) { mx = d; argmx = i; }
                }
                printf("             1:1 pairing holds. delay range %.0f..%.0f ms "
                       "(spread %.0f, window %.0f) at idx %zu/%zu\n",
                       mn, mx, mx - mn, std::max(4.0f, 0.4f * sig.model.ditMs),
                       argmn, argmx);
            }
        }
    }
    printf("\n");
}

// Hypothesis test for the clean-25 spurious events.
//
// StagedCore sizes its matched filter from timing->getDitDuration() on every
// block, and resizing zeroes the ring buffer. If ditEst sits near an integer
// window boundary it flips back and forth, resetting the filter mid-signal.
//
// ClairvoyantTiming reports a constant dit, so the window never changes. Same
// signal, same detector, same front end — only the resize is removed. If the
// spurious events vanish, the resize is the cause.
TEST_CASE("detector diagnostic: matched-filter resize hypothesis", "[cw][.][detector-diag]") {
    struct Case { const char* name; SignalParams p; };
    std::vector<Case> cases = {
        {"clean-15wpm", profileClean(80.0f)},
        {"clean-25wpm", profileClean(48.0f)},
        {"handkeyed-25wpm", profileHandKeyed(48.0f)},
    };

    printf("\n%-18s %26s %26s\n", "", "adaptive dit (real)", "constant dit (no resize)");
    printf("%-18s %10s %8s %6s %10s %8s %6s\n",
           "profile", "detected", "false", "miss", "detected", "false", "miss");
    printf("%s\n", std::string(74, '-').c_str());

    for (auto& c : cases) {
        SignalParams p = c.p;
        p.seed = 1000;
        auto sig = generateMessage(MSG_FULL(), p);
        auto truth = truthTransitions(sig, p.sampleRate, 1000.0f);

        int nDet[2]; float fr[2], mr[2];
        for (int variant = 0; variant < 2; variant++) {
            auto rec = std::make_unique<RecordingDetector>(
                           std::make_unique<cw::SchmittDetector>());
            RecordingDetector* probe = rec.get();

            std::unique_ptr<cw::ITiming> tim;
            if (variant == 0) { tim = std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN); }
            else              { tim = std::make_unique<ClairvoyantTiming>(sig.model); }

            auto core = std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(), std::move(rec),
                std::move(tim), std::make_unique<cw::BeamSymbolDecoder>());
            cw::Channel ch;
            ch.initWithCore(0, p.toneFreq, std::move(core), "probe");
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }

            auto s = scoreDetector(truth, probe->events(), probe->processed(),
                                   1000.0f, sig.model.ditMs);
            nDet[variant] = (int)probe->events().size();
            fr[variant] = s.falseRate;
            mr[variant] = s.missRate;
        }

        printf("%-18s %10d %8.3f %6.3f %10d %8.3f %6.3f    (truth %d)\n",
               c.name, nDet[0], fr[0], mr[0], nDet[1], fr[1], mr[1], (int)truth.size());
    }
    printf("\n");
}

// legacy vs legacy+mf: does preserving the matched filter across a resize
// remove the spurious events, and does that reach CER?
TEST_CASE("matched-filter resize fix: detector and CER", "[cw][.][mf-fix]") {
    constexpr int SEEDS = 24;

    printf("\n%-18s %17s %17s   %17s\n",
           "", "false/elem", "CER mean", "ins / del");
    printf("%-18s %8s %8s %8s %8s   %8s %8s\n",
           "profile", "legacy", "+mf", "legacy", "+mf", "legacy", "+mf");
    printf("%s\n", std::string(80, '-').c_str());

    for (const auto& p : detectorProfiles()) {
        auto dLegacy = measureDetectorMulti(p.message, p.params, SEEDS, 1000, cw::MF_RESET).mean;
        auto dFixed  = measureDetectorMulti(p.message, p.params, SEEDS, 1000, cw::MF_PRESERVE).mean;

        auto cLegacy = runCell("legacy",    p.name, p.message, p.params, SEEDS);
        auto cFixed  = runCell("legacy+mf", p.name, p.message, p.params, SEEDS);

        printf("%-18s %8.3f %8.3f %8.4f %8.4f   %.3f/%.3f %.3f/%.3f\n",
               p.name, dLegacy.falseRate, dFixed.falseRate,
               cLegacy.cerMean, cFixed.cerMean,
               cLegacy.insRate, cLegacy.delRate, cFixed.insRate, cFixed.delRate);

        // Reported, not gated: promotion is decided from the table, and the
        // always-on gates in test_benchmark_multiseed.cpp protect `legacy`.
        CHECK(dFixed.falseRate >= 0.0f);
    }
    printf("\n");
}

// Instrument self-checks. If these break, every number above is measuring the
// harness rather than the detector.
TEST_CASE("detector scoring recovers a clean signal exactly", "[cw][detector-self]") {
    SignalParams p = profileClean(80.0f);
    p.seed = 3;

    auto s = measureDetector(MSG_CQ(), p);

    INFO("false=" << s.falseRate << " miss=" << s.missRate
         << " matched=" << s.matched << "/" << s.trueTransitions
         << " delay=" << s.groupDelayMs << "ms");

    // A noiseless signal must produce exactly the keyed transitions.
    CHECK(s.falseRate == 0.0f);
    CHECK(s.missRate == 0.0f);
    CHECK(s.matched == s.trueTransitions);

    // Group delay is a real property of the chain, not an alignment artifact:
    // BPF alone is 18.5 ms and the matched filter adds more.
    CHECK(s.groupDelayMs > 5.0f);
    CHECK(s.groupDelayMs < 120.0f);
}

// The alignment must survive a known constant offset without inventing errors —
// otherwise group delay would masquerade as false detections and misses.
TEST_CASE("detector scoring separates latency from error", "[cw][detector-self]") {
    SignalParams p = profileClean(80.0f);
    p.seed = 5;
    auto sig = generateMessage("PARIS", p);
    auto truth = truthTransitions(sig, p.sampleRate, 1000.0f);

    const long long len = (long long)sig.samples.size() / 8;
    const long long shift = 37;   // ms at 1 kHz internal rate

    std::vector<TruthTransition> shifted;
    for (auto t : truth) { shifted.push_back({t.sample + shift, t.keyDown}); }

    auto s = scoreDetector(truth, shifted, len, 1000.0f, sig.model.ditMs);

    CHECK(s.groupDelayMs == Approx(37.0f).margin(2.0f));
    CHECK(s.falseRate == 0.0f);
    CHECK(s.missRate == 0.0f);
    CHECK(s.onStretchMs == Approx(0.0f).margin(0.5f));
}

// A deliberately asymmetric shift must show up as ON stretch, and only there.
TEST_CASE("detector scoring reports edge asymmetry as ON stretch", "[cw][detector-self]") {
    SignalParams p = profileClean(80.0f);
    p.seed = 5;
    auto sig = generateMessage("PARIS", p);
    auto truth = truthTransitions(sig, p.sampleRate, 1000.0f);

    const long long len = (long long)sig.samples.size() / 8;

    // Key-down edges 20 ms late, key-up edges 28 ms late -> ON stretched 8 ms.
    std::vector<TruthTransition> skewed;
    for (auto t : truth) { skewed.push_back({t.sample + (t.keyDown ? 20 : 28), t.keyDown}); }

    auto s = scoreDetector(truth, skewed, len, 1000.0f, sig.model.ditMs);

    CHECK(s.onStretchMs == Approx(8.0f).margin(1.0f));
    CHECK(s.falseRate == 0.0f);
    CHECK(s.missRate == 0.0f);
}
