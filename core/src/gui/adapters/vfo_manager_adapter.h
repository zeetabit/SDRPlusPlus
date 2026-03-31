#pragma once
#include <gui/interfaces/ivfo_manager.h>
#include <signal_path/signal_path.h>

class VFOManagerAdapter : public IVFOManager {
public:
    void updateFromWaterfall(ImGui::WaterFall* wf) override {
        sigpath::vfoManager.updateFromWaterfall(wf);
    }
    void setCenterOffset(const std::string& name, double offset) override {
        sigpath::vfoManager.setCenterOffset(name, offset);
    }
};
