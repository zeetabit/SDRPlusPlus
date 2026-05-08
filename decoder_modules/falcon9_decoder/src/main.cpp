#include <imgui.h>
#include <config.h>
#include <core.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <module.h>
#include <gui/gui.h>
#include <dsp/stream.h>
#include <dsp/block.h>
#include <dsp/demod/fm.h>
#include <dsp/clock_recovery/mm.h>
#include <dsp/routing/splitter.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>

#include <falcon_fec.h>
#include <falcon_packet.h>

#include <gui/widgets/symbol_diagram.h>

#include <fstream>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "falcon9_decoder",
    /* Description:     */ "Falcon9 telemetry decoder for SDR++",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

#define INPUT_SAMPLE_RATE 6000000

std::ofstream file("output.ts");

class Threshold : public dsp::block {
public:
    Threshold() {}

    Threshold(dsp::stream<float>* in) { init(in); }

    ~Threshold() {
        if (!_block_init) { return; }
        stop();
        delete[] normBuffer;
        _block_init = false;
    }

    void init(dsp::stream<float>* in) {
        _in = in;
        normBuffer = new float[STREAM_BUFFER_SIZE];
        registerInput(_in);
        registerOutput(&out);
        _block_init = true;
    }

    void setInput(dsp::stream<float>* in) {
        assert(_block_init);
        std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
        tempStop();
        unregisterInput(_in);
        _in = in;
        registerInput(_in);
        tempStart();
    }

    int run() override {
        int count = _in->read();
        if (count < 0) { return -1; }

        for (int i = 0; i < count; i++) {
            out.writeBuf[i] = (_in->readBuf[i] > 0.0f);
        }

        _in->flush();
        if (!out.swap(count)) { return -1; }
        return count;
    }

    dsp::stream<uint8_t> out;

private:
    float* normBuffer = nullptr;
    dsp::stream<float>* _in;
};

class Deframer : public dsp::block {
public:
    Deframer() {}

    Deframer(dsp::stream<uint8_t>* in, int frameLen, uint8_t* syncWord, int syncLen) { init(in, frameLen, syncWord, syncLen); }

    ~Deframer() {
        if (!_block_init) { return; }
        stop();
        delete[] buffer;
        delete[] _syncword;
        _block_init = false;
    }

    void init(dsp::stream<uint8_t>* in, int frameLen, uint8_t* syncWord, int syncLen) {
        _in = in;
        _frameLen = frameLen;
        _syncword = new uint8_t[syncLen];
        _syncLen = syncLen;
        memcpy(_syncword, syncWord, syncLen);

        buffer = new uint8_t[STREAM_BUFFER_SIZE + syncLen];
        memset(buffer, 0, syncLen);
        bufferStart = buffer + syncLen;

        registerInput(_in);
        registerOutput(&out);
        _block_init = true;
    }

    void setInput(dsp::stream<uint8_t>* in) {
        assert(_block_init);
        std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
        tempStop();
        unregisterInput(_in);
        _in = in;
        registerInput(_in);
        tempStart();
    }

    int run() override {
        count = _in->read();
        if (count < 0) { return -1; }

        memcpy(bufferStart, _in->readBuf, count - 1);

        for (int i = 0; i < count;) {
            if (bitsRead >= 0) {
                if ((bitsRead % 8) == 0) { out.writeBuf[bitsRead / 8] = 0; }
                out.writeBuf[bitsRead / 8] |= (buffer[i] << (7 - (bitsRead % 8)));
                i++;
                bitsRead++;

                if (bitsRead >= _frameLen) {
                    if (!out.swap((bitsRead / 8) + ((bitsRead % 8) > 0))) { return -1; }
                    bitsRead = -1;
                    if (allowSequential) { nextBitIsStartOfFrame = true; }
                }

                continue;
            }
            else if (memcmp(buffer + i, _syncword, _syncLen) == 0) {
                bitsRead = 0;
                badFrameCount = 0;
                continue;
            }
            else if (nextBitIsStartOfFrame) {
                nextBitIsStartOfFrame = false;
                if (badFrameCount < 5) {
                    badFrameCount++;
                    bitsRead = 0;
                    continue;
                }
            }
            else {
                i++;
            }

            nextBitIsStartOfFrame = false;
        }

        memcpy(buffer, &_in->readBuf[count - _syncLen], _syncLen);

        _in->flush();
        return count;
    }

    bool allowSequential = true;

    dsp::stream<uint8_t> out;

private:
    uint8_t* buffer = nullptr;
    uint8_t* bufferStart = nullptr;
    uint8_t* _syncword = nullptr;
    int count;
    int _frameLen;
    int _syncLen;
    int bitsRead = -1;
    int badFrameCount = 0;
    int callcount = 0;
    bool nextBitIsStartOfFrame = false;

    dsp::stream<uint8_t>* _in;
};

class Falcon9DecoderModule : public ModuleManager::Instance {
public:
    Falcon9DecoderModule(std::string name) {
        this->name = name;

        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 4000000, INPUT_SAMPLE_RATE, 4000000, 4000000, true);

