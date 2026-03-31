#pragma once
#include <fftw3.h>
#include <mutex>
#include <gui/interfaces/ifft_buffer.h>

class FFTManager {
public:
    FFTManager() = default;
    FFTManager(IFFTBuffer* buffer) : buffer(buffer) {}

    void inject(IFFTBuffer* buffer) { this->buffer = buffer; }
    void init(int size);

    static float* acquireFFTBuffer(void* ctx);
    static void releaseFFTBuffer(void* ctx);

private:
    IFFTBuffer* buffer = nullptr;
    int fftSize = 8192 * 8;
    std::mutex fft_mtx;
    fftwf_complex *fft_in = nullptr, *fft_out = nullptr;
    fftwf_plan fftwPlan = nullptr;
};
