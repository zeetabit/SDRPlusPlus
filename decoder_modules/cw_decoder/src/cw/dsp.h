#pragma once
#include <dsp/types.h>
#include <dsp/buffer/buffer.h>
#include <dsp/taps/tap.h>
#include <dsp/taps/low_pass.h>
#include <volk/volk.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

namespace cw {

    // Inline DSP processor: extracts CW tone envelope from IQ samples.
    // No streams or threading — called directly from a handler callback.
    //
    // Chain: FreqXlator → Decimate 8:1 → Narrow LPF → Magnitude → Smoothing LPF
    //
    // Input:  complex_t at SAMPLERATE (8000 Hz)
    // Output: float envelope at INTERNAL_RATE (1000 Hz)
    class EnvelopeDSP {
    public:
        // Filter geometry is parameterized because pre-detection bandwidth is a
        // decoding design choice, not a fixed constant — the benchmark matrix
        // varies it. Defaults reproduce the historical behaviour exactly.
        void init(float toneFreq, float sampleRate, float internalRate,
                  float bpfCutoff = 100.0, float bpfTrans = 100.0,
                  float smoothCutoff = 80.0, float smoothTrans = 100.0) {
            _sampleRate = sampleRate;
            _internalRate = internalRate;
            _decimRatio = (int)(sampleRate / internalRate);

            // Xlator state
            setToneFreq(toneFreq);

            // Narrow BPF (complex lowpass at internal rate)
            // 100 Hz cutoff, 100 Hz transition → ~200 Hz effective BW, ~20 taps.
            // Must be wide enough for CW keying bandwidth (50-80 Hz at typical WPM)
            // while rejecting adjacent signals 200+ Hz away.
            bpfTaps = dsp::taps::lowPass(bpfCutoff, bpfTrans, _internalRate);
            bpfBufSize = bpfTaps.size - 1;
            bpfBuffer = dsp::buffer::alloc<dsp::complex_t>(bpfBufSize + 65536);
            dsp::buffer::clear(bpfBuffer, bpfBufSize);
            bpfBufStart = &bpfBuffer[bpfBufSize];

            // Smoothing LPF (float lowpass at internal rate)
            // 80 Hz cutoff, 100 Hz transition → ~20 taps.
            // Wider bandwidth preserves edge timing for jittery/QSB signals.
            // The matched filter in channel.h provides additional narrowing.
            smoothTaps = dsp::taps::lowPass(smoothCutoff, smoothTrans, _internalRate);
            smoothBufSize = smoothTaps.size - 1;
            smoothBuffer = dsp::buffer::alloc<float>(smoothBufSize + 65536);
            dsp::buffer::clear(smoothBuffer, smoothBufSize);
            smoothBufStart = &smoothBuffer[smoothBufSize];

            // Intermediate buffers
            xlatedBuf = dsp::buffer::alloc<dsp::complex_t>(65536);
            decimBuf = dsp::buffer::alloc<dsp::complex_t>(65536);
            filteredBuf = dsp::buffer::alloc<dsp::complex_t>(65536);
            magBuf = dsp::buffer::alloc<float>(65536);

            // Wide-band (pre-BPF) SNR estimator (docs §32/§33). Measures noise on
            // the DECIMATED signal before the narrow BPF, so it reflects input
            // conditions rather than the post-filter residual the detector's
            // getSNR sees. 25th-percentile noise floor over ~2 s, peak with a
            // 0.5 s decay — the same estimators the detector uses, one stage
            // earlier where the full-band noise is still present.
            wideDecay = 1.0f - expf(-1.0f / (0.5f * _internalRate));
            wideNoiseSub = std::max(1, (int)(_internalRate / 125.0f));
            wideNoiseSize = 250;
            wideNoiseWin.assign(wideNoiseSize, 0.0f);
            wideNoiseSorted.resize(wideNoiseSize);
            wideNoisePos = 0;
            wideNoiseCount = 0;
            wideSubCtr = 0;
            wideNoiseFloor = 1e-6f;
            wideSignalPeak = 1e-6f;
        }

        ~EnvelopeDSP() {
            if (bpfTaps.taps) { dsp::taps::free(bpfTaps); }
            if (smoothTaps.taps) { dsp::taps::free(smoothTaps); }
            if (bpfBuffer) { dsp::buffer::free(bpfBuffer); }
            if (smoothBuffer) { dsp::buffer::free(smoothBuffer); }
            if (xlatedBuf) { dsp::buffer::free(xlatedBuf); }
            if (decimBuf) { dsp::buffer::free(decimBuf); }
            if (filteredBuf) { dsp::buffer::free(filteredBuf); }
            if (magBuf) { dsp::buffer::free(magBuf); }
        }

