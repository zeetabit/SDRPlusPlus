#include <gui/adapters/fft_buffer_adapter.h>
#include <gui/gui.h>

float* FFTBufferAdapter::getFFTBuffer() { return gui::waterfall.getFFTBuffer(); }
void FFTBufferAdapter::pushFFT() { gui::waterfall.pushFFT(); }
