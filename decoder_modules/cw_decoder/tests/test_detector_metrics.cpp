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

// Edge-bias correction: does removing the +9.8% ON stretch reach CER?
//
// Two routes with different costs. EDGE_SYMMETRIC removes the bias by
// collapsing the thresholds to their midpoint, which also removes the
// hysteresis. EDGE_COMPENSATE keeps the hysteresis and corrects the event time
// instead. Both are reported against ON stretch (does the mechanism work) and
// CER (does it matter) — §12.3 is the standing warning that the first does not
// imply the second.
TEST_CASE("edge-bias correction: mechanism and CER", "[cw][.][edge-fix]") {
    constexpr int SEEDS = 24;

    struct Variant { const char* label; const char* core; cw::EdgeBias bias; cw::PeakTracker peak; };
    const Variant vs[] = {
        {"legacy", "legacy",      cw::EDGE_RAW,        cw::PEAK_INSTANT_ATTACK},
        {"+sym",   "legacy+sym",  cw::EDGE_SYMMETRIC,  cw::PEAK_INSTANT_ATTACK},
        {"+edge",  "legacy+edge", cw::EDGE_COMPENSATE, cw::PEAK_INSTANT_ATTACK},
        {"+peak",  "legacy+peak", cw::EDGE_RAW,        cw::PEAK_PERCENTILE},
    };
    constexpr int N = 4;

    printf("\n%-18s %32s %36s\n", "", "ON stretch (%dit)", "CER mean");
    printf("%-18s %7s %7s %7s %7s %8s %8s %8s %8s\n",
           "profile", "legacy", "+sym", "+edge", "+peak",
           "legacy", "+sym", "+edge", "+peak");
    printf("%s\n", std::string(96, '-').c_str());

    for (const auto& p : detectorProfiles()) {
        float stretch[N], cer[N];
        for (int v = 0; v < N; v++) {
            stretch[v] = measureDetectorMulti(p.message, p.params, SEEDS, 1000,
                                              cw::MF_RESET, vs[v].bias, vs[v].peak).mean.onStretchPct;
            cer[v] = runCell(vs[v].core, p.name, p.message, p.params, SEEDS).cerMean;
        }
        printf("%-18s %+7.2f %+7.2f %+7.2f %+7.2f %8.4f %8.4f %8.4f %8.4f\n",
               p.name, stretch[0], stretch[1], stretch[2], stretch[3],
               cer[0], cer[1], cer[2], cer[3]);
        CHECK(cer[0] >= 0.0f);
    }
    printf("\n");
}

// Phase 16: the threshold reference's attack constant.
//
// Both known extremes fail for opposite reasons — instant attack chases a
// ~32 ms keying edge (+9.8% ON stretch), a ~2 s percentile cannot follow a
// ~3.3 s QSB cycle (qsb 0.0117 -> 0.5065). Two orders of magnitude between
// them, one constant. This sweeps it.
TEST_CASE("peak attack constant sweep", "[cw][.][attack-sweep]") {
    constexpr int SEEDS = 24;
    const float taus[] = {50.0f, 100.0f, 200.0f, 300.0f, 500.0f, 800.0f, 1200.0f};
    constexpr int NT = sizeof(taus) / sizeof(taus[0]);

    auto slowAttackCore = [](float tauMs) {
        return CoreFactory([tauMs](const GeneratedSignal&) {
            return std::unique_ptr<cw::IDecodeCore>(new cw::StagedCore(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::make_unique<cw::SchmittDetector>(cw::EDGE_RAW,
                                                      cw::PEAK_SLOW_ATTACK, tauMs),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>()));
        });
    };

    printf("\n%-18s %8s", "profile", "legacy");
    for (float t : taus) { printf(" %7.0f", t); }
    printf("   %8s\n", "+pctile");
    printf("%s\n", std::string(100, '-').c_str());

    for (const auto& p : detectorProfiles()) {
        printf("%-18s %8.4f", p.name,
               runCell("legacy", p.name, p.message, p.params, SEEDS).cerMean);
        for (int t = 0; t < NT; t++) {
            printf(" %7.4f", runCellWith(slowAttackCore(taus[t]), "slow",
                                         p.name, p.message, p.params, SEEDS).cerMean);
        }
        printf("   %8.4f\n",
               runCell("legacy+peak", p.name, p.message, p.params, SEEDS).cerMean);
    }

    // ON stretch on a noiseless signal: does the bias actually come out?
    printf("\nON stretch %%dit (clean-15wpm, noiseless):\n  legacy %+.2f",
           measureDetectorMulti(MSG_FULL(), profileClean(80.0f), SEEDS).mean.onStretchPct);
    for (float t : taus) {
        printf("   %.0fms %+.2f", t,
               measureDetectorMulti(MSG_FULL(), profileClean(80.0f), SEEDS, 1000,
                                    cw::MF_RESET, cw::EDGE_RAW,
                                    cw::PEAK_SLOW_ATTACK, t).mean.onStretchPct);
    }
    printf("\n\n");
    CHECK(NT > 0);
}

