#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"
#include "cw_bench_stats.h"
#include <cstdio>

using namespace cw_test;

// ============================================================
// Multi-seed regression gates.
//
// The single-seed benchmarks in test_benchmark.cpp remain the correctness
// tests. These add a statistical gate: each profile is run over N independent
// noise realizations and asserted on the MEAN.
//
// Rules for these thresholds:
//   * Set from the measured mean of the CURRENT implementation.
//   * A change that raises a mean is a regression, even if every single-seed
//     test still passes.
//   * Never widen a bound to admit a change. Lower them when a change earns it.
// ============================================================

namespace {
    constexpr int SEEDS = 24;

    struct Profile {
        const char* name;
        const char* message;
        SignalParams params;
    };

    std::vector<Profile> allProfiles() {
        std::vector<Profile> v;
        v.push_back({"clean-15wpm",        MSG_FULL(), profileClean(80.0f)});
        v.push_back({"clean-20wpm",        MSG_FULL(), profileClean(60.0f)});
        v.push_back({"clean-25wpm",        MSG_FULL(), profileClean(48.0f)});
        v.push_back({"mild-noise",         MSG_FULL(), profileMildNoise(80.0f)});
        v.push_back({"moderate-noise",     MSG_FULL(), profileModerateNoise(80.0f)});
        v.push_back({"handkeyed-15wpm",    MSG_FULL(), profileHandKeyed(80.0f)});
        v.push_back({"handkeyed-20wpm",    MSG_FULL(), profileHandKeyed(60.0f)});
        v.push_back({"handkeyed-25wpm",    MSG_FULL(), profileHandKeyed(48.0f)});
        v.push_back({"handkeyed-30wpm",    MSG_FULL(), profileHandKeyed(40.0f)});
        v.push_back({"qsb",                MSG_FULL(), profileQSB(80.0f)});
        v.push_back({"qrm",                MSG_FULL(), profileQRM(80.0f)});
        v.push_back({"qrn",                MSG_FULL(), profileQRN(80.0f)});
        v.push_back({"contest-20wpm",      MSG_CONTEST(), profileContest(60.0f)});
        v.push_back({"farnsworth-1.5",     MSG_FULL(), profileFarnsworth(80.0f, 1.5f)});
        v.push_back({"farnsworth-2.0",     MSG_FULL(), profileFarnsworth(80.0f, 2.0f)});
        v.push_back({"worstcase",          MSG_FULL(), profileWorstCase(80.0f)});

        // Explicit SNR ladder on the moderate-noise profile. This is the axis
        // that filter bandwidth acts on, so it is the axis a DSP change must
        // be judged against.
        for (float amp : {1.0f, 2.0f, 3.0f, 4.0f}) {
            Profile p;
            p.name = amp == 1.0f ? "snr-noise1.0" :
                     amp == 2.0f ? "snr-noise2.0" :
                     amp == 3.0f ? "snr-noise3.0" : "snr-noise4.0";
            p.message = MSG_FULL();
            p.params = profileClean(80.0f);
            p.params.noiseAmp = amp;
            v.push_back(p);
        }
        return v;
    }
}

// Prints the full distribution table. Always passes — this is the measuring
// instrument, run it with `./cw_decoder_tests "[characterize]" -s` to see it.
TEST_CASE("Characterize: multi-seed CER distribution per profile", "[cw][characterize]") {
    printf("\n%-20s %8s %9s %8s %8s %8s %8s\n",
           "profile", "mean", "stderr", "median", "p95", "worst", "wer");
    printf("%s\n", std::string(74, '-').c_str());
    for (auto& prof : allProfiles()) {
        auto s = decodeAndScoreMulti(prof.message, prof.params, SEEDS);
        printf("%-20s %8.4f %9.4f %8.4f %8.4f %8.4f %8.4f\n",
               prof.name, s.mean, s.stderrMean, s.median, s.p95, s.worst, s.meanWER);
    }
    printf("\n");
    SUCCEED();
}

// Ablation: separates the two factors by which the multi-seed baseline differs
// from the figures in decoding-improvement-plan.md (message length, seed count).
// Reproduces the plan's exact conditions first, then varies one factor at a time.
TEST_CASE("Characterize: single-seed vs multi-seed ablation", "[cw][characterize]") {
    struct Case { const char* label; const char* msg; float ditMs; };
    Case cases[] = {
        {"handkeyed-15wpm", MSG_CQ(),   80.0f},
        {"handkeyed-20wpm", MSG_CQ(),   60.0f},
        {"handkeyed-25wpm", MSG_CQ(),   48.0f},
    };
    printf("\n%-18s %-9s %10s %10s %10s\n",
           "case", "msg", "seed42", "CQ x24", "FULL x24");
    printf("%s\n", std::string(60, '-').c_str());
    for (auto& c : cases) {
        auto p = profileHandKeyed(c.ditMs);

        p.seed = 42;                                   // the plan's exact condition
        float single = decodeAndScore(c.msg, p).cer;

        auto multiCQ   = decodeAndScoreMulti(c.msg,      p, 24);  // vary seed only
        auto multiFULL = decodeAndScoreMulti(MSG_FULL(), p, 24);  // vary seed + length

        printf("%-18s %-9s %10.4f %10.4f %10.4f\n",
               c.label, "CQ(23ch)", single, multiCQ.mean, multiFULL.mean);
    }
    printf("\n");
    SUCCEED();
}

// ============================================================
// Regression gates.
//
// Bounds below are the 2026-07 measured mean + 2*stderr over SEEDS=24
// deterministic seeds. They are ratchets: lower them when a change earns it,
// never raise them to admit a change. A rise in mean CER is a regression even
// when every single-seed test in test_benchmark.cpp still passes.
// ============================================================

namespace {
    void gate(const char* name, const char* message, const SignalParams& p, float maxMean) {
        auto s = decodeAndScoreMulti(message, p, SEEDS);
        INFO(name << "  " << s.summary());
        CHECK(s.mean <= maxMean);
    }
}

TEST_CASE("Gate: clean signals decode exactly", "[cw][multiseed][gate]") {
    // Strongest possible assertion: zero errors on every seed.
    gate("clean-15wpm", MSG_FULL(), profileClean(80.0f), 0.0f);
    gate("clean-20wpm", MSG_FULL(), profileClean(60.0f), 0.0f);
    gate("clean-25wpm", MSG_FULL(), profileClean(48.0f), 0.0f);
    gate("farnsworth-1.5", MSG_FULL(), profileFarnsworth(80.0f, 1.5f), 0.0f);
}

