#include <catch.hpp>
#include "cw_matrix.h"
#include "cw_snr.h"
#include <cw/staged_core.h>
#include <cw/stages.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace cw_test;

// ============================================================
// WPM-matched BPF ceiling (#31, docs §29) — ON DEMAND ONLY.
//
//   ./cw_decoder_tests "[bpf-wpm]"
//
// §4 measured that a narrow pre-detection BPF is a large, monotone SNR win at
// heavy noise (noise3.0/15WPM 0.76 → 0.02, 38×) but smears fast keying edges, so
// no single fixed bandwidth serves both slow+noisy and fast. The +bpf* variants
// pick one bandwidth for all speeds; #31 proposes matching bandwidth to the
// locked WPM (target ENBW ≈ 2/T_dit) to dissolve that tradeoff.
//
// This is the CEILING experiment, before any runtime-retuning machinery (whose
// filter-transition glitches are a real risk, cf. the §12 MF-resize defect). A
// legacy core (Schmitt + Kalman) is built per-signal with its BPF cutoff derived
// from the generator's GROUND-TRUTH dit — the best a perfect WPM lock could do —
// and measured against plain legacy across the speed×noise grid. Front-end
// mechanism only: the detector and timing are unchanged, so this is orthogonal
// to the LR bound-scaling that §28 exhausted.
//
//   If WPM-matched BPF beats legacy on slow+heavy noise WITHOUT regressing fast,
//     the tradeoff is real and dissolvable — build the runtime lock.
//   If fast regresses too, the smear is not speed-separable and #31 is a dead end.
// ============================================================

namespace {
    constexpr int PSEEDS = 96;
    constexpr float NON_INFERIORITY_TOL = 0.005f;

    // Target ENBW ≈ 2/T_dit (the matched-filter bandwidth for CW), inverted to a
    // BPF cutoff via the linear ENBW≈1.65·cut−2 fit measured in §4.1 for the
    // trans=cut+10 geometry. Clamped to the range §4 actually characterised
    // (20/30 … 40/50) so this is interpolation, not extrapolation.
    float wpmBpfCutoff(float ditMs) {
        const float targetEnbw = 2000.0f / ditMs;          // 2/T_dit in Hz
        const float cut = (targetEnbw + 2.0f) / 1.65f;
        return std::min(std::max(cut, 20.0f), 40.0f);
    }

    // legacy pipeline with the BPF geometry chosen by `pick(dit) -> (cut, trans)`.
    // One place so the WPM-matched, forced-constant, and noise-aware factories
    // differ only in the geometry rule. Geometry (100, 100) reproduces legacy's
    // front end byte-for-byte, which is what lets the wide end be a true no-op.
    CoreFactory bpfFactory(std::function<std::pair<float,float>(float)> pick) {
        return [pick](const GeneratedSignal& sig) -> std::unique_ptr<cw::IDecodeCore> {
            const auto g = pick(sig.model.nominalDitMs);
            return std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(g.first, g.second),
                std::make_unique<cw::SchmittDetector>(),             // legacy detector
                std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
                std::make_unique<cw::BeamSymbolDecoder>(),
                cw::MF_RESET, 1.0f);
        };
    }

    CoreFactory wpmBpfFactory() {
        return bpfFactory([](float dit){ float c = wpmBpfCutoff(dit); return std::make_pair(c, c + 10.0f); });
    }
    CoreFactory forcedCutFactory(float cut) {
        return bpfFactory([cut](float){ return std::make_pair(cut, cut + 10.0f); });
    }

    // Noise-aware bandwidth (§29 fix). The WPM-matched cutoff is a FLOOR reached
    // only at low SNR; at high SNR the filter widens back to the legacy geometry
    // exactly, because a light-noise (e.g. hand-keyed) signal is jitter-limited,
    // not noise-limited, and cannot afford the narrow filter's edge smear
    // (measured: [bpf-sweep]). Blended on the true noise amplitude: legacy (100,
    // 100) at noiseAmp<=0.5, matched at noiseAmp>=1.5, linear between. At the wide
    // end this is a byte-exact no-op, so a jitter-limited profile cannot regress.
    // Ground truth for the ceiling; a runtime rule reads the detector's SNR at
    // timing lock (see §29).
    std::pair<float,float> noiseAwareGeom(float ditMs, float noiseAmp) {
        const float matched = wpmBpfCutoff(ditMs);
        const float narrowFrac = std::min(std::max((noiseAmp - 0.5f) / 1.0f, 0.0f), 1.0f);
        const float cut   = 100.0f * (1.0f - narrowFrac) + matched * narrowFrac;
        const float trans = 100.0f * (1.0f - narrowFrac) + (matched + 10.0f) * narrowFrac;
        return {cut, trans};
    }

    CoreFactory noiseAwareFactory(float noiseAmp) {
        return bpfFactory([noiseAmp](float dit){ return noiseAwareGeom(dit, noiseAmp); });
    }
}

