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

#ifndef __ANDROID__
#include <libhackrf/hackrf.h>
#else
#include <android_backend.h>
#include <hackrf.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "hackrf_source",
    /* Description:     */ "HackRF source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "hackrf_source",
    /* Description:     */ "HackRF source module for SDR++",
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

const char* sampleRatesTxt = "20MHz\00016MHz\00010MHz\0008MHz\0005MHz\0004MHz\0002MHz\000";

const int sampleRates[] = {
    20000000,
    16000000,
    10000000,
    8000000,
    5000000,
    4000000,
    2000000,
};

const int bandwidths[] = {
    1750000,
    2500000,
    3500000,
    5000000,
    5500000,
    6000000,
    7000000,
    8000000,
    9000000,
    10000000,
    12000000,
    14000000,
    15000000,
    20000000,
    24000000,
    28000000,
};

const char* bandwidthsTxt = "1.75MHz\0"
                            "2.5MHz\0"
                            "3.5MHz\0"
                            "5MHz\0"
                            "5.5MHz\0"
                            "6MHz\0"
                            "7MHz\0"
                            "8MHz\0"
                            "9MHz\0"
                            "10MHz\0"
                            "12MHz\0"
                            "14MHz\0"
                            "15MHz\0"
                            "20MHz\0"
                            "24MHz\0"
                            "28MHz\0"
                            "Auto\0";

class HackRFSourceModule : public ModuleManager::Instance, public ISource {
public:
    HackRFSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        hackrf_init();

        // Select the last samplerate option
        sampleRate = 2000000;
        srId = 6;

        refresh();

        std::string confSerial;
        config.readConfig([&](const json& conf) { confSerial = conf["device"]; });
        selectBySerial(confSerial);

        sigpath::sourceManager.registerSource("HackRF", static_cast<ISource*>(this));
    }

    ~HackRFSourceModule() {
        stop();
        hackrf_exit();
        sigpath::sourceManager.unregisterSource("HackRF");
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
        devList.clear();
        devListTxt = "";

#ifndef __ANDROID__
        uint64_t serials[256];
        hackrf_device_list_t* _devList = hackrf_device_list();

        for (int i = 0; i < _devList->devicecount; i++) {
            // Skip devices that are in use
            if (_devList->serial_numbers[i] == NULL) { continue; }

            // Save the device serial number
            devList.push_back(_devList->serial_numbers[i]);
            devListTxt += (char*)(_devList->serial_numbers[i] + 16);
            devListTxt += '\0';
        }

        hackrf_device_list_free(_devList);
#else
        int vid, pid;
        devFd = backend::getDeviceFD(vid, pid, backend::HACKRF_VIDPIDS);
        if (devFd < 0) { return; }
        std::string fakeName = "HackRF USB";
        devList.push_back("fake_serial");
        devListTxt += fakeName;
        devListTxt += '\0';
#endif
    }

    void selectFirst() {
        if (devList.size() != 0) {
            selectBySerial(devList[0]);
            return;
        }
        selectedSerial = "";
    }

    void selectBySerial(std::string serial) {
        if (std::find(devList.begin(), devList.end(), serial) == devList.end()) {
            selectFirst();
            return;
        }

        // Set default values
        srId = 0;
        sampleRate = 2000000;
        biasT = false;
        amp = false;
        lna = 0;
        vga = 0;
        bwId = 16;

        config.withConfig([&](json& conf) {
            if (!conf["devices"].contains(serial)) {
                conf["devices"][serial]["sampleRate"] = 2000000;
                conf["devices"][serial]["biasT"] = false;
                conf["devices"][serial]["amp"] = false;
                conf["devices"][serial]["lnaGain"] = 0;
                conf["devices"][serial]["vgaGain"] = 0;
                conf["devices"][serial]["bandwidth"] = 16;
            }

            // Load from config if available and validate
            if (conf["devices"][serial].contains("sampleRate")) {
                int psr = conf["devices"][serial]["sampleRate"];
                for (int i = 0; i < 7; i++) {
                    if (sampleRates[i] == psr) {
                        sampleRate = psr;
                        srId = i;
                    }
                }
            }
            if (conf["devices"][serial].contains("biasT")) {
                biasT = conf["devices"][serial]["biasT"];
            }
            if (conf["devices"][serial].contains("amp")) {
                amp = conf["devices"][serial]["amp"];
            }
            if (conf["devices"][serial].contains("lnaGain")) {
                lna = conf["devices"][serial]["lnaGain"];
            }
            if (conf["devices"][serial].contains("vgaGain")) {
                vga = conf["devices"][serial]["vgaGain"];
            }
            if (conf["devices"][serial].contains("bandwidth")) {
                bwId = conf["devices"][serial]["bandwidth"];
                bwId = std::clamp<int>(bwId, 0, 16);
            }
        });

        selectedSerial = serial;
    }

