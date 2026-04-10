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
#include <uhd.h>
#include <uhd/device.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <utils/optionlist.h>
#include <utils/freq_formatting.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "usrp_source",
    /* Description:     */ "USRP source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "usrp_source",
    /* Description:     */ "USRP source module for SDR++",
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

class USRPSourceModule : public ModuleManager::Instance, public ISource {
public:
    USRPSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        sampleRate = 8000000.0;

        sigpath::sourceManager.registerSource("USRP", static_cast<ISource*>(this));
    }

    ~USRPSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("USRP");
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
        devices.clear();
        uhd::device_addr_t hint;
        uhd::device_addrs_t devList = uhd::device::find(hint);

        char buf[1024];
        for (const auto& devAddr : devList) {
            std::string serial = devAddr["serial"];
            std::string model = devAddr.has_key("product") ? devAddr["product"] : devAddr["type"];
            sprintf(buf, "USRP %s [%s]", model.c_str(), serial.c_str());

            // Work-around for UHD sometimes reporting the same device twice
            if (devices.keyExists(serial)) { continue; }

            devices.define(serial, buf, devAddr);
        }
    }

    void select(std::string serial) {
        // If no device, give up
        if (!devices.size()) {
            selectedSer.clear();
            return;
        }

        // If the wanted serial is not available, select first
        if (!devices.keyExists(serial)) {
            select(devices.key(0));
            return;
        }

        // Update selection
        selectedSer = serial;
        devId = devices.keyId(serial);

        // Make device
        auto dev = uhd::usrp::multi_usrp::make(devices[devId]);

        // List subdevices
        char buf[1024];
        channels.clear();
        auto subdevs = dev->get_rx_subdev_spec();
        for (int i = 0; i < subdevs.size(); i++) {
            std::string slot = subdevs[i].db_name + ',' + subdevs[i].sd_name;
            sprintf(buf, "%s [%s]", dev->get_rx_subdev_name(i).c_str(), slot.c_str());
            channels.define(buf, buf, buf);
        }

        // Select channel
        std::string chan = "";
        config.readConfig([&](const json& conf) {
            if (conf["devices"][selectedSer].contains("channel")) {
                chan = conf["devices"][selectedSer]["channel"];
            }
        });
        selectChannel(dev, chan);
    }

    void selectChannel(uhd::usrp::multi_usrp::sptr dev, std::string chan) {
        // If wanted channel is not available, select first
        if (!channels.keyExists(chan)) {
            selectChannel(dev, channels.key(0));
            return;
        }

        // Update selection
        selectedChan = chan;
        chanId = channels.keyId(chan);

        // List samplerates
        samplerates.clear();
        auto srList = dev->get_rx_rates(chanId);
        for (const auto& l : srList) {
            double step = (l.step() == 0.0) ? 100e3 : l.step();
            for (double f = l.start(); f <= l.stop(); f += step) {
                samplerates.define(f, utils::formatFreq(f), f);
            }
        }

        // List antennas
        antennas.clear();
        auto ants = dev->get_rx_antennas(chanId);
        for (const auto& a : ants) {
            antennas.define(a,a,a);
        }

        // Get gain range
        gainRange = dev->get_rx_gain_range(chanId)[0];

        // Get bandwidth ranges
        bandwidths.clear();
        bandwidths.define(0, "Auto", 0);
        uhd::meta_range_t bwRange = dev->get_rx_bandwidth_range(chanId);
        for (const auto& r : bwRange) {
            double step = (r.step() == 0.0) ? 100e3 : r.step();
            for (double i = r.start(); i <= r.stop(); i += step) {
                bandwidths.define((int)i, utils::formatFreq(i), i);
            }
        }

        // Get clock sources
        clockSources.clear();
        auto cSources = dev->get_clock_sources(0);
        for (const auto& s : cSources) {
            std::string name = s;
            name[0] = std::toupper(name[0]);
            clockSources.define(s, name, s);
        }
        
        // Load settings
        srId = 0;
        antId = 0;
        bwId = 0;
        csId = 0;
        gain = gainRange.start();
        config.readConfig([&](const json& conf) {
            if (conf["devices"][selectedSer].contains("channels") && conf["devices"][selectedSer]["channels"].contains(selectedChan)) {
                auto cconf = conf["devices"][selectedSer]["channels"][selectedChan];
                if (cconf.contains("samplerate")) {
                    int sr = cconf["samplerate"];
                    if (samplerates.keyExists(sr)) { srId = samplerates.keyId(sr); }
                }
                if (cconf.contains("antenna")) {
                    std::string ant = cconf["antenna"];
                    if (antennas.keyExists(ant)) { antId = antennas.keyId(ant); }
                }
                if (cconf.contains("bandwidth")) {
                    int bw = cconf["bandwidth"];
                    if (bandwidths.keyExists(bw)) { bwId = bandwidths.keyId(bw); }
                }
                if (cconf.contains("clock")) {
                    std::string clk = cconf["clock"];
                    if (clockSources.keyExists(clk)) { csId = clockSources.keyId(clk); }
                }
                if (cconf.contains("gain")) {
                    gain = cconf["gain"];
                    gain = std::clamp<float>(gain, gainRange.start(), gainRange.stop());
                }
            }
        });

        // Apply samplerate
        sampleRate = samplerates.key(srId);
    }

    void setBandwidth(double bw) {
        if (bw > 0.0) {
            dev->set_rx_bandwidth(bw, chanId);
            return;
        }

        // If on auto, select the best depending on the samplerate
        // Note: Starts at 1 because 0 is the 'Auto' entry.
        int bestId;
        for (int i = 1; i < bandwidths.size(); i++) {
            bestId = i;
            if (bandwidths[i] >= sampleRate) { break; }
        }

        // Set it
        dev->set_rx_bandwidth(bandwidths[bestId], chanId);
    }

