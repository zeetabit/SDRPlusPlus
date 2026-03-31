#pragma once
#include <gui/interfaces/ifrequency_control.h>

class FrequencyControlAdapter : public IFrequencyControl {
public:
    void tune(int mode, const std::string& vfoName, double freq) override;
    void tuneSource(double freq) override;
    void setDisplayFrequency(double freq) override;
    double getDisplayFrequency() override;
    bool isDigitHovered() override;
    bool hasFrequencyChanged() override;
    void clearFrequencyChanged() override;
};
