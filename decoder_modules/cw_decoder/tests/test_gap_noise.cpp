#include <catch.hpp>
#include <cw/channel.h>
#include <cw/staged_core.h>
#include <cw/stages.h>
#include <cw/timing.h>
#include "cw_test_signals.h"
#include "cw_detector_score.h"

using namespace cw_test;

// ============================================================
// Gap classification under noise (docs §16.4, the ⊘ open item)
//
// The Farnsworth probe in test_farnsworth.cpp walks the generator's truth
// segments, so the durations it feeds AdaptiveTiming are identical at every
// noise level — it is structurally noise-blind. Noise reaches gap
// classification only through the detector, so measuring it requires
// DETECTOR-derived gaps, aligned to truth by the measured group delay.
//
// A detector gap can be wrong in two unrelated ways, and conflating them
// would make the result uninterpretable:
//
//   structural damage — the detector dropped or invented an edge, so the gap
//                       does not correspond to any single truth gap. This is
//                       a detector fault; the classifier never had a chance.
//   classification    — the gap corresponds to exactly one truth gap and the
//                       classifier still named it wrong. This is the
//                       classifier's own error.
//
// Only matched gaps enter the confusion matrix. That separation is what makes
// the actual question falsifiable: does classification error on *structurally
// intact* gaps grow with noise? If it does, the adaptive estimator is
// compounding its own errors — it learns centres from durations noise has
// already corrupted, and feeds them back. If it stays flat while structural
// damage climbs, the fault is entirely upstream.
// ============================================================

namespace {

    constexpr float RATE = 1000.0f;   // internal rate: 1 sample == 1 ms

    struct GapNoiseStats {
        int confusion[3][3] = {};   // [truth][classified], matched gaps only
        int matched = 0;
        int damaged = 0;            // no single truth gap corresponds
        int sourceHist[5] = {};
        float ditErrPct = 0;        // learned dit vs the generator's
        int seeds = 0;
    };

    int gapKindIndex(TruthKind k) {
        switch (k) {
            case TRUTH_ELEMENT_GAP: return 0;
            case TRUTH_CHAR_GAP:    return 1;
            case TRUTH_WORD_GAP:    return 2;
            default:                return -1;
        }
    }

    int gapClassIndex(cw::Gap g) {
        switch (g) {
            case cw::ELEMENT_GAP: return 0;
            case cw::CHAR_GAP:    return 1;
            case cw::WORD_GAP:    return 2;
        }
        return 0;
    }

    struct TruthSpan {
        long long start, end;
        int kind;        // gap class index, or -1 for a tone
    };

    std::vector<TruthSpan> truthSpans(const GeneratedSignal& sig, float sampleRate) {
        const double decim = sampleRate / RATE;
        std::vector<TruthSpan> out;
        for (const auto& seg : sig.segments) {
            if (seg.kind == TRUTH_WARMUP || seg.kind == TRUTH_TRAILING) { continue; }
            out.push_back({(long long)(seg.startSample / decim),
                           (long long)(seg.endSample / decim),
                           seg.tone ? -1 : gapKindIndex(seg.kind)});
        }
        return out;
    }

    // A detector gap is matched only if it overlaps exactly one truth gap and
    // its midpoint lies inside that gap. The first condition rejects merges
    // (a dropped element makes one detector gap span gap-tone-gap); the second
    // rejects gaps invented inside a tone by a mid-element dropout.
    int matchGap(const std::vector<TruthSpan>& spans, long long a, long long b) {
        int overlapping = 0, kind = -1;
        const long long mid = (a + b) / 2;
        bool midInGap = false;
        for (const auto& s : spans) {
            if (s.kind < 0) { continue; }
            if (s.start < b && a < s.end) {
                overlapping++;
                if (mid >= s.start && mid < s.end) { midInGap = true; kind = s.kind; }
            }
        }
        return (overlapping == 1 && midInGap) ? kind : -1;
    }

