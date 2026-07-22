#include <catch.hpp>
#include "cw_test_signals.h"
#include "cw_matrix.h"
#include "cw_bench_stats.h"
#include "cw_oracle.h"
#include "cw_snr.h"
#include "cw_detector_score.h"

using namespace cw_test;

// §39 decode-level verdict for the asymmetric dah-guard. The dit-source probe
// (§38b) showed the guard collapses ditEst to -57% at noise-3.0 regardless of
// factor. This confirms whether that estimator collapse produces garbage decode
// vs legacy and V2, across the regimes that matter.
TEST_CASE("ditguard decode verdict (§39)", "[cw][.][ditguard]") {
    constexpr int SEEDS = 48;

    struct Prof { const char* name; SignalParams params; };
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
    SignalParams n4 = profileClean(80.0f); n4.noiseAmp = 4.0f;
    const Prof profs[] = {
        {"clean",     profileClean(80.0f)},
        {"noise-2.0", [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }()},
        {"noise-3.0", n3},
        {"noise-4.0", n4},
        {"handkeyed", profileHandKeyed(80.0f)},
        {"worstcase", profileWorstCase(80.0f)},
    };
    const char* cores[] = {"legacy", "legacy+ditguard", "legacy+kalman2"};

    printf("\n=== §39 dah-guard decode CER (mean, %d seeds) ===\n", SEEDS);
    printf("%-11s", "profile");
    for (const char* c : cores) { printf(" %18s", c); }
    printf("\n");

    for (const auto& p : profs) {
        printf("%-11s", p.name);
        for (const char* c : cores) {
            auto cell = runCell(c, p.name, MSG_FULL(), p.params, SEEDS);
            printf(" %18.4f", cell.cerMean);
        }
        printf("\n");
    }
}

// §40 — is +kalman2's noise-4.0 regression load-bearing or a variance artifact?
//
// kalman2 (V2 confidence gate) is the symmetric fix for the dit runaway (§38b)
// and dominates legacy on 5 of 6 profiles, blocked only by noise-4.0. §35 found
// bpfauto's block was a variance/tail miss, not a mean regression. This applies
// the SAME paired n=96 gate the promotion test uses to decide whether kalman2's
// noise-4.0 delta is a significant paired regression (real block) or ns
// (underpowered — then kalman2 is the campaign's strongest promotion candidate).
namespace {
    void adjudicateVsLegacy(const char* candidate, int PSEEDS = 96) {
        constexpr float TOL = 0.005f;   // non-inferiority tolerance, as [promotion]
        const auto profiles = standardProfiles();

        printf("\n=== legacy+%s vs legacy (paired, n=%d) ===\n", candidate, PSEEDS);
        printf("%-16s %9s %9s %10s %8s %6s %10s  %s\n",
               "profile", "legacy", "cand", "delta", "t", "ndiff", "worstcase", "verdict");

        int better = 0, worse = 0, ns = 0, harmful = 0;
        for (const auto& pr : profiles) {
            auto base = runCell("legacy", pr.name, pr.message, pr.params, PSEEDS);
            auto cand = runCell(std::string("legacy+") + candidate,
                                pr.name, pr.message, pr.params, PSEEDS);
            auto d = comparePaired(base.cerSamples, cand.cerSamples);

            const bool harm = d.harmful(TOL);
            const char* v = !d.significant() ? "ns" : (d.meanDelta < 0 ? "BETTER" : "WORSE");
            printf("%-16s %9.4f %9.4f %+10.4f %6.2f %6d %+10.4f  %s%s\n",
                   pr.name, base.cerMean, cand.cerMean, d.meanDelta, d.t,
                   d.nDiffer, d.worstCaseDelta(), v, harm ? " HARM" : "");

            if (harm) { harmful++; }
            if (!d.significant())     { ns++; }
            else if (d.meanDelta < 0) { better++; }
            else                      { worse++; }
        }
        printf("  %d better, %d worse, %d ns, %d harmful  ->  %s\n",
               better, worse, ns, harmful,
               (harmful == 0 && better > 0) ? "PROMOTABLE" : "blocked");
    }
}

// §40/§41 — is +kalman2's noise-4.0 regression load-bearing? And does speed-gating
// the confidence gate (§41, kalman2s) separate the slow/moderate wins from the
// fast-CW regression §40 exposed?
TEST_CASE("kalman2 recheck: paired n=96 vs legacy (§40/§41)", "[cw][.][kalman2-recheck]") {
    adjudicateVsLegacy("kalman2");
    adjudicateVsLegacy("kalman2s");
}

// §42: both residual HARM are better/neutral-mean, so more power should clear the
// non-inferiority bound (confirmed for noise4.0 at n=768). This re-adjudicates
// kalman2s at n=192 to check whether the whole gate goes 0-harmful with power.
TEST_CASE("kalman2s adjudication at higher power (§42)", "[cw][.][kalman2s-power]") {
    adjudicateVsLegacy("kalman2s", 192);
}

// §46 — is bimodal's hand-keyed win noise-robust? Decides the selector design:
// if bimodal loses its edge as noise rises, the selection signal is simply
// "clean hand-keyed" (high jitter + low noise), and the switch is a narrow,
// safe extension of kalman2s. If bimodal stays best under noise, the selector
// needs a richer regime signal. Noise labelled in the real VFO band (§ REF_BW_VFO
// = 3000 Hz, CW_VFO_BANDWIDTH). n=192 (§45: hand-keyed MDE ~0.028 at 192).
TEST_CASE("bimodal hand-keyed win vs noise (§46)", "[cw][.][bimodal-noise]") {
    constexpr int SEEDS = 192;
    const char* cores[] = {"legacy", "legacy+kalman2s", "legacy+bimodal", "legacy+log"};
    const float amps[]  = {0.3f, 0.7f, 1.0f, 1.5f, 2.0f};
    const float dits[]  = {80.0f, 48.0f};   // 15, 25 WPM

    printf("\n=== §46 hand-keyed CER vs noise (jitter0.15+bias0.1), n=%d ===\n", SEEDS);
    printf("noise in dB re CW_VFO_BANDWIDTH 3000Hz; * marks the best core in each row\n");
    printf("%4s %8s %6s", "wpm", "noiseAmp", "dB");
    for (const char* c : cores) { printf(" %14s", c + 7); }  // strip "legacy"
    printf("\n");

    for (float dit : dits) {
        const int wpm = (int)(1200.0f / dit + 0.5f);
        for (float amp : amps) {
            SignalParams p = profileHandKeyed(dit);
            p.noiseAmp = amp;
            const float db = noiseAmpToSnrDb(amp, REF_BW_VFO, 1.0f, 8000.0f);
            char name[40]; snprintf(name, sizeof name, "hk%d-n%.1f", wpm, amp);
            float cer[4]; int bestI = 0;
            for (int i = 0; i < 4; i++) {
                cer[i] = runCell(cores[i], name, MSG_FULL(), p, SEEDS).cerMean;
                if (cer[i] < cer[bestI]) { bestI = i; }
            }
            printf("%4d %8.1f %6.1f", wpm, amp, db);
            for (int i = 0; i < 4; i++) { printf(" %13.4f%s", cer[i], i == bestI ? "*" : " "); }
            printf("\n");
        }
    }
    printf("\nIf bimodal(*) only at low noiseAmp -> selection signal is 'clean hand-keyed'.\n");
}

