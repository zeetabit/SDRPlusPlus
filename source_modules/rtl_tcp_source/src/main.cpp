#include <rtl_tcp_client.h>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <core.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rtl_tcp_source",
    /* Description:     */ "RTL-TCP source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 1, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "rtl_tcp_source",
    /* Description:     */ "RTL-TCP source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 1, 1, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class RTLTCPSourceModule : public ModuleManager::Instance, public ISource {
public:
    RTLTCPSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Define samplerates
        samplerates.define(250e3, "250KHz", 250e3);
        samplerates.define(1.024e6, "1.024MHz", 1.024e6);
        samplerates.define(1.536e6, "1.536MHz", 1.536e6);
        samplerates.define(1.792e6, "1.792MHz", 1.792e6);
        samplerates.define(1.92e6, "1.92MHz", 1.92e6);
        samplerates.define(2.048e6, "2.048MHz", 2.048e6);
        samplerates.define(2.16e6, "2.16MHz", 2.16e6);
        samplerates.define(2.4e6, "2.4MHz", 2.4e6);
        samplerates.define(2.56e6, "2.56MHz", 2.56e6);
        samplerates.define(2.88e6, "2.88MHz", 2.88e6);
        samplerates.define(3.2e6, "3.2MHz", 3.2e6);

        // Define direct sampling modes
        directSamplingModes.define(0, "Disabled", 0);
        directSamplingModes.define(1, "I branch", 1);
        directSamplingModes.define(2, "Q branch", 2);

        // Select the default samplerate instead of id 0
        srId = samplerates.valueId(2.4e6);

        // Load config
        config.readConfig([&](const json& conf) {
            if (conf.contains("host")) {
                std::string hostStr = conf["host"];
                strcpy(ip, hostStr.c_str());
            }
            if (conf.contains("port")) {
                port = conf["port"];
            }
            if (conf.contains("sampleRate")) {
                double sr = conf["sampleRate"];
                if (samplerates.keyExists(sr)) { srId = samplerates.keyId(sr); }
            }
            if (conf.contains("directSamplingMode")) {
                int mode = conf["directSamplingMode"];
                if (directSamplingModes.keyExists(mode)) { directSamplingId = directSamplingModes.keyId(mode); }
            }
            if (conf.contains("ppm")) {
                ppm = conf["ppm"];
            }
            if (conf.contains("gainIndex")) {
                gain = conf["gainIndex"];
            }
            if (conf.contains("biasTee")) {
                biasTee = conf["biasTee"];
            }
            if (conf.contains("offsetTuning")) {
                offsetTuning = conf["offsetTuning"];
            }
        });

        // Update samplerate
        sampleRate = samplerates[srId];

        // Register source
        sigpath::sourceManager.registerSource("RTL-TCP", static_cast<ISource*>(this));
    }

    ~RTLTCPSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("RTL-TCP");
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
    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("RTLTCPSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("RTLTCPSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        
        // Connect to the server
        try {
            client = rtltcp::connect(&stream, ip, port);
        }
        catch (const std::exception& e) {
            flog::error("Could connect to RTL-TCP server: {}", e.what());
            return;
        }
        
        // Sync settings
        client->setFrequency(freq);
        client->setSampleRate(sampleRate);
        client->setPPM(ppm);
        client->setDirectSampling(directSamplingId);
        client->setAGCMode(rtlAGC);
        client->setBiasTee(biasTee);
        client->setOffsetTuning(offsetTuning);
        if (tunerAGC) {
            client->setGainMode(0);
        }
        else {
            client->setGainMode(1);
            client->setGainIndex(gain);
        }

        running = true;
        flog::info("RTLTCPSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        client->close();
        running = false;
        flog::info("RTLTCPSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            client->setFrequency(freq);
        }
        freq = freq;
        flog::info("RTLTCPSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        if (SmGui::InputText(CONCAT("##_ip_select_", name), ip, 1024)) {
            config.withConfig([&](json& conf) { conf["host"] = std::string(ip); });
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_port_select_", name), &port, 0)) {
            config.withConfig([&](json& conf) { conf["port"] = port; });
        }

        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtltcp_sr_", name), &srId, samplerates.txt)) {
            sampleRate = samplerates[srId];
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["sampleRate"] = sampleRate; });
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Direct Sampling");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtltcp_ds_", name), &directSamplingId, "Disabled\0I branch\0Q branch\0")) {
            if (running) {
                client->setDirectSampling(directSamplingId);
                client->setGainIndex(gain);
            }
            config.withConfig([&](json& conf) { conf["directSamplingMode"] = directSamplingId; });
        }

        SmGui::LeftLabel("PPM Correction");
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_rtltcp_ppm_", name), &ppm, 1, 10)) {
            if (running) {
                client->setPPM(ppm);
            }
            config.withConfig([&](json& conf) { conf["ppm"] = ppm; });
        }

        if (tunerAGC) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt(CONCAT("##_gain_select_", name), &gain, 0, 28, SmGui::FMT_STR_NONE)) {
            if (running) {
                client->setGainIndex(gain);
            }
            config.withConfig([&](json& conf) { conf["gainIndex"] = gain; });
        }
        if (tunerAGC) { SmGui::EndDisabled(); }

        if (SmGui::Checkbox(CONCAT("Bias-T##_biast_select_", name), &biasTee)) {
            if (running) {
                client->setBiasTee(biasTee);
            }
            config.withConfig([&](json& conf) { conf["biasTee"] = biasTee; });
        }

        if (SmGui::Checkbox(CONCAT("Offset Tuning##_biast_select_", name), &offsetTuning)) {
            if (running) {
                client->setOffsetTuning(offsetTuning);
            }
            config.withConfig([&](json& conf) { conf["offsetTuning"] = offsetTuning; });
        }

        if (SmGui::Checkbox("RTL AGC", &rtlAGC)) {
            if (running) {
                client->setAGCMode(rtlAGC);
                if (!rtlAGC) {
                    client->setGainIndex(gain);
                }
            }
            config.withConfig([&](json& conf) { conf["rtlAGC"] = rtlAGC; });
        }

        SmGui::ForceSync();
        if (SmGui::Checkbox("Tuner AGC", &tunerAGC)) {
            if (running) {
                client->setGainMode(!tunerAGC);
                if (!tunerAGC) {
                    client->setGainIndex(gain);
                }
            }
            config.withConfig([&](json& conf) { conf["tunerAGC"] = tunerAGC; });
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    std::thread workerThread;
    std::shared_ptr<rtltcp::Client> client;
    bool running = false;
    double freq;

    char ip[1024] = "localhost";
    int port = 1234;
    int srId = 0;
    int directSamplingId = 0;
    int ppm = 0;
    int gain = 0;
    bool biasTee = false;
    bool offsetTuning = false;
    bool rtlAGC = false;
    bool tunerAGC = false;

    OptionList<double, double> samplerates;
    OptionList<int, int> directSamplingModes;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "rtl_tcp_config.json");
}

SDRPP_CREATE_INSTANCE_V2(RTLTCPSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RTLTCPSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}