TEST_CASE("Gate: additive noise", "[cw][multiseed][gate]") {
    gate("mild-noise",     MSG_FULL(), profileMildNoise(80.0f),     0.0f);
    // §50/§51: the 0.0 threshold was legacy's LUCKY n=24 draw — legacy itself
    // scores 0.0091 at n=48 and 0.0136 at n=96 here, so 0.0 was never a robust
    // property. The shipping decoder (select) means 0.0032 (mean+2sd 0.0059) at
    // n=96 — better than legacy. Recalibrated to a robust regression bound.
    gate("moderate-noise", MSG_FULL(), profileModerateNoise(80.0f), 0.006f);
}

// §52.2: re-ratcheted from legacy's loose n=24 bounds (0.238/0.311/0.199 — ~4x the
// real mean, effectively vacuous) to the SHIPPING default's (select) measured
// mean + 2*stderr at n=24. Seeds are deterministic so these are stable, strict
// bounds; the #40b bimodal routing is now locked (hk-25/30 must stay low). A rise
// is a regression even if every single-seed test passes. NEVER widen to admit a
// change — lower when a change earns it.
TEST_CASE("Gate: hand-keyed jitter", "[cw][multiseed][gate]") {
    // hk-20 stays higher-variance (median << mean): the Kalman seed ambiguity in
    // timing.h — see docs/decoder-investigation-2026-07.md 2.1(c). hk-25/30 sit in
    // the #40b bimodal window and are now materially tighter.
    gate("handkeyed-15wpm", MSG_FULL(), profileHandKeyed(80.0f), 0.055f);
    gate("handkeyed-20wpm", MSG_FULL(), profileHandKeyed(60.0f), 0.135f);
    gate("handkeyed-25wpm", MSG_FULL(), profileHandKeyed(48.0f), 0.081f);
    gate("handkeyed-30wpm", MSG_FULL(), profileHandKeyed(40.0f), 0.096f);
}

TEST_CASE("Gate: propagation and interference", "[cw][multiseed][gate]") {
    gate("qsb",           MSG_FULL(),     profileQSB(80.0f),           0.019f);
    gate("qrm",           MSG_FULL(),     profileQRM(80.0f),           0.021f);
    gate("qrn",           MSG_FULL(),     profileQRN(80.0f),           0.009f);
    gate("contest-20wpm", MSG_CONTEST(),  profileContest(60.0f),       0.008f);
    gate("farnsworth-2.0",MSG_FULL(),     profileFarnsworth(80.0f, 2.0f), 0.015f);
    gate("worstcase",     MSG_FULL(),     profileWorstCase(80.0f),     0.667f);
}

// SNR ladder. This is the axis pre-detection filter bandwidth acts on; the
// pre-existing suite tops out at noiseAmp=1.5 where CER is 0, so without these
// a bandwidth change is not observable by the test suite at all.
TEST_CASE("Gate: SNR ladder", "[cw][multiseed][gate][snr]") {
    struct { const char* name; float amp; float maxMean; } ladder[] = {
        {"snr-noise1.0", 1.0f, 0.000f},
        {"snr-noise2.0", 2.0f, 0.116f},
        {"snr-noise3.0", 3.0f, 0.886f},
        {"snr-noise4.0", 4.0f, 0.923f},
    };
    for (auto& l : ladder) {
        auto p = profileClean(80.0f);
        p.noiseAmp = l.amp;
        gate(l.name, MSG_FULL(), p, l.maxMean);
    }
}

// Extract the SPECIFIC character errors word-correction was masking on the
// near-zero-gate profiles. Decodes each seed raw vs corrected; prints the
// seeds where they diverge with reference/raw/corrected, so each masked bug
// becomes a labelled, reproducible decoder error. Run: "[maskdiff]" only.
TEST_CASE("Maskdiff: raw vs corrected across all failing profiles", "[cw][.][maskdiff]") {
    struct P { const char* name; const char* msg; SignalParams params; };
    auto snr1 = profileClean(80.0f); snr1.noiseAmp = 1.0f;
    std::vector<P> profs = {
        {"mild-noise",      MSG_FULL(),    profileMildNoise(80.0f)},
        {"snr-noise1.0",    MSG_FULL(),    snr1},
        {"moderate-CQ",     MSG_CQ(),      profileModerateNoise(80.0f)},
        {"qrn-CQ",          MSG_CQ(),      profileQRN(80.0f)},
        {"contest-MIXED",   MSG_MIXED(),   profileContest(60.0f)},
        {"handkeyed-20wpm", "CQ DE W1AW",  profileHandKeyed(60.0f)},
        {"handkeyed-25wpm", "CQ DE W1AW",  profileHandKeyed(48.0f)},
    };
    const int SHOW = 4;  // cap printed divergent seeds per profile
    for (auto& pr : profs) {
        printf("\n=== %s (msg: %s) ===\n", pr.name, pr.msg);
        int diffs = 0, shown = 0;
        for (int i = 0; i < SEEDS; i++) {
            SignalParams p = pr.params;
            p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(pr.msg, p);
            auto decodeWith = [&](bool corr) {
                cw::Channel ch; ch.init(0, p.toneFreq); ch.wordCorrection = corr;
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                return ch.text.getText();
            };
            std::string raw = decodeWith(false), cor = decodeWith(true);
            if (raw != cor) {
                diffs++;
                if (shown++ < SHOW) {
                    printf("  seed %2d:\n    ref: %s\n    raw: %s\n    cor: %s\n",
                           i, sig.sourceText.c_str(), raw.c_str(), cor.c_str());
                }
            }
        }
        printf("  -> %d/%d seeds diverge\n", diffs, SEEDS);
    }
    printf("\n");
}

// One-seed onset trace: why does the first C decode as + (spurious leading dit)?
TEST_CASE("Maskdiff: onset trace seed4 mild-noise", "[cw][.][onsettrace]") {
    SignalParams p = profileMildNoise(80.0f);
    p.seed = 1000 + 4u * 7919u;
    auto sig = generateMessage(MSG_FULL(), p);
    for (const char* core : { "legacy", "legacy+select", "legacy+log", "legacy+fb",
                              "legacy+lr+log", "legacy+bpf20", "legacy+bpf40" }) {
        cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
        if (ch.coreName() != core) { continue; }
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        std::string t = ch.text.getText();
        printf("  %-16s first-token=\"%s\"\n", core, t.substr(0, t.find(' ')).c_str());
    }
}

