#include "spectran_http_client.h"
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
#include <gui/smgui.h>
#include <gui/widgets/stepped_slider.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "spectran_http_source",
    /* Description:     */ "Spectran V6 HTTP source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "spectran_http_source",
    /* Description:     */ "Spectran V6 HTTP source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class SpectranHTTPSourceModule : public ModuleManager::Instance, public ISource {
public:
    SpectranHTTPSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        strcpy(hostname, "localhost");
        sampleRate = 5750000.0;

        sigpath::sourceManager.registerSource("Spectran HTTP", static_cast<ISource*>(this));
    }

    ~SpectranHTTPSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Spectran HTTP");
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

    // TODO: Implement select functions

private:
    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("SpectranHTTPSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        gui::mainWindow.playButtonLocked = false;
        flog::info("SpectranHTTPSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        bool connected = (client && client->isOpen());
        if (running && connected) { return; }

        // TODO: Start
        client->streaming(true);

        // TODO: Set options

        running = true;
        flog::info("SpectranHTTPSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        
        // TODO: Implement stop
        client->streaming(false);

        flog::info("SpectranHTTPSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        bool connected = (client && client->isOpen());
        if (connected) {
            int64_t newfreq = round(freq);
            if (newfreq != lastReportedFreq && gotReport) {
                flog::debug("Sending tuning command");
                lastReportedFreq = newfreq;
                client->setCenterFrequency(newfreq);
            }
        }
        freq = freq;
        flog::info("SpectranHTTPSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        bool connected = (client && client->isOpen());
        gui::mainWindow.playButtonLocked = !connected;

        if (connected) { SmGui::BeginDisabled(); }

        if (SmGui::InputText(CONCAT("##spectran_http_host_", name), hostname, 1023)) {
            config.withConfig([&](json& conf) { conf["hostname"] = hostname; });
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##spectran_http_port_", name), &port, 0, 0)) {
            config.withConfig([&](json& conf) { conf["port"] = port; });
        }

        if (connected) { SmGui::EndDisabled(); }

        if (running) { style::beginDisabled(); }
        SmGui::FillWidth();
        if (!connected && SmGui::Button("Connect##spectran_http_source")) {
            tryConnect();
        }
        else if (connected && SmGui::Button("Disconnect##spectran_http_source")) {
            disconnect();
        }
        if (running) { style::endDisabled(); }

        SmGui::Text("Status:");
        SmGui::SameLine();
        if (connected) {
            SmGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected");
        }
        else {
            SmGui::Text("Not connected");
        }
    }

    void tryConnect() {
        try {
            gotReport = false;
            client = std::make_shared<SpectranHTTPClient>(hostname, port, &stream);
            onFreqChangedId = client->onCenterFrequencyChanged.bind(&SpectranHTTPSourceModule::onFreqChanged, this);
            onSamplerateChangedId = client->onSamplerateChanged.bind(&SpectranHTTPSourceModule::onSamplerateChanged, this);
            client->startWorker();
        }
        catch (std::runtime_error e) {
            flog::error("Could not connect: {0}", e.what());
        }
    }

    void disconnect() {
        client->onCenterFrequencyChanged.unbind(onFreqChangedId);
        client->onSamplerateChanged.unbind(onSamplerateChangedId);
        client->close();
    }

    void onFreqChanged(double newFreq) {
        if (lastReportedFreq == newFreq) { return; }
        lastReportedFreq = newFreq;
        tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", newFreq);
        gotReport = true;
    }

    void onSamplerateChanged(double newSr) {
        core::setInputSampleRate(newSr);
    }

    std::string name;
    bool enabled = true;
    double sampleRate;
    bool running = false;

    std::shared_ptr<SpectranHTTPClient> client;
    HandlerID onFreqChangedId;
    HandlerID onSamplerateChangedId;

    double freq;

    int64_t lastReportedFreq = 0;
    bool gotReport;

    char hostname[1024];
    int port = 54664;
    dsp::stream<dsp::complex_t> stream;

};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "spectran_http_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SpectranHTTPSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SpectranHTTPSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}