#pragma once

class IFFTBuffer {
public:
    virtual ~IFFTBuffer() = default;

    virtual float* getFFTBuffer() = 0;
    virtual void pushFFT() = 0;
};
