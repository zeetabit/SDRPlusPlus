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
#include <gui/smgui.h>
#include <utils/optionlist.h>
#include <codecvt>
#include <locale>
#include <aaroniartsaapi.h>

#ifndef _WIN32
#include <unistd.h>
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "spectran_source",
    /* Description:     */ "Spectran source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "spectran_source",
    /* Description:     */ "Spectran source module for SDR++",
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

class SpectranSourceModule : public ModuleManager::Instance, public ISource {
public:
    SpectranSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

        AARTSAAPI_Result res;
        if ((res = AARTSAAPI_Init(AARTSAAPI_MEMORY_MEDIUM)) != AARTSAAPI_OK) {
            flog::error("Failed to initialize the RTSAAPI: {}", (uint32_t)res);
            return;
        }
        if ((res = AARTSAAPI_Open(&api)) != AARTSAAPI_OK) {
            flog::error("Failed to open the RTSAAPI: {}", (uint32_t)res);
            return;
        }

        compList.define("Auto", L"auto");
        compList.define("Raw", L"raw");
        compList.define("Compressed", L"compressed");

        agcModeList.define("Off", L"manual");
        agcModeList.define("Peak", L"peak");
        agcModeList.define("Power", L"power");

        clockList.define("Consumer", L"Consumer");
        clockList.define("Internal", L"Oscillator");
        clockList.define("GPSDO", L"GPS");
        clockList.define("PPS", L"PPS");
        clockList.define("10MHz Ref", L"10MHz");

        samplerate.effective = 1000000.0;

        refresh();

        // Select device from config
        std::string devSerial;
        config.readConfig([&](const json& conf) { devSerial = conf["device"]; });
        // TODO: Select
        selectSerial("");