// §48 — adjudicate the selector vs the default at the pre-committed n=384 (§45).
TEST_CASE("select adjudication n=384 (§48)", "[cw][.][select-adj]") {
    adjudicateVsLegacy("select", 384);
}

// §48 — n=768 escalation on the select HARM cells (§42 protocol): which clear
// (favourable mean + shrinking stderr) vs which are genuine garbage-cell bounds.
TEST_CASE("select HARM escalation n=768 (§48)", "[cw][.][select-esc]") {
    constexpr int SEEDS = 768;
    constexpr float TOL = 0.005f;
    struct Prof { const char* name; SignalParams params; };
    auto n = [](float dit, float amp){ auto p = profileClean(dit); p.noiseAmp = amp; return p; };
    auto hk = [](float dit, float amp){ auto p = profileHandKeyed(dit); p.noiseAmp = amp; return p; };
    const Prof profs[] = {
        {"noise4.0",       n(80.0f,4.0f)}, {"noise4.0-25wpm", n(48.0f,4.0f)},
        {"noise4.0-30wpm", n(40.0f,4.0f)}, {"noise3.0-25wpm", n(48.0f,3.0f)},
        {"noise3.0-30wpm", n(40.0f,3.0f)}, {"hk25-n1.5",      hk(48.0f,1.5f)},
    };
    printf("\n=== §48 select HARM cells at n=%d ===\n", SEEDS);
    printf("%-16s %9s %9s %9s %8s %10s  %s\n",
           "profile", "legacy", "select", "mean", "t", "bound", "verdict");
    for (const auto& p : profs) {
        auto a = runCell("legacy",        p.name, MSG_FULL(), p.params, SEEDS);
        auto b = runCell("legacy+select", p.name, MSG_FULL(), p.params, SEEDS);
        auto d = comparePaired(a.cerSamples, b.cerSamples);
        printf("%-16s %9.4f %9.4f %+9.4f %8.2f %+10.4f  %s\n",
               p.name, a.cerMean, b.cerMean, d.meanDelta, d.t, d.worstCaseDelta(),
               d.harmful(TOL) ? "HARM" : "clean");
    }
}

// §48 — is the selector's gate-acquisition overhead a real SHORT-message
// regression? The n=384 adjudication used MSG_FULL; the failing benchmark gates
// use MSG_CQ (short). Measure select vs legacy vs kalman2s on MSG_CQ, multi-seed.
TEST_CASE("select on short messages (§48)", "[cw][.][select-short]") {
    constexpr int SEEDS = 96;
    struct Prof { const char* name; SignalParams params; };
    const Prof profs[] = {
        {"moderate-1.5", profileModerateNoise(80.0f)},
        {"handkeyed-20", profileHandKeyed(60.0f)},
        {"handkeyed-25", profileHandKeyed(48.0f)},
    };
    printf("\n=== §48 MSG_CQ (short) multi-seed, n=%d ===\n", SEEDS);
    printf("%-14s %9s %9s %9s   %10s %6s\n", "profile", "legacy", "kal2s", "select", "sel-leg", "t");
    for (const auto& p : profs) {
        auto l = runCell("legacy",         p.name, MSG_CQ(), p.params, SEEDS);
        auto k = runCell("legacy+kalman2s",p.name, MSG_CQ(), p.params, SEEDS);
        auto s = runCell("legacy+select",  p.name, MSG_CQ(), p.params, SEEDS);
        auto d = comparePaired(l.cerSamples, s.cerSamples);
        printf("%-14s %9.4f %9.4f %9.4f   %+10.4f %6.2f\n",
               p.name, l.cerMean, k.cerMean, s.cerMean, d.meanDelta, d.t);
    }
    printf("\nIf sel-leg >0 significantly on MSG_CQ, the gate hurts short messages.\n");
}

// §48 — attribute the moderate-noise/contest ratchet failures: inherited from
// kalman2s (V2 timing) or added by the selector mechanism? These profiles are
// NOT in standardProfiles, so the [promotion] gate missed them.
TEST_CASE("select ratchet attribution (§48)", "[cw][.][select-ratchet]") {
    constexpr int SEEDS = 24;
    printf("\n=== §48 moderate-noise & contest, n=%d (ratchet: 0.0 / 0.008) ===\n", SEEDS);
    struct C { const char* name; const char* msg; SignalParams p; float ratchet; };
    const C cases[] = {
        {"moderate-noise", MSG_FULL(),  profileModerateNoise(80.0f), 0.0f},
        {"contest-20wpm",  MSG_MIXED(), profileContest(60.0f),       0.008f},
    };
    for (const auto& c : cases) {
        printf("%-16s ratchet=%.3f\n", c.name, c.ratchet);
        for (const char* core : {"legacy", "legacy+kalman2s", "legacy+log", "legacy+select"}) {
            auto s = runCell(core, c.name, c.msg, c.p, SEEDS);
            printf("    %-18s mean=%.4f p95=%.4f worst=%.4f\n",
                   core, s.cerMean, s.cerP95, s.cerP95);
        }
    }
}

// §48 — the operator-change validation the re-armable latch exists for. Two
// operators back-to-back (different WPM => dit drifts >25%): select must re-arm at
// the boundary and adapt, not stay frozen on the first operator. Decode the
// concatenation and check both halves decode — vs the failure mode where a frozen
// mode garbles the second operator.
TEST_CASE("select adapts to operator change (§48)", "[cw][.][select-opchange]") {
    constexpr int SEEDS = 48;
    // Op A: hand-keyed 15 WPM (jittered -> log). Op B: machine 30 WPM (precise -> kalman2s).
    int good = 0, total = 0;
    float cerSum = 0;
    for (int s = 0; s < SEEDS; s++) {
        SignalParams a = profileHandKeyed(80.0f); a.seed = 1000 + s * 7919u;
        SignalParams b = profileClean(40.0f);     b.seed = 7000 + s * 7919u; b.noiseAmp = 0.5f;
        auto sa = generateMessage("CQ CQ DE W1AW", a);
        auto sb = generateMessage("TEST DE K2ORS", b);
        std::vector<dsp::complex_t> iq = sa.samples;
        iq.insert(iq.end(), sb.samples.begin(), sb.samples.end());
        std::string ref = normalize(sa.sourceText) + " " + normalize(sb.sourceText);

        cw::Channel ch;
        ch.initWithCore(0, a.toneFreq,
            cw::findCore("legacy+select")->make(), "opchange");
        for (int off = 0; off < (int)iq.size(); off += 512) {
            int n = std::min(512, (int)iq.size() - off);
            ch.process(n, &iq[off]);
        }
        auto sc = score(ref, ch.text.getText());
        cerSum += sc.cer;
        // "second operator decoded" heuristic: the tail callsign K2ORS present
        if (ch.text.getText().find("K2") != std::string::npos ||
            ch.text.getText().find("ORS") != std::string::npos) { good++; }
        total++;
    }
    printf("\n=== §48 operator-change: 15wpm hand -> 30wpm machine, %d seeds ===\n", SEEDS);
    printf("mean CER (concatenation) = %.4f;  2nd-operator recovered = %d/%d\n",
           cerSum / SEEDS, good, total);
    CHECK(good > total / 2);   // re-arm must let the majority recover the 2nd operator
}

