#pragma once
#include <dsp/types.h>
#include <dsp/buffer/buffer.h>
#include <dsp/taps/tap.h>
#include <dsp/taps/low_pass.h>
#include <volk/volk.h>
#include <cmath>
#include <cstring>

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
            if (bpfTaps.taps) { dsp::taps::free(bpfTaps); }
            if (bpfBuffer) { dsp::buffer::free(bpfBuffer); }
            bpfTaps = dsp::taps::lowPass(bpfCutoff, bpfTrans, _internalRate);
            bpfBufSize = bpfTaps.size - 1;
            bpfBuffer = dsp::buffer::alloc<dsp::complex_t>(bpfBufSize + 65536);
            dsp::buffer::clear(bpfBuffer, bpfBufSize);
            bpfBufStart = &bpfBuffer[bpfBufSize];
        }

        float getToneFreq() const { return _toneFreq; }

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
    };

}
