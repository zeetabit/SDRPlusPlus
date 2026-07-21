#include <catch.hpp>
#include "cw_test_signals.h"
#include "cw_snr.h"
#include <cw/channel.h>
#include <cstdio>

using namespace cw_test;

// Calibrated SNR (docs §31). Turns the benchmark's raw `noiseAmp` into a
// physical, reproducible SNR in dB within a stated bandwidth, so results are
// comparable to the external CER-vs-SNR literature. Always-on: the conversion
// underlies the calibrated gates, so its correctness is gated here.

TEST_CASE("Calibrated SNR: round-trip and monotonicity", "[cw][snr]") {
    // noiseAmp -> dB -> noiseAmp is the identity (the calibration is invertible).
    for (float na : {0.1f, 0.3f, 0.5f, 1.0f, 2.0f, 3.0f, 5.0f}) {
        float db = noiseAmpToSnrDb(na);
        float back = snrDbToNoiseAmp(db);
        INFO("noiseAmp " << na << " -> " << db << " dB -> " << back);
        CHECK(back == Approx(na).epsilon(1e-4));
    }
    // More noise is lower SNR.
    CHECK(noiseAmpToSnrDb(3.0f) < noiseAmpToSnrDb(1.0f));
    // A 2x noiseAmp is 4x noise power = -6.02 dB, in any reference bandwidth.
    CHECK(noiseAmpToSnrDb(1.0f) - noiseAmpToSnrDb(2.0f) == Approx(6.0206f).epsilon(1e-3));
    // Narrower reference bandwidth reports a higher SNR (same noise, less of it):
    // 2500 -> 500 Hz is a 5x ratio = +6.99 dB.
    CHECK(noiseAmpToSnrDb(2.0f, REF_BW_CW) - noiseAmpToSnrDb(2.0f, REF_BW_SSB)
          == Approx(6.9897f).epsilon(1e-3));
    // profileAtSnr hits its target SNR exactly.
    CHECK(noiseAmpToSnrDb(profileAtSnr(80.0f, -6.0f).noiseAmp) == Approx(-6.0f).epsilon(1e-4));
}

// On-demand: the calibration table for the standard profiles, plus the detector's
// post-BPF getSNR for contrast. The two columns differ because getSNR is a
// post-filter peak/percentile heuristic, not the input SNR -- which is exactly
// why the benchmark must be labelled in the input dB, not in noiseAmp or getSNR.
TEST_CASE("Calibrated SNR: standard profile table", "[cw][.][snr-table]") {
    printf("\n=== INPUT SNR: calibrated vs measured getSNR (post-BPF) vs inputSnr (pre-BPF) ===\n");
    printf("%-10s %11s %12s %12s\n",
           "noiseAmp", "calib/2500", "getSNR(pB)", "inputSnr(preB)");
    for (float na : {0.3f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
        double snrSum = 0, inSum = 0; int accTot = 0;
        for (unsigned s = 0; s < 12; s++) {
            SignalParams p = profileClean(80.0f);
            p.noiseAmp = na; p.seed = 1000 + s * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.init(0, p.toneFreq);
            double acc = 0, accIn = 0; int accN = 0;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int c = std::min(512, (int)sig.samples.size() - off);
                ch.process(c, &sig.samples[off]);
                if (ch.snr > 0) { acc += ch.snr; accIn += ch.inputSnr; accN++; }
            }
            if (accN > 0) { snrSum += acc / accN; inSum += accIn / accN; accTot++; }
        }
        printf("%-10.1f %+10.1f %+11.1f %+13.1f\n",
               na, noiseAmpToSnrDb(na, REF_BW_SSB),
               accTot > 0 ? snrSum / accTot : 0.0,
               accTot > 0 ? inSum / accTot : 0.0);
    }
    // Named profiles: does a narrowband interferer (qrm) fool inputSnr into
    // reading "noisy" and mis-triggering the narrowing? (§33)
    printf("\n%-14s %12s %14s\n", "profile", "getSNR(pB)", "inputSnr(preB)");
    struct P { const char* n; SignalParams p; };
    std::vector<P> named = {
        {"clean-15", profileClean(80.0f)}, {"handkeyed-15", profileHandKeyed(80.0f)},
        {"qsb", profileQSB(80.0f)}, {"qrm", profileQRM(80.0f)}, {"qrn", profileQRN(80.0f)},
        {"noise2.0", [] { auto q = profileClean(80.0f); q.noiseAmp = 2.0f; return q; }()},
    };
    for (auto& np : named) {
        double acc = 0, accIn = 0; int accN = 0;
        for (unsigned s = 0; s < 12; s++) {
            SignalParams p = np.p; p.seed = 1000 + s * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.init(0, p.toneFreq);
            double a = 0, ai = 0; int an = 0;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int c = std::min(512, (int)sig.samples.size() - off);
                ch.process(c, &sig.samples[off]);
                if (ch.snr > 0) { a += ch.snr; ai += ch.inputSnr; an++; }
            }
            if (an > 0) { acc += a / an; accIn += ai / an; accN++; }
        }
        printf("%-14s %+11.1f %+13.1f\n", np.n,
               accN > 0 ? acc / accN : 0.0, accN > 0 ? accIn / accN : 0.0);
    }
    CHECK(true);   // reporting only; correctness gated in the always-on case above
}
