#include <gui/fft_manager.h>
#include <string.h>

void FFTManager::init(int size) {
    fftSize = size;
    fft_in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fft_out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * fftSize);
    fftwPlan = fftwf_plan_dft_1d(fftSize, fft_in, fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
}

float* FFTManager::acquireFFTBuffer(void* ctx) {
    FFTManager* _this = (FFTManager*)ctx;
    return _this->buffer->getFFTBuffer();
}

void FFTManager::releaseFFTBuffer(void* ctx) {
    FFTManager* _this = (FFTManager*)ctx;
    _this->buffer->pushFFT();
}