        // Process IQ samples, output envelope. Returns output sample count.
        // Input count must not exceed 65536 samples.
        int process(int count, const dsp::complex_t* in, float* out) {
            if (count > 65536) { count = 65536; }
            // 1. Frequency shift tone to DC
#if VOLK_VERSION_MAJOR > 3 || (VOLK_VERSION_MAJOR == 3 && VOLK_VERSION_MINOR >= 1)
            volk_32fc_s32fc_x2_rotator2_32fc((lv_32fc_t*)xlatedBuf, (lv_32fc_t*)in, &xlPhaseDelta, &xlPhase, count);
#else
            volk_32fc_s32fc_x2_rotator_32fc((lv_32fc_t*)xlatedBuf, (lv_32fc_t*)in, xlPhaseDelta, &xlPhase, count);
#endif

            // 2. Decimate (simple take-every-Nth after the narrow filter will alias,
            //    but we're about to narrow-filter at the decimated rate anyway.
            //    For proper anti-alias, decimate with averaging.)
            int decimCount = count / _decimRatio;
            for (int i = 0; i < decimCount; i++) {
                // Average _decimRatio samples for anti-alias
                dsp::complex_t sum = {0, 0};
                int base = i * _decimRatio;
                for (int j = 0; j < _decimRatio; j++) {
                    sum.re += xlatedBuf[base + j].re;
                    sum.im += xlatedBuf[base + j].im;
                }
                float scale = 1.0f / _decimRatio;
                decimBuf[i].re = sum.re * scale;
                decimBuf[i].im = sum.im * scale;
            }

            // 2b. Wide-band (pre-BPF) SNR estimate for the adaptive-filter
            //     trigger (§33). Runs on the decimated signal before the narrow
            //     BPF, so noiseFloor tracks the FULL decimated-band noise — the
            //     input-referred estimate the post-BPF getSNR is not (§32).
            for (int i = 0; i < decimCount; i++) {
                const float m = sqrtf(decimBuf[i].re * decimBuf[i].re + decimBuf[i].im * decimBuf[i].im);
                if (m > wideSignalPeak) { wideSignalPeak = m; }
                else { wideSignalPeak += (m - wideSignalPeak) * wideDecay; }
                if (++wideSubCtr >= wideNoiseSub) {
                    wideSubCtr = 0;
                    wideNoiseWin[wideNoisePos] = m;
                    wideNoisePos = (wideNoisePos + 1) % wideNoiseSize;
                    if (wideNoiseCount < wideNoiseSize) { wideNoiseCount++; }
                    if (wideNoiseCount >= 10) {
                        const int n = wideNoiseCount;
                        wideNoiseSorted.resize(n);
                        memcpy(wideNoiseSorted.data(), wideNoiseWin.data(), n * sizeof(float));
                        const int idx = n / 4;
                        std::nth_element(wideNoiseSorted.begin(), wideNoiseSorted.begin() + idx,
                                         wideNoiseSorted.begin() + n);
                        wideNoiseFloor = std::max(wideNoiseSorted[idx], 1e-9f);
                    }
                }
            }

            // 3. Narrow bandpass FIR (complex)
            memcpy(bpfBufStart, decimBuf, decimCount * sizeof(dsp::complex_t));
            for (int i = 0; i < decimCount; i++) {
                volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&filteredBuf[i],
                    (lv_32fc_t*)&bpfBuffer[i], bpfTaps.taps, bpfTaps.size);
            }
            memmove(bpfBuffer, &bpfBuffer[decimCount], bpfBufSize * sizeof(dsp::complex_t));

            // 4. Magnitude (envelope)
            volk_32fc_magnitude_32f(magBuf, (lv_32fc_t*)filteredBuf, decimCount);

            // 5. Smoothing LPF (float)
            memcpy(smoothBufStart, magBuf, decimCount * sizeof(float));
            for (int i = 0; i < decimCount; i++) {
                volk_32f_x2_dot_prod_32f(&out[i], &smoothBuffer[i], smoothTaps.taps, smoothTaps.size);
            }
            memmove(smoothBuffer, &smoothBuffer[decimCount], smoothBufSize * sizeof(float));

            return decimCount;
        }

        void setToneFreq(float freq) {
            _toneFreq = freq;
            float omega = -2.0f * M_PI * _toneFreq / _sampleRate;
            xlPhaseDelta = {cosf(omega), sinf(omega)};
            xlPhase = {1.0f, 0.0f};
        }

        // Rebuild the pre-detection BPF at runtime (docs §30, WPM-locked
        // bandwidth). Mirrors the BPF half of init(): new taps, new history
        // buffer sized to the new tap count. The buffer is cleared, so this
        // injects one filter-length transient — acceptable because the WPM lock
        // that triggers it happens once, seconds into the stream, after the
        // pre-lock events are already captured. Only the BPF changes; the xlator,
        // decimator and smoothing LPF keep their state.
        void setBandwidth(float bpfCutoff, float bpfTrans) {
            // Preserve the recent input history across the rebuild instead of
            // zeroing it (docs §33). A hard clear injects a filter-length dropout;
            // on a near-clean profile that still triggers narrowing (e.g. QRM,
            // where a narrowband interferer reads as noise) that dropout is a
            // spurious character. The delay line holds recent decimated input
            // samples, which stay valid across a tap change — carry the newest
            // min(old, new) of them into the tail of the new history.
            std::vector<dsp::complex_t> hist;
            if (bpfBuffer && bpfBufSize > 0) {
                hist.assign(bpfBuffer, bpfBuffer + bpfBufSize);   // oldest → newest
            }
            if (bpfTaps.taps) { dsp::taps::free(bpfTaps); }
            if (bpfBuffer) { dsp::buffer::free(bpfBuffer); }
            bpfTaps = dsp::taps::lowPass(bpfCutoff, bpfTrans, _internalRate);
            bpfBufSize = bpfTaps.size - 1;
            bpfBuffer = dsp::buffer::alloc<dsp::complex_t>(bpfBufSize + 65536);
            dsp::buffer::clear(bpfBuffer, bpfBufSize);
            const int keep = std::min((int)hist.size(), bpfBufSize);
            if (keep > 0) {
                memcpy(&bpfBuffer[bpfBufSize - keep], &hist[hist.size() - keep],
                       keep * sizeof(dsp::complex_t));
            }
            bpfBufStart = &bpfBuffer[bpfBufSize];
        }

