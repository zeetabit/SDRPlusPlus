#include "sdrpp_server_client.h"
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/widgets/stepped_slider.h>
#include <utils/optionlist.h>
#include <gui/dialogs/dialog_box.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sdrpp_server_source",
    /* Description:     */ "SDR++ Server source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "sdrpp_server_source",
    /* Description:     */ "SDR++ Server source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class SDRPPServerSourceModule : public ModuleManager::Instance, public ISource {
public:
    SDRPPServerSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Yeah no server-ception, sorry...
        if (core::args["server"].b()) { return; }

        // Initialize lists
        sampleTypeList.define("Int8", dsp::compression::PCM_TYPE_I8);
        sampleTypeList.define("Int16", dsp::compression::PCM_TYPE_I16);
        sampleTypeList.define("Float32", dsp::compression::PCM_TYPE_F32);
        sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);

        // Load config
        config.readConfig([&](const json& conf) {
            std::string hostStr = conf["hostname"];
            strcpy(hostname, hostStr.c_str());
            port = conf["port"];
        });

        sigpath::sourceManager.registerSource("SDR++ Server", static_cast<ISource*>(this));
    }

    ~SDRPPServerSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("SDR++ Server");
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

    // ISource implementation
    dsp::stream<dsp::complex_t>* getStream() override { return &stream; }

private:
    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    void onSelect() override {
        if (client) {
            core::setInputSampleRate(client->getSampleRate());
        }
        gui::mainWindow.playButtonLocked = !(client && client->isOpen());
        flog::info("SDRPPServerSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        gui::mainWindow.playButtonLocked = false;
        flog::info("SDRPPServerSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // Try to connect if not already connected (Play button is locked anyway so not sure why I put this here)
        if (!connected()) {
            tryConnect();
            if (!connected()) { return; }
        }

        // Set configuration
        client->setFrequency(freq);
        client->start();

        running = true;
        flog::info("SDRPPServerSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }

        if (connected()) { client->stop(); }

        running = false;
        flog::info("SDRPPServerSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running && connected()) {
            client->setFrequency(freq);
        }
        freq = freq;
        flog::info("SDRPPServerSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        float menuWidth = ImGui::GetContentRegionAvail().x;

        bool isConnected = connected();
        gui::mainWindow.playButtonLocked = !isConnected;

        ImGui::GenericDialog("##sdrpp_srv_src_err_dialog", serverBusy, GENERIC_DIALOG_BUTTONS_OK, [=](){
            ImGui::TextUnformatted("This server is already in use.");
        });

        if (isConnected) { style::beginDisabled(); }
        if (ImGui::InputText(CONCAT("##sdrpp_srv_srv_host_", name), hostname, 1023)) {
            config.withConfig([&](json& conf) { conf["hostname"] = hostname; });
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(CONCAT("##sdrpp_srv_srv_port_", name), &port, 0, 0)) {
            config.withConfig([&](json& conf) { conf["port"] = port; });
        }
        if (isConnected) { style::endDisabled(); }

        if (running) { style::beginDisabled(); }
        if (!isConnected && ImGui::Button("Connect##sdrpp_srv_source", ImVec2(menuWidth, 0))) {
            tryConnect();
        }
        else if (isConnected && ImGui::Button("Disconnect##sdrpp_srv_source", ImVec2(menuWidth, 0))) {
            client->close();
        }
        if (running) { style::endDisabled(); }


        if (isConnected) {
            ImGui::LeftLabel("Sample type");
            ImGui::FillWidth();
            if (ImGui::Combo("##sdrpp_srv_source_samp_type", &sampleTypeId, sampleTypeList.txt)) {
                client->setSampleType(sampleTypeList[sampleTypeId]);

                // Save config
                config.withConfig([&](json& conf) { conf["servers"][devConfName]["sampleType"] = sampleTypeList.key(sampleTypeId); });
            }
            
            if (ImGui::Checkbox("Compression", &compression)) {
                client->setCompression(compression);

                // Save config
                config.withConfig([&](json& conf) { conf["servers"][devConfName]["compression"] = compression; });
            }

            bool dummy = true;
            style::beginDisabled();
            ImGui::Checkbox("Full IQ", &dummy);
            style::endDisabled();

            // Calculate datarate
            frametimeCounter += ImGui::GetIO().DeltaTime;
            if (frametimeCounter >= 0.2f) {
                datarate = ((float)client->bytes / (frametimeCounter * 1024.0f * 1024.0f)) * 8;
                frametimeCounter = 0;
                client->bytes = 0;
            }

            ImGui::TextUnformatted("Status:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected (%.3f Mbit/s)", datarate);

            ImGui::CollapsingHeader("Source [REMOTE]", ImGuiTreeNodeFlags_DefaultOpen);

            client->showMenu();
        }
        else {
            ImGui::TextUnformatted("Status:");
            ImGui::SameLine();
            ImGui::TextUnformatted("Not connected (--.--- Mbit/s)");
        }
    }

    bool connected() {
        return client && client->isOpen();
    }

    void tryConnect() {
        try {
            if (client) { client.reset(); }
            client = server::connect(hostname, port, &stream);
            deviceInit();
        }
        catch (const std::exception& e) {
            flog::error("Could not connect to SDR: {}", e.what());
            if (!strcmp(e.what(), "Server busy")) { serverBusy = true; }
        }
    }

    void deviceInit() {
        // Generate the config name
        char buf[4096];
        sprintf(buf, "%s:%05d", hostname, port);
        devConfName = buf;

        // Load settings
        sampleTypeId = sampleTypeList.valueId(dsp::compression::PCM_TYPE_I16);
        config.readConfig([&](const json& conf) {
            if (conf["servers"][devConfName].contains("sampleType")) {
                std::string key = conf["servers"][devConfName]["sampleType"];
                if (sampleTypeList.keyExists(key)) { sampleTypeId = sampleTypeList.keyId(key); }
            }
            if (conf["servers"][devConfName].contains("compression")) {
                compression = conf["servers"][devConfName]["compression"];
            }
        });

        // Set settings
        client->setSampleType(sampleTypeList[sampleTypeId]);
        client->setCompression(compression);
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    
    double freq;
    bool serverBusy = false;

    float datarate = 0;
    float frametimeCounter = 0;

    char hostname[1024];
    int port = 50000;
    std::string devConfName = "";

    dsp::stream<dsp::complex_t> stream;
    OptionList<std::string, dsp::compression::PCMType> sampleTypeList;
    int sampleTypeId;
    bool compression = false;

    std::shared_ptr<server::Client> client;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "sdrpp_server_source_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SDRPPServerSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SDRPPServerSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}