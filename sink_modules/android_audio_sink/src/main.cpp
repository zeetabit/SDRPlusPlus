#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <signal_path/sink.h>
#include <dsp/buffer/packer.h>
#include <utils/flog.h>
#include <config.h>
#include <utils/optionlist.h>
#include <aaudio/AAudio.h>
#include <core.h>
#include <module_manifest.h>
#include <module_config.h>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "audio_sink",
    /* Description:     */ "Android audio sink module for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    "audio_sink", "Android audio sink module for SDR++", "Ryzerth", 0, 1, 0, 1,
    SDRPP_API_VERSION, MOD_CAP_SINK, 0, nullptr,
    nullptr, "audio_sink_config.json"
};

ConfigManager config;
SDRPP_MOD_CONFIG(config);

class AudioSink : SinkManager::Sink {
public:
    AudioSink(SinkManager::Stream* stream, std::string streamName) {
        _stream = stream;
        _streamName = streamName;

        packer.init(_stream->sinkOut, 512);

        // TODO: Add choice? I don't think anyone cares on android...
        sampleRate = 48000;
        _stream->setSampleRate(sampleRate);
    }

    ~AudioSink() {
    }

    void start() {
        if (running) {
            return;
        }
        doStart();
        running = true;
    }

    void stop() {
        if (!running) {
            return;
        }
        doStop();
        running = false;
    }

    void menuHandler() {
        // Draw menu here
    }

private:
    void doStart() {
        // Create stream builder
        AAudioStreamBuilder *builder;
        aaudio_result_t result = AAudio_createStreamBuilder(&builder);

        // Set stream options
        bufferSize = round(sampleRate / 60.0);
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setSampleRate(builder, sampleRate);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setBufferCapacityInFrames(builder, bufferSize);
        AAudioStreamBuilder_setErrorCallback(builder, errorCallback, this);
        packer.setSampleCount(bufferSize);
        
        // Open the stream
        AAudioStreamBuilder_openStream(builder, &stream);

        // Stream stream and packer
        packer.start();
        AAudioStream_requestStart(stream);

        // We no longer need the builder
        AAudioStreamBuilder_delete(builder);

        // Start worker thread
        workerThread = std::thread(&AudioSink::worker, this);
    }

    void doStop() {
        packer.stop();
        packer.out.stopReader();
        AAudioStream_requestStop(stream);
        AAudioStream_close(stream);
        if (workerThread.joinable()) { workerThread.join(); }
        packer.out.clearReadStop();
    }

    void worker() {
        while (true) {
            int count = packer.out.read();
            if (count < 0) { return; }
            AAudioStream_write(stream, packer.out.readBuf, count, 100000000);
            packer.out.flush();
        }
    }

    static void errorCallback(AAudioStream *stream, void *userData, aaudio_result_t error){
        // detect an audio device detached and restart the stream
        if (error == AAUDIO_ERROR_DISCONNECTED){
            std::thread thr(&AudioSink::restart, (AudioSink*)userData);
            thr.detach();
        }
    }

    void restart() {
        if (running) { doStop(); }
        if (running) { doStart(); }
    }

    std::thread workerThread;

    AAudioStream *stream = NULL;
    SinkManager::Stream* _stream;
    dsp::buffer::Packer<dsp::stereo_t> packer;

    std::string _streamName;
    double sampleRate;
    int bufferSize;

    bool running = false;
};

class AudioSinkModule : public ModuleManager::Instance {
public:
    AudioSinkModule(std::string name, ModuleConfig* cfg) {
        this->name = name;
        provider.create = create_sink;
        provider.ctx = this;

        sigpath::sinkManager.registerSinkProvider("Audio", provider);
    }

    ~AudioSinkModule() {
        // Unregister sink, this will automatically stop and delete all instances of the audio sink
        sigpath::sinkManager.unregisterSinkProvider("Audio");
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

private:
    static SinkManager::Sink* create_sink(SinkManager::Stream* stream, std::string streamName, void* ctx) {
        return (SinkManager::Sink*)(new AudioSink(stream, streamName));
    }

    std::string name;
    bool enabled = true;
    SinkManager::SinkProvider provider;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "audio_sink_config.json");
}

SDRPP_CREATE_INSTANCE_V2(AudioSinkModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (AudioSinkModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}