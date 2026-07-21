#pragma once
#include "cw_test_signals.h"
#include "cw_parallel.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <string>
#include <sstream>
#include <limits>

// Multi-seed benchmark statistics.
//
// Single-seed CER is quantized to 1/refChars and carries no variance estimate,
// so it cannot distinguish a real change from a lucky draw. These helpers run a
// profile over N independent noise realizations and report the distribution.
//
// Thresholds in tests must be set from measured means, never relaxed to admit a
// change. A regression is a rise in mean CER, not a single failing draw.
namespace cw_test {

    struct CERStats {
        float mean = 0;
        float median = 0;
        float p95 = 0;
        float worst = 0;
        float best = 0;
        float stddev = 0;
        float stderrMean = 0;   // sd / sqrt(n) — resolution of the mean
        float meanWER = 0;
        int seeds = 0;
        std::vector<float> samples;

        std::string summary() const {
            std::ostringstream o;
            o.setf(std::ios::fixed);
            o.precision(4);
            o << "n=" << seeds
              << " mean=" << mean << "(+-" << stderrMean << ")"
              << " med=" << median
              << " p95=" << p95
              << " worst=" << worst
              << " sd=" << stddev
              << " wer=" << meanWER;
            return o.str();
        }
    };

    inline CERStats summarize(std::vector<float> cers, float werSum) {
        CERStats s;
        s.seeds = (int)cers.size();
        if (cers.empty()) { return s; }

        double sum = 0;
        for (float c : cers) { sum += c; }
        s.mean = (float)(sum / cers.size());
        s.meanWER = werSum / cers.size();

        double var = 0;
        for (float c : cers) { double d = c - s.mean; var += d * d; }
        s.stddev = (cers.size() > 1) ? (float)std::sqrt(var / (cers.size() - 1)) : 0.0f;
        s.stderrMean = (cers.size() > 1) ? s.stddev / std::sqrt((float)cers.size()) : 0.0f;

        s.samples = cers;
        std::sort(cers.begin(), cers.end());
        s.best = cers.front();
        s.worst = cers.back();
        s.median = cers[cers.size() / 2];
        s.p95 = cers[std::min(cers.size() - 1, (size_t)std::llround(0.95 * (cers.size() - 1)))];
        return s;
    }

    // ── Paired comparison ───────────────────────────────────────
    //
    // Two cores run over the same seed list decode the same noise
    // realizations, so seed difficulty is a shared term that cancels in the
    // per-seed difference. Comparing the two means separately does not cancel
    // it: on worstcase the between-seed spread is an order of magnitude larger
    // than the effect being measured.
    //
    // Counting "better/worse" off mean differences with a 1e-6 float epsilon
    // treats a one-character shift across the whole seed set as a regression.

    struct PairedDelta {
        float meanDelta = 0;      // candidate - baseline; negative is an improvement
        float stderrDelta = 0;
        float t = 0;              // meanDelta / stderrDelta
        int nSeeds = 0;
        int nDiffer = 0;          // seeds where the two cores disagreed at all

        // |t| >= 2 is roughly the 95% two-sided threshold for these seed counts.
        bool significant() const { return nSeeds > 1 && std::fabs(t) >= 2.0f; }

        // Upper 95% bound on the regression. Significance alone cannot gate a
        // promotion: "not significant" is absence of evidence of harm, not
        // evidence of absence, and a real regression on a high-variance profile
        // reads ns. Promotion requires this bound to sit under a tolerance —
        // the candidate must demonstrate it is not harmful, not merely fail to
        // be caught.
        float worstCaseDelta() const { return meanDelta + 2.0f * stderrDelta; }

        // tol is in CER. One character of MSG_FULL is 1/71 = 0.0141, so a
        // tolerance below that is sub-character.
        bool harmful(float tol) const {
            return nSeeds < 2 || worstCaseDelta() > tol;
        }

        const char* verdict() const {
            if (!significant()) { return "ns"; }
            return meanDelta < 0 ? "BETTER" : "WORSE";
        }
    };

