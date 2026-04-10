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
#include <perseus-sdr.h>
#include <utils/optionlist.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "perseus_source",
    /* Description:     */ "Perseus SDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "perseus_source",
    /* Description:     */ "Perseus SDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

#define MAX_SAMPLERATE_COUNT    128

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class PerseusSourceModule : public ModuleManager::Instance, public ISource {
public:
    PerseusSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        sampleRate = 768000;

        perseus_set_debug(9);

        refresh();

        std::string serial;
        config.readConfig([&](const json& conf) { serial = conf["device"]; });
        select(serial);

        sigpath::sourceManager.registerSource("Perseus", static_cast<ISource*>(this));
    }

    ~PerseusSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Perseus");
        if (libInit) { perseus_exit(); }
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
        // Re-initialize driver
        if (libInit) { perseus_exit(); }
        int devCount = perseus_init();
        if (devCount < 0) {
            libInit = false;
            flog::error("Could not initialize libperseus: {}", perseus_errorstr());
            return;
        }
        libInit = true;

        // Open each device to get the serial number
        for (int i = 0; i < devCount; i++) {
            // Open device
            perseus_descr* dev = perseus_open(i);
            if (!dev) {
                flog::error("Failed to open Perseus device with ID {}: {}", i, perseus_errorstr());
                continue;
            }

            // Load firmware
            int err = perseus_firmware_download(dev, NULL);
            if (err) {
                flog::error("Could not upload firmware to device {}: {}", i, perseus_errorstr());
                perseus_close(dev);
                continue;
            }

            // Get info
            eeprom_prodid prodId;
            err = perseus_get_product_id(dev, &prodId);
            if (err) {
                flog::error("Could not getproduct info from device {}: {}", i, perseus_errorstr());
                perseus_close(dev);
                continue;
            }

            // Create entry
            char serial[128];
            char buf[128];
            sprintf(serial, "%05d", (int)prodId.sn);
            sprintf(buf, "Perseus %d.%d [%s]", (int)prodId.hwver, (int)prodId.hwrel, serial);
            devList.define(serial, buf, i);

            // Close device
            perseus_close(dev);
        }
    }

    void select(const std::string& serial) {
        // If there are no devices, give up
        if (devList.empty()) { 
            selectedSerial.clear();
            return;
        }

        // If the serial number is not available, select first instead
        if (!devList.keyExists(serial)) {
            select(devList.key(0));
            return;
        }

        // Open device
        selectedSerial = serial;
        selectedPerseusId = devList.value(devList.keyId(serial));
        perseus_descr* dev = perseus_open(selectedPerseusId);
        if (!dev) {
            flog::error("Failed to open device {}: {}", selectedPerseusId, perseus_errorstr());
            selectedSerial.clear();
            return;
        }

        // Load firmware
        int err = perseus_firmware_download(dev, NULL);
        if (err) {
            flog::error("Could not upload firmware to device: {}", perseus_errorstr());
            perseus_close(dev);
            selectedSerial.clear();
            return;
        }

        // Get info
        eeprom_prodid prodId;
        err = perseus_get_product_id(dev, &prodId);
        if (err) {
            flog::error("Could not getproduct info from device: {}", perseus_errorstr());
            perseus_close(dev);
            selectedSerial.clear();
            return;
        }

        // List samplerates
        srList.clear();
        int samplerates[MAX_SAMPLERATE_COUNT];
        memset(samplerates, 0, sizeof(int)*MAX_SAMPLERATE_COUNT);
        err = perseus_get_sampling_rates(dev, samplerates, MAX_SAMPLERATE_COUNT);
        if (err) {
            flog::error("Could not get samplerate list: {}", perseus_errorstr());
            perseus_close(dev);
            selectedSerial.clear();
            return;
        }
        for (int i = 0; i < MAX_SAMPLERATE_COUNT; i++) {
            if (!samplerates[i]) { break; }
            srList.define(samplerates[i], getBandwdithScaled(samplerates[i]), samplerates[i]);
        }

        // TODO: List attenuator values

        // Load options
        srId = 0;
        dithering = false;
        preamp = false;
        preselector = true;
        atten = 0;
        config.readConfig([&](const json& conf) {
            if (conf["devices"][selectedSerial].contains("samplerate")) {
                int sr = conf["devices"][selectedSerial]["samplerate"];
                if (srList.keyExists(sr)) {
                    srId = srList.keyId(sr);
                }
            }
            if (conf["devices"][selectedSerial].contains("dithering")) {
                dithering = conf["devices"][selectedSerial]["dithering"];
            }
            if (conf["devices"][selectedSerial].contains("preamp")) {
                preamp = conf["devices"][selectedSerial]["preamp"];
            }
            if (conf["devices"][selectedSerial].contains("preselector")) {
                preselector = conf["devices"][selectedSerial]["preselector"];
            }
            if (conf["devices"][selectedSerial].contains("attenuation")) {
                atten = conf["devices"][selectedSerial]["attenuation"];
            }
        });

        // Update samplerate
        sampleRate = srList[srId];

        // Close device
        perseus_close(dev);
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
        flog::info("PerseusSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("PerseusSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedSerial.empty()) {
            flog::error("No device is selected");
            return;
        }
        
        // Open device
        openDev = perseus_open(selectedPerseusId);
        if (!openDev) {
            flog::error("Failed to open device {}: {}", selectedPerseusId, perseus_errorstr());
            return;
        }

        // Load firmware
        int err = perseus_firmware_download(openDev, NULL);
        if (err) {
            flog::error("Could not upload firmware to device: {}", perseus_errorstr());
            perseus_close(openDev);
            return;
        }

        // Set samplerate
        err = perseus_set_sampling_rate(openDev, sampleRate);
        if (err) {
            flog::error("Could not set samplerate: {}", perseus_errorstr());
            perseus_close(openDev);
            return;
        }

        // Set options
        perseus_set_adc(openDev, dithering, preamp);
        perseus_set_attenuator_in_db(openDev, atten);
        perseus_set_ddc_center_freq(openDev, freq, preselector);

        // Start stream
        int idealBufferSize = sampleRate / 200;
        int multipleOf1024 = std::clamp<int>(idealBufferSize / 1024, 1, 2);
        int bufferSize = multipleOf1024 * 1024;
        int bufferBytes = bufferSize*6;
        err = perseus_start_async_input(openDev, bufferBytes, callback, this);
        if (err) {
            flog::error("Could not start stream: {}", perseus_errorstr());
            perseus_close(openDev);
            return;
        }

        running = true;
        flog::info("PerseusSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;

        // Stop stream
        stream.stopWriter();
        perseus_stop_async_input(openDev);
        stream.clearWriteStop();

        // Close device
        perseus_close(openDev);

        flog::info("PerseusSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            perseus_set_ddc_center_freq(openDev, freq, preselector);
        }
        freq = freq;
        flog::info("PerseusSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_airspyhf_dev_sel_", name), &devId, devList.txt)) {
            std::string serial = devList.key(devId);
            select(serial);
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = serial; });
        }

        if (SmGui::Combo(CONCAT("##_airspyhf_sr_sel_", name), &srId, srList.txt)) {
            sampleRate = srList[srId];
            core::setInputSampleRate(sampleRate);
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["samplerate"] = sampleRate; });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_airspyhf_refr_", name))) {
            refresh();
            select(selectedSerial);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Attenuation");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_airspyhf_atten_", name), &atten, 0, 30, 10, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (running) {
                perseus_set_attenuator_in_db(openDev, atten);
            }
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["attenuation"] = atten; });
            }
        }

        if (SmGui::Checkbox(CONCAT("Preamp##_airspyhf_preamp_", name), &preamp)) {
            if (running) {
                perseus_set_adc(openDev, dithering, preamp);
            }
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["preamp"] = preamp; });
            }
        }

        if (SmGui::Checkbox(CONCAT("Dithering##_airspyhf_dither_", name), &dithering)) {
            if (running) {
                perseus_set_adc(openDev, dithering, preamp);
            }
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["dithering"] = dithering; });
            }
        }

        if (SmGui::Checkbox(CONCAT("Preselector##_airspyhf_presel_", name), &preselector)) {
            if (running) {
                perseus_set_ddc_center_freq(openDev, freq, preselector);
            }
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["preselector"] = preselector; });
            }
        }
    }

    static int callback(void* buf, int bufferSize, void* ctx) {
        PerseusSourceModule* _this = (PerseusSourceModule*)ctx;
        uint8_t* samples = (uint8_t*)buf;
        int sampleCount = bufferSize / 6;
        for (int i = 0; i < sampleCount; i++) {
            int32_t re, im;
            re = *(samples++);
            re |= *(samples++) << 8;
            re |= *(samples++) << 16;
            re |= (re >> 23) * (0xFF << 24); // Sign extend
            im = *(samples++);
            im |= *(samples++) << 8;
            im |= *(samples++) << 16;
            im |= (im >> 23) * (0xFF << 24); // Sign extend
            _this->stream.writeBuf[i].re = ((float)re / (float)0x7FFFFF);
            _this->stream.writeBuf[i].im = ((float)im / (float)0x7FFFFF);
        }
        _this->stream.swap(sampleCount);
        return 0;
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    int sampleRate;
    bool running = false;
    double freq;
    int devId = 0;
    int srId = 0;
    bool libInit = false;
    perseus_descr* openDev;
    std::string selectedSerial = "";
    int selectedPerseusId;
    float atten = 0;
    bool preamp = false;
    bool dithering = false;
    bool preselector = true;

    OptionList<std::string, int> devList;
    OptionList<int, int> srList;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "perseus_config.json");
}

SDRPP_CREATE_INSTANCE_V2(PerseusSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (PerseusSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}