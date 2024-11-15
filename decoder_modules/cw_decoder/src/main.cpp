#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <gui/widgets/folder_select.h>
#include <utils/optionlist.h>
#include "decoder.h"
#include "cw/decoder.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "cw_decoder",
    /* Description:     */ "CW Decoder"
    /* Author:          */ "zetabit / zeetabit",
    /* Version:         */ 0, 0, 1,
    /* Max instances    */ -1
};

ConfigManager config;

enum Protocol {
    PROTOCOL_INVALID = -1,
    PROTOCOL_CW
};

class CWDecoderModule : public ModuleManager::Instance {
public:
    CWDecoderModule(std::string name) {
        this->name = name;

        // Define protocols
        protocols.define("CW", PROTOCOL_CW);

        // Initialize VFO with default values
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, BAUDRATE, SAMPLERATE, BAUDRATE, BAUDRATE, true);
        vfo->setSnapInterval(1);

        // Select the protocol
        selectProtocol(PROTOCOL_CW);


        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~CWDecoderModule() {
        gui::menu.removeEntry(name);
        // Stop DSP
        if (enabled) {
            decoder->stop();
            decoder.reset();
            sigpath::vfoManager.deleteVFO(vfo);
        }

        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        double bw = gui::waterfall.getBandwidth();
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, std::clamp<double>(0, -bw / 2.0, bw / 2.0), BAUDRATE, SAMPLERATE, BAUDRATE, BAUDRATE, true);
        vfo->setSnapInterval(1);

//        decoder->setVFO(vfo);
//        decoder->start();

        enabled = true;
    }

    void disable() {
//        decoder->stop();
        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    void selectProtocol(Protocol newProto) {
        // Cannot change while disabled
        if (!enabled) { return; }

        // If the protocol hasn't changed, no need to do anything
        if (newProto == proto) { return; }

        // Delete current decoder
        decoder.reset();

        // Create a new decoder
        switch (newProto) {
        case PROTOCOL_CW:
            decoder = std::make_unique<CWDecoder>(name, vfo);
            break;
        default:
            flog::error("Tried to select unknown cw protocol");
            return;
        }

        // Start the new decoder
        decoder->start();

        // Save selected protocol
        proto = newProto;
    }

private:
    static void menuHandler(void* ctx) {
        CWDecoderModule* _this = (CWDecoderModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::LeftLabel("Protocol");
        ImGui::FillWidth();
        if (ImGui::Combo(("##cw_decoder_proto_" + _this->name).c_str(), &_this->protoId, _this->protocols.txt)) {
            _this->selectProtocol(_this->protocols.value(_this->protoId));
        }

        if (_this->decoder) { _this->decoder->showMenu(); }

        ImGui::Button(("Record##cw_decoder_show_" + _this->name).c_str(), ImVec2(menuWidth, 0));
        ImGui::Button(("Show Messages##cw_decoder_show_" + _this->name).c_str(), ImVec2(menuWidth, 0));

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::string name;
    uint vfoBandwidth = SAMPLERATE / 2;
    bool enabled = true;

    Protocol proto = PROTOCOL_INVALID;
    int protoId = 0;

    OptionList<std::string, Protocol> protocols;

    // DSP Chain
    VFOManager::VFO* vfo;
    std::unique_ptr<Decoder> decoder;

    bool showLines = false;
};

MOD_EXPORT void _INIT_() {
    // Create default recording directory
    json def = json({});
    config.setPath(core::args["root"].s() + "/cw_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new CWDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (CWDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
