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
#include <rtl-sdr.h>

#ifdef __ANDROID__
#include <android_backend.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "rtl_sdr_source",
    /* Description:     */ "RTL-SDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "rtl_sdr_source",
    /* Description:     */ "RTL-SDR source module for SDR++",
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

const double sampleRates[] = {
    250000,
    1024000,
    1536000,
    1792000,
    1920000,
    2048000,
    2160000,
    2400000,
    2560000,
    2880000,
    3200000
};

const char* sampleRatesTxt[] = {
    "250KHz",
    "1.024MHz",
    "1.536MHz",
    "1.792MHz",
    "1.92MHz",
    "2.048MHz",
    "2.16MHz",
    "2.4MHz",
    "2.56MHz",
    "2.88MHz",
    "3.2MHz"
};

const char* directSamplingModesTxt = "Disabled\0I branch\0Q branch\0";

class RTLSDRSourceModule : public ModuleManager::Instance, public ISource {
public:
    RTLSDRSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        serverMode = (bool)core::args["server"];

        sampleRate = sampleRates[0];

        strcpy(dbTxt, "--");

        for (int i = 0; i < 11; i++) {
            sampleRateListTxt += sampleRatesTxt[i];
            sampleRateListTxt += '\0';
        }

        refresh();

        config.withConfig([&](json& conf) {
            if (!conf["device"].is_string()) {
                selectedDevName = "";
                conf["device"] = "";
            }
            else {
                selectedDevName = conf["device"];
            }
        });
        selectByName(selectedDevName);

        sigpath::sourceManager.registerSource("RTL-SDR", static_cast<ISource*>(this));
    }

    ~RTLSDRSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("RTL-SDR");
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
        devNames.clear();
        devListTxt = "";

#ifndef __ANDROID__
        devCount = rtlsdr_get_device_count();
        char buf[1024];
        char venBuf[256];
        char prodBuf[256];
        char snBuf[256];
        for (int i = 0; i < devCount; i++) {
            // Gather device info
            const char* devName = rtlsdr_get_device_name(i);
            int snErr = rtlsdr_get_device_usb_strings(i, venBuf, prodBuf, snBuf);

            // Build name
            if (venBuf[0] && prodBuf[0]) {
                sprintf(buf, "%s %s [%s]##%d", venBuf, prodBuf, (!snErr && snBuf[0]) ? snBuf : "No Serial", i);
            }
            else {
                sprintf(buf, "%s [%s]##%d", devName, (!snErr && snBuf[0]) ? snBuf : "No Serial", i);
            }

            // Add device to list
            devNames.push_back(buf);
            devListTxt += buf;
            devListTxt += '\0';
        }
#else
        // Check for device connection
        devCount = 0;
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::RTL_SDR_VIDPIDS);
        if (devFd < 0) { return; }

        // Generate fake device info
        devCount = 1;
        std::string fakeName = "RTL-SDR Dongle USB";
        devNames.push_back(fakeName);
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    void selectFirst() {
        if (devCount > 0) {
            selectById(0);
        }
    }

    void selectByName(std::string name) {
        for (int i = 0; i < devCount; i++) {
            if (name == devNames[i]) {
                selectById(i);
                return;
            }
        }
        selectFirst();
    }

    void selectById(int id) {
        selectedDevName = devNames[id];

#ifndef __ANDROID__
        int oret = rtlsdr_open(&openDev, id);
#else
        int oret = rtlsdr_open_sys_dev(&openDev, devFd);
#endif
        
        if (oret < 0) {
            selectedDevName = "";
            flog::error("Could not open RTL-SDR: {0}", oret);
            return;
        }

        gainList.clear();
        int gains[256];
        int n = rtlsdr_get_tuner_gains(openDev, gains);
        gainList = std::vector<int>(gains, gains + n);
        std::sort(gainList.begin(), gainList.end());

        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(selectedDevName)) {
                conf["devices"][selectedDevName]["sampleRate"] = 2400000.0;
                conf["devices"][selectedDevName]["directSampling"] = directSamplingMode;
                conf["devices"][selectedDevName]["ppm"] = 0;
                conf["devices"][selectedDevName]["biasT"] = biasT;
                conf["devices"][selectedDevName]["offsetTuning"] = offsetTuning;
                conf["devices"][selectedDevName]["rtlAgc"] = rtlAgc;
                conf["devices"][selectedDevName]["tunerAgc"] = tunerAgc;
                conf["devices"][selectedDevName]["gain"] = gainId;
            }

            if (conf["devices"][selectedDevName].contains("sampleRate")) {
                int selectedSr = conf["devices"][selectedDevName]["sampleRate"];
                for (int i = 0; i < 11; i++) {
                    if (sampleRates[i] == selectedSr) {
                        srId = i;
                        sampleRate = selectedSr;
                        break;
                    }
                }
            }

            if (conf["devices"][selectedDevName].contains("directSampling")) {
                directSamplingMode = conf["devices"][selectedDevName]["directSampling"];
            }

            if (conf["devices"][selectedDevName].contains("ppm")) {
                ppm = conf["devices"][selectedDevName]["ppm"];
            }

            if (conf["devices"][selectedDevName].contains("biasT")) {
                biasT = conf["devices"][selectedDevName]["biasT"];
            }

            if (conf["devices"][selectedDevName].contains("offsetTuning")) {
                offsetTuning = conf["devices"][selectedDevName]["offsetTuning"];
            }

            if (conf["devices"][selectedDevName].contains("rtlAgc")) {
                rtlAgc = conf["devices"][selectedDevName]["rtlAgc"];
            }

            if (conf["devices"][selectedDevName].contains("tunerAgc")) {
                tunerAgc = conf["devices"][selectedDevName]["tunerAgc"];
            }

            if (conf["devices"][selectedDevName].contains("gain")) {
                gainId = conf["devices"][selectedDevName]["gain"];
            }

            if (gainId >= gainList.size()) { gainId = gainList.size() - 1; }
            updateGainTxt();
        });

        rtlsdr_close(openDev);
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
        flog::info("RTLSDRSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("RTLSDRSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedDevName == "") {
            flog::error("No device selected");
            return;
        }

