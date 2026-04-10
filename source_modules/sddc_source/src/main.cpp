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
#include <sddc.h>

SDRPP_MOD_INFO{
    /* Name:            */ "sddc_source",
    /* Description:     */ "SDDC Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ -1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "sddc_source",
    /* Description:     */ "SDDC Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 2, 0,
    /* Max instances    */ -1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

#define CONCAT(a, b) ((std::string(a) + b).c_str())


class SDDCSourceModule : public ModuleManager::Instance, public ISource {
public:
    SDDCSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        // Set firmware image path for debugging
        sddc_set_firmware_path("C:/Users/ryzerth/Downloads/SDDC_FX3 (1).img");

        sampleRate = 128e6;

        // Initialize the DDC
        ddc.init(&ddcIn, 50e6, 50e6, 50e6, 0.0);

        // Refresh devices
        refresh();

        // Select device from config
        std::string devSerial;
        config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
        select(devSerial);

        sigpath::sourceManager.registerSource("SDDC", static_cast<ISource*>(this));
    }

    ~SDDCSourceModule() {
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
        
        // // Get device list
        // sddc_devinfo_t* devList;
        // int count = sddc_get_device_list(&devList);
        // if (count < 0) {
        //     flog::error("Failed to list SDDC devices: {}", count);
        //     return;
        // }

        // // Add every device found
        // for (int i = 0; i < count; i++) {
        //     // Create device name
        //     std::string name = sddc_model_to_string(devList[i].model);
        //     name += '[';
        //     name += devList[i].serial;
        //     name += ']';

        //     // Add an entry to the device list
        //     devices.define(devList[i].serial, name, devList[i].serial);
        // }

        devices.define("0009072C00C40C32", "TESTING", "0009072C00C40C32");

        // // Free the device list
        // sddc_free_device_list(devList);
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

        // Open the device
        sddc_dev_t* dev;
        int err = sddc_open(serial.c_str(), &dev);
        if (err) {
            flog::error("Failed to open device: {}", err);
            return;
        }

        // Generate samplerate list
        samplerates.clear();
        samplerates.define(4e6, "4 MHz", 4e6);
        samplerates.define(8e6, "8 MHz", 8e6);
        samplerates.define(16e6, "16 MHz", 16e6);
        samplerates.define(32e6, "32 MHz", 32e6);
        samplerates.define(64e6, "64 MHz", 64e6);

        // // Define the ports
        // ports.clear();
        // ports.define("hf", "HF", PORT_RF);
        // ports.define("vhf", "VHF", PORT_HF1);

        // Close the device
        sddc_close(dev);

        // Save serial number
        selectedSerial = serial;
        devId = id;

        // Load default options
        sampleRate = 64e6;
        srId = samplerates.valueId(sampleRate);
        // port = PORT_RF;
        // portId = ports.valueId(port);
        // lnaGain = 0;
        // vgaGain = 0;

        // Load config
        config.readConfig([&](const json& conf) {
            if (conf["devices"][selectedSerial].contains("samplerate")) {
                int desiredSr = conf["devices"][selectedSerial]["samplerate"];
                if (samplerates.keyExists(desiredSr)) {
                    srId = samplerates.keyId(desiredSr);
                    sampleRate = samplerates[srId];
                }
            }
            // if (conf["devices"][selectedSerial].contains("port")) {
            //     std::string desiredPort = conf["devices"][selectedSerial]["port"];
            //     if (ports.keyExists(desiredPort)) {
            //         portId = ports.keyId(desiredPort);
            //         port = ports[portId];
            //     }
            // }
            // if (conf["devices"][selectedSerial].contains("lnaGain")) {
            //     lnaGain = std::clamp<int>(conf["devices"][selectedSerial]["lnaGain"], FOBOS_LNA_GAIN_MIN, FOBOS_LNA_GAIN_MAX);
            // }
            // if (conf["devices"][selectedSerial].contains("vgaGain")) {
            //     vgaGain = std::clamp<int>(conf["devices"][selectedSerial]["vgaGain"], FOBOS_VGA_GAIN_MIN, FOBOS_VGA_GAIN_MAX);
            // }
        });

        // Update the samplerate
        core::setInputSampleRate(sampleRate);
    }

    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        flog::info("SDDCSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("SDDCSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // Open the device
        sddc_error_t err = sddc_open(selectedSerial.c_str(), &openDev);
        if (err) {
            flog::error("Failed to open device: {}", (int)err);
            return;
        }

        // // Get the selected port
        // port = ports[portId];

        // Configure the device
        sddc_set_samplerate(openDev, sampleRate * 2);

        // // Configure the DDC
        // if (port == PORT_RF && sampleRate >= 50e6) {
        //     // Set the frequency
        //     fobos_rx_set_frequency(openDev, freq, &actualFreq);
        // }
        // else if (port == PORT_RF) {
        //     // Set the frequency
        //     fobos_rx_set_frequency(openDev, freq, &actualFreq);

        //     // Configure and start the DDC for decimation only
        //     ddc.setInSamplerate(actualSr);
        //     ddc.setOutSamplerate(sampleRate, sampleRate);
        //     ddc.setOffset(0.0);
        //     ddc.start();
        // }
        // else {
            // Configure and start the DDC
            ddc.setInSamplerate(sampleRate * 2);
            ddc.setOutSamplerate(sampleRate, sampleRate);
            ddc.setOffset(freq);
            ddc.start();
        // }

        // Compute buffer size (Lower than usual, but it's a workaround for their API having broken streaming)
        bufferSize = sampleRate / 100.0;

        // Start streaming
        err = sddc_start(openDev);
        if (err) {
            flog::error("Failed to start stream: {}", (int)err);
            return;
        }

        // Start worker
        run = true;
        workerThread = std::thread(&SDDCSourceModule::worker, this);
        
        running = true;
        flog::info("SDDCSourceModule '{0}': Start!", name);
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
        sddc_stop(openDev);

        // Stop the DDC
        ddc.stop();

        // Close the device
        sddc_close(openDev);

        flog::info("SDDCSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            // if (port == PORT_RF) {
            //     double actual; // Dummy, don't care
            //     //fobos_rx_set_frequency(openDev, freq, &actual);
            // }
            // else {
                ddc.setOffset(freq);
            // }
        }
        freq = freq;
        flog::info("SDDCSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_sddc_dev_sel_", name), &devId, devices.txt)) {
            select(devices.key(devId));
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = selectedSerial; });
        }

        if (SmGui::Combo(CONCAT("##_sddc_sr_sel_", name), &srId, samplerates.txt)) {
            sampleRate = samplerates.value(srId);
            core::setInputSampleRate(sampleRate);
            if (!selectedSerial.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSerial]["samplerate"] = samplerates.key(srId); });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_sddc_refr_", name))) {
            refresh();
            select(selectedSerial);
            core::setInputSampleRate(sampleRate);
        }

        // SmGui::LeftLabel("Antenna Port");
        // SmGui::FillWidth();
        // if (SmGui::Combo(CONCAT("##_sddc_port_", name), &portId, ports.txt)) {
        //     if (!selectedSerial.empty()) {
        //         config.acquire();
        //         config.conf["devices"][selectedSerial]["port"] = ports.key(portId);
        //         config.release(true);
        //     }
        // }

        if (running) { SmGui::EndDisabled(); }

        // if (port == PORT_RF) {
        //     SmGui::LeftLabel("LNA Gain");
        //     SmGui::FillWidth();
        //     if (SmGui::SliderInt(CONCAT("##_sddc_lna_gain_", name), &lnaGain, FOBOS_LNA_GAIN_MIN, FOBOS_LNA_GAIN_MAX)) {
        //         if (running) {
        //             fobos_rx_set_lna_gain(openDev, lnaGain);
        //         }
        //         if (!selectedSerial.empty()) {
        //             config.acquire();
        //             config.conf["devices"][selectedSerial]["lnaGain"] = lnaGain;
        //             config.release(true);
        //         }
        //     }

        //     SmGui::LeftLabel("VGA Gain");
        //     SmGui::FillWidth();
        //     if (SmGui::SliderInt(CONCAT("##_sddc_vga_gain_", name), &vgaGain, FOBOS_VGA_GAIN_MIN, FOBOS_VGA_GAIN_MAX)) {
        //         if (running) {
        //             fobos_rx_set_vga_gain(openDev, vgaGain);
        //         }
        //         if (!selectedSerial.empty()) {
        //             config.acquire();
        //             config.conf["devices"][selectedSerial]["vgaGain"] = vgaGain;
        //             config.release(true);
        //         }
        //     }
        // }
    }

    void worker() {
        // // Select different processing depending on the mode
        // if (port == PORT_RF && sampleRate >= 50e6) {
        //     while (run) {
        //         // Read samples
        //         unsigned int sampCount = 0;
        //         int err = fobos_rx_read_sync(openDev, (float*)ddc.out.writeBuf, &sampCount);
        //         if (err) { break; }
                
        //         // Send out samples to the core
        //         if (!ddc.out.swap(sampCount)) { break; }
        //     }
        // }
        // else if (port == PORT_RF) {
        //     while (run) {
        //         // Read samples
        //         unsigned int sampCount = 0;
        //         int err = fobos_rx_read_sync(openDev, (float*)ddcIn.writeBuf, &sampCount);
        //         if (err) { break; }
                
        //         // Send samples to the DDC
        //         if (!ddcIn.swap(sampCount)) { break; }
        //     }
        // }
        // else if (port == PORT_HF1) {
        //     while (run) {
        //         // Read samples
        //         unsigned int sampCount = 0;
        //         int err = fobos_rx_read_sync(openDev, (float*)ddcIn.writeBuf, &sampCount);
        //         if (err) { break; }

        //         // Null out the HF2 samples
        //         for (int i = 0; i < sampCount; i++) {
        //             ddcIn.writeBuf[i].im = 0.0f;
        //         }
                
        //         // Send samples to the DDC
        //         if (!ddcIn.swap(sampCount)) { break; }
        //     }
        // }
        // else if (port == PORT_HF2) {
            // Allocate the sample buffer
            int16_t* buffer = dsp::buffer::alloc<int16_t>(bufferSize);
            float* fbuffer = dsp::buffer::alloc<float>(bufferSize);
            float* nullBuffer = dsp::buffer::alloc<float>(bufferSize);

            // Clear the null buffer
            dsp::buffer::clear(nullBuffer, bufferSize);

            while (run) {
                // Read samples
                int err = sddc_rx(openDev, buffer, bufferSize);
                if (err) { break; }

                // Convert the samples to float
                volk_16i_s32f_convert_32f(fbuffer, buffer, 32768.0f, bufferSize);

                // Interleave into a complex value
                volk_32f_x2_interleave_32fc((lv_32fc_t*)ddcIn.writeBuf, fbuffer, nullBuffer, bufferSize);
                
                // Send samples to the DDC
                if (!ddcIn.swap(bufferSize)) { break; }
            }

            // Free the buffer
            dsp::buffer::free(buffer);
        // }
    }

    std::string name;
    bool enabled = true;
    double sampleRate;
    bool running = false;
    double freq;

    OptionList<std::string, std::string> devices;
    OptionList<int, int> samplerates;
    OptionList<std::string, Port> ports;
    int devId = 0;
    int srId = 0;
    int portId = 0;
    Port port;
    int lnaGain = 0;
    int vgaGain = 0;
    std::string selectedSerial;

    sddc_dev_t* openDev;

    int bufferSize;
    std::thread workerThread;
    std::atomic<bool> run = false;

    dsp::stream<dsp::complex_t> ddcIn;
    dsp::channel::RxVFO ddc;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "sddc_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SDDCSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SDDCSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}