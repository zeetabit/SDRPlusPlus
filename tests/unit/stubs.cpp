// Stubs for symbols needed by core sources compiled in the test binary.
// These satisfy the linker for code paths that tests don't exercise.

#include <gui/widgets/waterfall.h>
#include <signal_path/vfo_manager.h>

namespace gui {
    ImGui::WaterFall waterfall;
}

namespace sigpath {
    VFOManager vfoManager;
}

// VFOManager minimal stubs
VFOManager::VFOManager() {}
VFOManager::VFO::~VFO() {}
std::string VFOManager::VFO::getName() { return ""; }

// WaterFall minimal stubs
namespace ImGui {
    WaterFall::WaterFall() {}
    void WaterFall::init() {}
}
