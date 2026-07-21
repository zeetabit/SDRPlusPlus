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
    printf("\n=== calibrated INPUT SNR vs raw noiseAmp (and detector getSNR) ===\n");
    printf("%-10s %11s %11s %12s %s\n",
           "noiseAmp", "SNR/2500Hz", "SNR/500Hz", "getSNR(pB)", "human-copy?");
    for (float na : {0.3f, 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
        // Detector getSNR, mean over seeds at 15 WPM through the wide/legacy filter.
        double snrSum = 0; int snrN = 0;
        for (unsigned s = 0; s < 12; s++) {
            SignalParams p = profileClean(80.0f);
            p.noiseAmp = na; p.seed = 1000 + s * 7919u;
            auto sig = generateMessage(MSG_FULL(), p);
            cw::Channel ch; ch.init(0, p.toneFreq);
            double acc = 0; int accN = 0;
            for (int off = 0; off < (int)sig.samples.size(); off += 512) {
                int c = std::min(512, (int)sig.samples.size() - off);
                ch.process(c, &sig.samples[off]);
                if (ch.snr > 0) { acc += ch.snr; accN++; }
            }
            if (accN > 0) { snrSum += acc / accN; snrN++; }
        }
        const float db2500 = noiseAmpToSnrDb(na, REF_BW_SSB);
        // PA3FWM: CW by ear copies to about -18 dB in 2500 Hz.
        printf("%-10.1f %+10.1f %+10.1f %+11.1f   %s\n",
               na, db2500, noiseAmpToSnrDb(na, REF_BW_CW),
               snrN > 0 ? snrSum / snrN : 0.0,
               db2500 > -18.0f ? "yes (above -18 dB)" : "no");
    }
    CHECK(true);   // reporting only; correctness gated in the always-on case above
}
