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
#include <gui/smgui.h>
#include <hydrasdr.h>
#include <utils/optionlist.h>

#ifdef __ANDROID__
#include <android_backend.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hydrasdr_source",
    /* Description:     */ "HydraSDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "hydrasdr_source",
    /* Description:     */ "HydraSDR source module for SDR++",
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

class HydraSDRSourceModule : public ModuleManager::Instance, public ISource {
public:
    HydraSDRSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Define the ports
        ports.define("rx0", "RX0", HYDRASDR_RF_PORT_RX0);
        ports.define("rx1", "RX1", HYDRASDR_RF_PORT_RX1);
        ports.define("rx2", "RX2", HYDRASDR_RF_PORT_RX2);

        regStr[0] = 0;
        valStr[0] = 0;

        sampleRate = 10000000.0;

        refresh();

        // Select device from config
        std::string devSerial;
        config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
        selectByString(devSerial);

        sigpath::sourceManager.registerSource("HydraSDR", static_cast<ISource*>(this));
    }

    ~HydraSDRSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("HydraSDR");;
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

    void refresh() {
#ifndef __ANDROID__
        devices.clear();

        uint64_t serials[256];
        int n = hydrasdr_list_devices(serials, 256);

        char buf[1024];
        for (int i = 0; i < n; i++) {
            sprintf(buf, "%016" PRIX64, serials[i]);
            devices.define(buf, buf, serials[i]);
        }
#else
        // Check for device presence
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::HYDRASDR_VIDPIDS);
        if (devFd < 0) { return; }

        // Get device info
        std::string fakeName = "HydraSDR USB";
        devices.define(fakeName, 0);
#endif
    }

    void selectFirst() {
        if (!devices.empty()) {
            selectBySerial(devices.value(0));
        }
    }

    void selectByString(std::string serial) {
        if (devices.keyExists(serial)) {
            selectBySerial(devices.value(devices.keyId(serial)));
            return;
        }
        selectFirst();
    }

    void selectBySerial(uint64_t serial) {
        hydrasdr_device* dev;
        try {
#ifndef __ANDROID__
            int err = hydrasdr_open_sn(&dev, serial);
#else
            int err = hydrasdr_open_fd(&dev, devFd);
#endif
            if (err != 0) {
                char buf[1024];
                sprintf(buf, "%016" PRIX64, serial);
                flog::error("Could not open HydraSDR {0}", buf);
                selectedSerial = 0;
                return;
            }
        }
        catch (const std::exception& e) {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, serial);
            flog::error("Could not open HydraSDR {}", buf);
        }
        devId = devices.valueId(serial);
        selectedSerial = serial;
        selectedSerStr = devices.key(devId);

        uint32_t sampleRates[256];
        hydrasdr_get_samplerates(dev, sampleRates, 0);
        int n = sampleRates[0];
        hydrasdr_get_samplerates(dev, sampleRates, n);
        samplerates.clear();
        for (int i = 0; i < n; i++) {
            samplerates.define(sampleRates[i], getBandwdithScaled(sampleRates[i]), sampleRates[i]);
        }

        // Load config here
        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(selectedSerStr)) {
                conf["devices"][selectedSerStr]["sampleRate"] = 10000000;
                conf["devices"][selectedSerStr]["gainMode"] = 0;
                conf["devices"][selectedSerStr]["sensitiveGain"] = 0;
                conf["devices"][selectedSerStr]["linearGain"] = 0;
                conf["devices"][selectedSerStr]["lnaGain"] = 0;
                conf["devices"][selectedSerStr]["mixerGain"] = 0;
                conf["devices"][selectedSerStr]["vgaGain"] = 0;
                conf["devices"][selectedSerStr]["lnaAgc"] = false;
                conf["devices"][selectedSerStr]["mixerAgc"] = false;
                conf["devices"][selectedSerStr]["biasT"] = false;
            }

            // Load sample rate
            srId = 0;
            sampleRate = samplerates.value(0);
            if (conf["devices"][selectedSerStr].contains("sampleRate")) {
                int selectedSr = conf["devices"][selectedSerStr]["sampleRate"];
                if (samplerates.keyExists(selectedSr)) {
                    srId = samplerates.keyId(selectedSr);
                    sampleRate = samplerates[srId];
                }
            }

            // Load port
            if (conf["devices"][selectedSerStr].contains("port")) {
                std::string portStr = conf["devices"][selectedSerStr]["port"];
                if (ports.keyExists(portStr)) {
                    portId = ports.keyId(portStr);
                }
            }

            // Load gains
            if (conf["devices"][selectedSerStr].contains("gainMode")) {
                gainMode = conf["devices"][selectedSerStr]["gainMode"];
            }
            if (conf["devices"][selectedSerStr].contains("sensitiveGain")) {
                sensitiveGain = conf["devices"][selectedSerStr]["sensitiveGain"];
            }
            if (conf["devices"][selectedSerStr].contains("linearGain")) {
                linearGain = conf["devices"][selectedSerStr]["linearGain"];
            }
            if (conf["devices"][selectedSerStr].contains("lnaGain")) {
                lnaGain = conf["devices"][selectedSerStr]["lnaGain"];
            }
            if (conf["devices"][selectedSerStr].contains("mixerGain")) {
                mixerGain = conf["devices"][selectedSerStr]["mixerGain"];
            }
            if (conf["devices"][selectedSerStr].contains("vgaGain")) {
                vgaGain = conf["devices"][selectedSerStr]["vgaGain"];
            }
            if (conf["devices"][selectedSerStr].contains("lnaAgc")) {
                lnaAgc = conf["devices"][selectedSerStr]["lnaAgc"];
            }
            if (conf["devices"][selectedSerStr].contains("mixerAgc")) {
                mixerAgc = conf["devices"][selectedSerStr]["mixerAgc"];
            }

            // Load Bias-T
            if (conf["devices"][selectedSerStr].contains("biasT")) {
                biasT = conf["devices"][selectedSerStr]["biasT"];
            }
        });

        hydrasdr_close(dev);
    }

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
        flog::info("HydraSDRSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("HydraSDRSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedSerial == 0) {
            flog::error("Tried to start HydraSDR source with null serial");
            return;
        }