#ifndef __ANDROID__
        int oret = rtlsdr_open(&openDev, devId);
#else
        int oret = rtlsdr_open_sys_dev(&openDev, devFd);
#endif

        if (oret < 0) {
            flog::error("Could not open RTL-SDR");
            return;
        }

        flog::info("RTL-SDR Sample Rate: {0}", sampleRate);

        rtlsdr_set_sample_rate(openDev, sampleRate);
        rtlsdr_set_center_freq(openDev, freq);
        rtlsdr_set_freq_correction(openDev, ppm);
        rtlsdr_set_tuner_bandwidth(openDev, 0);
        rtlsdr_set_direct_sampling(openDev, directSamplingMode);
        rtlsdr_set_bias_tee(openDev, biasT);
        rtlsdr_set_agc_mode(openDev, rtlAgc);
        rtlsdr_set_tuner_gain(openDev, gainList[gainId]);
        if (tunerAgc) {
            rtlsdr_set_tuner_gain_mode(openDev, 0);
        }
        else {
            rtlsdr_set_tuner_gain_mode(openDev, 1);
            rtlsdr_set_tuner_gain(openDev, gainList[gainId]);
        }
        rtlsdr_set_offset_tuning(openDev, offsetTuning);

        asyncCount = (int)roundf(sampleRate / (200 * 512)) * 512;

        workerThread = std::thread(&RTLSDRSourceModule::worker, this);

        running = true;
        flog::info("RTLSDRSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        stream.stopWriter();
        rtlsdr_cancel_async(openDev);
        if (workerThread.joinable()) { workerThread.join(); }
        stream.clearWriteStop();
        rtlsdr_close(openDev);
        flog::info("RTLSDRSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            uint32_t newFreq = freq;
            int i;
            for (i = 0; i < 10; i++) {
                rtlsdr_set_center_freq(openDev, freq);
                if (rtlsdr_get_center_freq(openDev) == newFreq) { break; }
            }
            if (i > 1) {
                flog::warn("RTL-SDR took {0} attempts to tune...", i);
            }
        }
        freq = freq;
        flog::info("RTLSDRSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_rtlsdr_dev_sel_", name), &devId, devListTxt.c_str())) {
            selectById(devId);
            core::setInputSampleRate(sampleRate);
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["device"] = selectedDevName; });
            }
        }

        if (SmGui::Combo(CONCAT("##_rtlsdr_sr_sel_", name), &srId, sampleRateListTxt.c_str())) {
            sampleRate = sampleRates[srId];
            core::setInputSampleRate(sampleRate);
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["sampleRate"] = sampleRate; });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_rtlsdr_refr_", name)/*, ImVec2(refreshBtnWdith, 0)*/)) {
            refresh();
            selectByName(selectedDevName);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }

        // Rest of rtlsdr config here
        SmGui::LeftLabel("Direct Sampling");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_rtlsdr_ds_", name), &directSamplingMode, directSamplingModesTxt)) {
            if (running) {
                rtlsdr_set_direct_sampling(openDev, directSamplingMode);

                // Update gains (fix for librtlsdr bug)
                if (directSamplingMode == false) {
                    rtlsdr_set_agc_mode(openDev, rtlAgc);
                    if (tunerAgc) {
                        rtlsdr_set_tuner_gain_mode(openDev, 0);
                    }
                    else {
                        rtlsdr_set_tuner_gain_mode(openDev, 1);
                        rtlsdr_set_tuner_gain(openDev, gainList[gainId]);
                    }
                }
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["directSampling"] = directSamplingMode; });
            }
        }

        SmGui::LeftLabel("PPM Correction");
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_rtlsdr_ppm_", name), &ppm, 1, 10)) {
            ppm = std::clamp<int>(ppm, -1000000, 1000000);
            if (running) {
                rtlsdr_set_freq_correction(openDev, ppm);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["ppm"] = ppm; });
            }
        }

        if (tunerAgc || gainList.size() == 0) { SmGui::BeginDisabled(); }

        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        SmGui::ForceSync();
        // TODO: FIND ANOTHER WAY
        if (serverMode) {
            if (SmGui::SliderInt(CONCAT("##_rtlsdr_gain_", name), &gainId, 0, gainList.size() - 1, SmGui::FMT_STR_NONE)) {
                updateGainTxt();
                if (running) {
                    rtlsdr_set_tuner_gain(openDev, gainList[gainId]);
                }
                if (selectedDevName != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["gain"] = gainId; });
                }
            }
        }
        else {
            if (ImGui::SliderInt(CONCAT("##_rtlsdr_gain_", name), &gainId, 0, gainList.size() - 1, dbTxt)) {
                updateGainTxt();
                if (running) {
                    rtlsdr_set_tuner_gain(openDev, gainList[gainId]);
                }
                if (selectedDevName != "") {
                    config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["gain"] = gainId; });
                }
            }
        }

        
        if (tunerAgc || gainList.size() == 0) { SmGui::EndDisabled(); }

        if (SmGui::Checkbox(CONCAT("Bias T##_rtlsdr_rtl_biast_", name), &biasT)) {
            if (running) {
                rtlsdr_set_bias_tee(openDev, biasT);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["biasT"] = biasT; });
            }
        }

        if (SmGui::Checkbox(CONCAT("Offset Tuning##_rtlsdr_rtl_ofs_", name), &offsetTuning)) {
            if (running) {
                rtlsdr_set_offset_tuning(openDev, offsetTuning);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["offsetTuning"] = offsetTuning; });
            }
        }

        if (SmGui::Checkbox(CONCAT("RTL AGC##_rtlsdr_rtl_agc_", name), &rtlAgc)) {
            if (running) {
                rtlsdr_set_agc_mode(openDev, rtlAgc);
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["rtlAgc"] = rtlAgc; });
            }
        }

        SmGui::ForceSync();
        if (SmGui::Checkbox(CONCAT("Tuner AGC##_rtlsdr_tuner_agc_", name), &tunerAgc)) {
            if (running) {
                if (tunerAgc) {
                    rtlsdr_set_tuner_gain_mode(openDev, 0);
                }
                else {
                    rtlsdr_set_tuner_gain_mode(openDev, 1);
                    rtlsdr_set_tuner_gain(openDev, gainList[gainId]);
                }
            }
            if (selectedDevName != "") {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevName]["tunerAgc"] = tunerAgc; });
            }
        }
    }

    void worker() {
        rtlsdr_reset_buffer(openDev);
        rtlsdr_read_async(openDev, asyncHandler, this, 0, asyncCount);
    }

    static void asyncHandler(unsigned char* buf, uint32_t len, void* ctx) {
        RTLSDRSourceModule* _this = (RTLSDRSourceModule*)ctx;
        int sampCount = len / 2;
        for (int i = 0; i < sampCount; i++) {
            _this->stream.writeBuf[i].re = ((float)buf[i * 2] - 127.4) / 128.0f;
            _this->stream.writeBuf[i].im = ((float)buf[(i * 2) + 1] - 127.4) / 128.0f;
        }
        if (!_this->stream.swap(sampCount)) { return; }
    }

    void updateGainTxt() {
        sprintf(dbTxt, "%.1f dB", (float)gainList[gainId] / 10.0f);
    }

    std::string name;
    rtlsdr_dev_t* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    double freq;
    std::string selectedDevName = "";
    int devId = 0;
    int srId = 0;
    int devCount = 0;
    std::thread workerThread;
    bool serverMode = false;

#ifdef __ANDROID__
    int devFd = -1;
#endif

    int ppm = 0;

    bool biasT = false;

    int gainId = 0;
    std::vector<int> gainList;

    bool rtlAgc = false;
    bool tunerAgc = false;
    bool offsetTuning = false;

    int directSamplingMode = 0;

    // Handler stuff
    int asyncCount = 0;

    char dbTxt[128];

    std::vector<std::string> devNames;
    std::string devListTxt;
    std::string sampleRateListTxt;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "rtl_sdr_config.json");
}

SDRPP_CREATE_INSTANCE_V2(RTLSDRSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (RTLSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