// Phase 16b: the percentile window length.
//
// The attack sweep refuted the timescale framing — an EMA of any constant
// varies within an element (fast ones chase the edge, slow ones sag toward the
// duty-cycle mean). What makes the percentile work is element-INDEPENDENCE: its
// window spans many key cycles. So the knob is the window length, and it has a
// genuine interior optimum: long enough to span several elements, short enough
// to follow a fade. The default 2000 ms against a 3300 ms QSB cycle is why
// `+peak` breaks on that profile.
TEST_CASE("peak percentile window sweep", "[cw][.][window-sweep]") {
    constexpr int SEEDS = 24;
    const float wins[] = {250.0f, 500.0f, 750.0f, 1000.0f, 1500.0f, 2000.0f, 3000.0f};
    constexpr int NW = sizeof(wins) / sizeof(wins[0]);

    auto pctileCore = [](float winMs) {
        return CoreFactory([winMs](const GeneratedSignal&) {
            return std::unique_ptr<cw::IDecodeCore>(new cw::StagedCore(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::make_unique<cw::SchmittDetector>(cw::EDGE_RAW,
                                                      cw::PEAK_PERCENTILE, 300.0f, winMs),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>()));
        });
    };

    printf("\n%-18s %8s", "profile", "legacy");
    for (float w : wins) { printf(" %7.0f", w); }
    printf("\n%s\n", std::string(90, '-').c_str());

    double bestSum = 1e9; int bestIdx = -1;
    std::vector<std::vector<float>> grid;

    for (const auto& p : detectorProfiles()) {
        std::vector<float> row;
        printf("%-18s %8.4f", p.name,
               runCell("legacy", p.name, p.message, p.params, SEEDS).cerMean);
        for (int w = 0; w < NW; w++) {
            float c = runCellWith(pctileCore(wins[w]), "pctile",
                                  p.name, p.message, p.params, SEEDS).cerMean;
            row.push_back(c);
            printf(" %7.4f", c);
        }
        printf("\n");
        grid.push_back(row);
    }

    for (int w = 0; w < NW; w++) {
        double sum = 0;
        for (auto& row : grid) { sum += row[w]; }
        if (sum < bestSum) { bestSum = sum; bestIdx = w; }
    }
    printf("\nlowest CER sum: %.0f ms (%.4f across %zu profiles)\n",
           wins[bestIdx], bestSum, grid.size());
    printf("NOTE: sum is a search aid, not a promotion criterion — promotion is\n"
           "      no-worse-on-every-profile, judged from the table above.\n\n");
    CHECK(bestIdx >= 0);
}

