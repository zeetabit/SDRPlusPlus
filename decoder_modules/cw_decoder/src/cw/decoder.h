#pragma once
#include "../decoder.h"
#include <signal_path/vfo_manager.h>
#include <utils/optionlist.h>
#include <gui/widgets/symbol_diagram.h>
#include <gui/style.h>
#include <dsp/sink/handler_sink.h>
#include "dsp.h"
#include "cw.h"

#define BAUDRATE    512
#define SAMPLERATE  (BAUDRATE*10)

class CWDecoder : public Decoder {
public:
    CWDecoder(const std::string& name, VFOManager::VFO* vfo) : diag(0.6, BAUDRATE) {
        this->name = name;
        this->vfo = vfo;

        // Define baudrate options
        baudrates.define(512, "512 Baud", 512);

        // Init DSP
        vfo->setBandwidthLimits(BAUDRATE, BAUDRATE, true);
        vfo->setSampleRate(SAMPLERATE, BAUDRATE);
        dsp.init(vfo->output, SAMPLERATE, BAUDRATE);
        reshape.init(&dsp.soft, BAUDRATE, (BAUDRATE / 30.0) - BAUDRATE);
        dataHandler.init(&dsp.out, _dataHandler, this);
        diagHandler.init(&reshape.out, _diagHandler, this);

        // Init decoder
        decoder.onMessage.bind(&CWDecoder::messageHandler, this);
    }

    ~CWDecoder() {
        stop();
    }

    void showMenu() {
        ImGui::LeftLabel("Baudrate");
        ImGui::FillWidth();
        if (ImGui::Combo(("##cw_br_" + name).c_str(), &brId, baudrates.txt)) {
            // TODO
        }

        ImGui::FillWidth();
        diag.draw();
    }

    void setVFO(VFOManager::VFO* vfo) {
        this->vfo = vfo;
        vfo->setBandwidthLimits(BAUDRATE, BAUDRATE, true);
        vfo->setSampleRate(SAMPLERATE, BAUDRATE);
        dsp.setInput(vfo->output);
    }

    void start() {
        dsp.start();
        reshape.start();
        dataHandler.start();
        diagHandler.start();
    }

    void stop() {
        dsp.stop();
        reshape.stop();
        dataHandler.stop();
        diagHandler.stop();
    }

private:
    static void _dataHandler(uint8_t* data, int count, void* ctx) {
        CWDecoder* _this = (CWDecoder*)ctx;
        _this->decoder.process(data, count);
    }

    static void _diagHandler(float* data, int count, void* ctx) {
        CWDecoder* _this = (CWDecoder*)ctx;
        float* buf = _this->diag.acquireBuffer();
        memcpy(buf, data, count * sizeof(float));
        _this->diag.releaseBuffer();
    }

    void messageHandler(cw::Address addr, cw::MessageType type, const std::string& msg) {
        flog::debug("[{}]: '{}'", (uint32_t)addr, msg);
    }

    std::string name;
    VFOManager::VFO* vfo;

    CWDSP dsp;
    dsp::buffer::Reshaper<float> reshape;
    dsp::sink::Handler<uint8_t> dataHandler;
    dsp::sink::Handler<float> diagHandler;

    cw::Decoder decoder;

    ImGui::SymbolDiagram diag;

    int brId = 2;

    OptionList<int, int> baudrates;
};