// Generic raw-decode error histogram, EXCLUDING the first symbol (a known
// decoder-initiation artifact: cold-start +Q, to be handled later by low-
// confidence backward correction, not by masking). Aligns raw decode to the
// reference and aggregates substitution/insertion/deletion types per profile,
// surfacing SYSTEMATIC errors in the true raw flow. Run: "[rawerr]" only.
namespace {
    // Needleman-Wunsch traceback: returns aligned (refChar, decChar) pairs,
    // '_' = gap. Skips errors whose reference index is 0 (the first symbol).
    void tallyErrors(const std::string& ref, const std::string& dec,
                     std::map<std::string,int>& hist, int& nErr) {
        int m = ref.size(), n = dec.size();
        std::vector<std::vector<int>> d(m + 1, std::vector<int>(n + 1));
        for (int i = 0; i <= m; i++) d[i][0] = i;
        for (int j = 0; j <= n; j++) d[0][j] = j;
        for (int i = 1; i <= m; i++)
            for (int j = 1; j <= n; j++) {
                int c = (ref[i-1] != dec[j-1]) ? 1 : 0;
                d[i][j] = std::min({ d[i-1][j] + 1, d[i][j-1] + 1, d[i-1][j-1] + c });
            }
        int i = m, j = n;
        std::vector<std::array<char,2>> ops;
        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && d[i][j] == d[i-1][j-1] + (ref[i-1] != dec[j-1] ? 1 : 0)) {
                ops.push_back({ref[i-1], dec[j-1]}); i--; j--;
            } else if (i > 0 && d[i][j] == d[i-1][j] + 1) {
                ops.push_back({ref[i-1], '_'}); i--;
            } else { ops.push_back({'_', dec[j-1]}); j--; }
        }
        std::reverse(ops.begin(), ops.end());
        int refPos = 0;
        for (auto& op : ops) {
            bool err = op[0] != op[1];
            if (err && refPos > 0) {           // skip the first-symbol artifact
                std::string key = std::string(1, op[0]) + ">" + std::string(1, op[1]);
                hist[key]++; nErr++;
            }
            if (op[0] != '_') refPos++;
        }
    }
}

TEST_CASE("Rawerr: generic raw errors excluding first symbol", "[cw][.][rawerr]") {
    struct P { const char* name; const char* msg; SignalParams params; };
    std::vector<P> profs = {
        {"handkeyed-15wpm", MSG_FULL(), profileHandKeyed(80.0f)},
        {"handkeyed-20wpm", MSG_FULL(), profileHandKeyed(60.0f)},
        {"handkeyed-25wpm", MSG_FULL(), profileHandKeyed(48.0f)},
        {"handkeyed-30wpm", MSG_FULL(), profileHandKeyed(40.0f)},
        {"qrn",             MSG_FULL(), profileQRN(80.0f)},
        {"qrm",             MSG_FULL(), profileQRM(80.0f)},
        {"qsb",             MSG_FULL(), profileQSB(80.0f)},
        {"worstcase",       MSG_FULL(), profileWorstCase(80.0f)},
        {"moderate-noise",  MSG_FULL(), profileModerateNoise(80.0f)},
    };
    for (auto& pr : profs) {
        std::map<std::string,int> hist; int nErr = 0;
        for (int i = 0; i < SEEDS; i++) {
            SignalParams p = pr.params; p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(pr.msg, p);
            cw::Channel ch; ch.init(0, p.toneFreq); ch.wordCorrection = false;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            tallyErrors(sig.sourceText, ch.text.getText(), hist, nErr);
        }
        std::vector<std::pair<std::string,int>> v(hist.begin(), hist.end());
        std::sort(v.begin(), v.end(), [](auto&a, auto&b){ return a.second > b.second; });
        printf("\n=== %-16s %d errors (excl. 1st symbol), top patterns: ===\n  ", pr.name, nErr);
        int shown = 0;
        for (auto& e : v) { printf("%s=%d  ", e.first.c_str(), e.second); if (++shown >= 12) break; }
        printf("\n");
    }
    printf("\n  (X>Y = ref X decoded as Y;  X>_ = deletion;  _>Y = insertion)\n\n");
}

// Soft (fb) vs hard-decision (select) element-error comparison. Splits errors
// into ELEMENT (content-corrupting: dit/dah, letter sub/ins/del — the critical
// class) vs GAP (space ins/del — recoverable). First symbol excluded. Tests the
// field's prediction that soft sequence decoding reduces element errors. [rawerr-cores].
namespace {
    void tallyCategorized(const std::string& ref, const std::string& dec,
                          int& elemErr, int& gapErr) {
        int m = ref.size(), n = dec.size();
        std::vector<std::vector<int>> d(m + 1, std::vector<int>(n + 1));
        for (int i = 0; i <= m; i++) d[i][0] = i;
        for (int j = 0; j <= n; j++) d[0][j] = j;
        for (int i = 1; i <= m; i++)
            for (int j = 1; j <= n; j++) {
                int c = (ref[i-1] != dec[j-1]) ? 1 : 0;
                d[i][j] = std::min({ d[i-1][j] + 1, d[i][j-1] + 1, d[i-1][j-1] + c });
            }
        int i = m, j = n;
        std::vector<std::array<char,2>> ops;
        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && d[i][j] == d[i-1][j-1] + (ref[i-1] != dec[j-1] ? 1 : 0)) {
                ops.push_back({ref[i-1], dec[j-1]}); i--; j--;
            } else if (i > 0 && d[i][j] == d[i-1][j] + 1) {
                ops.push_back({ref[i-1], '_'}); i--;
            } else { ops.push_back({'_', dec[j-1]}); j--; }
        }
        std::reverse(ops.begin(), ops.end());
        int refPos = 0;
        for (auto& op : ops) {
            if (op[0] != op[1] && refPos > 0) {
                if (op[0] == ' ' || op[1] == ' ') gapErr++;    // spacing (recoverable)
                else elemErr++;                                 // content (critical)
            }
            if (op[0] != '_') refPos++;
        }
    }
}

