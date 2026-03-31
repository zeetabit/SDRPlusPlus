#pragma once
#include <string>
#include <gui/widgets/waterfall.h>

class IVFOManager {
public:
    virtual ~IVFOManager() = default;

    virtual void updateFromWaterfall(ImGui::WaterFall* wf) = 0;
    virtual void setCenterOffset(const std::string& name, double offset) = 0;
};
