#pragma once
#include <cmath>
#include "cw_test_signals.h"

// Calibrated SNR for the benchmark's AWGN model (docs §31).
//
// The suite parametrizes noise by `noiseAmp` — a raw amplitude — which is not a
// physical, reproducible quantity: it depends on signal amplitude, sample rate,
// and the measurement bandwidth, so "noise3.0" cannot be placed on any external
// CER-vs-SNR curve (AG1LE, PA3FWM) and cannot be reproduced by another lab. This
// header converts noiseAmp to a calibrated SNR in dB within a stated reference
// bandwidth, and back, so profiles can be specified and reported in dB.
//
// Model (from CWSignalGenerator::generate):
//   signal  x = env·A·e^{jφ}, so key-down power S = A²  (env → 1).
//   noise   n added to re and im independently, each ~ noiseAmp·N(0,1), so the
//           complex noise power is E[nr²+ni²] = 2·noiseAmp², white across the
//           full complex bandwidth = sampleRate.
//   ⇒ N0 = 2·noiseAmp² / sampleRate  (power spectral density, per Hz)
//   ⇒ SNR(B) = S / (N0·B) = A²·sampleRate / (2·noiseAmp²·B)
//
// This is the INPUT (pre-detection) SNR — a property of the signal, independent
// of the decoder's filters. It is NOT the detector's getSNR(), which is a
// post-BPF peak/percentile ratio used as a runtime heuristic; the two must not
// be conflated (getSNR at noiseAmp 2.0 reads ~4.5 dB post-BPF, while the input
// SNR in 2500 Hz is −4.0 dB — different quantities, different purposes).

namespace cw_test {

    // Reference bandwidths. 2500 Hz is the SSB/voice channel PA3FWM and most
    // amateur SNR figures use; 500 Hz is the common CW crystal-filter width.
    constexpr float REF_BW_SSB = 2500.0f;
    constexpr float REF_BW_CW  = 500.0f;

    inline float noiseAmpToSnrDb(float noiseAmp, float refBwHz = REF_BW_SSB,
                                 float amplitude = 1.0f, float sampleRate = 8000.0f) {
        if (noiseAmp <= 0.0f) { return INFINITY; }
        const float N0 = 2.0f * noiseAmp * noiseAmp / sampleRate;
        const float snr = (amplitude * amplitude) / (N0 * refBwHz);
        return 10.0f * log10f(snr);
    }

    inline float snrDbToNoiseAmp(float snrDb, float refBwHz = REF_BW_SSB,
                                 float amplitude = 1.0f, float sampleRate = 8000.0f) {
        const float snr = powf(10.0f, snrDb / 10.0f);
        const float N0 = (amplitude * amplitude) / (snr * refBwHz);
        return sqrtf(N0 * sampleRate / 2.0f);
    }

    // A clean AWGN profile specified by calibrated SNR (dB in refBwHz) rather
    // than by raw noiseAmp. Everything else at defaults, so noise is the only
    // impairment and the dB label is exact.
    inline SignalParams profileAtSnr(float ditMs, float snrDb,
                                     float refBwHz = REF_BW_SSB) {
        SignalParams p = profileClean(ditMs);
        p.noiseAmp = snrDbToNoiseAmp(snrDb, refBwHz, p.amplitude, p.sampleRate);
        return p;
    }
}