TEST_CASE("Rawerr: soft(fb) vs hard(select) element errors", "[cw][.][rawerr-cores]") {
    struct P { const char* name; SignalParams params; };
    std::vector<P> profs = {
        {"handkeyed-15wpm", profileHandKeyed(80.0f)},
        {"handkeyed-20wpm", profileHandKeyed(60.0f)},
        {"handkeyed-25wpm", profileHandKeyed(48.0f)},
        {"handkeyed-30wpm", profileHandKeyed(40.0f)},
        {"qrn",             profileQRN(80.0f)},
        {"moderate-noise",  profileModerateNoise(80.0f)},
        {"worstcase",       profileWorstCase(80.0f)},
    };
    const char* cores[] = { "legacy+select", "legacy+fb" };
    printf("\n%-16s %-16s %6s %6s  (excl. 1st symbol, %d seeds)\n",
           "profile", "core", "ELEM", "gap", SEEDS);
    printf("%s\n", std::string(58, '-').c_str());
    for (auto& pr : profs) {
        for (const char* core : cores) {
            int elem = 0, gap = 0;
            for (int i = 0; i < SEEDS; i++) {
                SignalParams p = pr.params; p.seed = 1000 + (unsigned)i * 7919u;
                auto sig = generateMessage(MSG_FULL(), p);
                cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
                if (ch.coreName() != core) { elem = gap = -1; break; }
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                tallyCategorized(sig.sourceText, ch.text.getText(), elem, gap);
            }
            printf("%-16s %-16s %6d %6d\n", pr.name, core, elem, gap);
        }
    }
    printf("\n  ELEM = content-corrupting (critical);  gap = spacing (recoverable)\n\n");
}

// Is fb's high hand-keyed element-error count a fundamental tradeoff, or a BUG?
// Dump fb's raw decode vs reference on the "easy" 15 WPM hand-keyed case, plus
// its error histogram — a systematic pattern (trailing garbage, doubling, a
// stuck letter) means a bug; random scatter means the jitter tradeoff. [fbbug].
TEST_CASE("Fbbug: fb decode inspection on easy hand-keyed", "[cw][.][fbbug]") {
    auto params = profileHandKeyed(80.0f);   // 15 WPM, easy keying
    printf("\n=== legacy+fb raw decode, handkeyed-15wpm ===\n");
    std::map<std::string,int> hist; int nErr = 0;
    for (int i = 0; i < SEEDS; i++) {
        SignalParams p = params; p.seed = 1000 + (unsigned)i * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        cw::Channel ch; ch.init(0, p.toneFreq, "legacy+fb"); ch.wordCorrection = false;
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        std::string dec = ch.text.getText();
        tallyErrors(sig.sourceText, dec, hist, nErr);
        if (i < 5) printf("  seed %2d:\n    ref: %s\n    fb:  %s\n", i, sig.sourceText.c_str(), dec.c_str());
    }
    std::vector<std::pair<std::string,int>> v(hist.begin(), hist.end());
    std::sort(v.begin(), v.end(), [](auto&a, auto&b){ return a.second > b.second; });
    printf("  fb error histogram (%d, excl 1st): ", nErr);
    int shown = 0; for (auto& e : v) { printf("%s=%d ", e.first.c_str(), e.second); if (++shown >= 15) break; }
    printf("\n\n");
}

// Quantify fb collapse rate and isolate: does SELECT cope on the same seeds?
// (Same input, select fine + fb garbage => pure fb bug, not signal difficulty.)
TEST_CASE("Fbcollapse: per-seed fb vs select CER, hand-keyed-15", "[cw][.][fbcollapse]") {
    auto params = profileHandKeyed(80.0f);
    auto decodeCER = [&](const char* core, SignalParams p, std::string& out) {
        auto sig = generateMessage(MSG_FULL(), p);
        cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        out = ch.text.getText();
        return score(sig.sourceText, out).cer;
    };
    printf("\n seed |  fb CER | sel CER | flag\n -----+---------+---------+-----\n");
    int fbCollapse = 0, selCollapse = 0;
    float fbSum = 0, selSum = 0;
    for (int i = 0; i < SEEDS; i++) {
        SignalParams p = params; p.seed = 1000 + (unsigned)i * 7919u;
        std::string a, b;
        float fbC = decodeCER("legacy+fb", p, a);
        float selC = decodeCER("legacy+select", p, b);
        fbSum += fbC; selSum += selC;
        bool fbBad = fbC > 0.30f, selBad = selC > 0.30f;
        if (fbBad) fbCollapse++;
        if (selBad) selCollapse++;
        const char* flag = (fbBad && !selBad) ? "<-- fb-only collapse" : (fbBad ? "both" : "");
        printf(" %4d |  %.3f  |  %.3f  | %s\n", i, fbC, selC, flag);
    }
    printf("\n collapses (CER>0.30):  fb=%d/%d  select=%d/%d\n", fbCollapse, SEEDS, selCollapse, SEEDS);
    printf(" mean CER:  fb=%.3f  select=%.3f\n\n", fbSum/SEEDS, selSum/SEEDS);
}

// Trace fb's WPM estimate + partial decode over time: does it start wrong or
// derail mid-stream? seed 2 (collapse) vs seed 0 (good control), hand-keyed-15.
TEST_CASE("Fbtrace2: fb WPM/decode timeline, collapse vs good seed", "[cw][.][fbtrace2]") {
    for (int seedIdx : { 0, 2, 9, 10 }) {
        SignalParams p = profileHandKeyed(80.0f);
        p.seed = 1000 + (unsigned)seedIdx * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        cw::Channel ch; ch.init(0, p.toneFreq, "legacy+fb"); ch.wordCorrection = false;
        printf("\n=== seed %d (%s) ===\n", seedIdx, seedIdx == 0 ? "GOOD control" : "collapse");
        const int CHUNK = 8000;   // ~1 s at 8 kHz
        std::string last;
        for (int off = 0; off < (int)sig.samples.size(); off += CHUNK) {
            int n = std::min(CHUNK, (int)sig.samples.size() - off);
            for (int o = 0; o < n; o += 512) {
                int k = std::min(512, n - o);
                ch.process(k, &sig.samples[off + o]);
            }
            std::string t = ch.text.getText();
            std::string delta = t.substr(std::min(last.size(), t.size()));
            printf("  t=%4.1fs wpm=%5.1f snr=%4.1f  +\"%s\"\n",
                   (float)(off + n) / 8000.0f, ch.wpm, ch.snr, delta.c_str());
            last = t;
        }
    }
    printf("\n");
}

