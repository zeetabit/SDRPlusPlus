#include <SoapySDR/Constants.h>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <gui/widgets/stepped_slider.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Modules.hpp>
#include <SoapySDR/Logger.hpp>
#include <core.h>
#include <gui/style.h>
#include <gui/smgui.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "soapy_source",
    /* Description:     */ "SoapySDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 5,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "soapy_source",
    /* Description:     */ "SoapySDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 5,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class SoapyModule : public ModuleManager::Instance, public ISource {
public:
    SoapyModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        //TODO: Make module tune on source select change (in sdrpp_core)

        uiGains = new float[1];

        refresh();

        // Select default device
        std::string devName;
        config.readConfig([&](const json& conf) { devName = conf["device"]; });
        selectDevice(devName);

        sigpath::sourceManager.registerSource("SoapySDR", static_cast<ISource*>(this));
    }

    ~SoapyModule() {
        stop();
        sigpath::sourceManager.unregisterSource("SoapySDR");
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

    template <typename T>
    std::string to_string_with_precision(const T a_value, const int n = 6) {
        std::ostringstream out;
        out.precision(n);
        out << std::fixed << a_value;
        return out.str();
    }

private:
    void refresh() {
        txtDevList = "";
        try {
            devList = SoapySDR::Device::enumerate();
        }
        catch (const std::exception& e) {
            flog::error("Could not list devices: {}", e.what());
            return;
        }
        
        int i = 0;
        for (auto& dev : devList) {
            txtDevList += dev["label"] != "" ? dev["label"] : dev["driver"];
            txtDevList += '\0';
            i++;
        }
    }

    float selectBwBySr(double samplerate) {
        float cur = bandwidthList[1];
        std::vector<float> bwListReversed = bandwidthList;
        std::reverse(bwListReversed.begin(), bwListReversed.end());
        for (auto bw : bwListReversed) {
            if (bw >= samplerate) {
                cur = bw;
            }
            else {
                break;
            }
        }
        flog::info("Bandwidth for samplerate {0} is {1}", samplerate, cur);
        return cur;
    }

    void selectSampleRate(double samplerate) {
        flog::info("Setting sample rate to {0}", samplerate);
        if (sampleRates.size() == 0) {
            devId = -1;
            return;
        }
        bool found = false;
        int i = 0;
        for (auto& sr : sampleRates) {
            if (sr == samplerate) {
                srId = i;
                sampleRate = sr;
                found = true;
                core::setInputSampleRate(sampleRate);
                break;
            }
            i++;
        }
        if (!found) {
            // Select default sample rate
            selectSampleRate(sampleRates[0]);
        }
    }

    void selectDevice(std::string name) {
        if (devList.size() == 0) {
            devId = -1;
            return;
        }
        bool found = false;
        int i = 0;
        for (auto& args : devList) {
            if (args["label"] == name) {
                devArgs = args;
                devId = i;
                found = true;
                break;
            }
            i++;
        }
        if (!found) {
            // If device was not found, select default device instead
            selectDevice(devList[0]["label"]);
            return;
        }

        SoapySDR::Device* dev = NULL;
        try {
            dev = SoapySDR::Device::make(devArgs);
        }
        catch (const std::exception& e) {
            flog::error("Could not open device: {}", e.what());
            return;
        }

        antennaList = dev->listAntennas(SOAPY_SDR_RX, channelId);
        txtAntennaList = "";
        for (const std::string& ant : antennaList) {
            txtAntennaList += ant + '\0';
        }

        gainList = dev->listGains(SOAPY_SDR_RX, channelId);
        delete[] uiGains;
        uiGains = new float[gainList.size()];
        gainRanges.clear();

        for (auto gain : gainList) {
            gainRanges.push_back(dev->getGainRange(SOAPY_SDR_RX, channelId, gain));
        }

        SoapySDR::RangeList bandwidthRange = dev->getBandwidthRange(SOAPY_SDR_RX, channelId);

        txtBwList = "";
        bandwidthList.clear();
        bandwidthList.push_back(-1);
        txtBwList += "Auto";
        txtBwList += '\0';

        for (auto bwr : bandwidthRange) {
            float bw = bwr.minimum();
            bandwidthList.push_back(bw);
            if (bw > 1.0e3 && bw <= 1.0e6) {
                txtBwList += to_string_with_precision((bw / 1.0e3), 2) + " kHz";
            }
            else if (bw > 1.0e6) {
                txtBwList += to_string_with_precision((bw / 1.0e6), 2) + " MHz";
            }
            else {
                txtBwList += to_string_with_precision(bw, 0);
            }
            txtBwList += '\0';
        }

        sampleRates = dev->listSampleRates(SOAPY_SDR_RX, channelId);
        txtSrList = "";
        for (double sr : sampleRates) {
            if (sr > 1.0e3 && sr <= 1.0e6) {
                txtSrList += to_string_with_precision((sr / 1.0e3), 2) + " kHz";
            }
            else if (sr > 1.0e6) {
                txtSrList += to_string_with_precision((sr / 1.0e6), 2) + " MHz";
            }
            else {
                txtSrList += to_string_with_precision(sr, 0);
            }
            txtSrList += '\0';
        }

        hasAgc = dev->hasGainMode(SOAPY_SDR_RX, channelId);

        SoapySDR::Device::unmake(dev);

        config.readConfig([&](const json& conf) {
            if (conf["devices"].contains(name)) {
                if (conf["devices"][name].contains("antenna")) {
                    uiAntennaId = conf["devices"][name]["antenna"];
                }
                else {
                    uiAntennaId = 0;
                }
                int i = 0;
                for (auto gain : gainList) {
                    if (conf["devices"][name].contains("gains") && conf["devices"][name]["gains"].contains(gain)) {
                        uiGains[i] = conf["devices"][name]["gains"][gain];
                    }
                    else {
                        uiGains[i] = gainRanges[i].minimum();
                    }
                    i++;
                }
                if (conf["devices"][name].contains("bandwidth")) {
                    uiBandwidthId = conf["devices"][name]["bandwidth"];
                }
                else if (bandwidthList.size() > 2) {
                    uiBandwidthId = 0;
                }
                if (hasAgc && conf["devices"][name].contains("agc")) {
                    agc = conf["devices"][name]["agc"];
                }
                else {
                    agc = false;
                }
                if (conf["devices"][name].contains("sampleRate")) {
                    selectSampleRate(conf["devices"][name]["sampleRate"]);
                }
                else {
                    selectSampleRate(sampleRates[0]);
                }
            }
            else {
                uiAntennaId = 0;
                int i = 0;
                for (auto gain : gainList) {
                    uiGains[i] = gainRanges[i].minimum();
                    i++;
                }
                if (bandwidthList.size() > 2)
                    uiBandwidthId = 0;
                if (hasAgc) {
                    agc = false;
                }
                selectSampleRate(sampleRates[0]); // Select default
            }
        });
    }

    void saveCurrent() {
        json conf;
        conf["sampleRate"] = sampleRate;
        conf["antenna"] = uiAntennaId;
        int i = 0;
        for (auto gain : gainList) {
            conf["gains"][gain] = uiGains[i];
            i++;
        }
        if (bandwidthList.size() > 2)
            conf["bandwidth"] = uiBandwidthId;
        if (hasAgc) {
            conf["agc"] = agc;
        }
        config.withConfig([&](json& cconf) { cconf["devices"][devArgs["label"]] = conf; });
    }

    void onSelect() override {
        flog::info("SoapyModule '{0}': Menu Select!", name);
        if (devList.size() == 0) {
            return;
        }
        core::setInputSampleRate(sampleRate);
    }

    void onDeselect() override {
        flog::info("SoapyModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (devId < 0) {
            flog::error("No device available");
            return;
        }

        try {
            dev = SoapySDR::Device::make(devArgs);
        }
        catch (const std::exception& e) {
            flog::error("Failed to open device: {}", e.what());
            return;
        }

        dev->setSampleRate(SOAPY_SDR_RX, channelId, sampleRate);

        dev->setAntenna(SOAPY_SDR_RX, channelId, antennaList[uiAntennaId]);

        if (bandwidthList.size() > 2) {
            if (bandwidthList[uiBandwidthId] == -1)
                dev->setBandwidth(SOAPY_SDR_RX, channelId, selectBwBySr(sampleRates[srId]));
            else
                dev->setBandwidth(SOAPY_SDR_RX, channelId, bandwidthList[uiBandwidthId]);
        }

        if (hasAgc) {
            dev->setGainMode(SOAPY_SDR_RX, channelId, agc);
        }

        int i = 0;
        for (auto gain : gainList) {
            dev->setGain(SOAPY_SDR_RX, channelId, gain, uiGains[i]);
            i++;
        }

        dev->setFrequency(SOAPY_SDR_RX, channelId, freq);

        devStream = dev->setupStream(SOAPY_SDR_RX, "CF32");
        dev->activateStream(devStream);
        running = true;
        workerThread = std::thread(_worker, this);
        flog::info("SoapyModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        dev->deactivateStream(devStream);
        dev->closeStream(devStream);
        stream.stopWriter();
        workerThread.join();
        stream.clearWriteStop();
        SoapySDR::Device::unmake(dev);

        flog::info("SoapyModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        freq = freq;
        if (running) {
            dev->setFrequency(SOAPY_SDR_RX, channelId, freq);
        }
        flog::info("SoapyModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        // If no device is selected, draw only the refresh button
        if (devId < 0) {
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Button(CONCAT("Refresh##_dev_select_", name))) {
                refresh();
                std::string devName;
                config.readConfig([&](const json& conf) { devName = conf["device"]; });
                selectDevice(devName);
            }
            return;
        }

        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_dev_select_", name), &devId, txtDevList.c_str())) {
            selectDevice(devList[devId]["label"]);
            config.withConfig([&](json& conf) { conf["device"] = devList[devId]["label"]; });
        }

        if (SmGui::Combo(CONCAT("##_sr_select_", name), &srId, txtSrList.c_str())) {
            selectSampleRate(sampleRates[srId]);
            if (bandwidthList.size() > 2 && running && bandwidthList[uiBandwidthId] == -1)
                dev->setBandwidth(SOAPY_SDR_RX, channelId, selectBwBySr(sampleRates[srId]));
            saveCurrent();
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::Button(CONCAT("Refresh##_dev_select_", name))) {
            refresh();
            std::string devName;
            config.readConfig([&](const json& conf) { devName = conf["device"]; });
            selectDevice(devName);
        }

        if (running) { SmGui::EndDisabled(); }

        if (antennaList.size() > 1) {
            SmGui::LeftLabel("Antenna");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##_antenna_select_", name), &uiAntennaId, txtAntennaList.c_str())) {
                if (running)
                    dev->setAntenna(SOAPY_SDR_RX, channelId, antennaList[uiAntennaId]);
                saveCurrent();
            }
        }

        // float gainNameLen = 0;
        // float len;
        // for (auto gain : gainList) {
        //     len = ImGui::CalcTextSize((gain + " gain").c_str()).x;
        //     if (len > gainNameLen) {
        //         gainNameLen = len;
        //     }
        // }
        // gainNameLen += 5.0f;

        if (hasAgc) {
            if (SmGui::Checkbox((std::string("AGC##_agc_sel_") + name).c_str(), &agc)) {
                if (running) { dev->setGainMode(SOAPY_SDR_RX, channelId, agc); }
                // When disabled, reset the gains
                if (!agc) {
                    int i = 0;
                    for (auto gain : gainList) {
                        dev->setGain(SOAPY_SDR_RX, channelId, gain, uiGains[i]);
                        i++;
                    }
                }
                saveCurrent();
            }
        }

        int i = 0;
        char buf[128];
        for (auto gain : gainList) {
            sprintf(buf, "%s gain", gain.c_str());
            SmGui::LeftLabel(buf);
            // ImGui::SetCursorPosX(gainNameLen);
            // ImGui::SetNextItemWidth(menuWidth - gainNameLen);
            float step = gainRanges[i].step();
            bool res;
            SmGui::FillWidth();
            if (step == 0.0f) {
                res = SmGui::SliderFloat((std::string("##_gain_sel_") + name + gain).c_str(), &uiGains[i], gainRanges[i].minimum(), gainRanges[i].maximum());
            }
            else {
                res = SmGui::SliderFloatWithSteps((std::string("##_gain_sel_") + name + gain).c_str(), &uiGains[i], gainRanges[i].minimum(), gainRanges[i].maximum(), step);
            }
            if (res) {
                if (running) {
                    dev->setGain(SOAPY_SDR_RX, channelId, gain, uiGains[i]);
                }
                saveCurrent();
            }
            i++;
        }
        if (bandwidthList.size() > 2) {
            SmGui::LeftLabel("Bandwidth");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##_bw_select_", name), &uiBandwidthId, txtBwList.c_str())) {
                if (running) {
                    if (bandwidthList[uiBandwidthId] == -1)
                        dev->setBandwidth(SOAPY_SDR_RX, channelId, selectBwBySr(sampleRates[srId]));
                    else
                        dev->setBandwidth(SOAPY_SDR_RX, channelId, bandwidthList[uiBandwidthId]);
                }
                saveCurrent();
            }
        }
    }

    static void _worker(SoapyModule* _this) {
        int blockSize = _this->sampleRate / 200.0f;
        int flags = 0;
        long long timeMs = 0;

        while (_this->running) {
            int res = _this->dev->readStream(_this->devStream, (void**)&_this->stream.writeBuf, blockSize, flags, timeMs);
            if (res < 1) {
                continue;
            }
            if (!_this->stream.swap(res)) { return; }
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    SoapySDR::Stream* devStream;
    SoapySDR::KwargsList devList;
    SoapySDR::Kwargs devArgs;
    SoapySDR::Device* dev;
    std::string txtDevList;
    std::string txtSrList;
    std::thread workerThread;
    int devId = -1;
    double freq;
    double sampleRate;
    bool running = false;
    bool hasAgc = false;
    bool agc = false;
    std::vector<double> sampleRates;
    int srId = -1;
    float* uiGains;
    int channelCount = 1;
    int channelId = 0;
    int uiAntennaId = 0;
    std::vector<std::string> antennaList;
    std::string txtAntennaList;
    std::vector<std::string> gainList;
    std::vector<SoapySDR::Range> gainRanges;
    int uiBandwidthId = 0;
    std::vector<float> bandwidthList;
    std::string txtBwList;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "soapy_source_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SoapyModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SoapyModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