        sigpath::sourceManager.registerSource("Spectran", static_cast<ISource*>(this));
    }

    ~SpectranSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Spectran");
        AARTSAAPI_Close(&api);
        AARTSAAPI_Shutdown();
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
        // Clear device list
        devList.clear();

        // Rescan
        if (AARTSAAPI_RescanDevices(&api, 2000) != AARTSAAPI_OK) {
            return;
        }

        // List spectran V6s
        std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
        AARTSAAPI_DeviceInfo dinfo = { sizeof(AARTSAAPI_DeviceInfo) };
        for (int i = 0; AARTSAAPI_EnumDevice(&api, L"spectranv6", i, &dinfo) == AARTSAAPI_OK; i++) {
            if (!dinfo.ready) { continue; }
            std::string serial = conv.to_bytes(dinfo.serialNumber);
            devList.define(serial, serial, dinfo.serialNumber);
        }
    }

    void selectSerial(std::string serial) {
        // If no serial is available, deselect
        if (!devList.size()) {
            selectedSerial.clear();
            return;
        }

        // If serial doesn't exist, select first
        if (!devList.keyExists(serial)) {
            selectSerial(devList.key(0));
            return;
        }

        // Set ID
        devId = devList.keyId(serial);

        // Open device
        if (AARTSAAPI_OpenDevice(&api, &dev, L"spectranv6/raw", devList[devId].c_str()) != AARTSAAPI_OK) {
            flog::error("Failed to open device");
            selectedSerial.clear();
            return;
        }

        // Get config root
        AARTSAAPI_Config config;
        AARTSAAPI_ConfigRoot(&dev, &croot);

        // Get valid clock rates
        AARTSAAPI_ConfigInfo clockInfo;
        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/receiverclock");
        AARTSAAPI_ConfigGetInfo(&dev, &config, &clockInfo);

        // Enumerate valid samplerates
        flog::warn("{0}", clockInfo.disabledOptions);
        std::vector<SRCombo> srs;
        for (int i = 0; i < 4; i++) {
            if ((clockInfo.disabledOptions >> i) & 1) { continue; }
            int dCount = sizeof(decimations)/sizeof(decimations[0]);
            for (int j = 0; j < dCount; j++) {
                SRCombo combo;
                combo.baseId = i;
                combo.decimId = j;
                combo.effective = clockRates[combo.baseId] / (double)decimations[combo.decimId];
                srs.push_back(combo);
            }
        }

        // Sort list in ascending order
        std::sort(srs.begin(), srs.end(), [](SRCombo a, SRCombo b) {
            if (a.effective != b.effective) {
                return a.effective < b.effective; 
            }
            else {
                return a.baseId < b.baseId;
            }
        });

        // Create SR list
        char buf[128];
        sampleRateList.clear();
        for (const auto& sr : srs) {
            sprintf(buf, "%s (%s / %d)", getBandwdithScaled(sr.effective).c_str(), getBandwdithScaled(clockRates[sr.baseId]).c_str(), decimations[sr.decimId]);
            sampleRateList.define(buf, sr);
        }

        // Default config
        srId = 0;
        compId = 0;
        agcModeId = 0;
        refLevel = -20.0;
        amp = false;
        preAmp = false;

        // TODO: Load config

        // Set samplerate
        samplerate = sampleRateList.value(srId);

        // Close device
        AARTSAAPI_CloseDevice(&api, &dev);

        selectedSerial = serial;
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
        core::setInputSampleRate(samplerate.effective);
        flog::info("SpectranSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("SpectranSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (selectedSerial.empty()) { return; }

        if (AARTSAAPI_OpenDevice(&api, &dev, L"spectranv6/raw", devList[devId].c_str()) != AARTSAAPI_OK) {
            flog::error("Failed to open device");
            return;
        }

        AARTSAAPI_Config config;
        AARTSAAPI_ConfigRoot(&dev, &croot);

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/usbcompression");
        AARTSAAPI_ConfigSetString(&dev, &config, compList[compId].c_str());

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/gaincontrol");
        AARTSAAPI_ConfigSetString(&dev, &config, agcModeList[agcModeId].c_str());

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/receiverchannel");
        AARTSAAPI_ConfigSetString(&dev, &config, L"Rx1");

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/outputformat");
        AARTSAAPI_ConfigSetString(&dev, &config, L"iq");

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/receiverclock");
        AARTSAAPI_ConfigSetString(&dev, &config, clkRatesTxt[samplerate.baseId]);

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"main/decimation");
        AARTSAAPI_ConfigSetString(&dev, &config, decimationsTxt[samplerate.decimId]);

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"main/centerfreq");
        AARTSAAPI_ConfigSetFloat(&dev, &config, freq);

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"main/reflevel");
        AARTSAAPI_ConfigSetFloat(&dev, &config, refLevel);

        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"calibration/rffilter");
        AARTSAAPI_ConfigSetString(&dev, &config, L"Auto Extended");

        updateAmps();

        if (AARTSAAPI_ConnectDevice(&dev) != AARTSAAPI_OK) {
            flog::error("Failed to connect device");
            return;
        }

        if (AARTSAAPI_StartDevice(&dev) != AARTSAAPI_OK) {
            flog::error("Failed to start device");
            return;
        }

        // Wait for first packet
        AARTSAAPI_Packet pkt = { sizeof(AARTSAAPI_Packet) };
        while (AARTSAAPI_GetPacket(&dev, 0, 0, &pkt) == AARTSAAPI_EMPTY) {
#ifdef _WIN32
            Sleep(1);
#else
            usleep(1000);
#endif
        }

        workerThread = std::thread(&SpectranSourceModule::worker, this);

        running = true;
        flog::info("SpectranSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        
        stream.stopWriter();
        AARTSAAPI_StopDevice(&dev);
        AARTSAAPI_DisconnectDevice(&dev);
        AARTSAAPI_CloseDevice(&api, &dev);

        if (workerThread.joinable()) {
            workerThread.join();
        }

        stream.clearWriteStop();

        flog::info("SpectranSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            AARTSAAPI_Config config;
            AARTSAAPI_ConfigFind(&dev, &croot, &config, L"main/centerfreq");
            AARTSAAPI_ConfigSetFloat(&dev, &config, freq);
        }
        freq = freq;
        flog::info("SpectranSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_spectran_dev_", name), &devId, devList.txt)) {
            selectSerial(devList.key(devId));
            core::setInputSampleRate(samplerate.effective);
        }
        
        if (SmGui::Combo(CONCAT("##_spectran_sr_", name), &srId, sampleRateList.txt)) {
            samplerate = sampleRateList.value(srId);
            core::setInputSampleRate(samplerate.effective);
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_spectran_refr_", name))) {
            refresh();
            selectSerial(selectedSerial);
            core::setInputSampleRate(samplerate.effective);
        }

        if (running) { SmGui::EndDisabled(); }

        SmGui::LeftLabel("USB Compression");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_spectran_comp_", name), &compId, compList.txt)) {
            if (running) {
                AARTSAAPI_Config config;
                AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/usbcompression");
                AARTSAAPI_ConfigSetString(&dev, &config, compList[compId].c_str());
            }
        }

        SmGui::LeftLabel("AGC Mode");
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT("##_spectran_agc_", name), &agcModeId, agcModeList.txt)) {
            if (running) {
                AARTSAAPI_Config config;
                AARTSAAPI_ConfigFind(&dev, &croot, &config, L"device/gaincontrol");
                AARTSAAPI_ConfigSetString(&dev, &config, agcModeList[agcModeId].c_str());
            }
        }
        
        if (agcModeId) { SmGui::BeginDisabled(); }
        SmGui::LeftLabel("Ref Level");
        SmGui::FillWidth();
        if (SmGui::SliderFloatWithSteps(CONCAT("##_spectran_ref_", name), &refLevel, minRef, maxRef, refStep, SmGui::FMT_STR_FLOAT_DB_ONE_DECIMAL)) {
            if (running) {
                AARTSAAPI_Config config;
                AARTSAAPI_ConfigFind(&dev, &croot, &config, L"main/reflevel");
                AARTSAAPI_ConfigSetFloat(&dev, &config, refLevel);
            }
        }
        if (agcModeId) { SmGui::EndDisabled(); }

        if (SmGui::Checkbox(CONCAT("Amp##_spectran_amp_", name), &amp)) {
            if (running) {
                updateAmps();
            }
        }

        if (SmGui::Checkbox(CONCAT("Preamp##_spectran_preamp_", name), &preAmp)) {
            if (running) {
                updateAmps();
            }
        }
    }

    void worker() {
        AARTSAAPI_Packet pkt = { sizeof(AARTSAAPI_Packet) };
        AARTSAAPI_Result res;

        while (true) {
            // Get next packet
            while ((res = AARTSAAPI_GetPacket(&dev, 0, 0, &pkt)) == AARTSAAPI_EMPTY) {
#ifdef _WIN32
                Sleep(1);
#else
                usleep(1000);
#endif
            }

            // If there was an error, return
            if (res != AARTSAAPI_OK) { break; }

            if (pkt.num > STREAM_BUFFER_SIZE) {
                flog::error("Buffer too big!!!!");
                continue;
            }

            // Write data
            memcpy(stream.writeBuf, pkt.fp32, pkt.num * sizeof(dsp::complex_t));
            if (!stream.swap(pkt.num)) {
                AARTSAAPI_ConsumePackets(&dev, 0, 1);
                break;
            }

            // Consume packet
            AARTSAAPI_ConsumePackets(&dev, 0, 1);
        }
    }

    void updateAmps() {
        AARTSAAPI_Config config;
        AARTSAAPI_ConfigFind(&dev, &croot, &config, L"calibration/preamp");
        if (amp && preAmp) {
            AARTSAAPI_ConfigSetString(&dev, &config, L"Both");
        }
        else if (amp) {
            AARTSAAPI_ConfigSetString(&dev, &config, L"Amp");
        }
        else if (preAmp) {
            AARTSAAPI_ConfigSetString(&dev, &config, L"Preamp");
        }
        else {
            AARTSAAPI_ConfigSetString(&dev, &config, L"None");
        }
        updateRef();
    }

    void updateRef() {
        // Get and update bounds
        AARTSAAPI_Config config = {};
        AARTSAAPI_ConfigInfo refInfo = {};
        auto res = AARTSAAPI_ConfigFind(&dev, &croot, &config, L"main/reflevel");
        flog::debug("Res A: {}", res);
        res = AARTSAAPI_ConfigGetInfo(&dev, &config, &refInfo);
        flog::debug("Res B: {}", res);
        minRef = refInfo.minValue;
        maxRef = refInfo.maxValue;
        refStep = refInfo.stepValue;
        flog::debug("Gain: {} -> {}", refInfo.minValue, refInfo.maxValue);
        refLevel = std::clamp<float>(refLevel, minRef, maxRef);

        // Apply new ref level
        AARTSAAPI_ConfigSetFloat(&dev, &config, refLevel);
    }

    const double clockRates[4] = {
        92160000.0,
        122880000.0,
        184320000.0,
        245760000.0
    };

    const wchar_t* clkRatesTxt[4] = {
        L"92MHz",
        L"122MHz",
        L"184MHz",
        L"245MHz"
    };

    const int decimations[10] = {
        512, 256, 128, 64, 32, 16, 8, 4, 2, 1
    };

    const wchar_t* decimationsTxt[10] = {
        L"1 / 512", L"1 / 256", L"1 / 128", L"1 / 64", L"1 / 32",
        L"1 / 16", L"1 / 8", L"1 / 4", L"1 / 2", L"Full"
    };

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    bool running = false;
    double freq;
    
    std::string selectedSerial;
    int devId = 0;
    int srId = 0;
    int compId = 0;
    int agcModeId = 0;
    float refLevel = -20.0f;
    bool amp = false;
    bool preAmp = false;
    float minRef = -20.0f;
    float maxRef = 10.0f;
    float refStep = 0.5;

    struct SRCombo {
        bool operator==(const SRCombo& b) const {
            return baseId == b.baseId && decimId == b.decimId;
        }

        int baseId;
        int decimId;
        double effective;
    };

    SRCombo samplerate;

    OptionList<std::string, std::wstring> devList;
    OptionList<std::string, SRCombo> sampleRateList;
    OptionList<std::string, std::wstring> compList;
    OptionList<std::string, std::wstring> agcModeList;
    OptionList<std::string, std::wstring> clockList;

    AARTSAAPI_Handle api;
    AARTSAAPI_Device dev;
    AARTSAAPI_Config croot;

    std::thread workerThread;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "spectran_config.json");
}

SDRPP_CREATE_INSTANCE_V2(SpectranSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SpectranSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
