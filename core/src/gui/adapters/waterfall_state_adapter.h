#pragma once
#include <gui/interfaces/iwaterfall_state.h>

class WaterfallStateAdapter : public IWaterfallState {
public:
    double getBandwidth() override;
    double getViewBandwidth() override;
    double getViewOffset() override;
    double getCenterFrequency() override;

    void setViewBandwidth(double bw) override;
    void setViewOffset(double offset) override;

    const std::string& getSelectedVFO() override;
    ImGui::WaterfallVFO* getVFO(const std::string& name) override;

    bool isMouseInFFT() override;
    bool isMouseInWaterfall() override;
    double getSelectedVFOSNR() override;

    bool hasSelectedVFOChanged() override;
    void clearSelectedVFOChanged() override;

    bool hasCenterFreqMoved() override;
    void clearCenterFreqMoved() override;

    int getFFTHeight() override;
};
