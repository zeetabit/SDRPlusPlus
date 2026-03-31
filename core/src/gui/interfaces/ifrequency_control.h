#pragma once
#include <string>

class IFrequencyControl {
public:
    virtual ~IFrequencyControl() = default;

    virtual void tune(int mode, const std::string& vfoName, double freq) = 0;
    virtual void tuneSource(double freq) = 0;
    virtual void setDisplayFrequency(double freq) = 0;
    virtual double getDisplayFrequency() = 0;
    virtual bool isDigitHovered() = 0;
    virtual bool hasFrequencyChanged() = 0;
    virtual void clearFrequencyChanged() = 0;
};