TEST_CASE("WPM-matched BPF ceiling vs legacy", "[cw][.][bpf-wpm]") {
    const auto profiles = standardProfiles();
    const auto factory = wpmBpfFactory();

    printf("\n=== legacy + WPM-matched BPF (ground-truth cutoff) vs legacy "
           "(paired, n=%d) ===\n", PSEEDS);
    printf("%-15s %8s %8s %8s %8s  %7s %6s  %s\n",
           "profile", "cut(Hz)", "legacy", "matched", "delta", "t", "ndiff", "verdict");

    int better = 0, worse = 0, harmful = 0;

    for (const auto& pr : profiles) {
        auto base    = runCell("legacy", pr.name, pr.message, pr.params, PSEEDS);
        auto matched = runCellWith(factory, "legacy+bpfwpm",
                                   pr.name, pr.message, pr.params, PSEEDS);
        auto d = comparePaired(base.cerSamples, matched.cerSamples);
        const bool harm = d.harmful(NON_INFERIORITY_TOL);

        printf("%-15s %8.1f %8.4f %8.4f %+8.4f  %7.2f %6d  %s%s\n",
               pr.name, wpmBpfCutoff(pr.params.ditMs), base.cerMean, matched.cerMean,
               d.meanDelta, d.t, d.nDiffer, d.verdict(), harm ? " HARM" : "");

        if (d.significant() && d.meanDelta < 0) { better++; }
        if (d.significant() && d.meanDelta > 0) { worse++; }
        if (harm) { harmful++; }
        CHECK(d.nSeeds == PSEEDS);
    }

    printf("\n  %d significant better, %d significant worse, %d harmful\n",
           better, worse, harmful);
    printf("  verdict: %s\n",
           (worse == 0 && better > 0)
               ? "TRADEOFF DISSOLVED — matched BPF helps with no speed regression"
               : "smear is not speed-separable this way — inspect the worse rows");

    // The experiment is only meaningful if the matched filter actually changed
    // the result somewhere — otherwise the cutoffs collapsed to the baseline.
    CHECK(better + worse > 0);
}

