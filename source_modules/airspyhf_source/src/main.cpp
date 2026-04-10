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
#include <airspyhf.h>
#include <gui/widgets/stepped_slider.h>

#ifdef __ANDROID__
#include <android_backend.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "airspyhf_source",
    /* Description:     */ "Airspy HF+ source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "airspyhf_source",
    /* Description:     */ "Airspy HF+ source module for SDR++",
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

const char* AGG_MODES_STR = "Off\0Low\0High\0";

class AirspyHFSourceModule : public ModuleManager::Instance, public ISource {
public:
    AirspyHFSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        sampleRate = 768000.0;

        refresh();

        std::string devSerial;
        config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
        selectByString(devSerial);

        sigpath::sourceManager.registerSource("Airspy HF+", static_cast<ISource*>(this));
    }

    ~AirspyHFSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Airspy HF+");
    }

    void postInit() {}

    enum AGCMode {
        AGC_MODE_OFF,
        AGC_MODE_LOW,
        AGC_MODE_HIGG
    };

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
        devList.clear();
        devListTxt = "";

#ifndef __ANDROID__
        uint64_t serials[256];
        int n = airspyhf_list_devices(serials, 256);

        char buf[1024];
        for (int i = 0; i < n; i++) {
            sprintf(buf, "%016" PRIX64, serials[i]);
            devList.push_back(serials[i]);
            devListTxt += buf;
            devListTxt += '\0';
        }
#else
        // Check for device presence
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::AIRSPYHF_VIDPIDS);
        if (devFd < 0) { return; }

        // Get device info
        std::string fakeName = "Airspy HF+ USB";
        devList.push_back(0xDEADBEEF);
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    void selectFirst() {
        if (devList.size() != 0) {
            selectBySerial(devList[0]);
        }
    }

    void selectByString(std::string serial) {
        char buf[1024];
        for (int i = 0; i < devList.size(); i++) {
            sprintf(buf, "%016" PRIX64, devList[i]);
            std::string str = buf;
            if (serial == str) {
                selectBySerial(devList[i]);
                return;
            }
        }
        selectFirst();
    }

    void selectBySerial(uint64_t serial) {
        airspyhf_device_t* dev;
        try {
#ifndef __ANDROID__
            int err = airspyhf_open_sn(&dev, serial);
#else
            flog::warn("====  CALLING airspyhf_open_fd  ====");
            int err = airspyhf_open_fd(&dev, devFd);
            flog::warn("====  CALLED airspyhf_open_fd  => ({0}) ====", err);
#endif
            if (err != 0) {
                char buf[1024];
                sprintf(buf, "%016" PRIX64, serial);
                flog::error("Could not open Airspy HF+ {0}", buf);
                selectedSerial = 0;
                return;
            }
        }
        catch (const std::exception& e) {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, serial);
            flog::error("Could not open Airspy HF+ {}", buf);
        }

        selectedSerial = serial;

        uint32_t sampleRates[256];
        airspyhf_get_samplerates(dev, sampleRates, 0);
        int n = sampleRates[0];
        airspyhf_get_samplerates(dev, sampleRates, n);
        sampleRateList.clear();
        sampleRateListTxt = "";
        for (int i = 0; i < n; i++) {
            sampleRateList.push_back(sampleRates[i]);
            sampleRateListTxt += getBandwdithScaled(sampleRates[i]);
            sampleRateListTxt += '\0';
        }

        char buf[1024];
        sprintf(buf, "%016" PRIX64, serial);
        selectedSerStr = std::string(buf);

        // Load config here
        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(selectedSerStr)) {
                conf["devices"][selectedSerStr]["sampleRate"] = 768000;
                conf["devices"][selectedSerStr]["agcMode"] = 0;
                conf["devices"][selectedSerStr]["lna"] = false;
                conf["devices"][selectedSerStr]["attenuation"] = 0;
            }

            // Load sample rate
            srId = 0;
            sampleRate = sampleRateList[0];
            if (conf["devices"][selectedSerStr].contains("sampleRate")) {
                int selectedSr = conf["devices"][selectedSerStr]["sampleRate"];
                for (int i = 0; i < sampleRateList.size(); i++) {
                    if (sampleRateList[i] == selectedSr) {
                        srId = i;
                        sampleRate = selectedSr;
                        break;
                    }
                }
            }

            // Load Gains
            if (conf["devices"][selectedSerStr].contains("agcMode")) {
                agcMode = conf["devices"][selectedSerStr]["agcMode"];
            }
            if (conf["devices"][selectedSerStr].contains("lna")) {
                hfLNA = conf["devices"][selectedSerStr]["lna"];
            }
            if (conf["devices"][selectedSerStr].contains("attenuation")) {
                atten = conf["devices"][selectedSerStr]["attenuation"];
            }
        });

        airspyhf_close(dev);
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
        flog::info("AirspyHFSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("AirspyHFSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedSerial == 0) {
            flog::error("Tried to start AirspyHF+ source with null serial");
            return;
        }

