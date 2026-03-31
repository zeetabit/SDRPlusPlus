#pragma once
#include <gui/interfaces/ifft_buffer.h>

class FFTBufferAdapter : public IFFTBuffer {
public:
    float* getFFTBuffer() override;
    void pushFFT() override;
};
