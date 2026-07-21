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
        std::vector<float> ditOnMs;  // §38b: dit-cluster ON durations fed to classifyOn
        float trueDitMs = 0;
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
                                 cw::TimingStrategy strat = cw::TIMING_KALMAN,
                                 bool oracleDit = false, float guardFactor = 0.0f) {
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
            // Confound probe: force the classifier's gap centres to the true dit
            // while leaving the detector event stream (and thus the matched-gap
            // set) identical to the estimated-dit run.
            if (oracleDit && sig.model.ditMs > 0) { timing.setDitOverride(sig.model.ditMs); }
            if (guardFactor > 0.0f) { timing.setKalmanGuardFactor(guardFactor); }

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
                        if (onMs >= floorMs) {
                            timing.classifyOn(onMs);
                            // §38b: the dit cluster the estimator actually builds
                            // from (short ONs), captured post-gate as the Kalman
                            // sees them, to test mean-vs-median tail pull.
                            if (sig.model.ditMs > 0 && onMs < 2.0f * sig.model.ditMs) {
                                out.ditOnMs.push_back(onMs);
                            }
                        }
                    }
                    lastUp = e.sample;
                }
            }

            if (sig.model.ditMs > 0) {
                out.ditErrPct += 100.0f * (timing.getDitDuration() - sig.model.ditMs)
                               / sig.model.ditMs;
                out.trueDitMs = sig.model.ditMs;
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

// §38 CONFOUND CHECK for #25 (gap ambiguity inside the beam).
//
// #25 proposes propagating gap ambiguity as soft beam branches instead of a
// hard argmax. That is only the right lever if the misclassification is a
// genuine *ambiguity* — the measured gap duration sits between two centres.
// If instead it is driven by a BIASED dit estimate (the +38.9% overestimate,
// §17.3.2), the centres themselves are wrong and soft branching would only
// spread probability mass symmetrically about the wrong boundary — the fix
// would be a dit-bias correction, not the beam.
//
// This substitutes the generator's TRUE dit into the classifier while holding
// the detector event stream (hence the matched-gap set) identical, and reports
// how much misclassification survives. The residual is the part #25 can address;
// the removed part belongs to dit estimation, upstream of the beam.
TEST_CASE("gap classification: dit-bias confound (§38)", "[cw][.][gap-noise-confound]") {
    constexpr int SEEDS = 8;

    struct Prof { const char* name; SignalParams params; };
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
    const Prof profs[] = {
        {"noise-3.0", n3},
        {"worstcase", profileWorstCase(80.0f)},
    };

    printf("\n=== Gap misclassification: estimated dit vs TRUE dit (%d seeds) ===\n", SEEDS);
    printf("est  = shipping default (Schmitt+Kalman), dit self-estimated\n");
    printf("true = same detector events, classifier fed the generator's dit\n");
    printf("%-11s %-4s %8s %8s  %-28s\n",
           "profile", "dit", "ditErr%", "clsErr%", "confusion truth->cls");

    for (const auto& p : profs) {
        GapNoiseStats est  = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true,
                                            false, cw::TIMING_KALMAN, false);
        GapNoiseStats tru  = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true,
                                            false, cw::TIMING_KALMAN, true);

        auto row = [&](const char* tag, const GapNoiseStats& g) {
            printf("%-11s %-4s %+8.1f %7.1f%%  E>C%3d E>W%2d C>E%3d C>W%2d W>C%2d W>E%2d\n",
                   p.name, tag, g.ditErrPct, classErrPct(g),
                   g.confusion[0][1], g.confusion[0][2],
                   g.confusion[1][0], g.confusion[1][2],
                   g.confusion[2][1], g.confusion[2][0]);
        };
        row("est", est);
        row("true", tru);

        const int estE  = classErrors(est);
        const int truE  = classErrors(tru);
        printf("            -> true dit removes %d of %d errors (%.0f%%); "
               "residual %d is genuine ambiguity (#25's target)\n\n",
               estE - truE, estE,
               estE > 0 ? 100.0f * (estE - truE) / estE : 0.0f, truE);

        CHECK(est.matched > 0);
        CHECK(tru.matched > 0);
        // The confound is real: fixing the dit removes a substantial share of
        // the misclassification. If this fails, the dit bias is NOT load-bearing
        // and #25 can proceed on the beam alone.
        CHECK(truE < estE);
    }
}

