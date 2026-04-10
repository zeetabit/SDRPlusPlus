#include <imgui.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>

SDRPP_MOD_INFO{
    /* Name:            */ "demo",
    /* Description:     */ "My fancy new module",
    /* Author:          */ "author1;author2,author3,etc...",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

SDRPP_MOD_INFO_V2{
    "demo", "My fancy new module", "author1;author2,author3,etc...", 0, 1, 0, -1,
    SDRPP_API_VERSION, MOD_CAP_MISC, 0, nullptr, nullptr, nullptr
};

class DemoModule : public ModuleManager::Instance {
public:
    DemoModule(std::string name, ModuleConfig* cfg) {
        this->name = name;
        gui::menu.registerEntry(name, menuHandler, this, NULL);
    }

    ~DemoModule() {
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        DemoModule* _this = (DemoModule*)ctx;
        ImGui::Text("Hello SDR++, my name is %s", _this->name.c_str());
    }

    std::string name;
    bool enabled = true;
};

MOD_EXPORT void _INIT_() {}

SDRPP_CREATE_INSTANCE_V2(DemoModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) {
    delete (DemoModule*)inst;
}

MOD_EXPORT void _END_() {
    // Nothing here
}