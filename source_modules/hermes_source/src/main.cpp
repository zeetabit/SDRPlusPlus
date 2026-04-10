#include "hermes.h"
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
#include <dsp/routing/stream_link.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hermes_source",
    /* Description:     */ "Hermes Lite 2 source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 1,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "hermes_source",
    /* Description:     */ "Hermes Lite 2 source module for SDR++",
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

class HermesSourceModule : public ModuleManager::Instance, public ISource {
public:
    HermesSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Define samplerates
        samplerates.define(48000, "48KHz", hermes::HL_SAMP_RATE_48KHZ);
        samplerates.define(96000, "96KHz", hermes::HL_SAMP_RATE_96KHZ);
        samplerates.define(192000, "192KHz", hermes::HL_SAMP_RATE_192KHZ);
        samplerates.define(384000, "384KHz", hermes::HL_SAMP_RATE_384KHZ);

        srId = samplerates.keyId(384000);

        lnk.init(NULL, &stream);

        sampleRate = 384000.0;

        sigpath::sourceManager.registerSource("Hermes", static_cast<ISource*>(this));
    }

    ~HermesSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Hermes");
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
    void refresh() {
        char mac[128];
        char buf[128];
        devices.clear();
        auto devList = hermes::discover();
        for (auto& d : devList) {
            sprintf(mac, "%02X%02X%02X%02X%02X%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
            sprintf(buf, "Hermes-Lite 2 [%s]", mac);
            devices.define(mac, buf, d);
        }
    }

    void selectMac(std::string mac) {
        // If the device list is empty, don't select anything
        if (!devices.size()) {
            selectedMac.clear();
            return;
        }

        // If the mac doesn't exist, select the first available one instead
        if (!devices.keyExists(mac)) {
            selectMac(devices.key(0));
            return;
        }

        // Default config
        srId = samplerates.valueId(hermes::HL_SAMP_RATE_384KHZ);
        gain = 0;

        // Load config
        devId = devices.keyId(mac);
        selectedMac = mac;
        config.readConfig([&](const json& conf) {
            if (conf["devices"][selectedMac].contains("samplerate")) {
                int sr = conf["devices"][selectedMac]["samplerate"];
                if (samplerates.keyExists(sr)) { srId = samplerates.keyId(sr); }
            }
            if (conf["devices"][selectedMac].contains("gain")) {
                gain = conf["devices"][selectedMac]["gain"];
            }
        });

        // Update host samplerate
        sampleRate = samplerates.key(srId);
    }

    void onSelect() override {
        if (firstSelect) {
            firstSelect = false;

            // Refresh
            refresh();

            // Select device
            config.readConfig([&](const json& conf) { selectedMac = conf["device"]; });
            selectMac(selectedMac);
        }

        core::setInputSampleRate(sampleRate);
        flog::info("HermesSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("HermesSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running || selectedMac.empty()) { return; }
        
        // TODO: Implement start
        dev = hermes::open(devices[devId].addr);

        // TODO: STOP USING A LINK, FIND A BETTER WAY
        lnk.setInput(&dev->out);
        lnk.start();
        dev->start();

        // TODO: Check if the USB commands are accepted before start
        dev->setSamplerate(samplerates[srId]);
        dev->setFrequency(freq);
        dev->setGain(gain);

        running = true;
        flog::info("HermesSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        
        // TODO: Implement stop
        dev->stop();
        dev->close();
        lnk.stop();

        flog::info("HermesSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            // TODO: Check if dev exists
            dev->setFrequency(freq);
        }
        freq = freq;
        flog::info("HermesSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_hermes_dev_sel_", name), &devId, devices.txt)) {
            selectMac(devices.key(devId));
            core::setInputSampleRate(sampleRate);
            if (!selectedMac.empty()) {
                config.withConfig([&](json& conf) { conf["device"] = devices.key(devId); });
            }
        }

        if (SmGui::Combo(CONCAT("##_hermes_sr_sel_", name), &srId, samplerates.txt)) {
            sampleRate = samplerates.key(srId);
            core::setInputSampleRate(sampleRate);
            if (!selectedMac.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedMac]["samplerate"] = samplerates.key(srId); });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_hermes_refr_", name))) {
            refresh();
            std::string mac;
            config.readConfig([&](const json& conf) { mac = conf["device"]; });
            selectMac(mac);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }

        // TODO: Device parameters

        SmGui::LeftLabel("LNA Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt("##hermes_source_lna_gain", &gain, 0, 60)) {
            if (running) {
                dev->setGain(gain);
            }
            if (!selectedMac.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedMac]["gain"] = gain; });
            }
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    dsp::routing::StreamLink<dsp::complex_t> lnk;
    double sampleRate;
    bool running = false;
    std::string selectedMac = "";

    OptionList<std::string, hermes::Info> devices;
    OptionList<int, hermes::HermesLiteSamplerate> samplerates;

    double freq;
    int devId = 0;
    int srId = 0;
    int gain = 0;

    bool firstSelect = true;

    std::shared_ptr<hermes::Client> dev;

};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "hermes_config.json");
}

SDRPP_CREATE_INSTANCE_V2(HermesSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HermesSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}