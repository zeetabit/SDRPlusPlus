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
#include <utils/optionlist.h>
#include <RtAudio.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_source",
    /* Description:     */ "Audio source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "audio_source",
    /* Description:     */ "Audio source module for SDR++",
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

struct DeviceInfo {
    RtAudio::DeviceInfo info;
    int id;
    bool operator==(const struct DeviceInfo& other) const {
        return other.id == id;
    }
};

class AudioSourceModule : public ModuleManager::Instance, public ISource {
public:
    AudioSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;

#if RTAUDIO_VERSION_MAJOR >= 6
        audio.setErrorCallback(&errorCallback);
#endif

        sampleRate = 48000.0;

        // Refresh devices
        refresh();

        // Select device
        std::string device = "";
        config.readConfig([&](const json& conf) {
            if (conf.contains("device")) {
                device = conf["device"];
            }
        });
        select(device);
        
        sigpath::sourceManager.registerSource("Audio", static_cast<ISource*>(this));
    }

    ~AudioSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Audio");
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
        devices.clear();

#if RTAUDIO_VERSION_MAJOR >= 6
        for (int i : audio.getDeviceIds()) {
#else
        int count = audio.getDeviceCount();
        for (int i = 0; i < count; i++) {
#endif
            try {
                // Get info
                auto info = audio.getDeviceInfo(i);

#if !defined(RTAUDIO_VERSION_MAJOR) || RTAUDIO_VERSION_MAJOR < 6
                if (!info.probed) { continue; }
#endif
                // Check that it has a stereo input
                if (info.inputChannels < 2) { continue; }

                // Save info
                DeviceInfo dinfo = { info, i };
                devices.define(info.name, info.name, dinfo);
            }
            catch (const std::exception& e) {
                flog::error("Error getting audio device ({}) info: {}", i, e.what());
            }
        }
    }

    void select(std::string name) {
        if (devices.empty()) {
            selectedDevice.clear();
            return;
        }

        // Check that such a device exist. If not select first
        if (!devices.keyExists(name)) {
            select(devices.key(0));
            return;
        }
        
        // Get device info
        devId = devices.keyId(name);
        auto info = devices.value(devId).info;
        selectedDevice = name;

        // List samplerates and save ID of the preference one
        sampleRates.clear();
        for (const auto& sr : info.sampleRates) {
            std::string name = getBandwdithScaled(sr);
            sampleRates.define(sr, name, sr);
            if (sr == info.preferredSampleRate) {
                srId = sampleRates.valueId(sr);
            }
        }

        // Load samplerate from config (guard: device may not exist in config yet)
        config.readConfig([&](const json& conf) {
            if (conf.contains("devices") && conf["devices"].contains(selectedDevice)
                && conf["devices"][selectedDevice].contains("sampleRate")) {
                sampleRate = conf["devices"][selectedDevice]["sampleRate"];
                if (sampleRates.keyExists(sampleRate)) {
                    srId = sampleRates.keyId(sampleRate);
                }
            }
        });

        // Update samplerate from ID
        sampleRate = sampleRates[srId];
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
        flog::info("AudioSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("AudioSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }

        // If no device is selected, give up
        if (selectedDevice.empty()) { return; }
        
        // Stream options
        RtAudio::StreamParameters parameters;
        parameters.deviceId = devices[devId].id;
        parameters.nChannels = 2;
        unsigned int bufferFrames = sampleRate / 200;
        RtAudio::StreamOptions opts;
        opts.flags = RTAUDIO_MINIMIZE_LATENCY;
        opts.streamName = "SDR++ Audio Source";

        // Open and start stream
        try {
            audio.openStream(NULL, &parameters, RTAUDIO_FLOAT32, sampleRate, &bufferFrames, callback, this, &opts);
            audio.startStream();
            running = true;
        }
        catch (const std::exception& e) {
            flog::error("Error opening audio device: {}", e.what());
        }
        
        flog::info("AudioSourceModule '{}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }
        running = false;
        
        audio.stopStream();
        audio.closeStream();

        flog::info("AudioSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        // Not possible
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Combo(CONCAT("##_audio_dev_sel_", name), &devId, devices.txt)) {
            std::string dev = devices.key(devId);
            select(dev);
            core::setInputSampleRate(sampleRate);
            config.withConfig([&](json& conf) { conf["device"] = dev; });
        }

        if (SmGui::Combo(CONCAT("##_audio_sr_sel_", name), &srId, sampleRates.txt)) {
            sampleRate = sampleRates[srId];
            core::setInputSampleRate(sampleRate);
            if (!selectedDevice.empty()) {
                config.withConfig([&](json& conf) { conf["devices"][selectedDevice]["sampleRate"] = sampleRate; });
            }
        }

        SmGui::SameLine();
        SmGui::FillWidth();
        SmGui::ForceSync();
        if (SmGui::Button(CONCAT("Refresh##_audio_refr_", name))) {
            refresh();
            select(selectedDevice);
            core::setInputSampleRate(sampleRate);
        }

        if (running) { SmGui::EndDisabled(); }
    }

    static int callback(void* outputBuffer, void* inputBuffer, unsigned int nBufferFrames, double streamTime, RtAudioStreamStatus status, void* userData) {
        AudioSourceModule* _this = (AudioSourceModule*)userData;
        memcpy(_this->stream.writeBuf, inputBuffer, nBufferFrames * sizeof(dsp::complex_t));
        _this->stream.swap(nBufferFrames);
        return 0;
    }

#if RTAUDIO_VERSION_MAJOR >= 6
    static void errorCallback(RtAudioErrorType type, const std::string& errorText) {
        switch (type) {
        case RtAudioErrorType::RTAUDIO_NO_ERROR:
            return;
        case RtAudioErrorType::RTAUDIO_WARNING:
        case RtAudioErrorType::RTAUDIO_NO_DEVICES_FOUND:
        case RtAudioErrorType::RTAUDIO_DEVICE_DISCONNECT:
            flog::warn("AudioSourceModule Warning: {} ({})", errorText, (int)type);
            break;
        default:
            throw std::runtime_error(errorText);
        }
    }
#endif

    std::string name;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    double sampleRate;
    bool running = false;
    
    OptionList<std::string, DeviceInfo> devices;
    OptionList<double, double> sampleRates;
    std::string selectedDevice = "";
    int devId = 0;
    int srId = 0;

    RtAudio audio;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "audio_source_config.json");
}

SDRPP_CREATE_INSTANCE_V2(AudioSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (AudioSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
