#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <core.h>
#include <gui/style.h>
#include <gui/smgui.h>
#include <iio.h>
#include <ad9361.h>
#include <utils/optionlist.h>
#include <algorithm>
#include <regex>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "plutosdr_source",
    /* Description:     */ "PlutoSDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "plutosdr_source",
    /* Description:     */ "PlutoSDR source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

const std::vector<const char*> deviceWhiteList = {
    "PlutoSDR",
    "ANTSDR",
    "LibreSDR"
};

class PlutoSDRSourceModule : public ModuleManager::Instance, public ISource {
public:
    PlutoSDRSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Define valid samplerates
        for (int sr = 1000000; sr <= 61440000; sr += 500000) {
            samplerates.define(sr, getBandwdithScaled(sr), sr);
        }
        samplerates.define(61440000, getBandwdithScaled(61440000.0), 61440000.0);

        // Define valid bandwidths
        bandwidths.define(0, "Auto", 0);
        for (int bw = 1000000.0; bw <= 52000000; bw += 500000) {
            bandwidths.define(bw, getBandwdithScaled(bw), bw);
        }

        // Define gain modes
        gainModes.define("manual", "Manual", "manual");
        gainModes.define("fast_attack", "Fast Attack", "fast_attack");
        gainModes.define("slow_attack", "Slow Attack", "slow_attack");
        gainModes.define("hybrid", "Hybrid", "hybrid");

        // Enumerate devices
        refresh();

        // Select device
        config.readConfig([&](const json& conf) { devDesc = conf["device"]; });
        select(devDesc);

