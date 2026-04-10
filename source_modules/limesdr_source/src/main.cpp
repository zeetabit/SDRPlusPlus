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
#include <lime/LimeSuite.h>


#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "limesdr_source",
    /* Description:     */ "LimeSDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "limesdr_source",
    /* Description:     */ "LimeSDR source module for SDR++",
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

class LimeSDRSourceModule : public ModuleManager::Instance, public ISource {
public:
    LimeSDRSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Init limesuite if needed

        sampleRate = 10000000.0;

        refresh();

        // Select device from config
        selectFirst();

        sigpath::sourceManager.registerSource("LimeSDR", static_cast<ISource*>(this));
    }

    ~LimeSDRSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("LimeSDR");
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
        devCount = LMS_GetDeviceList(devList);
        char buf[256];
        devListTxt = "";

        for (int i = 0; i < devCount; i++) {
            lms_device_t* dev = NULL;
            LMS_Open(&dev, devList[i], NULL);
            const lms_dev_info_t* info = LMS_GetDeviceInfo(dev);
            sprintf(buf, "%s [%" PRIX64 "]", info->deviceName, info->boardSerialNumber);
            LMS_Close(dev);

            devNames.push_back(buf);
            devListTxt += buf;
            devListTxt += '\0';
        }
    }

    void selectFirst() {
        if (devCount > 0) {
            selectByInfoStr(devList[0]);
            return;
        }
        selectedDevName = "";
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devCount; i++) {
            if (devNames[i] == name) {
                selectByInfoStr(devList[i]);
                break;
            }
        }
        selectFirst();
    }

    void selectByInfoStr(lms_info_str_t info) {
        if (devCount == 0) {
            selectedDevName = "";
            return;
        }

        // Set devId and selectedDevNames
        for (int i = 0; i < devCount; i++) {
            if (info == devList[i]) {
                devId = i;
                selectedDevName = devNames[i];
                break;
            }
        }

        lms_device_t* dev = NULL;
        LMS_Open(&dev, info, NULL);

        channelCount = LMS_GetNumChannels(dev, false);
        char buf[32];
        for (int i = 0; i < channelCount; i++) {
            sprintf(buf, "CH %d", i + 1);
            channelNamesTxt += buf;
            channelNamesTxt += '\0';
        }

        config.readConfig([&](const json& conf) {
            if (conf["devices"].contains(selectedDevName)) {
                if (conf["devices"][selectedDevName].contains("channel")) {
                    chanId = conf["devices"][selectedDevName]["channel"];
                }
                else {
                    chanId = 0;
                }
            }
            else {
                chanId = 0;
            }
        });

        chanId = std::clamp<int>(chanId, 0, channelCount - 1);

        // List antennas
        lms_name_t antennaNames[16];
        antennaCount = LMS_GetAntennaList(dev, false, chanId, antennaNames);
        antennaNameList.clear();
        antennaListTxt = "";
        for (int i = 0; i < antennaCount; i++) {
            antennaNameList.push_back(antennaNames[i]);
            antennaListTxt += antennaNames[i];
            antennaListTxt += '\0';
        }

        // List supported sample rates
        lms_range_t srRange;
        LMS_GetSampleRateRange(dev, false, &srRange);
        sampleRates.clear();
        sampleRatesTxt = "";
        sampleRates.push_back(srRange.min);
        sampleRatesTxt += getBandwdithScaled(srRange.min);
        sampleRatesTxt += '\0';
        for (int i = 1000000; i < srRange.max; i += 1000000) {
            sampleRates.push_back(i);
            sampleRatesTxt += getBandwdithScaled(i);
            sampleRatesTxt += '\0';
        }
        sampleRates.push_back(srRange.max);
        sampleRatesTxt += getBandwdithScaled(srRange.max);
        sampleRatesTxt += '\0';

        // List supported bandwidths
        lms_range_t bwRange;
        LMS_GetLPFBWRange(dev, false, &bwRange);
        bandwidths.clear();
        bandwidthsTxt = "";
        bandwidths.push_back(bwRange.min);
        bandwidthsTxt += getBandwdithScaled(bwRange.min);
        bandwidthsTxt += '\0';
        for (int i = 2000000; i < bwRange.max; i += 1000000) {
            bandwidths.push_back(i);
            bandwidthsTxt += getBandwdithScaled(i);
            bandwidthsTxt += '\0';
        }
        bandwidths.push_back(bwRange.max);
        bandwidthsTxt += getBandwdithScaled(bwRange.max);
        bandwidthsTxt += '\0';
        bandwidthsTxt += "Auto";
        bandwidthsTxt += '\0';

        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(selectedDevName)) {
                conf["devices"][selectedDevName]["sampleRate"] = sampleRates[0];
                conf["devices"][selectedDevName]["channel"] = 0;
                conf["devices"][selectedDevName]["antenna"] = "LNAW";
                conf["devices"][selectedDevName]["bandwidth"] = bandwidths.size();
                conf["devices"][selectedDevName]["gain"] = 0;
            }

            // Load sample rate
            if (conf["devices"][selectedDevName].contains("sampleRate")) {
                bool found = false;
                int sr = conf["devices"][selectedDevName]["sampleRate"];
                for (int i = 0; i < sampleRates.size(); i++) {
                    if (sr == sampleRates[i]) {
                        srId = i;
                        sampleRate = sampleRates[i];
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    srId = 0;
                    sampleRate = sampleRates[0];
                }
            }
            else {
                srId = 0;
                sampleRate = sampleRates[0];
            }

            // Load antenna
            if (conf["devices"][selectedDevName].contains("antenna")) {
                std::string antName = conf["devices"][selectedDevName]["antenna"];
                bool found = false;
                for (int i = 0; i < antennaCount; i++) {
                    if (antennaNames[i] == antName) {
                        antennaId = i;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    for (int i = 0; i < antennaCount; i++) {
                        if (antennaNames[i] == "LNAW") {
                            antennaId = i;
                            found = true;
                            break;
                        }
                    }
                    if (!found) { antennaId = 0; }
                }
            }
            else {
                bool found = false;
                for (int i = 0; i < antennaCount; i++) {
                    if (antennaNames[i] == "LNAW") {
                        antennaId = i;
                        found = true;
                        break;
                    }
                }
                if (!found) { antennaId = 0; }
            }

            // Load bandwidth
            if (conf["devices"][selectedDevName].contains("bandwidth")) {
                bwId = conf["devices"][selectedDevName]["bandwidth"];
                bwId = std::clamp<int>(bwId, 0, bandwidths.size());
            }
            else {
                bwId = bandwidths.size();
            }

            // Load gain
            if (conf["devices"][selectedDevName].contains("gain")) {
                gain = conf["devices"][selectedDevName]["gain"];
                gain = std::clamp<int>(gain, 0, 73);
            }
            else {
                gain = 0;
            }
        });

        LMS_Close(dev);
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

    int getBestBandwidth(int sampleRate) {
        for (int i = 0; i < bandwidths.size(); i++) {
            if (bandwidths[i] >= sampleRate) {
                flog::warn("Selected bandwidth is {0}", bandwidths[i]);
                return bandwidths[i];
            }
        }
        return bandwidths[bandwidths.size() - 1];
    }

    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("LimeSDRSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("LimeSDRSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedDevName.empty()) { return; }

        // Open device
        openDev = NULL;
        LMS_Open(&openDev, devList[devId], NULL);
        int err = LMS_Init(openDev);

        // On open fail, retry (work around for LimeSuite bug)
        if (err) {
            LMS_Close(openDev);
            LMS_Open(&openDev, devList[devId], NULL);
            if (err = LMS_Init(openDev)) {
                flog::error("Failed to re-initialize device ({})", err);
                return;
            }
        }

        flog::warn("Channel count: {0}", LMS_GetNumChannels(openDev, false));

        // Set options
        LMS_EnableChannel(openDev, false, chanId, true);
        LMS_SetAntenna(openDev, false, chanId, antennaId);
        LMS_SetSampleRate(openDev, sampleRate, 0);
        LMS_SetLOFrequency(openDev, false, chanId, freq);
        LMS_SetGaindB(openDev, false, chanId, gain);
        LMS_SetLPFBW(openDev, false, chanId, (bwId == bandwidths.size()) ? getBestBandwidth(sampleRate) : bandwidths[bwId]);
        LMS_SetLPF(openDev, false, chanId, true);

        // Setup and start stream
        int sampCount = sampleRate / 200;
        devStream.isTx = false;
        devStream.channel = chanId;
        devStream.fifoSize = sampCount; // TODO: Check what it's actually supposed to be
        devStream.throughputVsLatency = 0.5f;
        devStream.dataFmt = devStream.LMS_FMT_F32;
        LMS_SetupStream(openDev, &devStream);

        // Start stream
        streamRunning = true;
        LMS_StartStream(&devStream);
        workerThread = std::thread(&LimeSDRSourceModule::worker, this);


        running = true;
        flog::info("LimeSDRSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;

        streamRunning = false;
        if (workerThread.joinable()) { workerThread.join(); }

        LMS_StopStream(&devStream);
        LMS_DestroyStream(openDev, &devStream);
        LMS_EnableChannel(openDev, false, chanId, false);

        LMS_Close(openDev);

        flog::info("LimeSDRSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        freq = freq;
        if (running) {
            LMS_SetLOFrequency(openDev, false, chanId, freq);
        }
        flog::info("LimeSDRSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo("##limesdr_dev_sel", &devId, devListTxt.c_str())) {
            selectByInfoStr(devList[devId]);
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = selectedDevName; });
        }

        if (SmGui::Combo(CONCAT("##_limesdr_sr_sel_", name), &srId, sampleRatesTxt.c_str())) {
            sampleRate = sampleRates[srId];
            core::setInputSampleRate(sampleRate);
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["sampleRate"] = sampleRates[srId]; });
            }
        }

        // Refresh button
        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_limesdr_refr_", name))) {
            refresh();
            selectByName(selectedDevName);
            core::setInputSampleRate(sampleRate);
        }

        if (channelCount > 1) {
            SmGui::LeftLabel("RX Channel");
            SmGui::FillWidth();
            if (SmGui::Combo("##limesdr_ch_sel", &chanId, channelNamesTxt.c_str()) && selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["channel"] = chanId; });
            }
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Antenna");
        SmGui::FillWidth();
        if (SmGui::Combo("##limesdr_ant_sel", &antennaId, antennaListTxt.c_str())) {
            if (running) {
                LMS_SetAntenna(openDev, false, chanId, antennaId);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["antenna"] = antennaNameList[antennaId]; });
            }
        }

        SmGui::LeftLabel("Bandwidth");
        SmGui::FillWidth();
        if (SmGui::Combo("##limesdr_bw_sel", &bwId, bandwidthsTxt.c_str())) {
            if (running) {
                LMS_SetLPFBW(openDev, false, chanId, (bwId == bandwidths.size()) ? getBestBandwidth(sampleRate) : bandwidths[bwId]);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["bandwidth"] = bwId; });
            }
        }

        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt("##limesdr_gain_sel", &gain, 0, 73, SmGui::FMT_STR_INT_DB)) {
            if (running) {
                LMS_SetGaindB(openDev, false, chanId, gain);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["gain"] = gain; });
            }
        }
    }

    void worker() {
        int sampCount = sampleRate / 200;
        lms_stream_meta_t meta;
        while (streamRunning) {
            int ret = LMS_RecvStream(&devStream, stream.writeBuf, sampCount, &meta, 1000);
            if (!stream.swap(sampCount) || ret < 0) { break; }
        }
    }

    std::string name;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    bool enabled = true;
    bool streamRunning = false;
    double freq;

    int channelCount = 0;

    int devId = 0;
    int chanId = 0;
    int srId = 0;
    int bwId = 0;
    int gain = 0;

    std::vector<int> sampleRates;
    std::string sampleRatesTxt;
    std::vector<int> bandwidths;
    std::string bandwidthsTxt;

    lms_info_str_t devList[128];
    int devCount = 0;
    std::string devListTxt;
    std::vector<std::string> devNames;
    std::string selectedDevName;

    lms_device_t* openDev;

    lms_stream_t devStream;

    std::string channelNamesTxt;

    int antennaId = 0;
    std::string antennaListTxt;
    std::vector<std::string> antennaNameList;
    int antennaCount = 0;

    std::thread workerThread;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "limesdr_config.json");
}

SDRPP_CREATE_INSTANCE_V2(LimeSDRSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (LimeSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