private:
    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("HackRFSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("HackRFSourceModule '{0}': Menu Deselect!", name);
    }

    int bandwidthIdToBw(int id) {
        if (id == 16) { return hackrf_compute_baseband_filter_bw(sampleRate); }
        return bandwidths[id];
    }

    void start() override {
        if (running) { return; }
        if (selectedSerial == "") {
            flog::error("Tried to start HackRF source with empty serial");
            return;
        }

#ifndef __ANDROID__
        hackrf_error err = (hackrf_error)hackrf_open_by_serial(selectedSerial.c_str(), &openDev);
#else
        hackrf_error err = (hackrf_error)hackrf_open_by_fd(devFd, &openDev);
#endif
        if (err != HACKRF_SUCCESS) {
            flog::error("Could not open HackRF {0}: {1}", selectedSerial, hackrf_error_name(err));
            return;
        }

        hackrf_set_sample_rate(openDev, sampleRate);
        hackrf_set_baseband_filter_bandwidth(openDev, bandwidthIdToBw(bwId));
        hackrf_set_freq(openDev, freq);

        hackrf_set_antenna_enable(openDev, biasT);
        hackrf_set_amp_enable(openDev, amp);
        hackrf_set_lna_gain(openDev, lna);
        hackrf_set_vga_gain(openDev, vga);

        hackrf_start_rx(openDev, callback, this);

        running = true;
        flog::info("HackRFSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        stream.stopWriter();
        // TODO: Stream stop
        hackrf_error err = (hackrf_error)hackrf_close(openDev);
        if (err != HACKRF_SUCCESS) {
            flog::error("Could not close HackRF {0}: {1}", selectedSerial, hackrf_error_name(err));
        }
        stream.clearWriteStop();
        flog::info("HackRFSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            hackrf_set_freq(openDev, freq);
        }
        freq = freq;
        flog::info("HackRFSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_hackrf_dev_sel_", name), &devId, devListTxt.c_str())) {
            selectBySerial(devList[devId]);
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = selectedSerial; });
        }

        if (SmGui::Combo(CONCAT("##_hackrf_sr_sel_", name), &srId, sampleRatesTxt)) {
            sampleRate = sampleRates[srId];
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["sampleRate"] = sampleRate; });
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_hackrf_refr_", name))) {
            refresh();
            selectBySerial(selectedSerial);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Bandwidth");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_hackrf_bw_sel_", name), &bwId, bandwidthsTxt)) {
            if (running) {
                hackrf_set_baseband_filter_bandwidth(openDev, bandwidthIdToBw(bwId));
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["bandwidth"] = bwId; });
        }

        SmGui::LeftLabel("LNA Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_hackrf_lna_", name), &lna, 0, 40, 8, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (running) {
                hackrf_set_lna_gain(openDev, lna);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["lnaGain"] = (int)lna; });
        }

        SmGui::LeftLabel("VGA Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_hackrf_vga_", name), &vga, 0, 62, 2, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (running) {
                hackrf_set_vga_gain(openDev, vga);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["vgaGain"] = (int)vga; });
        }

        if (SmGui::Checkbox(CONCAT("Bias-T##_hackrf_bt_", name), &biasT)) {
            if (running) {
                hackrf_set_antenna_enable(openDev, biasT);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["biasT"] = biasT; });
        }

        if (SmGui::Checkbox(CONCAT("Amp Enabled##_hackrf_amp_", name), &amp)) {
            if (running) {
                hackrf_set_amp_enable(openDev, amp);
            }
            config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["amp"] = amp; });
        }
    }

    static int callback(hackrf_transfer* transfer) {
        HackRFSourceModule* _this = (HackRFSourceModule*)transfer->rx_ctx;
        volk_8i_s32f_convert_32f((float*)_this->stream.writeBuf, (int8_t*)transfer->buffer, 128.0f, transfer->valid_length);
        if (!_this->stream.swap(transfer->valid_length / 2)) { return -1; }
        return 0;
    }

    std::string name;
    hackrf_device* openDev;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    int sampleRate;
    bool running = false;
    double freq;
    std::string selectedSerial = "";
    int devId = 0;
    int srId = 0;
    int bwId = 16;
    bool biasT = false;
    bool amp = false;
    float lna = 0;
    float vga = 0;

#ifdef __ANDROID__
    int devFd = -1;
#endif

    std::vector<std::string> devList;
    std::string devListTxt;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "hackrf_config.json");
}

SDRPP_CREATE_INSTANCE_V2(HackRFSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (HackRFSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