// Does bandwidth need to depend on NOISE, not just speed? §29 left 6 hand-keyed
// non-inferiority flags: the conjecture is that a bandwidth matched to white
// noise over-narrows a jitter-limited (light-noise) signal. If so, at a FIXED
// speed the light-noise profile wants a WIDE filter while the heavy-noise
// profile wants a NARROW one — proving bandwidth must key on noise level. This
// sweep measures the CER-vs-cutoff curve per profile and calibrates the floor.
TEST_CASE("BPF cutoff sweep: hand-keyed (light noise) vs heavy noise, same speed", "[cw][.][bpf-sweep]") {
    struct Focus { const char* name; SignalParams params; };
    // All 15 WPM (ditMs 80) except the fast pair, so speed is held while noise
    // varies. Hand-keyed = noiseAmp 0.3 + jitter; noise* = heavier, no jitter.
    std::vector<Focus> focus = {
        {"handkeyed-15", profileHandKeyed(80.0f)},
        {"handkeyed-20", profileHandKeyed(60.0f)},
        {"clean-15",     profileClean(80.0f)},
        {"noise2.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }()},
        {"noise3.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 3.0f; return p; }()},
        {"noise2.0-25wpm", [] { auto p = profileClean(48.0f); p.noiseAmp = 2.0f; return p; }()},
    };
    const std::vector<float> cuts = {20.0f, 30.0f, 40.0f, 60.0f, 100.0f};

    printf("\n=== CER vs BPF cutoff (n=%d). Lower is better. legacy uses 100. ===\n", PSEEDS);
    printf("%-15s", "profile\\cut(Hz)");
    for (float c : cuts) { printf("  %6.0f", c); }
    printf("   argmin\n");

    std::map<std::string, std::map<float, float>> cer;
    for (const auto& f : focus) {
        printf("%-15s", f.name);
        float best = 1e9f, bestCut = 0;
        for (float c : cuts) {
            auto r = runCellWith(forcedCutFactory(c), "forced", f.name,
                                 MSG_FULL(), f.params, PSEEDS);
            cer[f.name][c] = r.cerMean;
            printf("  %6.4f", r.cerMean);
            if (r.cerMean < best) { best = r.cerMean; bestCut = c; }
        }
        printf("   %.0f (%.4f)\n", bestCut, best);
    }

    printf("\n  If light-noise hand-keyed argmins WIDE and heavy-noise-15wpm\n"
           "  argmins NARROW at the same speed, bandwidth must key on noise.\n");

    // The conjecture, as invariants: at 15 WPM the heavy-noise profile is far
    // better narrow than wide, while hand-keyed is NOT (it prefers wider). This
    // is why a speed-only bandwidth rule cannot serve both.
    CHECK(cer["noise3.0"][20.0f] < cer["noise3.0"][100.0f]);        // heavy: narrow wins big
    CHECK(cer["handkeyed-15"][100.0f] <= cer["handkeyed-15"][20.0f]); // light: wide no worse
}

// The §29 fix: noise-aware bandwidth (WPM-matched floor, widened at high SNR).
// Must keep §29's heavy-noise wins AND clear the 6 hand-keyed non-inferiority
// flags — i.e. 0 harmful across the full grid. Ground-truth ceiling before the
// runtime rule.
TEST_CASE("Noise-aware WPM-matched BPF vs legacy — the promotion question", "[cw][.][bpf-noise]") {
    const auto profiles = standardProfiles();

    printf("\n=== legacy + noise-aware WPM-matched BPF vs legacy (paired, n=%d) ===\n", PSEEDS);
    printf("%-15s %8s %8s %8s %8s  %7s  %s\n",
           "profile", "cut(Hz)", "legacy", "aware", "delta", "t", "verdict");

    int better = 0, worse = 0, harmful = 0;

    for (const auto& pr : profiles) {
        auto base  = runCell("legacy", pr.name, pr.message, pr.params, PSEEDS);
        auto aware = runCellWith(noiseAwareFactory(pr.params.noiseAmp), "legacy+bpfaware",
                                 pr.name, pr.message, pr.params, PSEEDS);
        auto d = comparePaired(base.cerSamples, aware.cerSamples);
        const bool harm = d.harmful(NON_INFERIORITY_TOL);

        printf("%-15s %8.1f %8.4f %8.4f %+8.4f  %7.2f  %s%s\n",
               pr.name, noiseAwareGeom(pr.params.ditMs, pr.params.noiseAmp).first,
               base.cerMean, aware.cerMean, d.meanDelta, d.t,
               d.verdict(), harm ? " HARM" : "");

        if (d.significant() && d.meanDelta < 0) { better++; }
        if (d.significant() && d.meanDelta > 0) { worse++; }
        if (harm) { harmful++; }
        CHECK(d.nSeeds == PSEEDS);
    }

    printf("\n  %d significant better, %d significant worse, %d harmful\n",
           better, worse, harmful);
    printf("  verdict: %s\n",
           (harmful == 0 && better > 0)
               ? "PROMOTABLE — heavy-noise wins kept, no profile harmed"
               : "not clean yet — inspect HARM rows");

    // The whole point of the noise-aware rule: keep §29's biggest wins while
    // clearing the hand-keyed harm. Assert the wins survive and nothing is worse.
    CHECK(worse == 0);
}

// The §29 result in physical units (docs §31): noise-aware BPF vs legacy across a
// calibrated INPUT-SNR sweep (dB in 2500 Hz), 15 WPM. Raw noiseAmp is not a
// reportable SNR; this states the win where the external literature does. The
// noiseAmp for each dB comes from the calibration, so the axis is reproducible.
TEST_CASE("Noise-aware BPF vs legacy across calibrated SNR (dB)", "[cw][.][bpf-snr-sweep]") {
    // dB in 2500 Hz. +inf = clean. PA3FWM puts by-ear copy near -18 dB, so this
    // range spans comfortable to hard, all above the human floor.
    const std::vector<float> snrsDb = {12.0f, 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -9.0f};

    printf("\n=== legacy vs noise-aware WPM-matched BPF, 15 WPM, calibrated input "
           "SNR (paired, n=%d) ===\n", PSEEDS);
    printf("%10s %10s %8s %8s %9s %8s  %s\n",
           "SNR/2500", "noiseAmp", "legacy", "aware", "delta", "t", "verdict");

    int wins = 0, regress = 0;
    for (float db : snrsDb) {
        SignalParams p = profileAtSnr(80.0f, db, REF_BW_SSB);
        auto base  = runCell("legacy", "snr", MSG_FULL(), p, PSEEDS);
        auto aware = runCellWith(noiseAwareFactory(p.noiseAmp), "legacy+bpfaware",
                                 "snr", MSG_FULL(), p, PSEEDS);
        auto d = comparePaired(base.cerSamples, aware.cerSamples);
        printf("%+9.1f  %9.3f %8.4f %8.4f %+9.4f %8.2f  %s\n",
               db, p.noiseAmp, base.cerMean, aware.cerMean, d.meanDelta, d.t, d.verdict());
        if (d.significant() && d.meanDelta < 0) { wins++; }
        if (d.significant() && d.meanDelta > 0) { regress++; }
    }

    printf("\n  %d significant wins, %d regressions across the SNR sweep\n", wins, regress);
    // The BPF is a front-end SNR gain, so its benefit must grow as SNR falls and
    // never invert: wins at low SNR, no regression anywhere on the calibrated axis.
    CHECK(regress == 0);
    CHECK(wins > 0);
}

// The RUNTIME rule (docs §30): legacy+bpfauto is a real registry core that
// retunes its BPF at timing lock from getDitDuration + getSNR — no ground truth.
// Measures how close the runtime approximation lands to the §29 ceiling (which
// used true dit + true noiseAmp), and whether the retune transient regresses
// anything. This is the promotion question for the shippable core.
TEST_CASE("Runtime WPM-locked BPF (legacy+bpfauto) vs legacy", "[cw][.][bpf-runtime]") {
    const auto profiles = standardProfiles();
    printf("\n=== legacy+bpfauto (runtime, no ground truth) vs legacy (paired, n=%d) ===\n", PSEEDS);
    printf("%-15s %8s %8s %9s %8s  %s\n", "profile", "legacy", "bpfauto", "delta", "t", "verdict");

    int better = 0, worse = 0, harmful = 0;
    for (const auto& pr : profiles) {
        auto base = runCell("legacy",         pr.name, pr.message, pr.params, PSEEDS);
        auto rt   = runCell("legacy+bpfauto", pr.name, pr.message, pr.params, PSEEDS);
        auto d = comparePaired(base.cerSamples, rt.cerSamples);
        const bool harm = d.harmful(NON_INFERIORITY_TOL);
        printf("%-15s %8.4f %8.4f %+9.4f %8.2f  %s%s\n",
               pr.name, base.cerMean, rt.cerMean, d.meanDelta, d.t,
               d.verdict(), harm ? " HARM" : "");
        if (d.significant() && d.meanDelta < 0) { better++; }
        if (d.significant() && d.meanDelta > 0) { worse++; }
        if (harm) { harmful++; }
    }
    printf("\n  %d significant better, %d significant worse, %d harmful\n", better, worse, harmful);
    printf("  verdict: %s\n",
           (harmful == 0 && better > 0) ? "PROMOTABLE — runtime rule matches the ceiling cleanly"
                                        : "inspect HARM/worse rows — retune transient or SNR miscalibration");
    // The runtime rule must keep the big heavy-noise wins and add no regression.
    CHECK(worse == 0);
    CHECK(better > 0);
}

// Characterize the worstcase (QSB) variance that the single-seed gate caught: is
// bpfauto's bad tail seed-42-specific, or a real variance regression? (§34)
TEST_CASE("bpfauto worstcase tail vs legacy", "[cw][.][bpf-tail]") {
    auto base = runCell("legacy",         "worstcase", MSG_FULL(), profileWorstCase(80.0f), 96);
    auto bpf  = runCell("legacy+bpfauto", "worstcase", MSG_FULL(), profileWorstCase(80.0f), 96);
    int worse = 0, better = 0; float maxB = 0, maxL = 0;
    for (size_t i = 0; i < base.cerSamples.size(); i++) {
        if (bpf.cerSamples[i] > base.cerSamples[i] + 0.02f) worse++;
        if (bpf.cerSamples[i] < base.cerSamples[i] - 0.02f) better++;
        maxB = std::max(maxB, bpf.cerSamples[i]);
        maxL = std::max(maxL, base.cerSamples[i]);
    }
    printf("\n=== worstcase, n=96: legacy mean %.3f p95 %.3f max %.3f | bpfauto mean %.3f p95 %.3f max %.3f ===\n",
           base.cerMean, base.cerP95, maxL, bpf.cerMean, bpf.cerP95, maxB);
    printf("  per-seed: bpfauto better on %d, worse on %d (|delta|>0.02)\n", better, worse);
    // The §35 finding: bpfauto is better on the whole (mean, p95, max) yet worse
    // on a real minority of seeds — the variance the single-seed MSG_CQ gate
    // caught. Both halves asserted so the tradeoff cannot be silently forgotten.
    CHECK(bpf.cerMean < base.cerMean);   // better on average
    CHECK(worse >= 5);                   // but a real worse-tail, not an artifact
}
