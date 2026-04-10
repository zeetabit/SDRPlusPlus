#include <utils/net.h>
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

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "network_source",
    /* Description:     */ "UDP/TCP Source Module",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

SDRPP_MOD_INFO_V2{
    /* Name:            */ "network_source",
    /* Description:     */ "UDP/TCP Source Module",
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

enum Protocol {
    PROTOCOL_TCP_SERVER,
    PROTOCOL_TCP_CLIENT,
    PROTOCOL_UDP
};

enum SampleType {
    SAMPLE_TYPE_INT8,
    SAMPLE_TYPE_INT16,
    SAMPLE_TYPE_INT32,
    SAMPLE_TYPE_FLOAT32
};

const size_t SAMPLE_TYPE_SIZE[] {
    2*sizeof(int8_t),
    2*sizeof(int16_t),
    2*sizeof(int32_t),
    2*sizeof(float),
};

class NetworkSourceModule : public ModuleManager::Instance, public ISource {
public:
    NetworkSourceModule(std::string name, ModuleConfig* cfg) {
        this->name = name;
        this->cfg = cfg;

        samplerate = 1000000.0;

        // Define protocols
        // protocols.define("TCP (Server)", PROTOCOL_TCP_SERVER);
        protocols.define("TCP (Client)", PROTOCOL_TCP_CLIENT);
        protocols.define("UDP", PROTOCOL_UDP);

        // Define sample types
        sampleTypes.define("Int8", SAMPLE_TYPE_INT8);
        sampleTypes.define("Int16", SAMPLE_TYPE_INT16);
        sampleTypes.define("Int32", SAMPLE_TYPE_INT32);
        sampleTypes.define("Float32", SAMPLE_TYPE_FLOAT32);

        // Load config
        samplerate = cfg->get<int>("samplerate", samplerate);
        tempSamplerate = samplerate;
        {
            std::string protoStr = cfg->get<std::string>("protocol", "TCP (Server)");
            if (protocols.keyExists(protoStr)) { proto = protocols.value(protocols.keyId(protoStr)); }
        }
        {
            std::string sampTypeStr = cfg->get<std::string>("sampleType", "Float32");
            if (sampleTypes.keyExists(sampTypeStr)) { sampType = sampleTypes.value(sampleTypes.keyId(sampTypeStr)); }
        }
        {
            std::string hostStr = cfg->get<std::string>("host", "localhost");
            strcpy(hostname, hostStr.c_str());
        }
        port = std::clamp<int>(cfg->get<int>("port", 4992), 1, 65535);

        // Set menu IDs
        protoId = protocols.valueId(proto);
        sampTypeId = sampleTypes.valueId(sampType);

        sigpath::sourceManager.registerSource("Network", static_cast<ISource*>(this));
    }

    ~NetworkSourceModule() {
        stop();
        sigpath::sourceManager.unregisterSource("Network");
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

private:
    std::string getSrScaled(double sr) {
        char buf[1024];
        if (sr >= 1000000.0) {
            sprintf(buf, "%.1lf MS/s", sr / 1000000.0);
        }
        else if (sr >= 1000.0) {
            sprintf(buf, "%.1lf KS/s", sr / 1000.0);
        }
        else {
            sprintf(buf, "%.1lf S/s", sr);
        }
        return std::string(buf);
    }

    void onSelect() override {
        core::setInputSampleRate(samplerate);
        flog::info("NetworkSourceModule '{0}': Menu Select!", name);
    }

    void onDeselect() override {
        flog::info("NetworkSourceModule '{0}': Menu Deselect!", name);
    }

    void start() override {
        if (running) { return; }
        
        // Depends on protocol
        try {
            if (proto == PROTOCOL_TCP_SERVER) {
                // Create TCP listener
                // TODO

                // Start listen worker
                // TODO
            }
            else if (proto == PROTOCOL_TCP_CLIENT) {
                // Connect to TCP server
                sock = net::connect(hostname, port);
            }
            else if (proto == PROTOCOL_UDP) {
                // Open UDP socket
                sock = net::openudp("0.0.0.0", port, hostname, port, true);
            }
        }
        catch (const std::exception& e) {
            flog::error("Could not start Network Source: {}", e.what());
            return;
        }

        // Start receive worker
        workerThread = std::thread(&NetworkSourceModule::worker, this);

        running = true;
        flog::info("NetworkSourceModule '{0}': Start!", name);
    }

    void stop() override {
        if (!running) { return; }

        // Stop listen worker
        // TODO

        // Close connection
        if (sock) { sock->close(); }

        // Stop worker thread
        stream.stopWriter();
        if (workerThread.joinable()) { workerThread.join(); }
        stream.clearWriteStop();

        running = false;
        flog::info("NetworkSourceModule '{0}': Stop!", name);
    }

    void tune(double freq) override {
        if (running) {
            // Nothing for now
        }
        freq = freq;
        flog::info("NetworkSourceModule '{0}': Tune: {1}!", name, freq);
    }

    void drawMenu() override {
        if (running) { SmGui::BeginDisabled(); }

        // Hostname and port field
        if (SmGui::InputText(("##network_source_host_" + name).c_str(), hostname, sizeof(hostname))) {
            cfg->set("host", std::string(hostname));
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(("##network_source_port_" + name).c_str(), &port, 0, 0)) {
            port = std::clamp<int>(port, 1, 65535);
            cfg->set("port", port);
        }

        // Mode protocol selector
        SmGui::LeftLabel("Protocol");
        SmGui::FillWidth();
        if (SmGui::Combo(("##network_source_proto_" + name).c_str(), &protoId, protocols.txt)) {
            proto = protocols.value(protoId);
            cfg->set("protocol", protocols.key(protoId));
        }

        // Sample type selector
        SmGui::LeftLabel("Sample type");
        SmGui::FillWidth();
        if (SmGui::Combo(("##network_source_samp_" + name).c_str(), &sampTypeId, sampleTypes.txt)) {
            sampType = sampleTypes.value(sampTypeId);
            cfg->set("sampleType", sampleTypes.key(sampTypeId));
        }

        // Samplerate selector
        SmGui::LeftLabel("Samplerate");
        SmGui::FillWidth();
        if (SmGui::InputInt(("##network_source_sr_" + name).c_str(), &tempSamplerate)) {
            // Prevent silly values from silly users
            tempSamplerate = std::max<int>(tempSamplerate, 1000);
        }
        bool applyEn = (!running && tempSamplerate != samplerate);
        if (!applyEn) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        if (SmGui::Button(("Apply##network_source_apply_" + name).c_str())) {
            samplerate = tempSamplerate;
            core::setInputSampleRate(samplerate);
            cfg->set("samplerate", samplerate);
        }
        if (!applyEn) { SmGui::EndDisabled(); }

        if (tempSamplerate != samplerate) {
            SmGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Warning: Samplerate not applied yet");
        }

        if (running) { SmGui::EndDisabled(); }
    }

    void worker() {
        // Compute sizes
        int blockSize = samplerate / 200;
        int sampleSize = SAMPLE_TYPE_SIZE[sampType];

        // Chose amount of bytes to attempt to read
        bool forceSize = (proto != PROTOCOL_UDP);
        int frameSize = sampleSize * (forceSize ? blockSize : STREAM_BUFFER_SIZE);

        // Allocate receive buffer
        uint8_t* buffer = dsp::buffer::alloc<uint8_t>(frameSize);

        while (true) {
            // Read samples from socket
            int bytes = sock->recv(buffer, frameSize, forceSize);
            if (bytes <= 0) { break; }

            // Convert to CF32 (note: problem if partial sample)
            int count = bytes / sampleSize;
            switch (sampType) {
            case SAMPLE_TYPE_INT8:
                volk_8i_s32f_convert_32f((float*)stream.writeBuf, (int8_t*)buffer, 128.0f, count*2);
                break;
            case SAMPLE_TYPE_INT16:
                volk_16i_s32f_convert_32f((float*)stream.writeBuf, (int16_t*)buffer, 32768.0f, count*2);
                break;
            case SAMPLE_TYPE_INT32:
                volk_32i_s32f_convert_32f((float*)stream.writeBuf, (int32_t*)buffer, 2147483647.0f, count*2);
                break;
            case SAMPLE_TYPE_FLOAT32:
                memcpy(stream.writeBuf, buffer, bytes);
                break;
            default:
                break;
            }

            // Send out converted samples
            if (!stream.swap(count)) { break; }
        }

        // Free receive buffer
        dsp::buffer::free(buffer);
    }

    std::string name;
    ModuleConfig* cfg = nullptr;
    bool enabled = true;
    dsp::stream<dsp::complex_t> stream;
    bool running = false;
    double freq;
    
    int samplerate = 1000000;
    int tempSamplerate = 1000000;
    Protocol proto = PROTOCOL_UDP;
    int protoId;
    SampleType sampType = SAMPLE_TYPE_INT16;
    int sampTypeId;
    char hostname[1024] = "localhost";
    int port = 1234;

    OptionList<std::string, Protocol> protocols;
    OptionList<std::string, SampleType> sampleTypes;

    std::thread workerThread;
    std::thread listenWorkerThread;

    std::mutex sockMtx;
    std::shared_ptr<net::Socket> sock;
    std::shared_ptr<net::Listener> listener;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "network_source_config.json");
}

SDRPP_CREATE_INSTANCE_V2(NetworkSourceModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (NetworkSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