// Phase 16c: dual-window peak reference.
//
// The window sweep showed short windows track fading and long windows estimate
// precisely, with no single length serving both. Running 250 ms and 2000 ms
// concurrently and switching on their relative disagreement makes the
// disagreement itself the fade detector. This sweeps the switch threshold.
TEST_CASE("dual-window peak reference", "[cw][.][dual-window]") {
    constexpr int SEEDS = 24;
    const float thresholds[] = {0.05f, 0.10f, 0.15f, 0.25f, 0.40f};
    constexpr int NT = sizeof(thresholds) / sizeof(thresholds[0]);

    auto dualCore = [](float thresh) {
        return CoreFactory([thresh](const GeneratedSignal&) {
            return std::unique_ptr<cw::IDecodeCore>(new cw::StagedCore(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::make_unique<cw::SchmittDetector>(cw::EDGE_RAW, cw::PEAK_DUAL_WINDOW,
                                                      300.0f, 250.0f, thresh),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>()));
        });
    };

    printf("\n%-18s %8s", "profile", "legacy");
    for (float t : thresholds) { printf("  t=%.2f", t); }
    printf("  %7s %7s\n", "250only", "2000only");
    printf("%s\n", std::string(88, '-').c_str());

    for (const auto& p : detectorProfiles()) {
        printf("%-18s %8.4f", p.name,
               runCell("legacy", p.name, p.message, p.params, SEEDS).cerMean);
        for (int t = 0; t < NT; t++) {
            printf(" %7.4f", runCellWith(dualCore(thresholds[t]), "dual",
                                         p.name, p.message, p.params, SEEDS).cerMean);
        }
        auto only = [&](float w) {
            return runCellWith(CoreFactory([w](const GeneratedSignal&) {
                       return std::unique_ptr<cw::IDecodeCore>(new cw::StagedCore(
                           std::make_unique<cw::EnvelopeFrontEnd>(),
                           std::make_unique<cw::SchmittDetector>(cw::EDGE_RAW,
                                                                 cw::PEAK_PERCENTILE, 300.0f, w),
                           std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                           std::make_unique<cw::BeamSymbolDecoder>()));
                   }), "only", p.name, p.message, p.params, SEEDS).cerMean;
        };
        printf("  %7.4f %7.4f\n", only(250.0f), only(2000.0f));
    }
    printf("\n");
    CHECK(NT > 0);
}

// Phase 16d: short-window length x disagreement persistence.
//
// The dual sweep's remaining regressions were all on stationary profiles, with
// the short window's own numbers leaking through: a 250 ms window holds 31
// entries, so its 90th percentile has ~18% relative standard error against a 5%
// switch threshold — the two windows disagree by construction. Two independent
// ways to make estimator noise smaller than the fade it must detect.
TEST_CASE("dual-window: short length and persistence", "[cw][.][dual-refine]") {
    constexpr int SEEDS = 24;
    struct Cfg { float shortMs; int persist; };
    const Cfg cfgs[] = {
        {250.0f, 1}, {250.0f, 4}, {250.0f, 8}, {250.0f, 16},
        {500.0f, 1}, {500.0f, 4}, {500.0f, 8}, {500.0f, 16},
    };
    constexpr int NC = sizeof(cfgs) / sizeof(cfgs[0]);

    auto dualCore = [](Cfg c) {
        return CoreFactory([c](const GeneratedSignal&) {
            return std::unique_ptr<cw::IDecodeCore>(new cw::StagedCore(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::make_unique<cw::SchmittDetector>(cw::EDGE_RAW, cw::PEAK_DUAL_WINDOW,
                                                      300.0f, c.shortMs, 0.05f, c.persist),
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>()));
        });
    };

    printf("\n%-18s %8s", "profile", "legacy");
    for (auto c : cfgs) { printf(" %4.0f/%-2d", c.shortMs, c.persist); }
    printf("\n%s\n", std::string(90, '-').c_str());

    std::vector<std::vector<float>> grid;
    std::vector<float> base;
    for (const auto& p : detectorProfiles()) {
        float lg = runCell("legacy", p.name, p.message, p.params, SEEDS).cerMean;
        base.push_back(lg);
        printf("%-18s %8.4f", p.name, lg);
        std::vector<float> row;
        for (int c = 0; c < NC; c++) {
            float v = runCellWith(dualCore(cfgs[c]), "dual",
                                  p.name, p.message, p.params, SEEDS).cerMean;
            row.push_back(v);
            printf(" %7.4f", v);
        }
        printf("\n");
        grid.push_back(row);
    }

    // Promotion is no-worse-on-every-profile. Report the count directly so the
    // table does not have to be eyeballed.
    printf("\n%-18s", "worse than legacy:");
    for (int c = 0; c < NC; c++) {
        int worse = 0;
        for (size_t r = 0; r < grid.size(); r++) {
            if (grid[r][c] > base[r] + 1e-6f) { worse++; }
        }
        printf(" %7d", worse);
    }
    printf("\n%-18s", "better:");
    for (int c = 0; c < NC; c++) {
        int better = 0;
        for (size_t r = 0; r < grid.size(); r++) {
            if (grid[r][c] < base[r] - 1e-6f) { better++; }
        }
        printf(" %7d", better);
    }
    printf("\n\n");
    CHECK(NC > 0);
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