// §38b — SOURCE of the dit overestimate, aimed before any fix (user request).
//
// The +38.9% dit bias (§38) has two candidate sources, with different fixes:
//   (a) detector edge widening — the Schmitt holds key-down longer under noise,
//       so every ON is geometrically stretched before timing sees it. Measured
//       by onStretchPct (up-delay minus down-delay vs true dit). Fix lives in
//       the detector.
//   (b) estimator tail-pull — even if ONs are only mildly widened on average,
//       the Kalman tracks a mean and a noise-stretched right tail drags it up.
//       Measured by mean vs median vs p25 of the dit-cluster ONs the estimator
//       consumes. Fix is a robust dit estimator (median/mode), cheap and local.
//
// This does not fix anything; it points the fix.
TEST_CASE("gap classification: dit-bias source (§38b)", "[cw][.][gap-dit-source]") {
    constexpr int SEEDS = 8;

    struct Prof { const char* name; SignalParams params; };
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
    const Prof profs[] = {
        {"clean",     profileClean(80.0f)},
        {"noise-3.0", n3},
        {"worstcase", profileWorstCase(80.0f)},
    };

    auto pctile = [](std::vector<float> v, float q) {
        if (v.empty()) { return 0.0f; }
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
    };
    auto meanOf = [](const std::vector<float>& v) {
        if (v.empty()) { return 0.0f; }
        float s = 0; for (float x : v) { s += x; } return s / v.size();
    };

    printf("\n=== Dit overestimate: detector geometry vs estimator tail-pull (%d seeds) ===\n", SEEDS);
    printf("detector = onStretchPct (edge widening, source a)\n");
    printf("estimator = Kalman ditErr; ON-cluster mean/median/p25 err vs true dit (source b)\n");
    printf("%-11s %10s %10s   %8s %8s %8s\n",
           "profile", "onStretch%", "kalmanErr%", "on-mean%", "on-med%", "on-p25%");

    for (const auto& p : profs) {
        auto det = measureDetectorMulti(MSG_FULL(), p.params, SEEDS);
        GapNoiseStats g = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true);

        const float d = g.trueDitMs;
        auto errPct = [&](float v) { return d > 0 ? 100.0f * (v - d) / d : 0.0f; };
        const float onMean = errPct(meanOf(g.ditOnMs));
        const float onMed  = errPct(pctile(g.ditOnMs, 0.5f));
        const float onP25  = errPct(pctile(g.ditOnMs, 0.25f));

        // Same probe, same seeds, different timing strategy: isolates whether the
        // inflation is V1's ungated learning (V2 gates on classification
        // confidence) or the linear-ms coordinate (LOG is multiplicative).
        GapNoiseStats v2  = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true,
                                           false, cw::TIMING_KALMAN_V2);
        GapNoiseStats lg  = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true,
                                           false, cw::TIMING_LOG);
        GapNoiseStats gd  = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true,
                                           false, cw::TIMING_KALMAN_GUARD);

        printf("%-11s %+10.1f %+10.1f   %+8.1f %+8.1f %+8.1f   v2=%+.1f log=%+.1f guard=%+.1f\n",
               p.name, det.mean.onStretchPct, g.ditErrPct, onMean, onMed, onP25,
               v2.ditErrPct, lg.ditErrPct, gd.ditErrPct);

        CHECK(!g.ditOnMs.empty());
    }
    printf("\nRead: kalmanErr(V1) driven by absorbed dahs (boundary=2*ditEst runaway);\n");
    printf("      v2/log err << V1 err  => the confidence gate / log coord is the fix.\n");

    // §39 factor sweep: the guard band (factor*ditEst, 2*ditEst) also contains
    // jittered legitimate dits. Too low a factor blocks them and collapses ditEst
    // downward; too high catches no absorbed dahs. Find the factor whose noise-3.0
    // dit error is nearest zero without perturbing clean.
    printf("\n=== §39 dah-guard factor sweep (dit err %% vs true) ===\n");
    printf("%-11s %8s %8s %8s %8s %8s %8s\n",
           "profile", "1.5", "1.6", "1.7", "1.8", "1.9", "1.95");
    const float factors[] = {1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 1.95f};
    for (const auto& p : profs) {
        printf("%-11s", p.name);
        for (float f : factors) {
            GapNoiseStats gd = probeNoisyGaps(MSG_FULL(), p.params, SEEDS, true,
                                              false, cw::TIMING_KALMAN_GUARD, false, f);
            printf(" %+8.1f", gd.ditErrPct);
        }
        printf("\n");
    }
}