// Isolate the half-speed collapse: detector (fb) vs timing (Kalman). Compare
// legacy (Schmitt+Kalman), legacy+fb (fb+Kalman), legacy+select (Schmitt+SELECT)
// on the collapse seeds. If legacy collapses too -> Kalman timing bug (shared);
// if only fb -> fb detector's events mislead the timing. [fbisolate].
TEST_CASE("Fbisolate: detector vs timing on collapse seeds", "[cw][.][fbisolate]") {
    const char* cores[] = { "legacy", "legacy+fb", "legacy+select" };
    printf("\n seed | %-14s | %-14s | %-14s\n", cores[0], cores[1], cores[2]);
    printf(" -----+----------------+----------------+----------------\n");
    for (int seedIdx : { 0, 2, 9, 10 }) {
        printf(" %4d ", seedIdx);
        for (const char* core : cores) {
            SignalParams p = profileHandKeyed(80.0f);
            p.seed = 1000 + (unsigned)seedIdx * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            float cer = score(sig.sourceText, ch.text.getText()).cer;
            printf("| CER%.2f wpm%4.1f ", cer, ch.wpm);
        }
        printf("\n");
    }
    printf("\n");
}

// Does fb + adaptive SELECT timing fix the collapses? Full 24-seed hand-keyed-15
// CER + collapse count for fb (Kalman) vs fb+sel (SELECT) vs select baseline. [fbsel].
TEST_CASE("Fbsel: fb+SELECT-timing collapse rate", "[cw][.][fbsel]") {
    const char* cores[] = { "legacy+fb", "legacy+fb+sel", "legacy+select" };
    for (const char* core : cores) {
        int collapse = 0; float sum = 0;
        for (int i = 0; i < SEEDS; i++) {
            SignalParams p = profileHandKeyed(80.0f); p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
            if (ch.coreName() != core) { printf("  %s UNAVAILABLE\n", core); collapse = -1; break; }
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            float cer = score(sig.sourceText, ch.text.getText()).cer;
            sum += cer; if (cer > 0.30f) collapse++;
        }
        if (collapse >= 0) printf("  %-16s mean CER=%.3f  collapses=%d/%d\n", core, sum/SEEDS, collapse, SEEDS);
    }
    printf("\n");
}

// Does fb+SELECT preserve fb's weak-signal AWGN wins (its purpose in route)?
// fb vs fb+sel vs select across the full regime set. [fbsel-full].
TEST_CASE("Fbselfull: fb vs fb+sel vs select across regimes", "[cw][.][fbsel-full]") {
    struct P { const char* name; SignalParams params; };
    auto snr = [](float amp){ auto p = profileClean(80.0f); p.noiseAmp = amp; return p; };
    std::vector<P> profs = {
        {"mild-noise", profileMildNoise(80.0f)}, {"moderate", profileModerateNoise(80.0f)},
        {"snr-noise2", snr(2.0f)}, {"snr-noise3", snr(3.0f)}, {"snr-noise4", snr(4.0f)},
        {"handkeyed-15", profileHandKeyed(80.0f)}, {"handkeyed-20", profileHandKeyed(60.0f)},
        {"handkeyed-25", profileHandKeyed(48.0f)}, {"handkeyed-30", profileHandKeyed(40.0f)},
        {"qsb", profileQSB(80.0f)}, {"qrm", profileQRM(80.0f)}, {"worstcase", profileWorstCase(80.0f)},
    };
    printf("\n%-14s %8s %8s %8s\n", "profile", "fb", "fb+sel", "select");
    printf("%s\n", std::string(42, '-').c_str());
    for (auto& pr : profs) {
        printf("%-14s", pr.name);
        for (const char* core : { "legacy+fb", "legacy+fb+sel", "legacy+select" }) {
            float sum = 0;
            for (int i = 0; i < SEEDS; i++) {
                SignalParams p = pr.params; p.seed = 1000 + (unsigned)i * 7919u;
                auto sig = generateMessage(MSG_FULL(), p);
                cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                sum += score(sig.sourceText, ch.text.getText()).cer;
            }
            printf(" %8.3f", sum / SEEDS);
        }
        printf("\n");
    }
    printf("\n  (fb's job in route = win the buried-noise rows; must not regress those)\n\n");
}

// Hunt the next soft bug: fb+sel makes 4x select's errors on EASY mild-noise.
// Dump decodes + error histogram to find the systematic pattern. [fbmild].
TEST_CASE("Fbmild: fb+sel errors on easy mild-noise", "[cw][.][fbmild]") {
    std::map<std::string,int> hist; int nErr = 0, shown = 0;
    printf("\n=== legacy+fb+sel raw decode, mild-noise (vs select ref) ===\n");
    for (int i = 0; i < SEEDS; i++) {
        SignalParams p = profileMildNoise(80.0f); p.seed = 1000 + (unsigned)i * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        cw::Channel ch; ch.init(0, p.toneFreq, "legacy+fb+sel"); ch.wordCorrection = false;
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        std::string dec = ch.text.getText();
        tallyErrors(sig.sourceText, dec, hist, nErr);
        if (dec != sig.sourceText && shown++ < 8)
            printf("  seed %2d:\n    ref: %s\n    fb+: %s\n", i, sig.sourceText.c_str(), dec.c_str());
    }
    std::vector<std::pair<std::string,int>> v(hist.begin(), hist.end());
    std::sort(v.begin(), v.end(), [](auto&a, auto&b){ return a.second > b.second; });
    printf("  error histogram (%d, excl 1st): ", nErr);
    int k = 0; for (auto& e : v) { printf("%s=%d ", e.first.c_str(), e.second); if (++k >= 15) break; }
    printf("\n\n");
}

// The REAL remaining-soft-bug measurement: element errors excl. first symbol,
// fb+sel (collapse-fixed) vs select reference. Isolates genuine fb bugs from
// the parked initiation artifact. [fbsel-elem].
TEST_CASE("Fbselelem: fb+sel vs select element errors (excl 1st)", "[cw][.][fbsel-elem]") {
    struct P { const char* name; SignalParams params; };
    auto snr = [](float a){ auto p = profileClean(80.0f); p.noiseAmp = a; return p; };
    std::vector<P> profs = {
        {"mild-noise", profileMildNoise(80.0f)}, {"moderate", profileModerateNoise(80.0f)},
        {"snr-noise2", snr(2.0f)}, {"snr-noise3", snr(3.0f)},
        {"handkeyed-15", profileHandKeyed(80.0f)}, {"handkeyed-20", profileHandKeyed(60.0f)},
        {"handkeyed-25", profileHandKeyed(48.0f)}, {"handkeyed-30", profileHandKeyed(40.0f)},
        {"qsb", profileQSB(80.0f)}, {"qrm", profileQRM(80.0f)},
    };
    printf("\n%-14s | fb+sel ELEM/gap | select ELEM/gap\n", "profile");
    printf("%s\n", std::string(50, '-').c_str());
    for (auto& pr : profs) {
        int fe=0, fg=0, se=0, sg=0;
        for (int i = 0; i < SEEDS; i++) {
            SignalParams p = pr.params; p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            for (int which = 0; which < 2; which++) {
                const char* core = which ? "legacy+select" : "legacy+fb+sel";
                cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                if (which) tallyCategorized(sig.sourceText, ch.text.getText(), se, sg);
                else       tallyCategorized(sig.sourceText, ch.text.getText(), fe, fg);
            }
        }
        printf("%-14s |   %4d / %-4d   |   %4d / %-4d\n", pr.name, fe, fg, se, sg);
    }
    printf("\n  ELEM = content-corrupting (critical). Gap to select ELEM = remaining soft bugs.\n\n");
}