// §50 — WHY does kalman2s/log make a one-char error on moderate-noise that legacy
// (V1) does not? Find the failing seed(s) and dump legacy vs kalman2s vs log decodes.
TEST_CASE("moderate-noise one-char error diagnosis (§50)", "[cw][.][onechar]") {
    const std::string ref = normalize(generateMessage(MSG_FULL(), profileModerateNoise(80.0f)).sourceText);
    printf("\n=== §50 moderate-noise per-seed decode (ref len %zu) ===\n", ref.size());
    printf("ref: %s\n", ref.c_str());
    for (int i = 0; i < 24; i++) {
        SignalParams p = profileModerateNoise(80.0f);
        p.seed = 1000 + (unsigned)i * 7919u;
        auto decleg = normalize(decode(MSG_FULL(), p));   // default = legacy
        std::string dk, dl;
        for (const char* core : {"legacy+kalman2s", "legacy+log"}) {
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.initWithCore(0, p.toneFreq, cw::findCore(core)->make(), "d");
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            (std::string(core).find("kalman2s") != std::string::npos ? dk : dl) = normalize(ch.text.getText());
        }
        const bool legOk = decleg == ref, kOk = dk == ref, lOk = dl == ref;
        if (!kOk || !lOk || !legOk) {
            printf("seed %d (i=%d): leg=%s k2s=%s log=%s\n", p.seed, i,
                   legOk?"OK":"ERR", kOk?"OK":"ERR", lOk?"OK":"ERR");
            if (!kOk) { printf("   k2s: %s\n", dk.c_str()); }
            if (!legOk) { printf("   leg: %s\n", decleg.c_str()); }
        }
    }
}

// §51 — robust thresholds for the 4 gates being recalibrated from single-seed/n=24
// artifacts. Reports the SHIPPING decoder (default=select) mean + 2*stderr, the
// multiseed test's stated convention, at a robust n.
TEST_CASE("recalibration thresholds (§51)", "[cw][.][recal]") {
    struct G { const char* label; const char* msg; SignalParams p; int n; };
    const G gates[] = {
        {"bench145 modCQ",   MSG_CQ(),      profileModerateNoise(80.0f), 96},
        {"bench163 hk25CQ",  MSG_CQ(),      profileHandKeyed(48.0f),     96},
        {"bench497 hk20",    "CQ DE W1AW",  profileHandKeyed(60.0f),     96},
        {"multiseed modFULL",MSG_FULL(),    profileModerateNoise(80.0f), 96},
        {"matrix hk10",      "CQ DE W1AW",  profileHandKeyed(120.0f),    96},
        {"matrix hk15",      "CQ DE W1AW",  profileHandKeyed(80.0f),     96},
        {"matrix hk20",      "CQ DE W1AW",  profileHandKeyed(60.0f),     96},
        {"matrix hk25",      "CQ DE W1AW",  profileHandKeyed(48.0f),     96},
    };
    printf("\n=== §51 select mean + 2*stderr (n=96) for recalibration ===\n");
    printf("%-20s %8s %8s %10s\n", "gate", "mean", "2stderr", "mean+2se");
    for (const auto& g : gates) {
        auto s = decodeAndScoreMulti(g.msg, g.p, g.n);
        printf("%-20s %8.4f %8.4f %10.4f\n", g.label, s.mean, 2.0f*s.stderrMean, s.mean + 2.0f*s.stderrMean);
    }
}