        // Register source
        sigpath::sourceManager.registerSource("PlutoSDR", static_cast<ISource*>(this));
    }

    ~PlutoSDRSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("PlutoSDR");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = true;
    }

    bool isEnabled() {
        return enabled;
    }

    // ISource implementation
    dsp::stream<dsp::complex_t>* getStream() override { return &stream; }

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

    void refresh() {
        // Clear device list
        devices.clear();

        // Create scan context
        iio_scan_context* sctx = iio_create_scan_context(NULL, 0);
        if (!sctx) {
            flog::error("Failed get scan context");
            return;
        }

        // Create parsing regexes
        std::regex backendRgx(".+(?=:)", std::regex::ECMAScript);
        std::regex modelRgx("\\(.+(?=\\),)", std::regex::ECMAScript);
        std::regex serialRgx("serial=[0-9A-Za-z]+", std::regex::ECMAScript);

        // Enumerate devices
        iio_context_info** ctxInfoList;
        ssize_t count = iio_scan_context_get_info_list(sctx, &ctxInfoList);
        if (count < 0) {
            flog::error("Failed to enumerate contexts");
            return;
        }
        for (ssize_t i = 0; i < count; i++) {
            iio_context_info* info = ctxInfoList[i];
            std::string desc = iio_context_info_get_description(info);
            std::string duri = iio_context_info_get_uri(info);

            // If the device is not a plutosdr, don't include it
            bool isPluto = false;
            for (const auto type : deviceWhiteList) {
                if (desc.find(type) != std::string::npos) {
                    isPluto = true;
                    break;
                }
            }
            if (!isPluto) {
                flog::warn("Ignored IIO device: [{}] {}", duri, desc);
                continue;
            }

            // Extract the backend
            std::string backend = "unknown";
            std::smatch backendMatch;
            if (std::regex_search(duri, backendMatch, backendRgx)) {
                backend = backendMatch[0];
            }

            // Extract the model
            std::string model = "Unknown";
            std::smatch modelMatch;
            if (std::regex_search(desc, modelMatch, modelRgx)) {
                model = modelMatch[0];
                int parenthPos = model.find('(');
                if (parenthPos != std::string::npos) {
                    model = model.substr(parenthPos+1);
                }
            }

            // Extract the serial
            std::string serial = "unknown";
            std::smatch serialMatch;
            if (std::regex_search(desc, serialMatch, serialRgx)) {
                serial = serialMatch[0].str().substr(7);
            }

            // Construct the device name
            std::string devName = '(' + backend + ") " + model + " [" + serial + ']';

            // Skip duplicate devices
            if (devices.keyExists(desc) || devices.nameExists(devName) || devices.valueExists(duri)) { continue; }

            // Save device
            devices.define(desc, devName, duri);
        }
        iio_context_info_list_free(ctxInfoList);
        
        // Destroy scan context
        iio_scan_context_destroy(sctx);

#ifdef __ANDROID__
        // On Android, a default IP entry must be made (TODO: This is not ideal since the IP cannot be changed)
        const char* androidURI = "ip:192.168.2.1";
        const char* androidName = "Default (192.168.2.1)";
        devices.define(androidName, androidName, androidURI);
#endif
    }

    void select(const std::string& desc) {
        // If no device is available, give up
        if (devices.empty()) {
            devDesc.clear();
            return;
        }

        // If the device is not available, select the first one
        if (!devices.keyExists(desc)) {
            select(devices.key(0));
        }

        // Update URI
        devDesc = desc;
        uri = devices.value(devices.keyId(desc));

        // TODO: Enumerate capabilities

        // Load defaults
        samplerate = 4000000;
        bandwidth = 0;
        gmId = 0;
        gain = -1.0f;

        // Load device config
        config.readConfig([&](const json& conf) {
            if (conf["devices"][devDesc].contains("samplerate")) {
                samplerate = conf["devices"][devDesc]["samplerate"];
            }
            if (conf["devices"][devDesc].contains("bandwidth")) {
                bandwidth = conf["devices"][devDesc]["bandwidth"];
            }
            if (conf["devices"][devDesc].contains("gainMode")) {
                std::string gm = conf["devices"][devDesc]["gainMode"];
                if (gainModes.keyExists(gm)) {
                    gmId = gainModes.keyId(gm);
                }
                else {
                    gmId = 0;
                }
            }
            if (conf["devices"][devDesc].contains("gain")) {
                gain = conf["devices"][devDesc]["gain"];
                gain = std::clamp<int>(gain, -1.0f, 73.0f);
            }
        });

        // Update samplerate ID
        if (samplerates.keyExists(samplerate)) {
            srId = samplerates.keyId(samplerate);
        }
        else {
            srId = 0;
            samplerate = samplerates.value(srId);
        }

        // Update bandwidth ID
        if (bandwidths.keyExists(bandwidth)) {
            bwId = bandwidths.keyId(bandwidth);
        }
        else {
            bwId = 0;
            bandwidth = bandwidths.value(bwId);
        }
    }

    void onSelect() override {
        core::setInputSampleRate(samplerate);
        flog::info("PlutoSDRSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("PlutoSDRSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // If no device is selected, give up
        if (devDesc.empty() || uri.empty()) { return; }

        // Open context
        ctx = iio_create_context_from_uri(uri.c_str());
        if (ctx == NULL) {
            flog::error("Could not open pluto ({})", uri);
            return;
        }

        // Get phy and device handle
        phy = iio_context_find_device(ctx, "ad9361-phy");
        if (phy == NULL) {
            flog::error("Could not connect to pluto phy");
            iio_context_destroy(ctx);
            return;
        }
        dev = iio_context_find_device(ctx, "cf-ad9361-lpc");
        if (dev == NULL) {
            flog::error("Could not connect to pluto dev");
            iio_context_destroy(ctx);
            return;
        }

        // Get RX channels
        rxChan = iio_device_find_channel(phy, "voltage0", false);
        rxLO = iio_device_find_channel(phy, "altvoltage0", true);

        // Enable RX LO and disable TX
        iio_channel_attr_write_bool(iio_device_find_channel(phy, "altvoltage1", true), "powerdown", true);
        iio_channel_attr_write_bool(rxLO, "powerdown", false);

        // Configure RX channel
        iio_channel_attr_write(rxChan, "rf_port_select", "A_BALANCED");
        iio_channel_attr_write_longlong(rxLO, "frequency", round(freq));                              // Freq
        iio_channel_attr_write_bool(rxChan, "filter_fir_en", true);                                          // Digital filter
        iio_channel_attr_write_longlong(rxChan, "sampling_frequency", round(samplerate));             // Sample rate
        iio_channel_attr_write_double(rxChan, "hardwaregain", gain);                                  // Gain
        iio_channel_attr_write(rxChan, "gain_control_mode", gainModes.value(gmId).c_str());    // Gain mode
        setBandwidth(bandwidth);
        
        // Configure the ADC filters
        ad9361_set_bb_rate(phy, round(samplerate));

        // Start worker thread
        running = true;
        workerThread = std::thread(worker, this);
        flog::info("PlutoSDRSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }

        // Stop worker thread
        running = false;
        stream.stopWriter();
        workerThread.join();
        stream.clearWriteStop();

        // Close device
        if (ctx != NULL) {
            iio_context_destroy(ctx);
            ctx = NULL;
        }

        flog::info("PlutoSDRSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        freq = freq;
        if (running) {
            // Tune device
            iio_channel_attr_write_longlong(rxLO, "frequency", round(freq));
        }
        flog::info("PlutoSDRSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo("##plutosdr_dev_sel", &devId, devices.txt)) {
            select(devices.key(devId));
            core::setInputSampleRate(samplerate);
            config.withConfig([&](json& conf) { conf["device"] = devices.key(devId); });
        }

        if (SmGui::Combo(CONCAT("##_pluto_sr_", name), &srId, samplerates.txt)) {
            samplerate = samplerates.value(srId);
            core::setInputSampleRate(samplerate);
            if (!devDesc.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][devDesc]["samplerate"] = samplerate; });
            }
        }

        // Refresh button
        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_pluto_refr_", name))) {
            refresh();
            select(devDesc);
            core::setInputSampleRate(samplerate);
        }
        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Bandwidth");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_pluto_bw_", name), &bwId, bandwidths.txt)) {
            bandwidth = bandwidths.value(bwId);
            if (running) {
                setBandwidth(bandwidth);
            }
            if (!devDesc.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][devDesc]["bandwidth"] = bandwidth; });
            }
        }

        SmGui::LeftLabel("Gain Mode");
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_pluto_gainmode_select_", name), &gmId, gainModes.txt)) {
            if (running) {
                iio_channel_attr_write(rxChan, "gain_control_mode", gainModes.value(gmId).c_str());
            }
            if (!devDesc.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][devDesc]["gainMode"] = gainModes.key(gmId); });
            }
        }

        SmGui::LeftLabel("Gain");
        if (gmId) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_pluto_gain__", name), &gain, -1.0f, 73.0f, 1.0f, SmGui::FMT_STR_FLOAT_DB_NO_DECIMAL)) {
            if (running) {
                iio_channel_attr_write_double(rxChan, "hardwaregain", gain);
            }
            if (!devDesc.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][devDesc]["gain"] = gain; });
            }
        }
        if (gmId) { SmGui::EndDisabled(); }
    }

    void setBandwidth(int bw) {
        if (bw > 0) {
            iio_channel_attr_write_longlong(rxChan, "rf_bandwidth", bw);
        }
        else {
            iio_channel_attr_write_longlong(rxChan, "rf_bandwidth", std::min<int>(samplerate, 52000000));
        }
    }

    static void worker(void* ctx) {
        PlutoSDRSourceModule* _this = (PlutoSDRSourceModule*)ctx;
        int blockSize = _this->samplerate / 200.0f;

        // Acquire channels
        iio_channel* rx0_i = iio_device_find_channel(_this->dev, "voltage0", 0);
        iio_channel* rx0_q = iio_device_find_channel(_this->dev, "voltage1", 0);
        if (!rx0_i || !rx0_q) {
            flog::error("Failed to acquire RX channels");
            return;
        }

        // Start streaming
        iio_channel_enable(rx0_i);
        iio_channel_enable(rx0_q);

        // Allocate buffer
        iio_buffer* rxbuf = iio_device_create_buffer(_this->dev, blockSize, false);
        if (!rxbuf) {
            flog::error("Could not create RX buffer");
            return;
        }

        // Receive loop
        while (true) {
            // Read samples
            iio_buffer_refill(rxbuf);

            // Get buffer pointer
            int16_t* buf = (int16_t*)iio_buffer_first(rxbuf, rx0_i);
            if (!buf) { break; }

            // Convert samples to CF32
            volk_16i_s32f_convert_32f((float*)_this->stream.writeBuf, buf, 32768.0f, blockSize * 2);

            // Send out the samples
            if (!_this->stream.swap(blockSize)) { break; };
        }

        // Stop streaming
        iio_channel_disable(rx0_i);
        iio_channel_disable(rx0_q);

        // Free buffer
        iio_buffer_destroy(rxbuf);
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    std::thread workerThread;
    iio_context* ctx = NULL;
    iio_device* phy = NULL;
    iio_device* dev = NULL;
    iio_channel* rxLO = NULL;
    iio_channel* rxChan = NULL;
    bool running = false;

    std::string devDesc = "";
    std::string uri = "";

    double freq;
    int samplerate = 4000000;
    int bandwidth = 0;
    float gain = -1;

    int devId = 0;
    int srId = 0;
    int bwId = 0;
    int gmId = 0;

    OptionList<std::string, std::string> devices;
    OptionList<int, double> samplerates;
    OptionList<int, double> bandwidths;
    OptionList<std::string, std::string> gainModes;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "plutosdr_source_config.json");
}

SDRPP_CREATE_INSTANCE_V2(PlutoSDRSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (PlutoSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}