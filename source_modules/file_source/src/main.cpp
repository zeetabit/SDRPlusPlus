#define NOMINMAX
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <module_manifest.h>
#include <utils/service_registry.h>
#include <utils/radio_control.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/isource.h>
#include <wavreader.h>
#include <core.h>
#include <gui/widgets/file_select.h>
#include <filesystem>
#include <regex>
#include <gui/tuner.h>
#include <algorithm>
#include <stdexcept>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "file_source",
    /* Description:     */ "Wav file source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 1,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "file_source",
    /* Description:     */ "Wav file source module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 1,
    /* Max instances    */ 1,
    /* API version      */ SDRPP_API_VERSION,
    /* Capabilities     */ MOD_CAP_SOURCE,
    /* Dependency count */ 0,
    /* Dependencies     */ nullptr
};

ConfigManager config;

class FileSourceModule : public ModuleManager::Instance, public ISource {
public:
    FileSourceModule(std::string name) : fileSelect("", { "Wav IQ Files (*.wav)", "*.wav", "All Files", "*" }) {
        this->name = name;

        if (core::args["server"].b()) { return; }

        config.readConfig([&](const json& conf) {
            if (conf.contains("path")) {
                fileSelect.setPath(conf["path"], true);
            }
        });

        if (fileSelect.pathIsValid()) {
            try {
                reader = new WavReader(fileSelect.path);
                if (reader->getSampleRate() == 0) {
                    reader->close();
                    delete reader;
                    reader = NULL;
                }
                else {
                    sampleRate = reader->getSampleRate();
                    std::string filename = std::filesystem::path(fileSelect.path).filename().string();
                    centerFreq = getFrequency(filename);
                    // Clear the changed flag so drawMenu doesn't re-open on first render
                    fileSelect.clearChanged();
                }
            }
            catch (const std::exception& e) {
                flog::error("Error opening saved file: {}", e.what());
            }
        }

        sigpath::sourceManager.registerSource("File", static_cast<ISource*>(this));
    }

    ~FileSourceModule() {
        stopSource();
        sigpath::sourceManager.unregisterSource("File");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

    // ISource implementation
    dsp::stream<dsp::complex_t>* getStream() override { return &stream; }

    void onSelect() override {
        core::setInputSampleRate(sampleRate);
        tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", centerFreq);
        sigpath::iqFrontEnd.setBuffering(false);
        ServiceRegistry::get().query<IRadioStateControl>("core")->setCenterFrequencyLocked(true);
        flog::info("FileSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        sigpath::iqFrontEnd.setBuffering(true);
        ServiceRegistry::get().query<IRadioStateControl>("core")->setCenterFrequencyLocked(false);
        flog::info("FileSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        if (reader == NULL) { return; }
        running = true;
        workerThread = float32Mode ? std::thread(&FileSourceModule::floatWorker, this) : std::thread(&FileSourceModule::worker, this);
        flog::info("FileSourceModule '{0}': Start!", name);
    }

    void stop() override {
        stopSource();
    }

    void tune(double freq) override {
        flog::info("FileSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (fileSelect.render("##file_source_" + name)) {
            if (fileSelect.pathIsValid()) {
                if (reader != NULL) {
                    reader->close();
                    delete reader;
                }
                try {
                    reader = new WavReader(fileSelect.path);
                    if (reader->getSampleRate() == 0) {
                        reader->close();
                        delete reader;
                        reader = NULL;
                        throw std::runtime_error("Sample rate may not be zero");
                    }
                    sampleRate = reader->getSampleRate();
                    core::setInputSampleRate(sampleRate);
                    std::string filename = std::filesystem::path(fileSelect.path).filename().string();
                    centerFreq = getFrequency(filename);
                    tuner::tune(tuner::TUNER_MODE_IQ_ONLY, "", centerFreq);
                }
                catch (const std::exception& e) {
                    flog::error("Error: {}", e.what());
                }
                config.withConfig([&](json& conf) { conf["path"] = fileSelect.path; });
            }
        }

        ImGui::Checkbox("Float32 Mode##_file_source", &float32Mode);
    }

private:
    void stopSource() {
        if (!running) { return; }
        if (reader == NULL) { return; }
        stream.stopWriter();
        workerThread.join();
        stream.clearWriteStop();
        running = false;
        reader->rewind();
        flog::info("FileSourceModule '{0}': Stop!", name);
    }

    void worker() {
        double sr = std::max(reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sr / 200.0f), (int)STREAM_BUFFER_SIZE);
        int16_t* inBuf = new int16_t[blockSize * 2];

        while (true) {
            reader->readSamples(inBuf, blockSize * 2 * sizeof(int16_t));
            volk_16i_s32f_convert_32f((float*)stream.writeBuf, inBuf, 32768.0f, blockSize * 2);
            if (!stream.swap(blockSize)) { break; };
        }

        delete[] inBuf;
    }

    void floatWorker() {
        double sr = std::max(reader->getSampleRate(), (uint32_t)1);
        int blockSize = std::min((int)(sr / 200.0f), (int)STREAM_BUFFER_SIZE);

        while (true) {
            reader->readSamples(stream.writeBuf, blockSize * sizeof(dsp::complex_t));
            if (!stream.swap(blockSize)) { break; };
        }
    }

    double getFrequency(std::string filename) {
        std::regex expr("[0-9]+Hz");
        std::smatch matches;
        std::regex_search(filename, matches, expr);
        if (matches.empty()) { return 0; }
        std::string freqStr = matches[0].str();
        return std::atof(freqStr.substr(0, freqStr.size() - 2).c_str());
    }

    FileSelect fileSelect;
    std::string name;
    dsp::stream<dsp::complex_t> stream;
    WavReader* reader = NULL;
    bool running = false;
    bool enabled = true;
    float sampleRate = 1000000;
    std::thread workerThread;
    double centerFreq = 100000000;
    bool float32Mode = false;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["path"] = "";
    config.setPath(core::args["root"].s() + "/file_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT void* _CREATE_INSTANCE_(std::string name) {
    return new FileSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FileSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