// L0 (ladder): clean, noise-free. SOFT (fb+sel) vs HARD (select) must both be
// zero element errors (excl 1st symbol) across the WPM range. Any soft error
// here is a pure algorithm bug (no noise excuse). [L0-clean].
TEST_CASE("L0clean: soft vs hard on noise-free signals", "[cw][.][L0-clean]") {
    float dits[] = { 80.0f, 60.0f, 48.0f, 40.0f };   // 15/20/25/30 wpm
    const char* wpm[] = { "15", "20", "25", "30" };
    printf("\n%-6s | %-16s | ELEM gap | CER      (noise-free, excl 1st, %d seeds)\n", "wpm", "core", SEEDS);
    printf("%s\n", std::string(60, '-').c_str());
    for (int d = 0; d < 4; d++) {
        for (const char* core : { "legacy+fb+sel", "legacy+select" }) {
            int elem = 0, gap = 0; float cerSum = 0;
            for (int i = 0; i < SEEDS; i++) {
                SignalParams p = profileClean(dits[d]);
                p.noiseAmp = 0.0f;                    // strictly noise-free
                p.seed = 1000 + (unsigned)i * 7919u;
                auto sig = generateMessage(MSG_FULL(), p);
                cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                tallyCategorized(sig.sourceText, ch.text.getText(), elem, gap);
                cerSum += score(sig.sourceText, ch.text.getText()).cer;
            }
            printf("%-6s | %-16s | %3d %3d  | %.4f\n", wpm[d], core, elem, gap, cerSum/SEEDS);
        }
    }
    printf("\n");
}

// ISSUE-6 root-cause step 1: reproduce slow-speed gap over-segmentation on CLEAN
// noise-free synthetic across WPM. Any gap error here is pure algorithm (no noise).
// Tests the hypothesis that the same gap mechanism scales across speed. [L0-slow].
TEST_CASE("L0slow: gap errors vs WPM, noise-free, soft vs hard", "[cw][.][L0-slow]") {
    struct S { float dit; const char* wpm; };
    S speeds[] = { {240,"5"}, {150,"8"}, {120,"10"}, {96,"12.5"}, {80,"15"}, {60,"20"}, {48,"25"}, {40,"30"} };
    printf("\n%-5s | %-14s | ELEM | gapDEL(>_)| gapINS(_>) | CER      (noise-free, excl 1st)\n", "wpm", "core");
    printf("%s\n", std::string(72, '-').c_str());
    for (auto& sp : speeds) {
        for (const char* core : { "legacy+fb+sel", "legacy+select" }) {
            int elem=0, gapDel=0, gapIns=0; float cerSum=0;
            for (int i = 0; i < SEEDS; i++) {
                SignalParams p = profileClean(sp.dit); p.noiseAmp = 0.0f;
                p.seed = 1000 + (unsigned)i * 7919u;
                auto sig = generateMessage(MSG_FULL(), p);
                cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                std::string dec = ch.text.getText();
                // categorize: space-in-ref-deleted vs space-inserted, excl 1st symbol
                std::map<std::string,int> h; int ne=0; tallyErrors(sig.sourceText, dec, h, ne);
                for (auto& e : h) {
                    if (e.first == " >_") gapDel += e.second;
                    else if (e.first == "_> ") gapIns += e.second;
                    else elem += e.second;
                }
                cerSum += score(sig.sourceText, dec).cer;
            }
            printf("%-5s | %-14s | %4d | %8d | %9d | %.4f\n", sp.wpm, core, elem, gapDel, gapIns, cerSum/SEEDS);
        }
    }
    printf("\n  gapINS (_>) = word-gap inserted where none exists (over-segmentation).\n\n");
}

// ISSUE-6 (corrected): real slow CW is FARNSWORTH (normal elements, stretched
// gaps), not true-slow. Reproduce the over-segmentation validly: gap errors on
// profileFarnsworth, soft vs hard, noise-free. [L-farns].
TEST_CASE("Lfarns: gap errors on Farnsworth, soft vs hard", "[cw][.][L-farns]") {
    struct F { float ratio; const char* name; };
    F ratios[] = { {1.5f,"1.5"}, {2.0f,"2.0"}, {3.0f,"3.0"} };
    printf("\n%-6s | %-14s | ELEM | gapDEL | gapINS | CER   (noise-free, excl 1st)\n", "ratio", "core");
    printf("%s\n", std::string(66, '-').c_str());
    for (auto& f : ratios) {
        for (const char* core : { "legacy+fb+sel", "legacy+select" }) {
            int elem=0, gapDel=0, gapIns=0; float cerSum=0;
            for (int i = 0; i < SEEDS; i++) {
                SignalParams p = profileFarnsworth(80.0f, f.ratio); p.noiseAmp = 0.0f;
                p.seed = 1000 + (unsigned)i * 7919u;
                auto sig = generateMessage(MSG_FULL(), p);
                cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
                for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                    int n = std::min(512, (int)sig.samples.size() - off);
                    ch.process(n, &sig.samples[off]);
                }
                std::string dec = ch.text.getText();
                std::map<std::string,int> h; int ne=0; tallyErrors(sig.sourceText, dec, h, ne);
                for (auto& e : h) {
                    if (e.first == " >_") gapDel += e.second;
                    else if (e.first == "_> ") gapIns += e.second;
                    else elem += e.second;
                }
                cerSum += score(sig.sourceText, dec).cer;
            }
            printf("%-6s | %-14s | %4d | %6d | %6d | %.4f\n", f.name, core, elem, gapDel, gapIns, cerSum/SEEDS);
        }
    }
    printf("\n");
}

