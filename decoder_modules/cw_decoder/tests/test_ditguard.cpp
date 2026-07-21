#include <catch.hpp>
#include "cw_test_signals.h"
#include "cw_matrix.h"
#include "cw_bench_stats.h"
#include "cw_oracle.h"
#include "cw_snr.h"

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

// §48 — verify legacy+select routes correctly: it should track log on good-SNR
// hand-keyed (where log wins) and kalman2s on noise (never log's catastrophic
// noise CER) and on weak hand-keyed (§46b). n=96 (routing is a large effect).
TEST_CASE("select routing verification (§48)", "[cw][.][select-verify]") {
    constexpr int SEEDS = 96;
    struct Prof { const char* name; SignalParams params; const char* want; };
    SignalParams n3 = profileClean(80.0f); n3.noiseAmp = 3.0f;
    SignalParams hk25n15 = profileHandKeyed(48.0f); hk25n15.noiseAmp = 1.5f;
    const Prof profs[] = {
        {"handkeyed-25", profileHandKeyed(48.0f),  "log"},      // good SNR jittered -> log
        {"handkeyed-40", profileHandKeyed(30.0f),  "log"},
        {"noise3.0",     n3,                        "kalman2s"}, // heavy noise -> kalman2s
        {"noise3.0-30wpm",[]{auto p=profileClean(40.0f);p.noiseAmp=3.0f;return p;}(), "kalman2s"},
        {"hk25-n1.5",    hk25n15,                   "kalman2s"}, // weak hand-keyed -> kalman2s
        {"noise2.0",     [] { auto p = profileClean(80.0f); p.noiseAmp = 2.0f; return p; }(), "kalman2s"},
    };

    printf("\n=== §48 select routing (CER, n=%d) — select should match its target ===\n", SEEDS);
    printf("%-16s %8s %8s %8s %8s  %s\n", "profile", "kal2s", "log", "select", "want", "ok?");
    for (const auto& p : profs) {
        auto k = runCell("legacy+kalman2s", p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        auto l = runCell("legacy+log",      p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        auto s = runCell("legacy+select",   p.name, MSG_FULL(), p.params, SEEDS).cerMean;
        const float target = std::string(p.want) == "log" ? l : k;
        const float other  = std::string(p.want) == "log" ? k : l;
        const bool ok = std::fabs(s - target) < std::fabs(s - other) + 1e-6f;
        printf("%-16s %8.4f %8.4f %8.4f %8s  %s\n", p.name, k, l, s, p.want, ok ? "OK" : "MISROUTED");
        CHECK(s <= std::max(k, l) + 0.02f);   // select never worse than the worse of its two options
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
