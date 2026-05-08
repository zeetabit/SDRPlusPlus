#include <imgui.h>
#include <config.h>
#include <module_config.h>
#include <module_manifest.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <filesystem>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <gui/widgets/constellation_diagram.h>
#include "vor_decoder.h"
#include <fstream>
#include <utils/service_registry.h>
#include <utils/radio_state.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "vor_receiver",
    /* Description:     */ "VOR Receiver for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

SDRPP_MOD_INFO_V2{
    "vor_receiver", "VOR Receiver for SDR++", "Ryzerth", 0, 1, 0, -1,
    SDRPP_API_VERSION, MOD_CAP_DECODER, 0, nullptr,
    R"({})",
    "vor_receiver_config.json"
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

#define INPUT_SAMPLE_RATE VOR_IN_SR

class VORReceiverModule : public ModuleManager::Instance {
public:
    VORReceiverModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, true);
        decoder = new vor::Decoder(vfo->output, 1);
        decoder->onBearing.bind(&VORReceiverModule::onBearing, this);

        decoder->start();

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~VORReceiverModule() {
        decoder->stop();
        sigpath::vfoManager.deleteVFO(vfo);
        gui::menu.removeEntry(name);
        delete decoder;
    }

    void postInit() {}

    void enable() {
        double bw = ServiceRegistry::get().query<IRadioState>("core")->getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, INPUT_SAMPLE_RATE, true);

        decoder->setInput(vfo->output);

        decoder->start();

        enabled = true;
    }

    void disable() {
        decoder->stop();

        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        VORReceiverModule* _this = (VORReceiverModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::Text("Bearing: %f°", _this->bearing);
        ImGui::Text("Quality: %0.1f%%", _this->quality);

        if (!_this->enabled) { style::endDisabled(); }
    }

    void onBearing(float nbearing, float nquality) {
        bearing = (180.0f * nbearing / FL_M_PI);
        quality = nquality * 100.0f;
    }

    std::string name;
    bool enabled = true;

    // DSP Chain
    VFOManager::VFO* vfo;
    vor::Decoder* decoder;

    float bearing = 0.0f, quality = 0.0f;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "vor_receiver_config.json");
}

SDRPP_CREATE_INSTANCE_V2(VORReceiverModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (VORReceiverModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}