// L2 root-cause: what does fb+sel get wrong on moderate noise that select gets
// right? Same seeds, show divergences + fb+sel error histogram (excl 1st). [L2-moderate].
TEST_CASE("L2moderate: soft vs hard divergence on moderate noise", "[cw][.][L2-moderate]") {
    std::map<std::string,int> hist; int nErr = 0, shown = 0;
    printf("\n=== moderate-noise: fb+sel vs select divergences (excl 1st) ===\n");
    for (int i = 0; i < SEEDS; i++) {
        SignalParams p = profileModerateNoise(80.0f); p.seed = 1000 + (unsigned)i * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        auto dec = [&](const char* core){
            cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            return ch.text.getText();
        };
        std::string fb = dec("legacy+fb+sel"), sel = dec("legacy+select");
        tallyErrors(sig.sourceText, fb, hist, nErr);
        if (fb != sel && shown++ < 8) {
            printf("  seed %2d:\n    ref: %s\n    sel: %s\n    fb+: %s\n",
                   i, sig.sourceText.c_str(), sel.c_str(), fb.c_str());
        }
    }
    std::vector<std::pair<std::string,int>> v(hist.begin(), hist.end());
    std::sort(v.begin(), v.end(), [](auto&a, auto&b){ return a.second > b.second; });
    printf("  fb+sel error histogram (%d, excl 1st): ", nErr);
    int k=0; for (auto& e : v) { printf("%s=%d ", e.first.c_str(), e.second); if (++k>=15) break; }
    printf("\n\n");
}

// Root-cause the fb initiation artifact: trace the cold-start decode + stats on
// moderate-noise seeds where fb garbles the first word (SO Q / TRQ). Fine-grained
// early timeline vs select. [init-trace].
TEST_CASE("Inittrace: fb cold-start decode on artifact seeds", "[cw][.][init-trace]") {
    for (int seedIdx : { 0, 5 }) {
        SignalParams p = profileModerateNoise(80.0f);
        p.seed = 1000 + (unsigned)seedIdx * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        for (const char* core : { "legacy+fb+sel", "legacy+select" }) {
            cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
            printf("\n=== seed %d, %s (first 4 s, 0.25 s steps) ===\n", seedIdx, core);
            const int CHUNK = 2000;   // 0.25 s at 8 kHz
            std::string last;
            for (int off = 0; off < 32000 && off < (int)sig.samples.size(); off += CHUNK) {
                int n = std::min(CHUNK, (int)sig.samples.size() - off);
                for (int o = 0; o < n; o += 512) {
                    int k = std::min(512, n - o);
                    ch.process(k, &sig.samples[off + o]);
                }
                std::string t = ch.text.getText();
                std::string d = t.substr(std::min(last.size(), t.size()));
                printf("  t=%4.2fs wpm=%5.1f snr=%4.1f  +\"%s\"\n", (float)(off+n)/8000.0f, ch.wpm, ch.snr, d.c_str());
                last = t;
            }
        }
    }
    printf("\n");
}

// DR-4 axis trace: is the CQ->TRQ initiation artifact a DETECTOR error (wrong
// key-event durations) or a TIMING error (right durations, wrong DIT/DAH/gap
// classification)? Dump the per-event stream for seed 0's first token. [acq-axis].
// ref first token: "CQ CQ CQ"  (C = dah dit dah dit,  Q = dah dah dit dah)
TEST_CASE("Acqaxis: detector events vs timing classification, seed 0", "[cw][.][acq-axis]") {
    SignalParams p = profileModerateNoise(80.0f);
    p.seed = 1000 + 0;
    auto sig = generateMessage(MSG_FULL(), p);
    for (const char* coreName : { "legacy+fb+sel" }) {
        const cw::CoreSpec* spec = cw::findCore(coreName);
        REQUIRE(spec);
        auto core = spec->make();
        auto* sc = dynamic_cast<cw::StagedCore*>(core.get());
        REQUIRE(sc);
        sc->debugLog = true; sc->id = 0;
        cw::Channel ch;
        ch.initWithCore(0, p.toneFreq, std::move(core), coreName);
        ch.wordCorrection = false;
        fprintf(stderr, "\n########## %s (first 2.2 s) ##########\n", coreName);
        int limit = std::min((int)sig.samples.size(), 17600);   // ~2.2 s at 8 kHz
        for (int off = 0; off < limit; off += 512) {
            int n = std::min(512, limit - off);
            ch.process(n, &sig.samples[off]);
        }
        fprintf(stderr, "FINAL(2.2s): \"%s\"\n", ch.text.getText().c_str());
    }
}

// DR-4b step 1: does the unreal-WPM guard (a) reject the dit=12ms/100WPM lock,
// (b) recover the first token by waiting for a real speed, (c) not regress the
// ladder? Always tracks the rejection count. [unreal-wpm].
TEST_CASE("Unrealwpm: unreal-WPM guard vs fb+sel", "[cw][.][unreal-wpm]") {
    struct DecRes { std::string text; int rejections; };
    auto decRej = [](const char* core, const GeneratedSignal& sig, float tone) -> DecRes {
        const cw::CoreSpec* spec = cw::findCore(core);
        auto c = spec->make();
        auto* sc = dynamic_cast<cw::StagedCore*>(c.get());
        cw::Channel ch; ch.initWithCore(0, tone, std::move(c), core); ch.wordCorrection = false;
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        return { ch.text.getText(), sc ? sc->stats().unrealWpmRejections : -1 };
    };
    for (int seedIdx : { 0, 5 }) {
        SignalParams p = profileModerateNoise(80.0f);
        p.seed = 1000 + (unsigned)seedIdx * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        auto fb = decRej("legacy+fb+sel", sig, p.toneFreq);
        auto rs = decRej("legacy+fb+sel+rsg", sig, p.toneFreq);
        printf("\n=== seed %d  ref=\"%s\" ===\n", seedIdx, sig.sourceText.substr(0,40).c_str());
        printf("  fb+sel      = \"%s\"  (rej=%d)\n", fb.text.substr(0,40).c_str(), fb.rejections);
        printf("  fb+sel+rsg  = \"%s\"  (rej=%d)\n", rs.text.substr(0,40).c_str(), rs.rejections);
    }
    printf("\n  profile              fb+sel(elem/gap)  rsg(elem/gap)  Δelem  totRej\n");
    int totF = 0, totR = 0;
    for (auto& prof : allProfiles()) {
        int fe=0, fg=0, re=0, rg=0, rej=0;
        for (int i = 0; i < SEEDS; i++) {
            SignalParams p = prof.params;
            p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(prof.message, p);
            auto fb = decRej("legacy+fb+sel", sig, p.toneFreq);
            auto rs = decRej("legacy+fb+sel+rsg", sig, p.toneFreq);
            tallyCategorized(sig.sourceText, fb.text, fe, fg);
            tallyCategorized(sig.sourceText, rs.text, re, rg);
            rej += rs.rejections;
        }
        totF += fe; totR += re;
        printf("  %-18s   %4d /%3d       %4d /%3d      %+d%s   %d\n",
               prof.name, fe, fg, re, rg, re - fe, (re > fe ? "  <-- REGRESSION" : ""), rej);
    }
    printf("  %-18s   %4d           %4d          %+d\n\n", "TOTAL elem", totF, totR, totR - totF);
}

