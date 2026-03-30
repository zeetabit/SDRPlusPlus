#pragma once
#include "radio_state.h"
#include "radio_control.h"
#include <gui/gui.h>

// Adapter that implements IRadioState and IRadioStateControl by
// delegating to gui::waterfall. Registered once during core init.
class RadioStateAdapter : public IRadioState, public IRadioStateControl {
public:
    // IRadioState (read-only)
    double getCenterFrequency() override { return gui::waterfall.getCenterFrequency(); }
    double getBandwidth() override { return gui::waterfall.getBandwidth(); }

    std::string getSelectedVFO() override { return gui::waterfall.selectedVFO; }

    bool vfoExists(const std::string& name) override {
        return gui::waterfall.vfos.find(name) != gui::waterfall.vfos.end();
    }

    double getVFOGeneralOffset(const std::string& name) override {
        auto it = gui::waterfall.vfos.find(name);
        if (it == gui::waterfall.vfos.end()) { return 0; }
        return it->second->generalOffset;
    }

    double getVFOCenterOffset(const std::string& name) override {
        auto it = gui::waterfall.vfos.find(name);
        if (it == gui::waterfall.vfos.end()) { return 0; }
        return it->second->centerOffset;
    }

    double getVFOBandwidth(const std::string& name) override {
        auto it = gui::waterfall.vfos.find(name);
        if (it == gui::waterfall.vfos.end()) { return 0; }
        return it->second->bandwidth;
    }

    std::vector<std::string> getVFONames() override {
        std::vector<std::string> names;
        for (auto& [name, _] : gui::waterfall.vfos) {
            names.push_back(name);
        }
        return names;
    }

    double getViewBandwidth() override { return gui::waterfall.getViewBandwidth(); }
    double getViewOffset() override { return gui::waterfall.getViewOffset(); }

    bool isCenterFrequencyLocked() override { return gui::waterfall.centerFrequencyLocked; }

    float* acquireLatestFFT(int& width) override { return gui::waterfall.acquireLatestFFT(width); }
    void releaseLatestFFT() override { gui::waterfall.releaseLatestFFT(); }

    // IRadioStateControl (mutations)
    void setCenterFrequency(double freq) override { gui::waterfall.setCenterFrequency(freq); }
    void setCenterFrequencyLocked(bool locked) override { gui::waterfall.centerFrequencyLocked = locked; }
    void setInputHandled(bool handled) override { gui::waterfall.inputHandled = handled; }
};
