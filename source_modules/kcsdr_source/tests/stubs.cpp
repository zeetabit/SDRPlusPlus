// Link-time stubs for kcsdr_source/main.cpp dependencies that V2 contract
// tests don't actually invoke. Bodies are empty — these only need to exist
// so the linker is satisfied. If a test path starts exercising a stub, the
// stub becomes a real test double here or moves to kcsdr_stub.c.

#include <utils/flog.h>
#include <gui/smgui.h>
#include <core.h>
#include <signal_path/source.h>
#include <signal_path/signal_path.h>
#include <vector>
#include <string>
#include <map>

// ── flog: no-op logger ────────────────────────────────────────────────────
namespace flog {
    void __log__(Type, const char*, const std::vector<std::string>&) {}
    std::string __toString__(bool v) { return v ? "true" : "false"; }
    std::string __toString__(char v) { return std::string(1, v); }
    std::string __toString__(int8_t v) { return std::to_string(v); }
    std::string __toString__(int16_t v) { return std::to_string(v); }
    std::string __toString__(int32_t v) { return std::to_string(v); }
    std::string __toString__(int64_t v) { return std::to_string(v); }
    std::string __toString__(uint8_t v) { return std::to_string(v); }
    std::string __toString__(uint16_t v) { return std::to_string(v); }
    std::string __toString__(uint32_t v) { return std::to_string(v); }
    std::string __toString__(uint64_t v) { return std::to_string(v); }
    std::string __toString__(float v) { return std::to_string(v); }
    std::string __toString__(double v) { return std::to_string(v); }
    std::string __toString__(const char* v) { return v ? v : ""; }
    std::string __toString__(const void* v) { return ""; }
}

// ── core: only the one function kcsdr/main.cpp calls ─────────────────────
namespace core {
    void setInputSampleRate(double) {}
}

// ── sigpath: recording SourceManager test double. Tracks registration
//    state so T5/T8 can assert without pulling in the real source.cpp
//    (which transitively depends on iq_frontend, server, etc.).
namespace sigpath {
    SourceManager sourceManager;
}

// Test-visible state. Reset via clearSourceManagerState() between tests.
std::map<std::string, ISource*> g_registeredSources;
std::map<std::string, SourceManager::SourceHandler*> g_registeredHandlers;

void clearSourceManagerState() {
    g_registeredSources.clear();
    g_registeredHandlers.clear();
}

SourceManager::SourceManager() {}
void SourceManager::registerSource(std::string name, SourceHandler* h) {
    g_registeredHandlers[name] = h;
}
void SourceManager::registerSource(std::string name, ISource* s) {
    g_registeredSources[name] = s;
}
void SourceManager::unregisterSource(std::string name) {
    g_registeredSources.erase(name);
    g_registeredHandlers.erase(name);
}
void SourceManager::selectSource(std::string) {}
void SourceManager::showSelectedMenu() {}
void SourceManager::start() {}
void SourceManager::stop() {}
void SourceManager::tune(double) {}
void SourceManager::setTuningOffset(double) {}
void SourceManager::setTuningMode(TuningMode) {}
void SourceManager::setPanadapterIF(double) {}
std::vector<std::string> SourceManager::getSourceNames() { return {}; }

// ── SmGui: no-op widget stubs (drawMenu is unreachable in T1+T2/T3) ──────
namespace SmGui {
    void ForceSync() {}
    void ForceSyncForNext() {}
    void FillWidth() {}
    void SameLine() {}
    void BeginDisabled() {}
    void EndDisabled() {}
    void LeftLabel(const char*) {}
    bool Combo(const char*, int*, const char*, int) { return false; }
    bool Button(const char*, ImVec2) { return false; }
    bool SliderInt(const char*, int*, int, int, FormatString, ImGuiSliderFlags) { return false; }
}