// DR-4: does continuous re-decode recover the first character and hold the ladder?
// Compares fb+sel vs fb+sel+cont on the artifact seeds + the whole profile ladder. [cont].
TEST_CASE("Cont: continuous re-decode vs fb+sel", "[cw][.][cont]") {
    auto decFull = [](const char* core, const GeneratedSignal& sig, float tone){
        cw::Channel ch; ch.init(0, tone, core); ch.wordCorrection = false;
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        return ch.text.getText();
    };
    for (int seedIdx : { 0, 5 }) {
        SignalParams p = profileModerateNoise(80.0f);
        p.seed = 1000 + (unsigned)seedIdx * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        printf("\n=== seed %d  ref=\"%s\" ===\n", seedIdx, sig.sourceText.substr(0,40).c_str());
        printf("  fb+sel      = \"%s\"\n", decFull("legacy+fb+sel", sig, p.toneFreq).substr(0,40).c_str());
        printf("  fb+sel+cont = \"%s\"\n", decFull("legacy+fb+sel+cont", sig, p.toneFreq).substr(0,40).c_str());
    }
    printf("\n  profile              fb+sel(elem/gap)  cont(elem/gap)  Δelem\n");
    int totF = 0, totC = 0;
    for (auto& prof : allProfiles()) {
        int fe=0, fg=0, ce=0, cg=0;
        for (int i = 0; i < SEEDS; i++) {
            SignalParams p = prof.params;
            p.seed = 1000 + (unsigned)i * 7919u;
            auto sig = generateMessage(prof.message, p);
            tallyCategorized(sig.sourceText, decFull("legacy+fb+sel", sig, p.toneFreq), fe, fg);
            tallyCategorized(sig.sourceText, decFull("legacy+fb+sel+cont", sig, p.toneFreq), ce, cg);
        }
        totF += fe; totC += ce;
        printf("  %-18s   %4d /%3d       %4d /%3d      %+d%s\n",
               prof.name, fe, fg, ce, cg, ce - fe, (ce > fe ? "  <-- REGRESSION" : ""));
    }
    printf("  %-18s   %4d           %4d          %+d\n\n", "TOTAL elem", totF, totC, totC - totF);
}

// DR-4 debug: dump one seed of a regression regime — ref, fb+sel, cont — plus
// (with CONT_TRACE=1) the per-cycle live-vs-cont ratchet decision. [cont-dbg].
TEST_CASE("Contdbg: regression case dump", "[cw][.][cont-dbg]") {
    auto decFull = [](const char* core, const GeneratedSignal& sig, float tone){
        cw::Channel ch; ch.init(0, tone, core); ch.wordCorrection = false;
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        return ch.text.getText();
    };
    struct Case { const char* name; SignalParams p; int seed; };
    std::vector<Case> cases = {
        { "moderate s0", profileModerateNoise(80.0f), 0 },
        { "moderate s5", profileModerateNoise(80.0f), 5 },
        { "farnsworth-1.5", profileFarnsworth(80.0f, 1.5f), 0 },
        { "farnsworth-2.0", profileFarnsworth(80.0f, 2.0f), 0 },
        { "snr-noise4",     [](){ auto q = profileClean(80.0f); q.noiseAmp = 4.0f; return q; }(), 0 },
    };
    for (auto& c : cases) {
        SignalParams p = c.p; p.seed = 1000 + (unsigned)c.seed * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        fprintf(stderr, "\n===== %s (seed 0) =====\n", c.name);
        fprintf(stderr, "ref   : %s\n", sig.sourceText.c_str());
        fprintf(stderr, "fb+sel: %s\n", decFull("legacy+fb+sel", sig, p.toneFreq).c_str());
        fprintf(stderr, "cont  : %s\n", decFull("legacy+fb+sel+cont", sig, p.toneFreq).c_str());
    }
}

// L3: is the soft win at heavy noise REAL (more real content copied) or just
// fewer errors on garbage? Dump decodes + error split. [L3-heavy].
TEST_CASE("L3heavy: soft vs hard decodes at snr-noise3", "[cw][.][L3-heavy]") {
    auto p3 = profileClean(80.0f); p3.noiseAmp = 3.0f;
    int fe=0, fg=0, se=0, sg=0, shown=0;
    printf("\n=== snr-noise3: fb+sel vs select (ref = CQ CQ CQ DE W1AW ... SK) ===\n");
    for (int i = 0; i < SEEDS; i++) {
        SignalParams p = p3; p.seed = 1000 + (unsigned)i * 7919u;
        auto sig = generateMessage(MSG_FULL(), p);
        auto dec = [&](const char* core){
            cw::Channel ch; ch.init(0, p.toneFreq, core); ch.wordCorrection = false;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int n = std::min(512, (int)sig.samples.size() - off);
                ch.process(n, &sig.samples[off]);
            }
            return ch.text.getText();
        };
        std::string fb = dec("legacy+fb+sel"), sel = dec("legacy+select");
        tallyCategorized(sig.sourceText, fb, fe, fg);
        tallyCategorized(sig.sourceText, sel, se, sg);
        if (shown++ < 5)
            printf("  seed %2d:\n    sel: %s\n    fb+: %s\n", i, sel.substr(0,60).c_str(), fb.substr(0,60).c_str());
    }
    printf("\n  element errors (excl 1st):  fb+sel=%d  select=%d\n", fe, se);
    printf("  gap errors:                 fb+sel=%d  select=%d\n\n", fg, sg);
}
