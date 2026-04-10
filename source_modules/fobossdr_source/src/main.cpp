#include <imgui.h>
#include <module.h>
#include <module_manifest.h>
#include <module_config.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <core.h>
#include <utils/optionlist.h>
#include <atomic>
#include <fobos.h>

SDRPP_MOD_INFO{
    /* Name:            */ "fobossdr_source",
    /* Description:     */ "FobosSDR Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "fobossdr_source",
    /* Description:     */ "FobosSDR Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

#define CONCAT(a, b) ((std::string(a) + b).c_str())

// Work around for the fobos API not including
#define FOBOS_LNA_GAIN_MIN  1
#define FOBOS_LNA_GAIN_MAX  3
#define FOBOS_VGA_GAIN_MIN  0
#define FOBOS_VGA_GAIN_MAX  31

class FobosSDRSourceModule : public ModuleManager::Instance, public ISource {
public:
    FobosSDRSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        sampleRate = 50000000.0;

        // Initialize the DDC
        ddc.init(&ddcIn, 50e6, 50e6, 50e6, 0.0);

        // Refresh devices
        refresh();

        // Select device from config
        std::string devSerial;
        config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
        select(devSerial);

        sigpath::sourceManager.registerSource("FobosSDR", static_cast<ISource*>(this));
    }

    ~FobosSDRSourceModule() {
        // Nothing to do
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
    dsp::stream<dsp::complex_t>* getStream() override { return &ddc.out; }

    enum Port {
        PORT_RF,
        PORT_HF1,
        PORT_HF2
    };

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
        devices.clear();
        
        // Get device list
        char serials[1024];
        memset(serials, 0, sizeof(serials));
        int devCount = fobos_rx_list_devices(serials);
        if (devCount < 0) {
            flog::error("Failed to get device list: {}", devCount);
            return;
        }

        // If no device, give up
        if (!devCount) { return; }

        // Generate device entries
        const char* _serials = serials;
        int index = 0;
        while (*_serials) {
            // Read serial until space
            std::string serial = "";
            while (*_serials) {
                // Get a character
                char c = *(_serials++);

                // If it's a space, we're done
                if (c == ' ') { break; }

                // Otherwise, add it to the string
                serial += c;
            }

            // Create entry
            devices.define(serial, serial, index++);
        }
    }

    void select(const std::string& serial) {
        // If there are no devices, give up
        if (devices.empty()) {
            selectedSerial.clear();
            return;
        }

        // If the serial was not found, select the first available serial
        if (!devices.keyExists(serial)) {
            select(devices.key(0));
            return;
        }

        // Get the ID in the list
        int id = devices.keyId(serial);
        selectedDevId = devices[id];

        // Open the device
        fobos_dev_t* dev;
        int err = fobos_rx_open(&dev, selectedDevId);
        if (err) {
            flog::error("Failed to open device: {}", err);
            return;
        }

        // Get a list of supported samplerates
        double srList[128];
        unsigned int srCount;
        err = fobos_rx_get_samplerates(dev, srList, &srCount);
        if (err) {
            flog::error("Failed to get samplerate list: {}", err);
            return;
        }

        // Generate samplerate list
        samplerates.clear();
        for (int i = 0; i < srCount; i++) {
            std::string str = getBandwdithScaled(srList[i]);
            samplerates.define(srList[i], str, srList[i]);
        }

        // Add some custom samplerates
        samplerates.define(5e6, "5.0MHz", 5e6);
        samplerates.define(2.5e6, "2.5MHz", 2.5e6);
        samplerates.define(1.25e6, "1.25MHz", 1.25e6);

        // Define the ports
        ports.clear();
        ports.define("rf", "RF", PORT_RF);
        ports.define("hf1", "HF1", PORT_HF1);
        ports.define("hf2", "HF2", PORT_HF2);

        // Define clock sources
        clockSources.clear();
        clockSources.define("internal", "Internal", 0);
        clockSources.define("external", "External", 1);

        // Close the device
        fobos_rx_close(dev);

        // Save serial number
        selectedSerial = serial;
        devId = id;

        // Load default options
        sampleRate = 50e6;
        srId = samplerates.valueId(sampleRate);
        port = PORT_RF;
        portId = ports.valueId(port);
        clkSrcId = clockSources.nameId("Internal");
        lnaGain = 0;
        vgaGain = 0;

        // Load config
        config.readConfig([&](const json& conf) {
            if (conf["devices"][selectedSerial].contains("samplerate")) {
                int desiredSr = conf["devices"][selectedSerial]["samplerate"];
                if (samplerates.keyExists(desiredSr)) {
                    srId = samplerates.keyId(desiredSr);
                    sampleRate = samplerates[srId];
                }
            }
            if (conf["devices"][selectedSerial].contains("port")) {
                std::string desiredPort = conf["devices"][selectedSerial]["port"];
                if (ports.keyExists(desiredPort)) {
                    portId = ports.keyId(desiredPort);
                    port = ports[portId];
                }
            }
            if (conf["devices"][selectedSerial].contains("clkSrc")) {
                std::string desiredClkSrc = conf["devices"][selectedSerial]["clkSrc"];
                if (clockSources.keyExists(desiredClkSrc)) {
                    clkSrcId = clockSources.keyId(desiredClkSrc);
                }
            }
            if (conf["devices"][selectedSerial].contains("lnaGain")) {
                lnaGain = std::clamp<int>(conf["devices"][selectedSerial]["lnaGain"], FOBOS_LNA_GAIN_MIN, FOBOS_LNA_GAIN_MAX);
            }
            if (conf["devices"][selectedSerial].contains("vgaGain")) {
                vgaGain = std::clamp<int>(conf["devices"][selectedSerial]["vgaGain"], FOBOS_VGA_GAIN_MIN, FOBOS_VGA_GAIN_MAX);
            }
        });

        // Update the samplerate
        core::setInputSampleRate(sampleRate);
    }

    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("FobosSDRSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("FobosSDRSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // Open the device
        int err = fobos_rx_open(&openDev, selectedDevId);
        if (err) {
            flog::error("Failed to open device: {}", err);
            return;
        }

        // Get the selected port
        port = ports[portId];

        // Configure the device
        double actualSr, actualFreq;
        fobos_rx_set_samplerate(openDev, (sampleRate >= 50e6) ? sampleRate : 50e6, &actualSr);
        fobos_rx_set_frequency(openDev, freq, &actualFreq);
        fobos_rx_set_direct_sampling(openDev, port != PORT_RF);
        fobos_rx_set_clk_source(openDev, clockSources[clkSrcId]);
        fobos_rx_set_lna_gain(openDev, lnaGain);
        fobos_rx_set_vga_gain(openDev, vgaGain);

        // Configure the DDC
        if (port == PORT_RF && sampleRate >= 50e6) {
            // Set the frequency
            fobos_rx_set_frequency(openDev, freq, &actualFreq);
        }
        else if (port == PORT_RF) {
            // Set the frequency
            fobos_rx_set_frequency(openDev, freq, &actualFreq);

            // Configure and start the DDC for decimation only
            ddc.setInSamplerate(actualSr);
            ddc.setOutSamplerate(sampleRate, sampleRate);
            ddc.setOffset(0.0);
            ddc.start();
        }
        else {
            // Configure and start the DDC
            ddc.setInSamplerate(actualSr);
            ddc.setOutSamplerate(sampleRate, sampleRate);
            ddc.setOffset(freq);
            ddc.start();
        }

        // Compute buffer size (Lower than usual, but it's a workaround for their API having broken streaming)
        bufferSize = sampleRate / 400.0;

        // Start streaming
        err = fobos_rx_start_sync(openDev, bufferSize);
        if (err) {
            flog::error("Failed to start stream: {}", err);
            return;
        }

        // Start worker
        run = true;
        workerThread = std::thread(&FobosSDRSourceModule::worker, this);
        
        running = true;
        flog::info("FobosSDRSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;

        // Stop worker
        run = false;
        if (port == PORT_RF && sampleRate >= 50e6) {
            ddc.out.stopWriter();
            if (workerThread.joinable()) { workerThread.join(); }
            ddc.out.clearWriteStop();
        }
        else {
            ddcIn.stopWriter();
            if (workerThread.joinable()) { workerThread.join(); }
            ddcIn.clearWriteStop();
        }

        // Stop streaming
        fobos_rx_stop_sync(openDev);

        // Stop the DDC
        ddc.stop();

        // Close the device
        fobos_rx_close(openDev);

        flog::info("FobosSDRSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            if (port == PORT_RF) {
                double actual; // Dummy, don't care
                fobos_rx_set_frequency(openDev, freq, &actual);
            }
            else {
                ddc.setOffset(freq);
            }
        }
        freq = freq;
        flog::info("FobosSDRSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_fobossdr_dev_sel_", name), &devId, devices.txt)) {
            select(devices.key(devId));
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = selectedSerial; });
        }

        if (SmGui::Combo(CONCAT("##_fobossdr_sr_sel_", name), &srId, samplerates.txt)) {
            sampleRate = samplerates.value(srId);
            core::setInputSampleRate(sampleRate);
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["samplerate"] = samplerates.key(srId); });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_fobossdr_refr_", name))) {
            refresh();
            select(selectedSerial);
            core::setInputSampleRate(sampleRate);
        }

        SmGui::LeftLabel("Antenna Port");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_fobossdr_port_", name), &portId, ports.txt)) {
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["port"] = ports.key(portId); });
            }
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("Clock Source");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_fobossdr_clk_", name), &clkSrcId, clockSources.txt)) {
            if (running) {
                fobos_rx_set_clk_source(openDev, clockSources[clkSrcId]);
            }
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["clkSrc"] = clockSources.key(clkSrcId); });
            }
        }

        if (port == PORT_RF) {
            SmGui::LeftLabel("LNA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_fobossdr_lna_gain_", name), &lnaGain, FOBOS_LNA_GAIN_MIN, FOBOS_LNA_GAIN_MAX)) {
                if (running) {
                    fobos_rx_set_lna_gain(openDev, lnaGain);
                }
                if (!selectedSerial.empty()) {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["lnaGain"] = lnaGain; });
                }
            }

            SmGui::LeftLabel("VGA Gain");
            SmGui::FillWidth();
            if (SmGui::SliderInt(CONCAT("##_fobossdr_vga_gain_", name), &vgaGain, FOBOS_VGA_GAIN_MIN, FOBOS_VGA_GAIN_MAX)) {
                if (running) {
                    fobos_rx_set_vga_gain(openDev, vgaGain);
                }
                if (!selectedSerial.empty()) {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["vgaGain"] = vgaGain; });
                }
            }
        }
    }

    void worker() {
        // Select different processing depending on the mode
        if (port == PORT_RF && sampleRate >= 50e6) {
            while (run) {
                // Read samples
                unsigned int sampCount = 0;
                int err = fobos_rx_read_sync(openDev, (float*)ddc.out.writeBuf, &sampCount);
                if (err) { break; }
                
                // Send out samples to the core
                if (!ddc.out.swap(sampCount)) { break; }
            }
        }
        else if (port == PORT_RF) {
            while (run) {
                // Read samples
                unsigned int sampCount = 0;
                int err = fobos_rx_read_sync(openDev, (float*)ddcIn.writeBuf, &sampCount);
                if (err) { break; }
                
                // Send samples to the DDC
                if (!ddcIn.swap(sampCount)) { break; }
            }
        }
        else if (port == PORT_HF1) {
            while (run) {
                // Read samples
                unsigned int sampCount = 0;
                int err = fobos_rx_read_sync(openDev, (float*)ddcIn.writeBuf, &sampCount);
                if (err) { break; }

                // Null out the HF2 samples
                for (int i = 0; i < sampCount; i++) {
                    ddcIn.writeBuf[i].im = 0.0f;
                }
                
                // Send samples to the DDC
                if (!ddcIn.swap(sampCount)) { break; }
            }
        }
        else if (port == PORT_HF2) {
            while (run) {
                // Read samples
                unsigned int sampCount = 0;
                int err = fobos_rx_read_sync(openDev, (float*)ddcIn.writeBuf, &sampCount);
                if (err) { break; }

                // Null out the HF2 samples
                for (int i = 0; i < sampCount; i++) {
                    ddcIn.writeBuf[i].re = 0.0f;
                }
                
                // Send samples to the DDC
                if (!ddcIn.swap(sampCount)) { break; }
            }
        }
    }

    std::string name;
    bool enabled = true;
    double sampleRate;
    bool running = false;
    double freq;

    OptionList<std::string, int> devices;
    OptionList<int, double> samplerates;
    OptionList<std::string, Port> ports;
    OptionList<std::string, int> clockSources;
    int devId = 0;
    int srId = 0;
    int portId = 0;
    int clkSrcId = 0;
    Port port;
    int lnaGain = 0;
    int vgaGain = 0;
    std::string selectedSerial;
    int selectedDevId;

    fobos_dev_t* openDev;

    int bufferSize;
    std::thread workerThread;
    std::atomic<bool> run = false;

    dsp::stream<dsp::complex_t> ddcIn;
    dsp::channel::RxVFO ddc;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "fobossdr_config.json");
}

SDRPP_CREATE_INSTANCE_V2(FobosSDRSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (FobosSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}