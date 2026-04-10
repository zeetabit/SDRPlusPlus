#include <rfspace_client.h>
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
#include <gui/smgui.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rfspace_source",
    /* Description:     */ "RFspace source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 1,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "rfspace_source",
    /* Description:     */ "RFspace source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 1,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class RFSpaceSourceModule : public ModuleManager::Instance, public ISource {
public:
    RFSpaceSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Load config
        config.readConfig([&](const json& conf) {
            std::string hostStr = conf["hostname"];
            strcpy(hostname, hostStr.c_str());
            port = conf["port"];
        });

        sigpath::sourceManager.registerSource("RFspace", static_cast<ISource*>(this));
    }

    ~RFSpaceSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("RFspace");
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
        core::setInputSampleRate(sampleRate);
        gui::mainWindow.playButtonLocked = !(client && client->isOpen());
        flog::info("RFSpaceSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        gui::mainWindow.playButtonLocked = false;
        flog::info("RFSpaceSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // TODO: Set configuration here
        if (client) { client->start(rfspace::RFSPACE_SAMP_FORMAT_COMPLEX, rfspace::RFSPACE_SAMP_FORMAT_16BIT); }

        running = true;
        flog::info("RFSpaceSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }

        if (client) { client->stop(); }

        running = false;
        flog::info("RFSpaceSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running && client) {
            client->setFrequency(freq);
        }
        freq = freq;
        flog::info("RFSpaceSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        bool connected = (client && client->isOpen());
        gui::mainWindow.playButtonLocked = !connected;

        if (connected) { SmGui::BeginDisabled(); }
        if (SmGui::InputText(CONCAT("##_rfspace_srv_host_", name), hostname, 1023)) {
            config.withConfig([&](json& conf) { conf["hostname"] = hostname; });
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_rfspace_srv_port_", name), &port, 0, 0)) {
            config.withConfig([&](json& conf) { conf["port"] = port; });
        }
        if (connected) { SmGui::EndDisabled(); }

        if (running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (!connected && SmGui::Button("Connect##rfspace_source")) {
            try {
                if (client) { client.reset(); }
                client = rfspace::connect(hostname, port, &stream);
                deviceInit();
            }
            catch (const std::exception& e) {
                flog::error("Could not connect to SDR: {}", e.what());
            }
        }
        else if (connected && SmGui::Button("Disconnect##rfspace_source")) {
            client->close();
        }
        if (running) { SmGui::EndDisabled(); }


        if (connected) {
            if (running) { SmGui::BeginDisabled(); }

            SmGui::LeftLabel("Samplerate");
            SmGui::FillWidth();
            if (SmGui::Combo("##rfspace_source_samp_rate", &srId, sampleRates.txt)) {
                sampleRate = sampleRates[srId];
                client->setSampleRate(sampleRate);
                core::setInputSampleRate(sampleRate);
                
                config.withConfig([&](json& conf) { conf["devices"][devConfName]["sampleRate"] = sampleRates.key(srId); });
            }

            if (running) { SmGui::EndDisabled(); }

            if (client->deviceId == rfspace::RFSPACE_DEV_ID_CLOUD_IQ) {
                SmGui::LeftLabel("Antenna Port");
                SmGui::FillWidth();
                if (SmGui::Combo("##rfspace_source_rf_port", &rfPortId, rfPorts.txt)) {
                    client->setPort(rfPorts[rfPortId]);

                    config.withConfig([&](json& conf) { conf["devices"][devConfName]["rfPort"] = rfPorts.key(rfPortId); });
                }
            }

            SmGui::LeftLabel("Gain");
            SmGui::FillWidth();
            if (SmGui::SliderFloatWithSteps("##rfspace_source_gain", &gain, -30, 0, 10, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
                client->setGain(gain);

                config.withConfig([&](json& conf) { conf["devices"][devConfName]["gain"] = gain; });
            }

            SmGui::Text("Status:");
            SmGui::SameLine();
            SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), connectedStr.c_str());
        }
        else {
            SmGui::Text("Status:");
            SmGui::SameLine();
            SmGui::Text("Not connected");
        }
    }

    void deviceInit() {
        // Generate the config name
        char buf[4096];
        sprintf(buf, "%s:%05d", hostname, port);
        devConfName = buf;
        sprintf(buf, "Connected (%s:%05d)", hostname, port);
        connectedStr = buf;

        // Get device name
        if (deviceNames.find(client->deviceId) != deviceNames.end()) {
            deviceName = deviceNames[client->deviceId];
        }
        else {
            deviceName = "Unknown";
        }
        
        // Create samplerate list
        auto srs = client->getSamplerates();
        sampleRates.clear();
        for (auto& sr : srs) {
            sampleRates.define(sr, getBandwdithScaled(sr), sr);
        }
        
        // Create RF port list
        rfPorts.clear();
        rfPorts.define("Port 1", rfspace::RFSPACE_RF_PORT_1);
        if (client->deviceId == rfspace::RFSPACE_DEV_ID_CLOUD_IQ) {
            rfPorts.define("Port 2", rfspace::RFSPACE_RF_PORT_2);
        }

        // Load config
        srId = 0;
        rfPortId = 0;
        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(devConfName)) {
                conf["devices"][devConfName]["sampleRate"] = sampleRates.key(0);
                conf["devices"][devConfName]["gain"] = 0;
                if (client->deviceId == rfspace::RFSPACE_DEV_ID_CLOUD_IQ) {
                    conf["devices"][devConfName]["rfPort"] = rfPorts.key(0);
                }
            }
            if (conf["devices"][devConfName].contains("sampleRate")) {
                uint32_t sr = conf["devices"][devConfName]["sampleRate"];
                if (sampleRates.keyExists(sr)) {
                    srId = sampleRates.keyId(sr);
                }
            }
            if (conf["devices"][devConfName].contains("gain")) {
                gain = conf["devices"][devConfName]["gain"];
            }
            if (conf["devices"][devConfName].contains("rfPort")) {
                std::string port = conf["devices"][devConfName]["rfPort"];
                if (rfPorts.keyExists(port)) {
                    rfPortId = rfPorts.keyId(port);
                }
            }
        });

        // Set options
        sampleRate = sampleRates[srId];
        client->setSampleRate(sampleRate);
        core::setInputSampleRate(sampleRate);
        client->setFrequency(freq);
        client->setGain(gain);
        if (client->deviceId == rfspace::RFSPACE_DEV_ID_CLOUD_IQ) {
            client->setPort(rfPorts[rfPortId]);
        }

        flog::warn("End");
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    double sampleRate = 1228800;
    double freq;

    OptionList<uint32_t, uint32_t> sampleRates;
    int srId = 0;

    OptionList<std::string, rfspace::RFPort> rfPorts;
    int rfPortId = 0;

    float gain = 0;

    char hostname[1024];
    int port = 50000;
    std::string devConfName = "";
    std::string connectedStr = "";

    std::string deviceName = "Unknown";
    std::map<rfspace::DeviceID, std::string> deviceNames = {
        { rfspace::RFSPACE_DEV_ID_CLOUD_SDR, "CloudSDR" },
        { rfspace::RFSPACE_DEV_ID_CLOUD_IQ, "CloudIQ" },
        { rfspace::RFSPACE_DEV_ID_NET_SDR, "NetSDR" },
        { rfspace::RFSPACE_DEV_ID_SDR_IP, "SDR-IP" }
    };

    dsp::stream<dsp::complex_t> stream;
    std::shared_ptr<rfspace::Client> client;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "rfspace_source_config.json");
}

SDRPP_CREATE_INSTANCE_V2(RFSpaceSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RFSpaceSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}