#ifndef __ANDROID__
            int err = airspyhf_open_sn(&openDev, selectedSerial);
#else
            int err = airspyhf_open_fd(&openDev, devFd);
#endif
        if (err != 0) {
            char buf[1024];
            sprintf(buf, "%016" PRIX64, selectedSerial);
            flog::error("Could not open Airspy HF+ {0}", buf);
            return;
        }

        airspyhf_set_samplerate(openDev, sampleRateList[srId]);
        airspyhf_set_freq(openDev, freq);
        airspyhf_set_hf_agc(openDev, (agcMode != 0));
        if (agcMode > 0) {
            airspyhf_set_hf_agc_threshold(openDev, agcMode - 1);
        }
        airspyhf_set_hf_att(openDev, atten / 6.0f);
        airspyhf_set_hf_lna(openDev, hfLNA);

        airspyhf_start(openDev, callback, this);

        running = true;
        flog::info("AirspyHFSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        stream.stopWriter();
        airspyhf_close(openDev);
        stream.clearWriteStop();
        flog::info("AirspyHFSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            airspyhf_set_freq(openDev, freq);
        }
        freq = freq;
        flog::info("AirspyHFSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_airspyhf_dev_sel_", name), &devId, devListTxt.c_str())) {
            selectBySerial(devList[devId]);
            core::setInputSampleRate(sampleRate);
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["device"] = selectedSerStr; });
            }
        }

        if (SmGui::Combo(CONCAT("##_airspyhf_sr_sel_", name), &srId, sampleRateListTxt.c_str())) {
            sampleRate = sampleRateList[srId];
            core::setInputSampleRate(sampleRate);
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["sampleRate"] = sampleRate; });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_airspyhf_refr_", name))) {
            refresh();
            std::string devSerial;
            config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
            selectByString(devSerial);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("AGC Mode");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_airspyhf_agc_", name), &agcMode, AGG_MODES_STR)) {
            if (running) {
                airspyhf_set_hf_agc(openDev, (agcMode != 0));
                if (agcMode > 0) {
                    airspyhf_set_hf_agc_threshold(openDev, agcMode - 1);
                }
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["agcMode"] = agcMode; });
            }
        }

        SmGui::LeftLabel("Attenuation");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_airspyhf_attn_", name), &atten, 0, 48, 6, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (running) {
                airspyhf_set_hf_att(openDev, atten / 6.0f);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["attenuation"] = atten; });
            }
        }

        if (SmGui::Checkbox(CONCAT("HF LNA##_airspyhf_lna_", name), &hfLNA)) {
            if (running) {
                airspyhf_set_hf_lna(openDev, hfLNA);
            }
            if (selectedSerStr != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerStr]["lna"] = hfLNA; });
            }
        }
    }

    static int callback(airspyhf_transfer_t* transfer) {
        AirspyHFSourceModule* _this = (AirspyHFSourceModule*)transfer->ctx;
        memcpy(_this->stream.writeBuf, transfer->samples, transfer->sample_count * sizeof(dsp::complex_t));
        if (!_this->stream.swap(transfer->sample_count)) { return -1; }
        return 0;
    }

    std::string name;
    airspyhf_device_t* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    double freq;
    uint64_t selectedSerial = 0;
    int devId = 0;
    int srId = 0;
    int agcMode = AGC_MODE_OFF;
    bool hfLNA = false;
    float atten = 0.0f;
    std::string selectedSerStr = "";

#ifdef __ANDROID__
    int devFd = 0;
#endif

    std::vector<uint64_t> devList;
    std::string devListTxt;
    std::vector<uint32_t> sampleRateList;
    std::string sampleRateListTxt;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "airspyhf_config.json");
}

SDRPP_CREATE_INSTANCE_V2(AirspyHFSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (AirspyHFSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
