#include <spyserver_client.h>
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
#include <gui/smgui.h>


#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "spyserver_source",
    /* Description:     */ "SpyServer source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "spyserver_source",
    /* Description:     */ "SpyServer source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

const char* deviceTypesStr[] = {
    "Unknown",
    "Airspy One",
    "Airspy HF+",
    "RTL-SDR"
};

const char* streamFormatStr = "UInt8\0"
                              "Int16\0"
                              "Float32\0";

const SpyServerStreamFormat streamFormats[] = {
    SPYSERVER_STREAM_FORMAT_UINT8,
    SPYSERVER_STREAM_FORMAT_INT16,
    SPYSERVER_STREAM_FORMAT_FLOAT
};

const int streamFormatsBitCount[] = {
    8,
    16,
    32
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class SpyServerSourceModule : public ModuleManager::Instance, public ISource {
public:
    SpyServerSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        config.readConfig([&](const json& conf) {
            std::string host = conf["hostname"];
            port = conf["port"];
            strcpy(hostname, host.c_str());
        });

        sigpath::sourceManager.registerSource("SpyServer", static_cast<ISource*>(this));
    }

    ~SpyServerSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("SpyServer");
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
        flog::info("SpyServerSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        gui::mainWindow.playButtonLocked = false;
        flog::info("SpyServerSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        
        // Try to connect if not already connected
        if (!client) {
            tryConnect();
            if (!client) { return; }
        }

        int srvBits = streamFormatsBitCount[iqType];
        client->setSetting(SPYSERVER_SETTING_IQ_FORMAT, streamFormats[iqType]);
        client->setSetting(SPYSERVER_SETTING_IQ_DECIMATION, srId + client->devInfo.MinimumIQDecimation);
        client->setSetting(SPYSERVER_SETTING_IQ_FREQUENCY, freq);
        client->setSetting(SPYSERVER_SETTING_STREAMING_MODE, SPYSERVER_STREAM_MODE_IQ_ONLY);
        client->setSetting(SPYSERVER_SETTING_GAIN, gain);
        client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, client->computeDigitalGain(srvBits, gain, srId + client->devInfo.MinimumIQDecimation));
        client->startStream();

        running = true;
        flog::info("SpyServerSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }

        client->stopStream();

        running = false;
        flog::info("SpyServerSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            client->setSetting(SPYSERVER_SETTING_IQ_FREQUENCY, freq);
        }
        freq = freq;
        flog::info("SpyServerSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        bool connected = (client && client->isOpen());
        gui::mainWindow.playButtonLocked = !connected;

        if (connected) { SmGui::BeginDisabled(); }
        if (SmGui::InputText(CONCAT("##_spyserver_srv_host_", name), hostname, 1023)) {
            config.withConfig([&](json& conf) { conf["hostname"] = hostname; });
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_spyserver_srv_port_", name), &port, 0, 0)) {
            config.withConfig([&](json& conf) { conf["port"] = port; });
        }
        if (connected) { SmGui::EndDisabled(); }

        if (running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (!connected && SmGui::Button("Connect##spyserver_source")) {
            tryConnect();
        }
        else if (connected && SmGui::Button("Disconnect##spyserver_source")) {
            client->close();
        }
        if (running) { SmGui::EndDisabled(); }


        if (connected) {
            if (running) { style::beginDisabled(); }
            SmGui::LeftLabel("Samplerate");
            SmGui::FillWidth();
            if (SmGui::Combo("##spyserver_source_sr", &srId, sampleRatesTxt.c_str())) {
                sampleRate = sampleRates[srId];
                core::setInputSampleRate(sampleRate);
                config.withConfig([&](json& conf) { conf["devices"][devRef]["sampleRateId"] = srId; });
            }
            if (running) { style::endDisabled(); }

            SmGui::LeftLabel("Sample bit depth");
            SmGui::FillWidth();
            if (SmGui::Combo("##spyserver_source_type", &iqType, streamFormatStr)) {
                int srvBits = streamFormatsBitCount[iqType];
                client->setSetting(SPYSERVER_SETTING_IQ_FORMAT, streamFormats[iqType]);
                client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, client->computeDigitalGain(srvBits, gain, srId + client->devInfo.MinimumIQDecimation));

                config.withConfig([&](json& conf) { conf["devices"][devRef]["sampleBitDepthId"] = iqType; });
            }

            if (client->devInfo.MaximumGainIndex) {
                SmGui::FillWidth();
                if (SmGui::SliderInt("##spyserver_source_gain", (int*)&gain, 0, client->devInfo.MaximumGainIndex)) {
                    int srvBits = streamFormatsBitCount[iqType];
                    client->setSetting(SPYSERVER_SETTING_GAIN, gain);
                    client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, client->computeDigitalGain(srvBits, gain, srId + client->devInfo.MinimumIQDecimation));
                    config.withConfig([&](json& conf) { conf["devices"][devRef]["gainId"] = gain; });
                }
            }

            SmGui::Text("Status:");
            SmGui::SameLine();
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected (%s)", deviceTypesStr[client->devInfo.DeviceType]);
        }
        else {
            SmGui::Text("Status:");
            SmGui::SameLine();
            SmGui::Text("Not connected");
        }
    }

    void tryConnect() {
        try {
            if (client) { client.reset(); }
            client = spyserver::connect(hostname, port, &stream);

            if (!client->waitForDevInfo(3000)) {
                flog::error("SpyServer didn't respond with device information");
            }
            else {
                char buf[1024];
                sprintf(buf, "%s [%08X]", deviceTypesStr[client->devInfo.DeviceType], client->devInfo.DeviceSerial);
                devRef = std::string(buf);

                config.withConfig([&](json& conf) {
                    if (!conf["devices"].contains(devRef)) {
                        conf["devices"][devRef]["sampleRateId"] = 0;
                        conf["devices"][devRef]["sampleBitDepthId"] = 1;
                        conf["devices"][devRef]["gainId"] = 0;
                    }
                    srId = conf["devices"][devRef]["sampleRateId"];
                    iqType = conf["devices"][devRef]["sampleBitDepthId"];
                    gain = conf["devices"][devRef]["gainId"];
                });

                gain = std::clamp<int>(gain, 0, client->devInfo.MaximumGainIndex);

                // Refresh sample rates
                sampleRates.clear();
                sampleRatesTxt.clear();
                for (int i = client->devInfo.MinimumIQDecimation; i <= client->devInfo.DecimationStageCount; i++) {
                    double sr = (double)client->devInfo.MaximumSampleRate / ((double)(1 << i));
                    sampleRates.push_back(sr);
                    sampleRatesTxt += getBandwdithScaled(sr);
                    sampleRatesTxt += '\0';
                }

                srId = std::clamp<int>(srId, 0, sampleRates.size() - 1);

                sampleRate = sampleRates[srId];
                core::setInputSampleRate(sampleRate);
                flog::info("Connected to server");
            }
        }
        catch (const std::exception& e) {
            flog::error("Could not connect to spyserver {}", e.what());
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    double sampleRate = 1000000;
    double freq;

    char hostname[1024];
    int port = 5555;
    int iqType = 0;

    int srId = 0;
    std::vector<double> sampleRates;
    std::string sampleRatesTxt;

    uint32_t gain = 0;

    std::string devRef = "";

    dsp::stream<dsp::complex_t> stream;
    spyserver::SpyServerClient client;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "spyserver_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SpyServerSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SpyServerSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}