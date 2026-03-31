#pragma once
#include <string>
#include <map>

namespace ImGui {
    class WaterfallVFO;
}

class IWaterfallState {
public:
    virtual ~IWaterfallState() = default;

    virtual double getBandwidth() = 0;
    virtual double getViewBandwidth() = 0;
    virtual double getViewOffset() = 0;
    virtual double getCenterFrequency() = 0;

    virtual void setViewBandwidth(double bw) = 0;
    virtual void setViewOffset(double offset) = 0;

    virtual const std::string& getSelectedVFO() = 0;
    virtual ImGui::WaterfallVFO* getVFO(const std::string& name) = 0;

    virtual bool isMouseInFFT() = 0;
    virtual bool isMouseInWaterfall() = 0;
    virtual double getSelectedVFOSNR() = 0;

    virtual bool hasSelectedVFOChanged() = 0;
    virtual void clearSelectedVFOChanged() = 0;

    virtual bool hasCenterFreqMoved() = 0;
    virtual void clearCenterFreqMoved() = 0;

    virtual int getFFTHeight() = 0;
};