        demod.init(vfo->output, INPUT_SAMPLE_RATE, 2000000.0f * 2.0f, false);
        recov.init(&demod.out, (float)INPUT_SAMPLE_RATE / 3571400.0f, powf(0.01f, 2) / 4.0f, 0.01, 100e-6f);
        split.init(&recov.out);
        split.bindStream(&reshapeInput);
        split.bindStream(&thrInput);
        reshape.init(&reshapeInput, 1024, 198976);
        symSink.init(&reshape.out, symSinkHandler, this);
        thr.init(&thrInput);
        deframe.init(&thr.out, 10232, syncWord, 32);
        falconRS.init(&deframe.out);
        pkt.init(&falconRS.out);
        sink.init(&pkt.out, sinkHandler, this);

        demod.start();
        recov.start();
        split.start();
        reshape.start();
        symSink.start();
        sink.start();
        pkt.start();
        falconRS.start();
        deframe.start();
        thr.start();

#ifdef _WIN32
        ffplay = _popen("ffplay -framedrop -infbuf -hide_banner -loglevel panic -window_title \"Falcon 9 Cameras\" -", "wb");
#else
        ffplay = popen("ffplay -framedrop -infbuf -hide_banner -loglevel panic -window_title \"Falcon 9 Cameras\" -", "wb");
#endif

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~Falcon9DecoderModule() {
    }

    void postInit() {}

    void enable() {
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 4000000, INPUT_SAMPLE_RATE, 4000000, 4000000, true);

        demod.setInput(vfo->output);

        demod.start();
        recov.start();
        split.start();
        reshape.start();
        symSink.start();
        sink.start();
        pkt.start();
        falconRS.start();
        deframe.start();
        thr.start();

        enabled = true;
    }

    void disable() {
        demod.stop();
        recov.stop();
        split.stop();
        reshape.stop();
        symSink.stop();
        sink.stop();
        pkt.stop();
        falconRS.stop();
        deframe.stop();
        thr.stop();

        sigpath::vfoManager.deleteVFO(vfo);
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void menuHandler(void* ctx) {
        Falcon9DecoderModule* _this = (Falcon9DecoderModule*)ctx;

        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        ImGui::SetNextItemWidth(menuWidth);
        _this->symDiag.draw();

        if (_this->logsVisible) {
            if (ImGui::Button("Hide logs", ImVec2(menuWidth, 0))) { _this->logsVisible = false; }
        }
        else {
            if (ImGui::Button("Show logs", ImVec2(menuWidth, 0))) { _this->logsVisible = true; }
        }

        if (_this->logsVisible) {
            std::lock_guard<std::mutex> lck(_this->logsMtx);

            ImGui::Begin("Falcon9 Telemetry");
            ImGui::BeginTabBar("Falcon9Tabs");

            // GPS Logs
            ImGui::BeginTabItem("GPS");
            if (ImGui::Button("Clear logs##GPSClear")) { _this->gpsLogs.clear(); }
            ImGui::BeginChild("GPSChild");
            ImGui::TextUnformatted(_this->gpsLogs.c_str());
            ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();

            // STMM1A Logs
            ImGui::BeginTabItem("STMM1A");

            ImGui::EndTabItem();

            // STMM1B Logs
            ImGui::BeginTabItem("STMM1B");

            ImGui::EndTabItem();

            // STMM1C Logs
            ImGui::BeginTabItem("STMM1C");

            ImGui::EndTabItem();

            ImGui::EndTabBar();
            ImGui::End();
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    static void sinkHandler(uint8_t* data, int count, void* ctx) {
        Falcon9DecoderModule* _this = (Falcon9DecoderModule*)ctx;

        uint16_t length = (((data[0] & 0b1111) << 8) | data[1]) + 2;
        uint64_t pktId = ((uint64_t)data[2] << 56) | ((uint64_t)data[3] << 48) | ((uint64_t)data[4] << 40) | ((uint64_t)data[5] << 32) | ((uint64_t)data[6] << 24) | ((uint64_t)data[7] << 16) | ((uint64_t)data[8] << 8) | data[9];

        if (pktId == 0x0117FE0800320303 || pktId == 0x0112FA0800320303) {
            data[length - 2] = 0;
            _this->logsMtx.lock();
            _this->gpsLogs += (char*)(data + 25);
            _this->logsMtx.unlock();
        }
        else if (pktId == 0x01123201042E1403) {
            fwrite(data + 25, 1, 940, _this->ffplay);
            file.write((char*)(data + 25), 940);
        }
    }

    static void symSinkHandler(float* data, int count, void* ctx) {
        Falcon9DecoderModule* _this = (Falcon9DecoderModule*)ctx;
        float* buf = _this->symDiag.acquireBuffer();
        memcpy(buf, data, 1024 * sizeof(float));
        _this->symDiag.releaseBuffer();
    }

    std::string name;
    bool enabled = true;

    bool logsVisible = false;

    std::mutex logsMtx;
    std::string gpsLogs = "";

    // DSP Chain
    dsp::demod::FM<float> demod;
    dsp::clock_recovery::MM<float> recov;

    dsp::routing::Splitter<float> split;

    dsp::stream<float> reshapeInput;
    dsp::buffer::Reshaper<float> reshape;
    dsp::sink::Handler<float> symSink;

    dsp::stream<float> thrInput;
    Threshold thr;

    uint8_t syncWord[32] = { 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1 };
    Deframer deframe;
    dsp::FalconRS falconRS;
    dsp::FalconPacketSync pkt;
    dsp::sink::Handler<uint8_t> sink;

    FILE* ffplay;

    VFOManager::VFO* vfo;

    ImGui::SymbolDiagram symDiag;
};

MOD_EXPORT void _INIT_() {
    // Nothing
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new Falcon9DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (Falcon9DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    // Nothing either
}