    // minElem replicates staged_core.h:265 — the decoder rejects short elements
    // before classifyOn once timing is locked, so a probe without it feeds the
    // estimator noise spikes the decoder never sees. The low-SNR factor (0.15)
    // is used because that is the branch taken at the noise levels in question;
    // the detector's per-event SNR is not observable from here, so this is the
    // permissive end of the decoder's real filter, not an exact replica.
    GapNoiseStats probeNoisyGaps(const char* message, SignalParams params, int seeds,
                                 bool minElem, bool useLR = false,
                                 cw::TimingStrategy strat = cw::TIMING_KALMAN) {
        GapNoiseStats out;
        out.seeds = seeds;

        for (int s = 0; s < seeds; s++) {
            params.seed = 1000 + (unsigned)s * 7919u;
            auto sig = generateMessage(message, params);

            std::unique_ptr<cw::IDetector> inner = useLR
                ? std::unique_ptr<cw::IDetector>(std::make_unique<cw::LikelihoodRatioDetector>())
                : std::unique_ptr<cw::IDetector>(std::make_unique<cw::SchmittDetector>());
            auto rec = std::make_unique<RecordingDetector>(std::move(inner));
            auto* probe = rec.get();

            cw::Channel ch;
            ch.initWithCore(0, params.toneFreq, std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(), std::move(rec),
                std::make_unique<cw::AdaptiveTimingStage>(strat),
                std::make_unique<cw::BeamSymbolDecoder>()), "gap-noise-probe");

            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }

            auto truth = truthTransitions(sig, params.sampleRate, RATE);
            auto score = scoreDetector(truth, probe->events(), probe->processed(),
                                       RATE, sig.model.ditMs);
            const long long lag = (long long)(score.groupDelayMs + 0.5f);
            const auto spans = truthSpans(sig, params.sampleRate);

            // A second timing instance fed the detector's durations. The
            // decoder's own instance cannot be read per-gap without changing
            // the pipeline, and this must observe the same sequence it sees.
            cw::AdaptiveTiming timing;
            timing.init(RATE, strat);

            long long lastDown = -1, lastUp = -1;
            for (const auto& e : probe->events()) {
                if (e.keyDown) {
                    if (lastUp >= 0) {
                        const float ms = (float)(e.sample - lastUp);
                        float el, ch2, wd;
                        out.sourceHist[(int)timing.gapCenters(el, ch2, wd)]++;
                        const cw::TimingEvent te = timing.classifyOff(ms);

                        const int truthKind = matchGap(spans, lastUp - lag, e.sample - lag);
                        if (truthKind < 0) { out.damaged++; }
                        else {
                            out.confusion[truthKind][gapClassIndex(te.gap)]++;
                            out.matched++;
                        }
                    }
                    lastDown = e.sample;
                } else {
                    if (lastDown >= 0) {
                        const float onMs = (float)(e.sample - lastDown);
                        const float floorMs = (minElem && timing.isLocked())
                            ? std::max(5.0f, timing.getDitDuration() * 0.15f) : 5.0f;
                        if (onMs >= floorMs) { timing.classifyOn(onMs); }
                    }
                    lastUp = e.sample;
                }
            }

            if (sig.model.ditMs > 0) {
                out.ditErrPct += 100.0f * (timing.getDitDuration() - sig.model.ditMs)
                               / sig.model.ditMs;
            }
        }

        out.ditErrPct /= seeds;
        return out;
    }

    int classErrors(const GapNoiseStats& g) {
        int e = 0;
        for (int t = 0; t < 3; t++) {
            for (int c = 0; c < 3; c++) { if (t != c) { e += g.confusion[t][c]; } }
        }
        return e;
    }

    float classErrPct(const GapNoiseStats& g) {
        return g.matched > 0 ? 100.0f * classErrors(g) / g.matched : 0.0f;
    }

    float damagePct(const GapNoiseStats& g) {
        const int total = g.matched + g.damaged;
        return total > 0 ? 100.0f * g.damaged / total : 0.0f;
    }

    struct GapProfile { const char* name; SignalParams params; };

    std::vector<GapProfile> gapProfiles() {
        std::vector<GapProfile> v;
        v.push_back({"clean",       profileClean(80.0f)});
        v.push_back({"mild-0.5",    profileMildNoise(80.0f)});
        v.push_back({"moderate-1.5", profileModerateNoise(80.0f)});
        SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
        v.push_back({"noise-3.0",   n3});
        v.push_back({"qrn",         profileQRN(80.0f)});
        v.push_back({"handkeyed",   profileHandKeyed(80.0f)});
        v.push_back({"worstcase",   profileWorstCase(80.0f)});
        v.push_back({"farns2.0",    profileFarnsworth(80.0f, 2.0f)});
        SignalParams f2n = profileFarnsworth(80.0f, 2.0f); f2n.noiseAmp = 1.5f;
        v.push_back({"farns2.0-n1.5", f2n});
        return v;
    }
}