private:
    void onSelect() override {
        if (firstSelect) {
            firstSelect = false;

            // List devices
            refresh();

            // Select device
            config.readConfig([&](const json& conf) { selectedSer = conf["device"]; });
            select(selectedSer);
        }

        core::setInputSampleRate(sampleRate);
        flog::info("USRPSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("USRPSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedSer.empty()) { return; }

        dev = uhd::usrp::multi_usrp::make(devices[devId]);

        dev->set_rx_rate(sampleRate, chanId);
        dev->set_rx_antenna(antennas.key(antId), chanId);
        dev->set_rx_gain(gain, chanId);
        dev->set_rx_freq(freq, chanId);
        dev->set_clock_source(clockSources.key(csId));
        setBandwidth(bandwidths[bwId]);
        
        uhd::stream_args_t sargs;
        sargs.channels.clear();
        sargs.channels.push_back(chanId);
        sargs.cpu_format = "fc32";
        sargs.otw_format = "sc16";
        streamer = dev->get_rx_stream(sargs);
        streamer->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
        
        stream.clearWriteStop();
        workerThread = std::thread(&USRPSourceModule::worker, this);

        running = true;
        flog::info("USRPSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        
        stream.stopWriter();
        streamer->issue_stream_cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
        if (workerThread.joinable()) { workerThread.join(); }
        stream.clearWriteStop();
        
        streamer.reset();
        dev.reset();

        flog::info("USRPSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            dev->set_rx_freq(freq, chanId);
        }
        freq = freq;
        flog::info("USRPSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_usrp_dev_sel_", name), &devId, devices.txt)) {
            select(devices.key(devId));
            core::setInputSampleRate(sampleRate);
            if (!selectedSer.empty()) {
                config.withConfig([&](json& conf) { conf["device"] = devices.key(devId); });
            }
        }

        if (SmGui::Combo(CONCAT("##_usrp_sr_sel_", name), &srId, samplerates.txt)) {
            sampleRate = samplerates.key(srId);
            core::setInputSampleRate(sampleRate);
            if (!selectedSer.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSer]["channels"][selectedChan]["samplerate"] = samplerates.key(srId); });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_usrp_refr_", name))) {
            refresh();
            select(selectedSer);
            core::setInputSampleRate(sampleRate);
        }

        if (channels.size() > 1) {
            SmGui::LeftLabel("Channel");
            SmGui::FillWidth();
            SmGui::ForceSync();
            if (SmGui::Combo(CONCAT("##_usrp_ch_sel_", name), &chanId, channels.txt)) {
                if (!selectedSer.empty()) {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSer]["channel"] = channels.key(chanId); });
                }
                select(devices.key(devId));
            }
        }

        if (running) { SmGui::EndDisabled(); }

        if (antennas.size() > 1) {
            SmGui::LeftLabel("Antenna");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##_usrp_ant_sel_", name), &antId, antennas.txt)) {
                if (running) {
                    dev->set_rx_antenna(antennas.key(antId), chanId);
                }
                if (!selectedSer.empty() && !selectedChan.empty()) {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSer]["channels"][selectedChan]["antenna"] = antennas.key(antId); });
                }
            }
        }

        if (bandwidths.size() > 2) {
            SmGui::LeftLabel("Bandwidth");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##_usrp_bw_sel_", name), &bwId, bandwidths.txt)) {
                if (running) {
                    setBandwidth(bandwidths[bwId]);
                }
                if (!selectedSer.empty() && !selectedChan.empty()) {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSer]["channels"][selectedChan]["bandwidth"] = bandwidths.key(bwId); });
                }
            }
        }

        if (clockSources.size() > 1) {
            SmGui::LeftLabel("Clock");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT("##_usrp_clk_sel_", name), &csId, clockSources.txt)) {
                if (running) {
                    dev->set_clock_source(clockSources.key(csId));
                }
                if (!selectedSer.empty()) {
                    config.withConfig([&](json& conf) { conf["devices"][selectedSer]["channels"][selectedChan]["clock"] = clockSources.key(csId); });
                }
            }
        }

        SmGui::LeftLabel("Gain");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_usrp_gain_", name), &gain, gainRange.start(), gainRange.stop(), gainRange.step(), SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            if (running) {
                dev->set_rx_gain(gain, chanId);
            }
            if (!selectedSer.empty() && !selectedChan.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedSer]["channels"][selectedChan]["gain"] = gain; });
            }
        }
    }

    uint32_t floor2(uint32_t val) {
        val |= val >> 1;
        val |= val >> 2;
        val |= val >> 4;
        val |= val >> 8;
        val |= val >> 16;
        return val - (val >> 1);
    }

    void worker() {
        // TODO: Select a better buffer size that will avoid bad timing
        int bufferSize = sampleRate / 200;
        try {
            while (true) {
                uhd::rx_metadata_t meta;
                void* ptr[] = { stream.writeBuf };
                uhd::rx_streamer::buffs_type buffers(ptr, 1);
                int len = streamer->recv(stream.writeBuf, bufferSize, meta, 1.0);
                if (len < 0) { break; }
                if (len != bufferSize) {
                    printf("%d\n", len);
                }
                if (len) {
                    if (!stream.swap(len)) { break; }
                }
            }
        }
        catch (const std::exception& e) {
            flog::error("Failed to receive samples: {}", e.what());
        }
    }

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    double freq;
    int devId = 0;
    int chanId = 0;
    int srId = 0;
    int antId = 0;
    int bwId = 0;
    int csId = 0;
    std::string selectedSer = "";
    std::string selectedChan = "";
    float gain = 0.0f;

    OptionList<std::string, uhd::device_addr_t> devices;
    OptionList<std::string, std::string> channels;
    OptionList<int, double> samplerates;
    OptionList<std::string, std::string> antennas;
    OptionList<int, double> bandwidths;
    OptionList<std::string, std::string> clockSources;
    uhd::range_t gainRange;

    uhd::usrp::multi_usrp::sptr dev;
    uhd::rx_streamer::sptr streamer;

    bool firstSelect = true;

    std::thread workerThread;

};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "usrp_config.json");
}

SDRPP_CREATE_INSTANCE_V2(USRPSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (USRPSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}