        // Rebuild the post-detection envelope-smoothing LPF at runtime (docs §52
        // step 2). The fb detector's optimal geometry narrows BOTH the pre-detection
        // BPF and this smoothing filter together under noise; setBandwidth moves only
        // the former, so this is its post-detection twin. Preserves the smoothing
        // history (real magnitude samples) across the tap change to avoid a dropout.
        void setSmoothing(float smoothCutoff, float smoothTrans) {
            std::vector<float> hist;
            if (smoothBuffer && smoothBufSize > 0) {
                hist.assign(smoothBuffer, smoothBuffer + smoothBufSize);
            }
            if (smoothTaps.taps) { dsp::taps::free(smoothTaps); }
            if (smoothBuffer) { dsp::buffer::free(smoothBuffer); }
            smoothTaps = dsp::taps::lowPass(smoothCutoff, smoothTrans, _internalRate);
            smoothBufSize = smoothTaps.size - 1;
            smoothBuffer = dsp::buffer::alloc<float>(smoothBufSize + 65536);
            dsp::buffer::clear(smoothBuffer, smoothBufSize);
            const int keep = std::min((int)hist.size(), smoothBufSize);
            if (keep > 0) {
                memcpy(&smoothBuffer[smoothBufSize - keep], &hist[hist.size() - keep],
                       keep * sizeof(float));
            }
            smoothBufStart = &smoothBuffer[smoothBufSize];
        }

        float getToneFreq() const { return _toneFreq; }

        // Input-referred SNR (dB), measured before the narrow BPF (docs §32/§33).
        // Unlike the detector's getSNR (post-BPF, reads high on real audio because
        // the filter already removed the noise), this tracks the full decimated-
        // band noise, so it transfers across signal type and is the correct
        // trigger for the adaptive filter.
        float getInputSnrDb() const {
            if (wideNoiseFloor < 1e-9f) { return 99.0f; }
            return 10.0f * log10f(wideSignalPeak / wideNoiseFloor);
        }

        // The wide noise window (~2 s) must fill before getInputSnrDb is
        // trustworthy: read at timing lock (~1.7 s) it is a huge transient
        // (floor still near its initial value), which is why the retune must wait
        // for this, not just for lock (docs §33).
        bool inputSnrReady() const { return wideNoiseCount >= wideNoiseSize; }

        // Provisional readiness (~0.5 s) for the fb adaptive switch (docs §52 step 2).
        // The full 2 s gate is for STABILITY (avoid false narrowing on a transient);
        // the initial clean-vs-noise call is bimodal (clean ~60 dB vs noise ~6 dB) and
        // safe to make on a coarser floor estimate long before the window fills.
        bool inputSnrReadyFast() const { return wideNoiseCount >= 60; }

    private:
        float _sampleRate = 8000;
        float _internalRate = 1000;
        float _toneFreq = 700;
        int _decimRatio = 8;

        // Frequency xlator state
        lv_32fc_t xlPhase = {1.0f, 0.0f};
        lv_32fc_t xlPhaseDelta = {1.0f, 0.0f};

        // Narrow BPF state
        dsp::tap<float> bpfTaps = {};
        dsp::complex_t* bpfBuffer = nullptr;
        dsp::complex_t* bpfBufStart = nullptr;
        int bpfBufSize = 0;

        // Smoothing LPF state
        dsp::tap<float> smoothTaps = {};
        float* smoothBuffer = nullptr;
        float* smoothBufStart = nullptr;
        int smoothBufSize = 0;

        // Intermediate buffers
        dsp::complex_t* xlatedBuf = nullptr;
        dsp::complex_t* decimBuf = nullptr;
        dsp::complex_t* filteredBuf = nullptr;
        float* magBuf = nullptr;

        // Wide-band (pre-BPF) SNR estimator (§32/§33)
        float wideDecay = 0.002f;
        float wideNoiseFloor = 1e-6f, wideSignalPeak = 1e-6f;
        std::vector<float> wideNoiseWin, wideNoiseSorted;
        int wideNoiseSub = 8, wideNoiseSize = 250;
        int wideNoisePos = 0, wideNoiseCount = 0, wideSubCtr = 0;
    };

}