    inline PairedDelta comparePaired(const std::vector<float>& baseline,
                                     const std::vector<float>& candidate) {
        PairedDelta d;
        if (baseline.size() != candidate.size() || baseline.size() < 2) { return d; }
        d.nSeeds = (int)baseline.size();

        double sum = 0;
        for (int i = 0; i < d.nSeeds; i++) {
            const double diff = (double)candidate[i] - baseline[i];
            sum += diff;
            if (std::fabs(diff) > 1e-6) { d.nDiffer++; }
        }
        d.meanDelta = (float)(sum / d.nSeeds);

        double var = 0;
        for (int i = 0; i < d.nSeeds; i++) {
            const double e = (candidate[i] - baseline[i]) - d.meanDelta;
            var += e * e;
        }
        const float sd = (float)std::sqrt(var / (d.nSeeds - 1));
        d.stderrDelta = sd / std::sqrt((float)d.nSeeds);

        if (d.stderrDelta > 1e-9f) {
            d.t = d.meanDelta / d.stderrDelta;
        }
        else if (std::fabs(d.meanDelta) > 1e-9f) {
            // Zero variance with a nonzero difference: a deterministic profile
            // (noiseAmp and jitterPct both 0) generates the identical signal
            // every seed, so the difference reproduced on all of them. That is
            // maximal certainty, not zero — returning t=0 here silently
            // discarded regressions on every noiseless profile.
            d.t = std::numeric_limits<float>::infinity() * (d.meanDelta > 0 ? 1.0f : -1.0f);
        }
        return d;
    }

    // ── Error-type breakdown ────────────────────────────────────
    //
    // Aggregate CER cannot distinguish a decoder that stays silent from one
    // that emits garbage: a deletion and a substitution cost the same. The
    // silent-character-deletion path in morse_tree.h (unmapped tree nodes emit
    // '\0', which callers drop) is invisible without this split.

    struct ErrorMix {
        int ins = 0;   // decoder emitted a character that isn't in the reference
        int del = 0;   // decoder dropped a reference character
        int sub = 0;   // decoder emitted the wrong character
        int refLen = 0;
    };

    inline ErrorMix alignErrors(const std::string& reference, const std::string& decoded) {
        std::string a = normalize(reference), b = normalize(decoded);
        int m = (int)a.size(), n = (int)b.size();
        std::vector<std::vector<int>> d(m + 1, std::vector<int>(n + 1, 0));
        for (int i = 0; i <= m; i++) { d[i][0] = i; }
        for (int j = 0; j <= n; j++) { d[0][j] = j; }
        for (int i = 1; i <= m; i++) {
            for (int j = 1; j <= n; j++) {
                int cost = (a[i-1] != b[j-1]) ? 1 : 0;
                d[i][j] = std::min({d[i-1][j] + 1, d[i][j-1] + 1, d[i-1][j-1] + cost});
            }
        }
        ErrorMix e;
        e.refLen = m;
        int i = m, j = n;
        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && d[i][j] == d[i-1][j-1] + ((a[i-1] != b[j-1]) ? 1 : 0)) {
                if (a[i-1] != b[j-1]) { e.sub++; }
                i--; j--;
            }
            else if (i > 0 && d[i][j] == d[i-1][j] + 1) { e.del++; i--; }
            else                                        { e.ins++; j--; }
        }
        return e;
    }

    // Run one profile across N independent seeds. The seed is the ONLY thing
    // that varies between runs.
    inline CERStats decodeAndScoreMulti(const std::string& message,
                                        SignalParams params,
                                        int nSeeds = 24,
                                        unsigned seedBase = 1000) {
        std::vector<float> cers(nSeeds), wers(nSeeds);
        parallelFor(nSeeds, [&](int i) {
            SignalParams p = params;                       // per-worker copy
            p.seed = seedBase + (unsigned)i * 7919u;       // spread seeds apart
            auto s = decodeAndScore(message, p);
            cers[i] = s.cer;
            wers[i] = s.wer;
        });

        float werSum = 0;
        for (float w : wers) { werSum += w; }
        return summarize(std::move(cers), werSum);
    }
}