#pragma once
#include <gui/interfaces/iwaterfall_state.h>
#include <gui/interfaces/iconfig_store.h>
#include <string>

class ViewStateCoordinator {
public:
    ViewStateCoordinator() = default;
    ViewStateCoordinator(IWaterfallState* wf, IConfigStore* config) : wf(wf), config(config) {}

    void inject(IWaterfallState* wf, IConfigStore* config) { this->wf = wf; this->config = config; }

    void loadFromConfig(float& sliderBw);

    void onZoomChange(float sliderValue);
    void onZoomChanged(float sliderValue, double viewBandwidth, bool save = true);

    void persistViewOffsetIfChanged();
    void persistFrequency();
    void persistFrequencyAndVFO(const std::string& vfoName, double vfoOffset);
    void persistVFOOffset(const std::string& vfoName, double offset);

private:
    IWaterfallState* wf = nullptr;
    IConfigStore* config = nullptr;
    double lastSavedViewOffset = 0.0;
};
