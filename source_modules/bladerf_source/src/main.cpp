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
#include <libbladeRF.h>
#include <gui/smgui.h>
#include <algorithm>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define NUM_BUFFERS   128
#define NUM_TRANSFERS 1

SDRPP_MOD_INFO{
    /* Name:            */ "bladerf_source",
    /* Description:     */ "BladeRF source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "bladerf_source",
    /* Description:     */ "BladeRF source module for SDR++",
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

enum BladeRFType {
    BLADERF_TYPE_UNKNOWN,
    BLADERF_TYPE_V1,
    BLADERF_TYPE_V2
};

class BladeRFSourceModule : public ModuleManager::Instance, public ISource {
public:
    BladeRFSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Define clocks
        clocks.define("onboard", "On-Board", CLOCK_SELECT_ONBOARD);
        clocks.define("external", "External", CLOCK_SELECT_EXTERNAL);

        sampleRate = 1000000.0;

        refresh();

        // Select device here
        std::string serial;
        config.readConfig([&](const json& conf) { serial = conf["device"]; });
        selectBySerial(serial);

        sigpath::sourceManager.registerSource("BladeRF", static_cast<ISource*>(this));
    }

    ~BladeRFSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("BladeRF");
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
        devListTxt = "";

        if (devInfoList != NULL) {
            bladerf_free_device_list(devInfoList);
        }

        devCount = bladerf_get_device_list(&devInfoList);
        if (devCount < 0) {
            flog::error("Could not list devices {0}", devCount);
            return;
        }
        for (int i = 0; i < devCount; i++) {
            // Keep only the first 32 character of the serial number for display
            devListTxt += std::string(devInfoList[i].serial).substr(0, 16);
            devListTxt += '\0';
        }
    }

    void selectFirst() {
        if (devCount > 0) { selectByInfo(&devInfoList[0]); }
        else {
            selectedSerial = "";
        }
    }

    void selectBySerial(std::string serial, bool reloadChannelId = true) {
        if (serial == "") {
            selectFirst();
            return;
        }
        for (int i = 0; i < devCount; i++) {
            bladerf_devinfo info = devInfoList[i];
            if (serial == info.serial) {
                devId = i;
                selectByInfo(&info, reloadChannelId);
                return;
            }
        }
        selectFirst();
    }

    void selectByInfo(bladerf_devinfo* info, bool reloadChannelId = true) {
        int ret = bladerf_open_with_devinfo(&openDev, info);
        if (ret != 0) {
            flog::error("Could not open device {0}", info->serial);
            selectedSerial = "";
            return;
        }

        selectedSerial = info->serial;
        for (int i = 0; i < devCount; i++) {
            if (selectedSerial == devInfoList[i].serial) { devId = i; }
        }

        // Get the board type
        const char* bname = bladerf_get_board_name(openDev);
        if (!strcmp(bname, "bladerf1")) {
            selectedBladeType = BLADERF_TYPE_V1;
        }
        else if (!strcmp(bname, "bladerf2")) {
            selectedBladeType = BLADERF_TYPE_V2;
        }
        else {
            selectedBladeType = BLADERF_TYPE_UNKNOWN;
        }

        // Gather info about the BladeRF's ranges
        channelCount = bladerf_get_channel_count(openDev, BLADERF_RX);

        // Load the channelId if there are more than 1 channel
        if (reloadChannelId) {
            config.readConfig([&](const json& conf) {
                if (channelCount > 1 && conf["devices"].contains(info->serial)) {
                    if (conf["devices"][info->serial].contains("channelId")) {
                        chanId = conf["devices"][info->serial]["channelId"];
                    }
                    else {
                        chanId = 0;
                    }
                }
                else {
                    chanId = 0;
                }
            });
        }

        chanId = std::clamp<int>(chanId, 0, channelCount - 1);

        bladerf_get_sample_rate_range(openDev, BLADERF_CHANNEL_RX(chanId), &srRange);
        bladerf_get_bandwidth_range(openDev, BLADERF_CHANNEL_RX(chanId), &bwRange);
        bladerf_get_gain_range(openDev, BLADERF_CHANNEL_RX(chanId), &gainRange);
        int gainModeCount = bladerf_get_gain_modes(openDev, BLADERF_CHANNEL_RX(chanId), &gainModes);

        // Generate sampleRate and Bandwidth lists
        sampleRates.clear();
        sampleRatesTxt = "";
        sampleRates.push_back(srRange->min);
        sampleRatesTxt += getBandwdithScaled(srRange->min);
        sampleRatesTxt += '\0';
        for (int i = 2000000; i < srRange->max; i += 2000000) {
            sampleRates.push_back(i);
            sampleRatesTxt += getBandwdithScaled(i);
            sampleRatesTxt += '\0';
        }
        sampleRates.push_back(srRange->max);
        sampleRatesTxt += getBandwdithScaled(srRange->max);
        sampleRatesTxt += '\0';

        // Generate bandwidth list
        bandwidths.clear();
        bandwidthsTxt = "";
        bandwidths.push_back(bwRange->min);
        bandwidthsTxt += getBandwdithScaled(bwRange->min);
        bandwidthsTxt += '\0';
        for (int i = 2000000; i < bwRange->max; i += 2000000) {
            bandwidths.push_back(i);
            bandwidthsTxt += getBandwdithScaled(i);
            bandwidthsTxt += '\0';
        }
        bandwidths.push_back(bwRange->max);
        bandwidthsTxt += getBandwdithScaled(bwRange->max);
        bandwidthsTxt += '\0';
        bandwidthsTxt += "Auto";
        bandwidthsTxt += '\0';

        // Generate list of channel names
        channelNamesTxt = "";
        char buf[32];
        for (int i = 0; i < channelCount; i++) {
            sprintf(buf, "RX %d", i + 1);
            channelNamesTxt += buf;
            channelNamesTxt += '\0';
        }

        // Generate gain mode list
        gainModeNames.clear();
        gainModesTxt = "";
        for (int i = 0; i < gainModeCount; i++) {
            std::string gm = gainModes[i].name;
            gm[0] = gm[0] & (~0x20);
            gainModeNames.push_back(gm);
            gainModesTxt += gm;
            gainModesTxt += '\0';
        }

        // Load settings here
        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(selectedSerial)) {
                conf["devices"][info->serial]["channelId"] = 0;
                conf["devices"][selectedSerial]["sampleRate"] = sampleRates[0];
                conf["devices"][selectedSerial]["bandwidth"] = bandwidths.size(); // Auto
                conf["devices"][selectedSerial]["gainMode"] = "Manual";
                conf["devices"][selectedSerial]["overallGain"] = gainRange->min;
            }

            // Load sample rate
            if (conf["devices"][selectedSerial].contains("sampleRate")) {
                bool found = false;
                uint64_t sr = conf["devices"][selectedSerial]["sampleRate"];
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

            // Load bandwidth
            if (conf["devices"][selectedSerial].contains("bandwidth")) {
                bwId = conf["devices"][selectedSerial]["bandwidth"];
                bwId = std::clamp<int>(bwId, 0, bandwidths.size());
            }
            else {
                bwId = 0;
            }

            // Load clock source
            clkId = clocks.keyId("onboard");
            if (conf["devices"][selectedSerial].contains("clock")) {
                std::string clkStr = conf["devices"][selectedSerial]["clock"];
                if (clocks.keyExists(clkStr)) {
                    clkId = clocks.keyId(clkStr);
                }
            }

            // Load gain mode
            if (conf["devices"][selectedSerial].contains("gainMode")) {
                std::string gm = conf["devices"][selectedSerial]["gainMode"];
                bool found = false;
                for (int i = 0; i < gainModeNames.size(); i++) {
                    if (gainModeNames[i] == gm) {
                        gainMode = i;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    for (int i = 0; i < gainModeNames.size(); i++) {
                        if (gainModeNames[i] == "Manual") {
                            gainMode = i;
                            break;
                        }
                    }
                }
            }
            else {
                for (int i = 0; i < gainModeNames.size(); i++) {
                    if (gainModeNames[i] == "Manual") {
                        gainMode = i;
                        break;
                    }
                }
            }

            // Load gain
            if (conf["devices"][selectedSerial].contains("overallGain")) {
                overallGain = conf["devices"][selectedSerial]["overallGain"];
                overallGain = std::clamp<int>(overallGain, gainRange->min, gainRange->max);
            }
            else {
                overallGain = gainRange->min;
            }

            // Load Bias-T
            if (selectedBladeType == BLADERF_TYPE_V2) {
                if (conf["devices"][selectedSerial].contains("biasT")) {
                    biasT = conf["devices"][selectedSerial]["biasT"];
                }
                else {
                    biasT = false;
                }
            }
        });

        bladerf_close(openDev);
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
        flog::info("BladeRFSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("BladeRFSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (devCount <= 0) { return; }

        // Open device
        bladerf_devinfo info = devInfoList[devId];
        int ret = bladerf_open_with_devinfo(&openDev, &info);
        if (ret != 0) {
            flog::error("Could not open device {0}", info.serial);
            return;
        }

        // Calculate buffer size, must be a multiple of 1024
        bufferSize = sampleRate / 200.0;
        bufferSize /= 1024;
        bufferSize *= 1024;
        if (bufferSize < 1024) { bufferSize = 1024; }

        // Setup device parameters
        setClockSource(clocks[clkId]);
        bladerf_set_sample_rate(openDev, BLADERF_CHANNEL_RX(chanId), sampleRate, NULL);
        bladerf_set_frequency(openDev, BLADERF_CHANNEL_RX(chanId), freq);
        bladerf_set_bandwidth(openDev, BLADERF_CHANNEL_RX(chanId), (bwId == bandwidths.size()) ? std::clamp<uint64_t>(sampleRate, bwRange->min, bwRange->max) : bandwidths[bwId], NULL);
        bladerf_set_gain_mode(openDev, BLADERF_CHANNEL_RX(chanId), gainModes[gainMode].mode);

        if (selectedBladeType == BLADERF_TYPE_V2) {
            bladerf_set_bias_tee(openDev, BLADERF_CHANNEL_RX(chanId), biasT);
        }

        // If gain mode is manual, set the gain
        if (gainModes[gainMode].mode == BLADERF_GAIN_MANUAL) {
            bladerf_set_gain(openDev, BLADERF_CHANNEL_RX(chanId), overallGain);
        }

        streamingEnabled = true;

        // Setup synchronous transfer
        bladerf_sync_config(openDev, BLADERF_RX_X1, BLADERF_FORMAT_SC16_Q11, 16, bufferSize, 8, 3500);

        // Enable streaming
        bladerf_enable_module(openDev, BLADERF_CHANNEL_RX(chanId), true);

        running = true;
        workerThread = std::thread(&BladeRFSourceModule::worker, this);

        flog::info("BladeRFSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        stream.stopWriter();

        streamingEnabled = false;
        // Wait for read worker to terminate
        if (workerThread.joinable()) {
            workerThread.join();
        }

        // Disable streaming
        bladerf_enable_module(openDev, BLADERF_CHANNEL_RX(chanId), false);

        // Close device
        bladerf_close(openDev);

        stream.clearWriteStop();
        flog::info("BladeRFSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        freq = freq;
        if (running) {
            bladerf_set_frequency(openDev, BLADERF_CHANNEL_RX(chanId), freq);
        }
        flog::info("BladeRFSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_balderf_dev_sel_", name), &devId, devListTxt.c_str())) {
            bladerf_devinfo info = devInfoList[devId];
            selectByInfo(&info);
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = selectedSerial; });
        }

        if (SmGui::Combo(CONCAT("##_balderf_sr_sel_", name), &srId, sampleRatesTxt.c_str())) {
            sampleRate = sampleRates[srId];
            core::setInputSampleRate(sampleRate);
            if (selectedSerial != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["sampleRate"] = sampleRates[srId]; });
            }
        }

        // Refresh button
        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_balderf_refr_", name))) {
            refresh();
            selectBySerial(selectedSerial, false);
            core::setInputSampleRate(sampleRate);
        }

        // Channel selection (only show if more than one channel)
        if (channelCount > 1) {
            SmGui::LeftLabel("RX Channel");
            SmGui::FillWidth();
            SmGui::Combo(CONCAT("##_balderf_ch_sel_", name), &chanId, channelNamesTxt.c_str());
            if (selectedSerial != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["channelId"] = chanId; });
            }
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Bandwidth");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_balderf_bw_sel_", name), &bwId, bandwidthsTxt.c_str())) {
            if (running) {
                bladerf_set_bandwidth(openDev, BLADERF_CHANNEL_RX(chanId), (bwId == bandwidths.size()) ? std::clamp<uint64_t>(sampleRate, bwRange->min, bwRange->max) : bandwidths[bwId], NULL);
            }
            if (selectedSerial != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["bandwidth"] = bwId; });
            }
        }

        SmGui::LeftLabel("Clock Source");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_balderf_clk_sel_", name), &clkId, clocks.txt)) {
            if (running) {
                setClockSource(clocks[clkId]);
            }
            if (selectedSerial != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["clock"] = clocks.key(clkId); });
            }
        }

        // General config BS
        SmGui::LeftLabel("Gain control mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_balderf_gm_sel_", name), &gainMode, gainModesTxt.c_str()) && selectedSerial != "") {
            if (running) {
                bladerf_set_gain_mode(openDev, BLADERF_CHANNEL_RX(chanId), gainModes[gainMode].mode);
            }
            // if switched to manual, reset gains
            if (gainModes[gainMode].mode == BLADERF_GAIN_MANUAL && running) {
                bladerf_set_gain(openDev, BLADERF_CHANNEL_RX(chanId), overallGain);
            }
            if (selectedSerial != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["gainMode"] = gainModeNames[gainMode]; });
            }
        }

        if (selectedSerial != "") {
            if (gainModes[gainMode].mode != BLADERF_GAIN_MANUAL) { SmGui::BeginDisabled(); }
        }
        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        if (SmGui::SliderInt("##_balderf_oag_sel_", &overallGain, (gainRange != NULL) ? gainRange->min : 0, (gainRange != NULL) ? gainRange->max : 60)) {
            if (running) {
                flog::info("Setting gain to {0}", overallGain);
                bladerf_set_gain(openDev, BLADERF_CHANNEL_RX(chanId), overallGain);
            }
            if (selectedSerial != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["overallGain"] = overallGain; });
            }
        }
        if (selectedSerial != "") {
            if (gainModes[gainMode].mode != BLADERF_GAIN_MANUAL) { SmGui::EndDisabled(); }
        }

        if (selectedBladeType == BLADERF_TYPE_V2) {
            if (SmGui::Checkbox("Bias-T##_balderf_biast_", &biasT)) {
                if (running) {
                    bladerf_set_bias_tee(openDev, BLADERF_CHANNEL_RX(chanId), biasT);
                }
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["biasT"] = biasT; });
            }
        }
    }

    void setClockSource(bladerf_clock_select clk) {
        if (selectedBladeType == BLADERF_TYPE_V1) {
            bladerf_set_smb_mode(openDev, (clk == CLOCK_SELECT_EXTERNAL) ? BLADERF_SMB_MODE_INPUT : BLADERF_SMB_MODE_DISABLED);
        }
        else {
            bladerf_set_clock_select(openDev, clk);
        }
    }

    void worker() {
        int16_t* buffer = new int16_t[bufferSize * 2];
        bladerf_metadata meta;

        while (streamingEnabled) {
            // Receive from the stream and break on error
            int ret = bladerf_sync_rx(openDev, buffer, bufferSize, &meta, 3500);
            if (ret != 0) { break; }

            // Convert to complex float and swap buffers
            volk_16i_s32f_convert_32f((float*)stream.writeBuf, buffer, 32768.0f, bufferSize * 2);
            if (!stream.swap(bufferSize)) { break; }
        }

        delete[] buffer;
    }

    std::string name;
    bladerf* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    double freq;
    int devId = 0;
    int srId = 0;
    int bwId = 0;
    int clkId = 0;
    int chanId = 0;
    int gainMode = 0;
    bool streamingEnabled = false;
    bool biasT = false;

    int channelCount;

    const bladerf_range* srRange = NULL;
    const bladerf_range* bwRange = NULL;
    const bladerf_range* gainRange = NULL;

    std::vector<uint64_t> sampleRates;
    std::string sampleRatesTxt;
    std::vector<uint64_t> bandwidths;
    std::string bandwidthsTxt;
    std::string channelNamesTxt;
    OptionList<std::string, bladerf_clock_select> clocks;

    int bufferSize;
    struct bladerf_stream* rxStream;

    int overallGain = 0;

    std::thread workerThread;

    int devCount = 0;
    bladerf_devinfo* devInfoList = NULL;
    std::string devListTxt;

    std::string selectedSerial;

    BladeRFType selectedBladeType = BLADERF_TYPE_UNKNOWN;

    const bladerf_gain_modes* gainModes;
    std::vector<std::string> gainModeNames;
    std::string gainModesTxt;
    int gainModeCount;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "bladerf_config.json");
}

SDRPP_CREATE_INSTANCE_V2(BladeRFSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (BladeRFSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