// §51 — do the getSNR values separate contest (light noise -> V1) from moderate
// (heavy -> V2)? The SNR-graded routing fix depends on a clean threshold.
TEST_CASE("getSNR per profile (§51)", "[cw][.][snr-sep]") {
    struct P { const char* name; SignalParams p; };
    const P profs[] = {
        {"clean",        profileClean(80.0f)},
        {"contest",      profileContest(60.0f)},
        {"moderate-1.5", profileModerateNoise(80.0f)},
        {"noise2.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }()},
        {"noise3.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 3.0f; return p; }()},
        {"handkeyed-20", profileHandKeyed(60.0f)},
    };
    printf("\n=== §51 mean getSNR (post-lock) per profile, 24 seeds ===\n");
    printf("%-14s %8s\n", "profile", "getSNR");
    for (const auto& pr : profs) {
        float snrSum = 0; int cnt = 0;
        for (int i = 0; i < 24; i++) {
            SignalParams p = pr.p; p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.init(0, p.toneFreq);
            float blkSum = 0; int blkN = 0;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
                if (ch.snr > 0) { blkSum += ch.snr; blkN++; }
            }
            if (blkN) { snrSum += blkSum / blkN; cnt++; }
        }
        printf("%-14s %8.2f\n", pr.name, snrSum / cnt);
    }
}

// §50 — WHY does V2 handle contest worse than V1? Dump per-seed legacy vs kalman2s
// decodes, find the error pattern (QRM + jitter 0.10 + light noise 0.8, 20 WPM).
TEST_CASE("contest V2 error diagnosis (§50)", "[cw][.][contest-err]") {
    const std::string ref = normalize(generateMessage(MSG_MIXED(), profileContest(60.0f)).sourceText);
    printf("\n=== §50 contest per-seed decode (ref: %s) ===\n", ref.c_str());
    int legErrs = 0, k2sErrs = 0;
    for (int i = 0; i < 48; i++) {
        SignalParams p = profileContest(60.0f);
        p.seed = 1000 + (unsigned)i * 7919u;
        auto sig = generateMessage(MSG_MIXED(), p);
        auto dec = [&](const char* core) {
            cw::Channel ch; ch.initWithCore(0, p.toneFreq, cw::findCore(core)->make(), "d");
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            return normalize(ch.text.getText());
        };
        auto dl = dec("legacy"), dk = dec("legacy+kalman2s");
        if (dl != ref) { legErrs++; }
        if (dk != ref) {
            k2sErrs++;
            if (dl == ref) {   // V2 errs where V1 is clean — the diagnostic case
                printf("seed %d: V1 OK, V2 ERR\n  ref: %s\n  V2:  %s\n", p.seed, ref.c_str(), dk.c_str());
            }
        }
    }
    printf("legacy errs=%d/48, kalman2s errs=%d/48\n", legErrs, k2sErrs);
}

// §50 — is the moderate-noise 0.0 ratchet a small-n artifact? Measure legacy's
// OWN moderate-noise CER at increasing n. If it rises above 0, the ratchet that
// blocks select is based on a lucky n=24 subset, not a robust legacy property.
TEST_CASE("moderate-noise ratchet is n-dependent (§50)", "[cw][.][ratchet-n]") {
    printf("\n=== §50 legacy & kalman2s on profileModerateNoise vs n ===\n");
    printf("%6s %10s %10s\n", "n", "legacy", "kalman2s");
    for (int n : {24, 48, 96, 192}) {
        auto l = decodeAndScoreMulti(MSG_FULL(), profileModerateNoise(80.0f), n).mean;
        auto k = runCell("legacy+kalman2s", "m", MSG_FULL(), profileModerateNoise(80.0f), n).cerMean;
        printf("%6d %10.4f %10.4f  %s\n", n, l, k, l > 0.0f ? "<- legacy FAILS its own 0.0 ratchet" : "");
    }
    printf("\n=== contest ratchet (0.008): legacy vs kalman2s vs select ===\n");
    printf("%6s %10s %10s %10s\n", "n", "legacy", "kalman2s", "select");
    for (int n : {24, 96, 192}) {
        auto l = runCell("legacy",          "c", MSG_MIXED(), profileContest(60.0f), n).cerMean;
        auto k = runCell("legacy+kalman2s", "c", MSG_MIXED(), profileContest(60.0f), n).cerMean;
        auto s = runCell("legacy+select",   "c", MSG_MIXED(), profileContest(60.0f), n).cerMean;
        printf("%6d %10.4f %10.4f %10.4f\n", n, l, k, s);
    }
}

// §50 — V1 vs V2 noise crossover: V2's advantage is only at heavy noise, so
// routing light-noise machine signals to V1 could pass the moderate/contest
// ratchets while keeping V2's heavy-noise wins (the fix hypothesis).
TEST_CASE("V1 vs V2 noise crossover (§50)", "[cw][.][v1v2-cross]") {
    constexpr int SEEDS = 48;
    printf("\n=== §50 legacy(V1) vs kalman2s(V2) vs noise (MSG_FULL, n=%d) ===\n", SEEDS);
    printf("%-10s %8s %8s %8s\n", "noiseAmp", "V1", "V2", "winner");
    for (float amp : {0.8f, 1.0f, 1.5f, 1.8f, 2.0f, 2.5f, 3.0f}) {
        SignalParams p = profileClean(80.0f); p.noiseAmp = amp;
        auto v1 = runCell("legacy",          "n", MSG_FULL(), p, SEEDS).cerMean;
        auto v2 = runCell("legacy+kalman2s", "n", MSG_FULL(), p, SEEDS).cerMean;
        printf("%-10.1f %8.4f %8.4f %8s\n", amp, v1, v2, v1 <= v2 ? "V1" : "V2");
    }
}

// §50 — mechanism: at the dropped word gap, what are the V1 vs V2 dit estimates
// and gap centres? Replay the real detector's events through both timings.
TEST_CASE("word-gap drop mechanism (§50)", "[cw][.][onechar-mech]") {
    SignalParams p = profileModerateNoise(80.0f);
    p.seed = 48514;   // §50 failing seed
    auto sig = generateMessage(MSG_FULL(), p);
    auto rec = std::make_unique<RecordingDetector>(std::make_unique<cw::SchmittDetector>());
    auto* probe = rec.get();
    cw::Channel ch;
    ch.initWithCore(0, p.toneFreq, std::make_unique<cw::StagedCore>(
        std::make_unique<cw::EnvelopeFrontEnd>(), std::move(rec),
        std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
        std::make_unique<cw::BeamSymbolDecoder>()), "d");
    for (int off = 0; off < (int)sig.samples.size(); off += 512) {
        int n = std::min(512, (int)sig.samples.size() - off);
        ch.process(n, &sig.samples[off]);
    }
    // Replay detector durations through V1 and V2, print the long (char/word) gaps.
    cw::AdaptiveTiming v1, v2;
    v1.init(1000.0f, cw::TIMING_KALMAN); v2.init(1000.0f, cw::TIMING_KALMAN_V2);
    printf("\n=== §50 seed %d: char/word gaps, V1 vs V2 (dit-derived centres) ===\n", p.seed);
    printf("%6s  %6s %5s %-6s  %6s %5s %-6s\n", "gapMs", "ditV1", "×dit", "clsV1", "ditV2", "×dit", "clsV2");
    long long lastDown = -1, lastUp = -1;
    auto gname = [](cw::Gap g){ return g==cw::WORD_GAP?"WORD":g==cw::CHAR_GAP?"CHAR":"elem"; };
    for (const auto& e : probe->events()) {
        if (e.keyDown) {
            if (lastUp >= 0) {
                float ms = (float)(e.sample - lastUp);
                float d1 = v1.getDitDuration(), d2 = v2.getDitDuration();
                auto g1 = v1.classifyOff(ms), g2 = v2.classifyOff(ms);
                if (ms > 1.8f * d1) {   // long gaps only
                    printf("%6.0f  %6.1f %5.1f %-6s  %6.1f %5.1f %-6s%s\n", ms,
                           d1, ms/d1, gname(g1.gap), d2, ms/d2, gname(g2.gap),
                           g1.gap != g2.gap ? "  <-- DIFFER" : "");
                }
            }
            lastDown = e.sample;
        } else {
            if (lastDown >= 0) { float on=(float)(e.sample-lastDown); if(on>=5) { v1.classifyOn(on); v2.classifyOn(on);} }
            lastUp = e.sample;
        }
    }
}

// §52.1 #40b Stage 1 — pin the log->bimodal crossover dit inside the good-SNR
// hand-keyed branch. select routes ALL hand-keyed to log, but bimodal overtakes
// log as the operator gets faster. This sweeps clean hand-keyed by WPM to set
// SELECT_BIMODAL_DIT_MS from data. Weak (n1.5) rows confirm bimodal must NOT be
// reached there (it is far worse), which the good-SNR gate already guarantees.
TEST_CASE("select bimodal crossover sweep (§52.1 #40b)", "[cw][.][select-bimodal-sweep]") {
    constexpr int SEEDS = 96;
    struct Cell { const char* name; float dit; float noise; };
    const Cell cells[] = {
        {"hk-15",  80.0f, 0.0f}, {"hk-18",  67.0f, 0.0f}, {"hk-20",  60.0f, 0.0f},
        {"hk-22",  55.0f, 0.0f}, {"hk-25",  48.0f, 0.0f}, {"hk-30",  40.0f, 0.0f},
        {"hk-40",  30.0f, 0.0f},
        {"hk-20-n1.5", 60.0f, 1.5f}, {"hk-25-n1.5", 48.0f, 1.5f},   // must stay off bimodal
    };
    printf("\n=== §52.1 #40b crossover: log vs bimodal on clean hand-keyed (n=%d) ===\n", SEEDS);
    printf("%-13s %6s %9s %9s %9s  %s\n", "cell", "dit", "log", "bimodal", "winner", "delta");
    for (const auto& c : cells) {
        auto p = profileHandKeyed(c.dit); p.noiseAmp = c.noise;
        auto l = runCell("legacy+log",     c.name, MSG_FULL(), p, SEEDS).cerMean;
        auto b = runCell("legacy+bimodal", c.name, MSG_FULL(), p, SEEDS).cerMean;
        printf("%-13s %6.0f %9.4f %9.4f %9s  %+.4f\n",
               c.name, c.dit, l, b, b < l ? "bimodal" : "log", b - l);
    }
    printf("\nSELECT_BIMODAL_DIT_MS = the dit where bimodal starts winning (clean rows only).\n");
}

// §48/§52.1 — verify legacy+select routes correctly: slow good-SNR hand-keyed ->
// log, mid-speed good-SNR hand-keyed (22-30 wpm) -> bimodal (§52.1 #40b), heavy
// noise / weak hand-keyed -> kalman2s. n=96 (routing is a large effect).
TEST_CASE("select routing verification (§48/§52.1)", "[cw][.][select-verify]") {
    constexpr int SEEDS = 96;
    // maxCer: strict absolute ratchet on select's deterministic n=96 CER (measured
    // value + a small margin). Replaces the old `s <= max(k,l,b)+0.02` bound, which
    // bimodal's catastrophic noise CER (2.05 on noise3.0-30wpm) inflated into a
    // vacuous gate. A rise trips it — never widen to admit a change (§52.2).
    struct Prof { const char* name; SignalParams params; const char* want; float maxCer; };
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
    SignalParams hk25n15 = profileHandKeyed(48.0f); hk25n15.noiseAmp = 1.5f;
    const Prof profs[] = {
        {"handkeyed-15", profileHandKeyed(80.0f),  "log",     0.046f}, // slow good-SNR -> log
        {"handkeyed-25", profileHandKeyed(48.0f),  "bimodal", 0.085f}, // mid-speed -> bimodal (#40b)
        {"handkeyed-30", profileHandKeyed(40.0f),  "bimodal", 0.090f},
        {"handkeyed-40", profileHandKeyed(30.0f),  "log",     0.107f}, // dit<LO -> log
        {"noise3.0",     n3,                        "kalman2s", 0.715f}, // heavy noise -> kalman2s
        {"noise3.0-30wpm",[]{auto p=profileClean(40.0f);p.noiseAmp=3.0f;return p;}(), "kalman2s", 0.925f},
        {"hk25-n1.5",    hk25n15,                   "kalman2s", 0.120f}, // weak hand-keyed -> kalman2s
        {"noise2.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }(), "kalman2s", 0.045f},
    };

    printf("\n=== §48/§52.1 select routing (CER, n=%d) — select should match its target ===\n", SEEDS);
    printf("%-16s %8s %8s %8s %8s %9s  %s\n",
           "profile", "kal2s", "log", "bimodal", "select", "want", "<=max?");
    for (const auto& p : profs) {
        auto k = runCell("legacy+kalman2s", p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        auto l = runCell("legacy+log",      p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        auto b = runCell("legacy+bimodal",  p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        auto s = runCell("legacy+select",   p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        const bool ok = s <= p.maxCer;
        printf("%-16s %8.4f %8.4f %8.4f %8.4f %9s  %s\n",
               p.name, k, l, b, s, p.want, ok ? "OK" : "OVER");
        INFO(p.name << " select=" << s << " maxCer=" << p.maxCer);
        CHECK(s <= p.maxCer);   // strict absolute ratchet on the shipping default
    }
}

// §47 — the conservative gate's CEILING before building switch machinery. A
// perfect regime gate picks the best core per profile (= per regime). Its win
// over kalman2s upper-bounds any real gate; a real jitter+SNR gate captures a
// fraction. If the ceiling is small or the wins scatter into cells a gate can't
// identify, the switch isn't worth it. n=192; full standard gate.
TEST_CASE("conservative gate ceiling (§47)", "[cw][.][gate-ceiling]") {
    constexpr int SEEDS = 192;
    const auto profiles = standardProfiles();

    printf("\n=== §47 perfect-regime-gate ceiling vs kalman2s (n=%d) ===\n", SEEDS);
    printf("gate2 = min(kal2s,log) [conservative]; gate3 = min(kal2s,log,bimodal)\n");
    printf("%-16s %8s %8s %8s   %9s %9s\n",
           "profile", "kal2s", "log", "bimodal", "gate2HR", "gate3HR");
    float sum2 = 0, sum3 = 0;
    for (const auto& pr : profiles) {
        auto k = runCell("legacy+kalman2s", pr.name, pr.message, pr.params, SEEDS).cerMean;
        auto l = runCell("legacy+log",      pr.name, pr.message, pr.params, SEEDS).cerMean;
        auto b = runCell("legacy+bimodal",  pr.name, pr.message, pr.params, SEEDS).cerMean;
        const float g2 = k - std::min(k, l);
        const float g3 = k - std::min({k, l, b});
        sum2 += g2; sum3 += g3;
        printf("%-16s %8.4f %8.4f %8.4f   %+9.4f %+9.4f\n", pr.name, k, l, b, g2, g3);
    }
    printf("  %-14s %8s %8s %8s   %+9.4f %+9.4f  (summed CER capturable)\n",
           "TOTAL", "", "", "", sum2, sum3);
    printf("\nA real jitter+SNR gate captures a FRACTION of gate2HR, only on cells it\n");
    printf("can identify at runtime. Small/scattered ceiling => switch not worth it.\n");
}

// §46b — the load-bearing check for jitter-gated log: does jitter separate the
// regime where log WINS (hand-keyed) from where it REGRESSES (machine fast-CW
// under noise, §25)? Same speed and noise, jitter 0.0 (machine) vs 0.15 (hand).
// If log-vs-kalman2s flips sign with jitter, the gate is valid.
TEST_CASE("jitter separates log win from log regression (§46b)", "[cw][.][jitter-sep]") {
    constexpr int SEEDS = 192;
    const float dits[] = {48.0f, 40.0f};   // 25, 30 WPM
    const float amps[] = {1.5f, 2.0f};

    printf("\n=== §46b log vs kalman2s: delta by jitter (n=%d) ===\n", SEEDS);
    printf("delta = log - kalman2s; negative = log wins. Gate needs: machine>0, hand<0\n");
    printf("%4s %6s %8s %10s %8s   %10s %8s\n",
           "wpm", "noise", "", "machine", "t", "hand-key", "t");
    for (float dit : dits) {
        const int wpm = (int)(1200.0f / dit + 0.5f);
        for (float amp : amps) {
            auto run = [&](float jit) {
                SignalParams p = profileClean(dit);
                p.noiseAmp = amp; p.jitterPct = jit;
                if (jit > 0) { p.weightBias = 0.1f; }
                char nm[40]; snprintf(nm, sizeof nm, "w%d-n%.1f-j%.2f", wpm, amp, jit);
                auto k = runCell("legacy+kalman2s", nm, MSG_FULL(), p, SEEDS);
                auto l = runCell("legacy+log",      nm, MSG_FULL(), p, SEEDS);
                return comparePaired(k.cerSamples, l.cerSamples);   // log - kalman2s
            };
            auto m = run(0.0f);    // machine
            auto h = run(0.15f);   // hand-keyed
            printf("%4d %6.1f %8s %+10.4f %8.2f   %+10.4f %8.2f\n",
                   wpm, amp, "", m.meanDelta, m.t, h.meanDelta, h.t);
        }
    }
    printf("\nValid gate iff machine delta > 0 (log worse) and hand delta < 0 (log better).\n");
}

// §45 — POWER analysis before building the regime-adaptive timing selector.
// The selector's target win is small (handkeyed-25 ceiling +0.035, Farnsworth
// +0.014, §44), and §42 showed n=96 misjudges high-variance profiles. This
// measures the minimum detectable effect (MDE = 2*stderr of the paired delta,
// = the non-inferiority bound width at zero mean) per profile at increasing n,
// using legacy-vs-kalman2s as a realistic variance proxy, so the selector is
// adjudicated at adequate n from the start rather than discovered post-hoc.
TEST_CASE("Selector power / MDE by seed count (§45)", "[cw][.][selector-power]") {
    struct Prof { const char* name; SignalParams params; };
    const Prof profs[] = {
        {"handkeyed-25", profileHandKeyed(48.0f)},           // target win
        {"farnsworth20", profileFarnsworth(80.0f, 2.0f)},    // target win
        {"noise2.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }()},
        {"qsb",          profileQSB(80.0f)},                 // high variance
        {"worstcase",    profileWorstCase(80.0f)},           // high variance
        {"noise3.0-30wpm",[] { auto p = profileClean(40.0f); p.noiseAmp = 3.0f; return p; }()},
        {"noise4.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 4.0f; return p; }()},
    };
    const int Ns[] = {96, 192, 384, 768};

    printf("\n=== §45 MDE (2*stderr of paired delta) by n — the smallest effect resolvable ===\n");
    printf("target selector wins: handkeyed-25 ~+0.035, farnsworth ~+0.014; tol=0.005\n");
    printf("%-15s", "profile");
    for (int n : Ns) { printf("  n=%-6d", n); }
    printf("\n");
    for (const auto& p : profs) {
        printf("%-15s", p.name);
        for (int n : Ns) {
            auto a = runCell("legacy",          p.name, MSG_FULL(), p.params, n);
            auto b = runCell("legacy+kalman2s", p.name, MSG_FULL(), p.params, n);
            auto d = comparePaired(a.cerSamples, b.cerSamples);
            printf("  %8.4f", 2.0f * d.stderrDelta);
        }
        printf("\n");
    }
    printf("\nRead: to DETECT a win, MDE must be < the effect; to PROVE non-inferiority,\n");
    printf("bound (mean+MDE) must be < tol 0.005, so a ~0 profile needs MDE < 0.005.\n");
}

// §44 — headroom for a Bell-style trellis (#27) BEFORE building a Very-High-
// complexity core. A trellis replaces {timing classify + gap classify + beam}
// with a joint MAP decode over the detector's REAL durations. Its ceiling is
// therefore "real detector + perfect per-element timing decisions + beam" =
// ClairvoyantTiming (classifies the real durations at the true Bayes boundaries,
// confidence 1.0). A trellis cannot know the boundaries better; its only edge is
// marginal joint-sequence inference the beam already approximates. So the gap
// [current-best -> clairvoyant] upper-bounds what a trellis could capture; the
// further gap [clairvoyant -> full oracle] is DETECTOR headroom a trellis cannot
// touch. If the first gap is small in the usable regime, the trellis is not worth
// its complexity.
TEST_CASE("Bell trellis headroom vs oracle (§44)", "[cw][.][trellis-headroom]") {
    constexpr int SEEDS = 48;

    struct Prof { const char* name; SignalParams params; };
    const Prof profs[] = {
        {"clean-15",    profileClean(80.0f)},
        {"handkeyed-15",profileHandKeyed(80.0f)},
        {"handkeyed-25",profileHandKeyed(48.0f)},
        {"qsb",         profileQSB(80.0f)},
        {"farnsworth20",profileFarnsworth(80.0f, 2.0f)},
        {"noise2.0",    [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }()},
        {"worstcase",   profileWorstCase(80.0f)},
    };

    auto oracle = [](SignalParams p, bool det, bool tim) {
        return [p, det, tim](const GeneratedSignal& sig) {
            return makeOracleCore(sig, p, OracleConfig{det, tim});
        };
    };

    // Include the best EXISTING timing cores: +log and +bimodal already beat
    // legacy on hand-keyed (§ results table). The trellis must beat the best of
    // ALL existing cores, not just legacy — that is its true marginal headroom.
    const char* cores[] = {"legacy", "legacy+kalman2s", "legacy+log", "legacy+bimodal"};

    printf("\n=== §44 trellis headroom: best EXISTING core vs timing-oracle vs full oracle ===\n");
    printf("clairv = real detector + PERFECT timing decisions + beam = trellis ceiling\n");
    printf("%-13s %8s %8s %8s %8s %9s %9s   %9s %9s\n",
           "profile", "legacy", "kal2s", "log", "bimodal", "clairvoy", "full-orac",
           "trellisHR", "detectHR");
    for (const auto& p : profs) {
        float best = 1e9f; float cer[4];
        for (int i = 0; i < 4; i++) {
            cer[i] = runCell(cores[i], p.name, MSG_FULL(), p.params, SEEDS).cerMean;
            best = std::min(best, cer[i]);
        }
        auto clair= runCellWith(oracle(p.params, false, true), "clairvoy",
                                p.name, MSG_FULL(), p.params, SEEDS);
        auto full = runCellWith(oracle(p.params, true, true), "full",
                                p.name, MSG_FULL(), p.params, SEEDS);
        printf("%-13s %8.4f %8.4f %8.4f %8.4f %9.4f %9.4f   %+9.4f %+9.4f\n",
               p.name, cer[0], cer[1], cer[2], cer[3], clair.cerMean, full.cerMean,
               best - clair.cerMean, clair.cerMean - full.cerMean);
    }
    printf("\ntrellisHR = best-of-ALL-existing minus clairvoyant (trellis's true marginal win).\n");
    printf("detectHR  = clairvoyant minus full-oracle (detector-limited, NOT trellis-addressable).\n");
}

// §52 #40 — speed-marginalisation headroom vs the SHIPPING default.
//
// Speed marginalisation (SparkGap: score N dit/WPM hypotheses, marginalise) can
// at best pick the correct global dit scale — its ceiling is the timing-oracle
// (clairvoyant: real detector + PERFECT dit/boundaries). §44 measured that ceiling
// but predates `select`. `select` now routes hand-keyed to `log` (§48), so the
// live question is not "vs legacy" but "does anything remain between the SHIPPING
// default and the timing-oracle on hand-keyed?" A ~0 gap kills #40 before a build.
// Weak-hand-keyed (hk*-n1.5) is included because select routes THOSE to kalman,
// not log (§46b) — the cells where speedmarg's marginal room is largest.
TEST_CASE("speedmarg headroom: select vs timing-oracle (§52 #40)", "[cw][.][speedmarg-headroom]") {
    constexpr int SEEDS = 96;

    struct Prof { const char* name; SignalParams params; };
    auto weak = [](float dit){ auto p = profileHandKeyed(dit); p.noiseAmp = 1.5f; return p; };
    const Prof profs[] = {
        {"hk-15",      profileHandKeyed(80.0f)},
        {"hk-20",      profileHandKeyed(60.0f)},
        {"hk-25",      profileHandKeyed(48.0f)},
        {"hk-30",      profileHandKeyed(40.0f)},
        {"hk-40",      profileHandKeyed(30.0f)},
        {"hk-15-n1.5", weak(80.0f)},
        {"hk-25-n1.5", weak(48.0f)},
    };

    auto oracle = [](SignalParams p) {
        return [p](const GeneratedSignal& sig) {
            return makeOracleCore(sig, p, OracleConfig{false, true});   // real detector + oracle timing
        };
    };

    printf("\n=== §52 #40 speedmarg headroom (decomposed vs best-existing) ===\n");
    printf("ceiling = clairvoyant (perfect dit). shipGap = select-ceiling (routing+timing).\n");
    printf("margGap = best-existing-ceiling = TRUE speedmarg marginal room (the §44 rule).\n");
    printf("%-12s %8s %8s %8s %8s %9s %9s\n",
           "profile", "select", "log", "bimodal", "ceiling", "shipGap", "margGap");

    float maxMarg = 0.0f;
    for (const auto& p : profs) {
        auto sel  = runCell("legacy+select",  p.name, MSG_FULL(), p.params, SEEDS);
        auto lg   = runCell("legacy+log",     p.name, MSG_FULL(), p.params, SEEDS);
        auto bm   = runCell("legacy+bimodal", p.name, MSG_FULL(), p.params, SEEDS);
        auto ceil = runCellWith(oracle(p.params), "ceiling", p.name, MSG_FULL(), p.params, SEEDS);
        float bestExisting = std::min({sel.cerMean, lg.cerMean, bm.cerMean});
        float margGap = bestExisting - ceil.cerMean;
        printf("%-12s %8.4f %8.4f %8.4f %8.4f %+9.4f %+9.4f\n",
               p.name, sel.cerMean, lg.cerMean, bm.cerMean, ceil.cerMean,
               sel.cerMean - ceil.cerMean, margGap);
        maxMarg = std::max(maxMarg, margGap);
    }
    printf("\nlargest margGap = %.4f CER. Two separate findings to read off this table:\n", maxMarg);
    printf(" (1) if select >> bimodal on hand-keyed, select mis-routes (cheap: add bimodal target).\n");
    printf(" (2) margGap is the TRUE speedmarg room over best-existing; ~0 => #40 not worth a build.\n");
}

// §43 — the noise×speed grid, including the fast×heavy-noise cells the standard
// gate lacks. kalman2s switches on the self-estimated WPM, which the dit runaway
// inflates under heavy noise (§38b) — so a genuinely-fast signal could read slow
// and flip into V2 exactly where V2 regresses fast CW. This tests every cell.
TEST_CASE("kalman2s noise x speed grid (§43)", "[cw][.][kalman2s-grid]") {
    constexpr int SEEDS = 96;
    const float dits[]  = {80.0f, 48.0f, 40.0f};   // 15, 25, 30 WPM
    const int   wpms[]  = {15, 25, 30};
    const float amps[]  = {2.0f, 3.0f, 4.0f};
    const char* cores[] = {"legacy", "legacy+kalman2s"};

    (void)cores;
    printf("\n=== §43 noise x speed grid: CER mean + paired t vs legacy ===\n");
    printf("kalman2 = pure V2 (attributes intrinsic vs switch); kalman2s = speed-switched\n");
    printf("%5s %5s %9s %9s %7s %9s %7s\n",
           "wpm", "n", "legacy", "kal2", "t2", "kal2s", "t2s");

    for (size_t s = 0; s < 3; s++) {
        for (float amp : amps) {
            SignalParams p = profileClean(dits[s]);
            p.noiseAmp = amp;
            char name[32]; snprintf(name, sizeof(name), "%dwpm-n%.0f", wpms[s], amp);
            auto base = runCell("legacy",          name, MSG_FULL(), p, SEEDS);
            auto k2   = runCell("legacy+kalman2",  name, MSG_FULL(), p, SEEDS);
            auto k2s  = runCell("legacy+kalman2s", name, MSG_FULL(), p, SEEDS);
            auto d2  = comparePaired(base.cerSamples, k2.cerSamples);
            auto d2s = comparePaired(base.cerSamples, k2s.cerSamples);
            auto tag = [](const PairedDelta& d) {
                return !d.significant() ? "  " : (d.meanDelta < 0 ? "<<" : ">>");  // >> = WORSE
            };
            printf("%5d %5.1f %9.4f %9.4f %+6.2f%s %9.4f %+6.2f%s\n",
                   wpms[s], amp, base.cerMean, k2.cerMean, d2.t, tag(d2),
                   k2s.cerMean, d2s.t, tag(d2s));
        }
    }
    printf("\n>> = significant WORSE. 30wpm x n3/n4: runaway-flips-switch-to-V2 hole.\n");
    printf("If kal2 also >> at a cell, it is V2-intrinsic, not the switch.\n");
}

// §42 — does noise4.0's non-inferiority bound converge under 0.005 with more seeds?
// The mean is ~zero (better/neutral), so the HARM is a wide CI from -10 dB
// per-seed variance. worstCaseDelta = meanDelta + 2*stderrDelta; stderr ~ 1/sqrt(n).
// This measures the bound's actual trajectory to decide whether more power clears
// it or the variance floor makes it infeasible.
TEST_CASE("noise4.0 bound vs seed count (§42)", "[cw][.][noise4-power]") {
    SignalParams n4 = profileClean(80.0f); n4.noiseAmp = 4.0f;
    const int Ns[] = {96, 192, 384, 768};

    printf("\n=== noise4.0: legacy+kalman2s vs legacy, bound vs n ===\n");
    printf("%6s %10s %10s %8s %8s %10s  %s\n",
           "n", "legacy", "cand", "mean", "2*stderr", "bound", "harmful?");
    for (int n : Ns) {
        auto base = runCell("legacy",         "noise4.0", MSG_FULL(), n4, n);
        auto cand = runCell("legacy+kalman2s","noise4.0", MSG_FULL(), n4, n);
        auto d = comparePaired(base.cerSamples, cand.cerSamples);
        printf("%6d %10.4f %10.4f %+8.4f %8.4f %+10.4f  %s\n",
               n, base.cerMean, cand.cerMean, d.meanDelta,
               2.0f * d.stderrDelta, d.worstCaseDelta(),
               d.harmful(0.005f) ? "HARM" : "clean");
    }
    printf("\nNote: at -10 dB both cores fail (CER ~0.9 = garbage); the bound\n");
    printf("compares garbage-vs-garbage variance, not usable decode.\n");
}

// §41 decomposition: the speed gate at 27 WPM did not fix the fast-CW regression.
// Two causes: (1) the gate is still active at 30 WPM (self-WPM reads low under the
// runaway), or (2) the regression is from V2's dah-gain fix, not the confidence
// gate. Sweeping the threshold separates them: gateWpm=1 disables the gate almost
// entirely (V1 learning + V2 dah-gain), so if noise2.0-30wpm STILL regresses
// there, the dah-gain is the culprit and speed-gating the gate cannot help.
TEST_CASE("kalman2s speed-gate threshold sweep (§41)", "[cw][.][kalman2s-sweep]") {
    constexpr int SEEDS = 96;
    auto gatedFactory = [](float gateWpm) {
        return [gateWpm](const GeneratedSignal&) -> std::unique_ptr<cw::IDecodeCore> {
            auto timing = std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN_V2S);
            timing->setSpeedGateWpm(gateWpm);
            return std::make_unique<cw::StagedCore>(
                std::make_unique<cw::EnvelopeFrontEnd>(),
                std::make_unique<cw::SchmittDetector>(),
                std::move(timing),
                std::make_unique<cw::BeamSymbolDecoder>());
        };
    };

    struct Prof { const char* name; SignalParams params; };
    const Prof profs[] = {
        {"noise2.0-30wpm", [] { auto p = profileClean(40.0f); p.noiseAmp = 2.0f; return p; }()},
        {"handkeyed-30",   profileHandKeyed(40.0f)},
        {"noise3.0",       [] { auto p = profileClean(80.0f); p.noiseAmp = 3.0f; return p; }()},
        {"worstcase",      profileWorstCase(80.0f)},
    };
    const float gates[] = {1.0f, 20.0f, 24.0f, 27.0f, 30.0f, 999.0f};  // 1=gate off, 999=pure V2

    printf("\n=== §41 speed-gate threshold sweep (CER mean, %d seeds) ===\n", SEEDS);
    printf("gateWpm=1 -> gate off (V1 learn + V2 dah-gain); 999 -> pure V2\n");
    printf("%-16s %8s", "profile", "legacy");
    for (float g : gates) { printf("  g=%-5.0f", g); }
    printf("\n");

    for (const auto& p : profs) {
        auto base = runCell("legacy", p.name, MSG_FULL(), p.params, SEEDS);
        printf("%-16s %8.4f", p.name, base.cerMean);
        for (float g : gates) {
            auto cell = runCellWith(gatedFactory(g), "kalman2s", p.name, MSG_FULL(), p.params, SEEDS);
            printf("  %7.4f", cell.cerMean);
        }
        printf("\n");
    }
    printf("\nRead: g=1 collapses to legacy (pure V1), g=999 is pure V2. A mid\n");
    printf("threshold keeps the slow-win rows near V2 and the 30wpm rows near legacy.\n");
    printf("(§41 history: with only the confidence gate switched, 30wpm was flat\n");
    printf("across gates => the fast regression is V2's dah-gain, so the whole\n");
    printf("V1/V2 behaviour must switch, not just the gate.)\n");
}

// §52.5 #42 headroom — should we build the forward-backward soft-posterior detector?
//
// The discipline (§44): before a Very-High-complexity joint HMM, measure its
// CAPTURABLE headroom. A soft detector REPLACES the Schmitt detector, so §44's
// clairvoyant table does NOT bound it — the relevant ceiling is the ORACLE DETECTOR
// (real timing held constant). And unlike §44 we have real soft-ish detectors
// already: legacy+lr (sequential CUSUM) and legacy+lr+soft (evidence-weighted) are
// approximations of the forward-backward posterior. So:
//   ceiling   = legacy - detOracle        (what any better DETECTOR can win, kalman timing)
//   captured  = legacy - best(lr, lrsoft) (what the existing soft-approx already wins)
//   residual  = best(lr, lrsoft) - detOracle (marginal room a FULL HMM has over them)
// If residual is small, the full HMM buys little over the cheaper LR detector -> defer
// (the §44 verdict). If large, #42 is justified. All cores use kalman timing so the
// comparison isolates the DETECTOR.
TEST_CASE("soft-detector headroom vs oracle detector (§52.5 #42)", "[cw][.][softdet-headroom]") {
    constexpr int SEEDS = 96;
    struct Prof { const char* name; SignalParams params; };
    auto n = [](float amp){ auto p = profileClean(80.0f); p.noiseAmp = amp; return p; };
    const Prof profs[] = {
        {"noise2.0",     n(2.0f)},
        {"noise3.0",     n(3.0f)},
        {"noise4.0",     n(4.0f)},
        {"qrm",          profileQRM(80.0f)},
        {"qrn",          profileQRN(80.0f)},
        {"worstcase",    profileWorstCase(80.0f)},
        {"handkeyed-25", profileHandKeyed(48.0f)},
    };
    auto detOracle = [](SignalParams p) {
        return [p](const GeneratedSignal& sig) {
            return makeOracleCore(sig, p, OracleConfig{true, false});   // oracle DET, real timing
        };
    };
    auto fullOracle = [](SignalParams p) {
        return [p](const GeneratedSignal& sig) {
            return makeOracleCore(sig, p, OracleConfig{true, true});
        };
    };

    printf("\n=== §52.5 #42 soft-detector headroom (kalman timing, n=%d) ===\n", SEEDS);
    printf("%-13s %8s %8s %8s %9s %9s   %8s %8s %8s\n",
           "profile", "legacy", "lr", "lrsoft", "detOrac", "fullOrac",
           "ceiling", "captured", "residual");
    for (const auto& p : profs) {
        float leg  = runCell("legacy",          p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        float lr   = runCell("legacy+lr",       p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        float lrs  = runCell("legacy+lr+soft",  p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        float dO   = runCellWith(detOracle(p.params),  "dO", p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        float fO   = runCellWith(fullOracle(p.params), "fO", p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        float best = std::min(lr, lrs);
        printf("%-13s %8.4f %8.4f %8.4f %9.4f %9.4f   %+8.4f %+8.4f %+8.4f\n",
               p.name, leg, lr, lrs, dO, fO, leg - dO, leg - best, best - dO);
    }
    printf("\nceiling = legacy-detOracle (max any detector can win, kalman timing).\n");
    printf("residual = best(lr,lrsoft)-detOracle = marginal room for the FULL HMM.\n");
    printf("Small residual => full HMM buys little over existing LR -> defer (§44 logic).\n");
}

// §52.5b #42 headroom — multi-condition robustness of the detOracle=0 claim.
// §52.5 tested the noise ladder ONLY at 15 wpm. §43's lesson: fast x heavy cells
// behave differently and hide the truth. The whole "build #42" verdict rests on
// detOracle ~= 0, so verify it across WPM x noise AND report the WORST seed, not
// just the mean (a mean-0 with a bad tail would weaken the ceiling claim).
TEST_CASE("soft-detector headroom grid: WPM x noise, mean+worst (§52.5b #42)", "[cw][.][softdet-grid]") {
    constexpr int SEEDS = 96;
    const float dits[] = {80.0f, 48.0f, 40.0f, 30.0f};   // 15, 25, 30, 40 wpm
    const int   wpms[] = {15, 25, 30, 40};
    const float amps[] = {2.0f, 3.0f, 4.0f};

    auto detOracle = [](SignalParams p) {
        return [p](const GeneratedSignal& sig) {
            return makeOracleCore(sig, p, OracleConfig{true, false});
        };
    };
    auto worst = [](const std::vector<float>& v) {
        return v.empty() ? 0.0f : *std::max_element(v.begin(), v.end());
    };

    printf("\n=== §52.5b #42 detOracle robustness (oracle DET + real timing, n=%d) ===\n", SEEDS);
    printf("If detOracle stays ~0 across the grid, the noise wall is a detector problem\n");
    printf("at every speed. A rise at fast x heavy = partly intrinsic (perfect detection\n");
    printf("still cannot decode short elements at -10 dB) -> smaller real headroom there.\n");
    printf("%5s %6s %9s %11s %11s\n", "wpm", "noise", "legacy", "detOrac_mean", "detOrac_worst");
    for (size_t i = 0; i < 4; i++) {
        for (float amp : amps) {
            SignalParams p = profileClean(dits[i]); p.noiseAmp = amp;
            char nm[24]; snprintf(nm, sizeof(nm), "%dwpm-n%.0f", wpms[i], amp);
            auto leg = runCell("legacy", nm, MSG_FULL(), p, SEEDS);
            auto dO  = runCellWith(detOracle(p), "dO", nm, MSG_FULL(), p, SEEDS);
            printf("%5d %6.1f %9.4f %11.4f %11.4f\n",
                   wpms[i], amp, leg.cerMean, dO.cerMean, worst(dO.cerSamples));
        }
    }
}
