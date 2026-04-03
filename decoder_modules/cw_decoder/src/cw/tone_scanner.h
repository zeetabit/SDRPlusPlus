#pragma once
#include <dsp/types.h>
#include <dsp/buffer/buffer.h>
#include <fftw3.h>
#include <volk/volk.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace cw {

    struct DetectedTone {
        float frequency;   // Hz, offset from VFO center
        float power;       // dB above noise floor
        float bandwidth;   // estimated occupied BW in Hz
        float modIndex;    // power variance / mean — high for keyed CW, low for steady carrier
    };

    // Periodically analyzes the VFO IQ spectrum to find active CW tones.
    // Uses FFT + CFAR-style peak detection with spectral noise floor estimation.
    //
    // Call feed() with IQ samples. When enough samples accumulate, the FFT runs
    // automatically. Call getDetectedTones() to retrieve results.
    class ToneScanner {
    public:
        void init(float sampleRate, int fftSize = 1024) {
            _sampleRate = sampleRate;
            _fftSize = fftSize;
            _binWidth = sampleRate / fftSize;

            fftIn = (dsp::complex_t*)fftwf_alloc_complex(fftSize);
            fftOut = (dsp::complex_t*)fftwf_alloc_complex(fftSize);
            plan = fftwf_plan_dft_1d(fftSize, (fftwf_complex*)fftIn, (fftwf_complex*)fftOut,
                                     FFTW_FORWARD, FFTW_ESTIMATE);

            powerSpectrum.resize(fftSize, 0.0f);
            avgSpectrum.resize(fftSize, 0.0f);
            varSpectrum.resize(fftSize, 0.0f);
            accumBuf = dsp::buffer::alloc<dsp::complex_t>(fftSize);
            accumPos = 0;
            frameCount = 0;

            // Hann window
            window.resize(fftSize);
            for (int i = 0; i < fftSize; i++) {
                window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (fftSize - 1)));
            }
        }

        ~ToneScanner() {
            if (plan) { fftwf_destroy_plan(plan); plan = nullptr; }
            if (fftIn) { fftwf_free(fftIn); fftIn = nullptr; }
            if (fftOut) { fftwf_free(fftOut); fftOut = nullptr; }
            if (accumBuf) { dsp::buffer::free(accumBuf); accumBuf = nullptr; }
        }

        // Feed IQ samples. Returns true when a new scan completed.
        bool feed(const dsp::complex_t* data, int count) {
            bool scanned = false;
            int pos = 0;
            while (pos < count) {
                int remaining = _fftSize - accumPos;
                int toCopy = std::min(remaining, count - pos);
                memcpy(&accumBuf[accumPos], &data[pos], toCopy * sizeof(dsp::complex_t));
                accumPos += toCopy;
                pos += toCopy;

                if (accumPos >= _fftSize) {
                    runFFT();
                    accumPos = 0;
                    scanned = true;
                }
            }
            return scanned;
        }

        // Get tones detected in the last scan. Sorted by power (strongest first).
        std::vector<DetectedTone> getDetectedTones(float thresholdDb = 10.0f, int maxTones = 10) {
            std::vector<DetectedTone> tones;

            // Estimate noise floor: median of power spectrum
            std::vector<float> sorted = avgSpectrum;
            std::nth_element(sorted.begin(), sorted.begin() + _fftSize / 2, sorted.end());
            float noiseFloor = sorted[_fftSize / 2];
            if (noiseFloor < 1e-12f) { noiseFloor = 1e-12f; }
            float noiseFloorDb = 10.0f * log10f(noiseFloor);

            // Find peaks above threshold
            for (int i = 2; i < _fftSize - 2; i++) {
                float p = avgSpectrum[i];
                if (p < 1e-12f) { continue; }
                float pDb = 10.0f * log10f(p);
                float snr = pDb - noiseFloorDb;

                // Must be above threshold and a local maximum
                if (snr < thresholdDb) { continue; }
                if (p <= avgSpectrum[i - 1] || p <= avgSpectrum[i + 1]) { continue; }
                if (p <= avgSpectrum[i - 2] || p <= avgSpectrum[i + 2]) { continue; }

                // Convert bin to frequency (centered: bin 0 = -sampleRate/2)
                float freq = binToFreq(i);

                // Estimate bandwidth: count bins within 6 dB of peak (peak-relative,
                // not noise-relative, so it works even when noise floor is very low).
                float bwThresh = p * 0.25f;  // -6 dB from peak
                int lo = i, hi = i;
                while (lo > 0 && avgSpectrum[lo - 1] > bwThresh) { lo--; }
                while (hi < _fftSize - 1 && avgSpectrum[hi + 1] > bwThresh) { hi++; }

                float bw = (float)(hi - lo + 1) * _binWidth;

                // Reject tones wider than 100 Hz — real CW is narrowband.
                if (bw > 100.0f) { continue; }

                // Modulation index: sqrt(variance) / mean at peak bin.
                // Keyed CW: power swings between high and ~0 → high modIndex (~0.5-1.0)
                // Steady carrier: constant power → low modIndex (~0.0-0.1)
                float modIndex = (p > 1e-12f) ? sqrtf(varSpectrum[i]) / p : 0.0f;

                tones.push_back({freq, snr, bw, modIndex});
            }

            // Sort by CW score: power weighted by modulation index.
            // Keyed CW signals (high modIndex) rank above steady carriers (low modIndex).
            std::sort(tones.begin(), tones.end(),
                [](const DetectedTone& a, const DetectedTone& b) {
                    float scoreA = a.power * (1.0f + a.modIndex * 2.0f);
                    float scoreB = b.power * (1.0f + b.modIndex * 2.0f);
                    return scoreA > scoreB;
                });

            // Merge nearby peaks: keying sidebands from the same CW signal
            // create multiple FFT peaks within ~100 Hz. Keep only the strongest
            // per group — the true carrier.
            const float mergeRadius = 120.0f; // Hz — must match BPF effective BW to prevent two channels on one signal
            std::vector<DetectedTone> merged;
            for (auto& t : tones) {
                bool tooClose = false;
                for (auto& m : merged) {
                    if (fabsf(t.frequency - m.frequency) < mergeRadius) {
                        tooClose = true;
                        break;
                    }
                }
                if (!tooClose) { merged.push_back(t); }
            }

            if ((int)merged.size() > maxTones) { merged.resize(maxTones); }
            return merged;
        }

        float getBinWidth() const { return _binWidth; }
        int getFFTSize() const { return _fftSize; }
        const std::vector<float>& getPowerSpectrum() const { return avgSpectrum; }

    private:
        void runFFT() {
            // Apply window
            volk_32fc_32f_multiply_32fc((lv_32fc_t*)fftIn, (lv_32fc_t*)accumBuf,
                                         window.data(), _fftSize);

            fftwf_execute(plan);

            // Compute power spectrum (magnitude squared)
            volk_32fc_magnitude_squared_32f(powerSpectrum.data(), (lv_32fc_t*)fftOut, _fftSize);

            // FFT shift: swap halves so DC is in the center
            int half = _fftSize / 2;
            for (int i = 0; i < half; i++) {
                std::swap(powerSpectrum[i], powerSpectrum[i + half]);
            }

            // Exponential averaging + variance tracking
            frameCount++;
            float alpha = (frameCount < 5) ? 1.0f / frameCount : 0.2f;
            for (int i = 0; i < _fftSize; i++) {
                float diff = powerSpectrum[i] - avgSpectrum[i];
                avgSpectrum[i] += alpha * diff;
                // EMA of squared deviation — measures power fluctuation per bin
                varSpectrum[i] += alpha * (diff * diff - varSpectrum[i]);
            }
        }

        float binToFreq(int bin) const {
            // After FFT shift: bin 0 = -sampleRate/2, bin N/2 = 0 (DC), bin N-1 = +sampleRate/2
            return (bin - _fftSize / 2) * _binWidth;
        }

        float _sampleRate = 8000;
        int _fftSize = 1024;
        float _binWidth = 8000.0f / 1024;

        fftwf_plan plan = nullptr;
        dsp::complex_t* fftIn = nullptr;
        dsp::complex_t* fftOut = nullptr;
        dsp::complex_t* accumBuf = nullptr;
        int accumPos = 0;
        int frameCount = 0;

        std::vector<float> window;
        std::vector<float> powerSpectrum;
        std::vector<float> avgSpectrum;
        std::vector<float> varSpectrum;  // per-bin power variance (high = keyed, low = carrier)
    };

}