TEST_CASE("gap classification under noise", "[cw][.][gap-noise]") {
    constexpr int SEEDS = 8;

    printf("\n=== Gap classification, detector-derived (%d seeds) ===\n", SEEDS);
    printf("raw = every detector event; filt = short elements rejected as the "
           "decoder does (staged_core.h:265)\n");
    printf("%-14s %7s %8s  %8s %8s %8s  %8s %8s %8s  %-24s %s\n",
           "profile", "gaps", "damaged",
           "clsErr%", "ditErr%", "clamp",
           "clsErr%", "ditErr%", "clamp",
           "confusion raw truth->cls", "");
    printf("%-14s %7s %8s  %-26s %-26s\n", "", "", "", "        --- raw ---", "        --- filt ---");

    GapNoiseStats clean, noisy;

    for (const auto& p : gapProfiles()) {
        GapNoiseStats g  = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, false);
        GapNoiseStats gf = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true);

        printf("%-14s %7d %7.1f%%  %7.1f%% %+8.1f %8d  %7.1f%% %+8.1f %8d  "
               "E>C%3d E>W%2d C>E%3d C>W%2d W>C%2d\n",
               p.name, g.matched + g.damaged, damagePct(g),
               classErrPct(g), g.ditErrPct, g.sourceHist[cw::AdaptiveTiming::GAPC_CLAMPED],
               classErrPct(gf), gf.ditErrPct, gf.sourceHist[cw::AdaptiveTiming::GAPC_CLAMPED],
               g.confusion[0][1], g.confusion[0][2],
               g.confusion[1][0], g.confusion[1][2], g.confusion[2][1]);

        CHECK(g.matched > 0);
        CHECK(gf.matched > 0);

        if (std::string(p.name) == "clean")     { clean = g; }
        if (std::string(p.name) == "noise-3.0") { noisy = g; }
    }
    printf("\n");

    // The instrument must be provably NOT noise-blind — this is the whole
    // reason the probe exists (docs §16.4). If heavy noise does not damage
    // more gaps than a clean signal, the chain is not reaching the classifier
    // and every number above is measuring something else.
    CHECK(damagePct(noisy) > damagePct(clean));
    CHECK(damagePct(clean) < 5.0f);

    // On a clean signal a structurally intact gap must never be misread: the
    // durations are exact and the estimator converges to them (§16.1).
    CHECK(classErrors(clean) <= SEEDS);
}

// §17.3.2 traced the noise-3.0 gap misclassification to a +38.9% dit
// OVERestimate under Schmitt+Kalman: gap centres are dit-derived, so an
// inflated dit drags char gaps into the element cluster. The promoted default
// is LR detector + log timing (§21). This measures whether that chain shares
// the bias — i.e. whether §17.3.2 still applies to the shipping decoder.
TEST_CASE("gap classification: default chain vs legacy (§23)", "[cw][.][gap-noise-lr]") {
    constexpr int SEEDS = 8;
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;

    struct Chain { const char* name; bool lr; cw::TimingStrategy strat; };
    const Chain chains[] = {
        {"Schmitt+Kalman (legacy)", false, cw::TIMING_KALMAN},
        {"LR+log (default)",        true,  cw::TIMING_LOG},
    };

    printf("\n=== noise-3.0 gap classification by chain (%d seeds) ===\n", SEEDS);
    printf("%-26s %8s %8s %10s  %s\n",
           "chain", "damaged", "clsErr%", "ditErr%", "confusion truth->cls");

    float legacyDitErr = 0, defaultDitErr = 0;
    int legacyClsErr = 0, defaultClsErr = 0;
    for (const auto& c : chains) {
        GapNoiseStats g = probeNoisyGaps(MSG_FULL(), n3, SEEDS, true, c.lr, c.strat);
        printf("%-26s %7.1f%% %7.1f%% %+10.1f  E>C%3d C>E%3d W>C%2d\n",
               c.name, damagePct(g), classErrPct(g), g.ditErrPct,
               g.confusion[0][1], g.confusion[1][0], g.confusion[2][1]);
        if (c.lr) { defaultDitErr = g.ditErrPct; defaultClsErr = classErrors(g); }
        else      { legacyDitErr  = g.ditErrPct; legacyClsErr  = classErrors(g); }
    }
    printf("\n");

    // The point of §23: the default's dit bias is far smaller than legacy's
    // +38.9%, and its gap misclassification is no worse. This is why the LR+log
    // promotion wins noise3.0 (§21) — the gap centres are no longer corrupted.
    CHECK(std::fabs(defaultDitErr) < std::fabs(legacyDitErr));
    CHECK(defaultClsErr <= legacyClsErr);
}
