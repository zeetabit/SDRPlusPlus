#pragma once
#include <algorithm>
#include <cmath>

namespace gui_math {
    // Maps a zoom slider value (0..1) to a view bandwidth.
    // slider=1.0 → full bandwidth, slider=0.0 → minimum (1000 Hz).
    inline double zoomSliderToBandwidth(float sliderValue, double wholeBandwidth) {
        double factor = (double)sliderValue * (double)sliderValue;
        double delta = wholeBandwidth - 1000.0;
        return std::min<double>(1000.0 + (factor * delta), wholeBandwidth);
    }

    // Inverse: maps a view bandwidth back to a slider value (0..1).
    // Accounts for changed total bandwidth since save.
    inline float bandwidthToZoomSlider(double viewBandwidth, double wholeBandwidth) {
        if (wholeBandwidth <= 1000.0) { return 1.0f; }
        double delta = wholeBandwidth - 1000.0;
        double factor = (viewBandwidth - 1000.0) / delta;
        factor = std::clamp(factor, 0.0, 1.0);
        return (float)sqrt(factor);
    }

    // Clamps a VFO offset to be within the visible view window.
    inline double clampVFOOffset(double offset, double viewOffset, double viewBandwidth) {
        double viewLower = viewOffset - (viewBandwidth / 2.0);
        double viewUpper = viewOffset + (viewBandwidth / 2.0);
        return std::clamp<double>(offset, viewLower, viewUpper);
    }

    // Steps frequency by one snap interval in the given direction, then rounds to the snap grid.
    inline double stepFrequency(double currentFreq, double snapInterval, int direction) {
        double nfreq = currentFreq + (snapInterval * direction);
        return roundl(nfreq / snapInterval) * snapInterval;
    }

    // Constrains fftMax to be at least minGap above fftMin.
    inline float constrainFFTMax(float fftMax, float fftMin, float minGap = 10.0f) {
        return std::max<float>(fftMax, fftMin + minGap);
    }

    // Constrains fftMin to be at least minGap below fftMax.
    inline float constrainFFTMin(float fftMin, float fftMax, float minGap = 10.0f) {
        return std::min<float>(fftMax - minGap, fftMin);
    }

    // Calculates scroll-based frequency change (no VFO, pan mode).
    inline double scrollPanFrequency(double centerFreq, double viewBandwidth, int wheelDelta) {
        return centerFreq - (viewBandwidth * wheelDelta / 20.0);
    }
}
