// Minimal stubs for symbols referenced by channel_manager.h but not
// needed when waterfall binding is disabled (bindWaterfall=false).
#include <imgui.h>
#include <gui/gui.h>
#include <utils/flog.h>
#include <string>
#include <vector>

namespace gui {
    ImGui::WaterFall waterfall;
}

ImGui::WaterFall::WaterFall() {}

double ImGui::WaterFall::getCenterFrequency() { return 0; }

void ImGui::WaterFall::selectFirstVFO() {
    bool found = false;
    for (auto const& [name, vfo] : vfos) {
        selectedVFO = name;
        selectedVFOChanged = true;
        found = true;
        break;
    }
    if (!found) {
        selectedVFO = "";
        selectedVFOChanged = true;
    }
}

void ImDrawList::AddLine(const ImVec2&, const ImVec2&, unsigned int, float) {}
void ImDrawList::AddText(const ImVec2&, unsigned int, const char*, const char*) {}

namespace flog {
    void __log__(Type, const char*, const std::vector<std::string>&) {}
    std::string __toString__(float v) { return std::to_string(v); }
    std::string __toString__(double v) { return std::to_string(v); }
}