#ifndef __ANDROID__
        int err = hydrasdr_open_sn(&openDev, selectedSerial);
#else
        int err = hydrasdr_open_fd(&openDev, devFd);
#endif
        if (err != 0) {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, selectedSerial);
            flog::error("Could not open HydraSDR {0}", buf);
            return;
        }

        hydrasdr_set_samplerate(openDev, samplerates[srId]);
        hydrasdr_set_freq(openDev, freq);

        hydrasdr_set_rf_port(openDev, ports[portId]);

        if (gainMode == 0) {
            hydrasdr_set_lna_agc(openDev, 0);
            hydrasdr_set_mixer_agc(openDev, 0);
            hydrasdr_set_sensitivity_gain(openDev, sensitiveGain);
        }
        else if (gainMode == 1) {
            hydrasdr_set_lna_agc(openDev, 0);
            hydrasdr_set_mixer_agc(openDev, 0);
            hydrasdr_set_linearity_gain(openDev, linearGain);
        }
        else if (gainMode == 2) {
            if (lnaAgc) {
                hydrasdr_set_lna_agc(openDev, 1);
            }
            else {
                hydrasdr_set_lna_agc(openDev, 0);
                hydrasdr_set_lna_gain(openDev, lnaGain);
            }
            if (mixerAgc) {
                hydrasdr_set_mixer_agc(openDev, 1);
            }
            else {
                hydrasdr_set_mixer_agc(openDev, 0);
                hydrasdr_set_mixer_gain(openDev, mixerGain);
            }
            hydrasdr_set_vga_gain(openDev, vgaGain);
        }

        hydrasdr_set_rf_bias(openDev, biasT);

        hydrasdr_start_rx(openDev, callback, this);

        running = true;
        flog::info("HydraSDRSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        stream.stopWriter();
        hydrasdr_close(openDev);
        stream.clearWriteStop();
        flog::info("HydraSDRSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            hydrasdr_set_freq(openDev, freq);
        }
        freq = freq;
        flog::info("HydraSDRSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_hydrasdr_dev_sel_", name), &devId, devices.txt)) {
            selectBySerial(devices[devId]);
            core::setInputSampleRate(sampleRate);
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["device"] = selectedSerStr; });
            }
        }

        if (SmGui::Combo(CONCAT("##_hydrasdr_sr_sel_", name), &srId, samplerates.txt)) {
            sampleRate = samplerates[srId];
            core::setInputSampleRate(sampleRate);
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["sampleRate"] = samplerates.key(srId); });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_hydrasdr_refr_", name))) {
            refresh();
            std::string devSerial;
            config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
            selectByString(devSerial);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Antenna Port");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_hydrasdr_port_", name), &portId, ports.txt)) {
            if (running) {
                hydrasdr_set_rf_port(openDev, ports[portId]);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["port"] = ports.key(portId); });
            }
        }

        SmGui::BeginGroup();
        SmGui::Columns(3, CONCAT("HydraSDRGainModeColumns##_", name), false);
        SmGui::ForceSync();
        if (SmGui::RadioButton(CONCAT("Sensitive##_hydrasdr_gm_", name), gainMode == 0)) {
            gainMode = 0;
            if (running) {
                hydrasdr_set_lna_agc(openDev, 0);
                hydrasdr_set_mixer_agc(openDev, 0);
                hydrasdr_set_sensitivity_gain(openDev, sensitiveGain);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["gainMode"] = 0; });
            }
        }
        SmGui::NextColumn();
        SmGui::ForceSync();
        if (SmGui::RadioButton(CONCAT("Linear##_hydrasdr_gm_", name), gainMode == 1)) {
            gainMode = 1;
            if (running) {
                hydrasdr_set_lna_agc(openDev, 0);
                hydrasdr_set_mixer_agc(openDev, 0);
                hydrasdr_set_linearity_gain(openDev, linearGain);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["gainMode"] = 1; });
            }
        }
        SmGui::NextColumn();
        SmGui::ForceSync();
        if (SmGui::RadioButton(CONCAT("Free##_hydrasdr_gm_", name), gainMode == 2)) {
            gainMode = 2;
            if (running) {
                if (lnaAgc) {
                    hydrasdr_set_lna_agc(openDev, 1);
                }
                else {
                    hydrasdr_set_lna_agc(openDev, 0);
                    hydrasdr_set_lna_gain(openDev, lnaGain);
                }
                if (mixerAgc) {
                    hydrasdr_set_mixer_agc(openDev, 1);
                }
                else {
                    hydrasdr_set_mixer_agc(openDev, 0);
                    hydrasdr_set_mixer_gain(openDev, mixerGain);
                }
                hydrasdr_set_vga_gain(openDev, vgaGain);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["gainMode"] = 2; });
            }
        }
        SmGui::Columns(1, CONCAT("EndHydraSDRGainModeColumns##_", name), false);
        SmGui::EndGroup();

        // Gain menus

        if (gainMode == 0) {
            SmGui::LeftLabel("Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_sens_gain_", name), &sensitiveGain, 0, 21)) {
                if (running) {
                    hydrasdr_set_sensitivity_gain(openDev, sensitiveGain);
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["sensitiveGain"] = sensitiveGain; });
                }
            }
        }
        else if (gainMode == 1) {
            SmGui::LeftLabel("Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_lin_gain_", name), &linearGain, 0, 21)) {
                if (running) {
                    hydrasdr_set_linearity_gain(openDev, linearGain);
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["linearGain"] = linearGain; });
                }
            }
        }
        else if (gainMode == 2) {
            // TODO: Switch to a table for alignment
            if (lnaAgc) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("LNA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_lna_gain_", name), &lnaGain, 0, 15)) {
                if (running) {
                    hydrasdr_set_lna_gain(openDev, lnaGain);
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["lnaGain"] = lnaGain; });
                }
            }
            if (lnaAgc) { SmGui::EndDisabled(); }

            if (mixerAgc) { SmGui::BeginDisabled(); }
            SmGui::LeftLabel("Mixer Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_mix_gain_", name), &mixerGain, 0, 15)) {
                if (running) {
                    hydrasdr_set_mixer_gain(openDev, mixerGain);
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["mixerGain"] = mixerGain; });
                }
            }
            if (mixerAgc) { SmGui::EndDisabled(); }

            SmGui::LeftLabel("VGA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_hydrasdr_vga_gain_", name), &vgaGain, 0, 15)) {
                if (running) {
                    hydrasdr_set_vga_gain(openDev, vgaGain);
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["vgaGain"] = vgaGain; });
                }
            }

            // AGC Control
            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("LNA AGC##_hydrasdr_", name), &lnaAgc)) {
                if (running) {
                    if (lnaAgc) {
                        hydrasdr_set_lna_agc(openDev, 1);
                    }
                    else {
                        hydrasdr_set_lna_agc(openDev, 0);
                        hydrasdr_set_lna_gain(openDev, lnaGain);
                    }
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["lnaAgc"] = lnaAgc; });
                }
            }
            SmGui::ForceSync();
            if (SmGui::Checkbox(CONCAT("Mixer AGC##_hydrasdr_", name), &mixerAgc)) {
                if (running) {
                    if (mixerAgc) {
                        hydrasdr_set_mixer_agc(openDev, 1);
                    }
                    else {
                        hydrasdr_set_mixer_agc(openDev, 0);
                        hydrasdr_set_mixer_gain(openDev, mixerGain);
                    }
                }
                if (selectedSerStr != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["mixerAgc"] = mixerAgc; });
                }
            }
        }

        // Bias T
        if (SmGui::Checkbox(CONCAT("Bias T##_hydrasdr_", name), &biasT)) {
            if (running) {
                hydrasdr_set_rf_bias(openDev, biasT);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["biasT"] = biasT; });
            }
        }
    }

    char valStr[256];
    char regStr[256];

    static int callback(hydrasdr_transfer_t* transfer) {
        HydraSDRSourceModule* _this = (HydraSDRSourceModule*)transfer->ctx;
        memcpy(_this->stream.writeBuf, transfer->samples, transfer->sample_count * sizeof(dsp::complex_t));
        if (!_this->stream.swap(transfer->sample_count)) { return -1; }
        return 0;
    }

    std::string name;
    hydrasdr_device* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    double freq;
    uint64_t selectedSerial = 0;
    std::string selectedSerStr = "";
    int devId = 0;
    int srId = 0;
    int portId = 0;

    bool biasT = false;

    int lnaGain = 0;
    int vgaGain = 0;
    int mixerGain = 0;
    int linearGain = 0;
    int sensitiveGain = 0;

    int gainMode = 0;

    bool lnaAgc = false;
    bool mixerAgc = false;

#ifdef __ANDROID__
    int devFd = 0;
#endif

    OptionList<std::string, uint64_t> devices;
    OptionList<uint32_t, uint32_t> samplerates;
    OptionList<std::string, hydrasdr_rf_port_t> ports;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "hydrasdr_config.json");
}

SDRPP_CREATE_INSTANCE_V2(HydraSDRSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HydraSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}