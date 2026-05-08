#pragma once
#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/routing/doubler.h>
#include <dsp/demod/fm.h>
#include <dsp/filter/fir.h>
#include <dsp/taps/root_raised_cosine.h>
#include <dsp/clock_recovery/mm.h>
#include <dsp/sink/null_sink.h>
#include <dsp/hier_block.h>
#include <utils/flog.h>

extern "C" {
#include <correct.h>
}

#define KGSSTV_DEVIATION        300
#define KGSSTV_BAUDRATE         1200
#define KGSSTV_RRC_ALPHA        0.7f
#define KGSSTV_4FSK_HIGH_CUT    0.5f

const uint8_t KGSSTV_SYNC_WORD[] = {
    0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 1, 0,
    0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0,
    1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1,
    0, 1, 0, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0
};

const uint8_t KGSSTV_SCRAMBLING[] = {
    1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0,
    1, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 1,
    0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0,
    1, 0, 0, 1, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 0,
    0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1,
    0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1,
    1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 0, 0,
    0, 1, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1
};

const uint8_t KGSSTV_SCRAMBLING_BYTES[] = {
    0b11101100, 0b11000100, 0b10011100, 0b11111001, 0b00000100,
    0b01101010, 0b10011011, 0b01001010, 0b00010110, 0b00011001,
    0b01111111, 0b01011011, 0b10111100, 0b01110100, 0b01010111,
    0b00000010
};

static const correct_convolutional_polynomial_t kgsstv_polynomial[] = {0155, 0117};

#define KGSSTV_SYNC_WORD_SIZE       sizeof(KGSSTV_SYNC_WORD)
#define KGSSTV_SYNC_SCRAMBLING_SIZE sizeof(KGSSTV_SCRAMBLING)

namespace kgsstv {
    class Deframer : public dsp::block {
    public:
        Deframer() {}

        Deframer(dsp::stream<float>* in) { init(in); }

        void init(dsp::stream<float>* in) {
            _in = in;
            conv = correct_convolutional_create(2, 7, kgsstv_polynomial);
            memset(convTmp, 0x00, 1024);
            registerInput(_in);
            registerOutput(&out);
            _block_init = true;
        }

        void setInput(dsp::stream<float>* in) {
            assert(_block_init);
            std::lock_guard<std::recursive_mutex> lck(ctrlMtx);
            tempStop();
            _in = in;
            tempStart();
        }

        int run() override {
            int count = _in->read();
            if (count < 0) { return -1; }

            for (int i = 0; i < count; i++) {
                if (syncing) {
                    if ((_in->readBuf[i] > 0.0f) && !KGSSTV_SYNC_WORD[match]) {
                        if (++err > 4) {
                            int backtrack = match - 1;
                            match = 0;
                            err = 0;
                            if (backtrack > 0 && i >= backtrack) {
                                i -= backtrack;
                            }
                            continue;
                        }
                    }

                    if (++match == KGSSTV_SYNC_WORD_SIZE) {
                        flog::warn("Frame detected");
                        syncing = false;
                        readCount = 0;
                        writeCount = 0;
                    }
                }
                else {
                    convTmp[readCount] = std::clamp<int>((_in->readBuf[i] + 1.0f) * 128.0f, 0, 255);

                    if (++readCount == 108) {
                        match = 0;
                        err = 0;
                        syncing = true;

                        for (int j = 0; j < 108; j++) {
                            if (KGSSTV_SCRAMBLING[j]) {
                                convTmp[j] = 255 - convTmp[j];
                            }
                        }

                        int convOutCount = correct_convolutional_decode_soft(conv, convTmp, 124, out.writeBuf);
                        (void)convOutCount;

                        flog::warn("Frames written: {0}", ++framesWritten);
                        if (!out.swap(7)) {
                            _in->flush();
                            return -1;
                        }
                    }
                }
            }

            _in->flush();
            return count;
        }

        dsp::stream<uint8_t> out;

    private:
        dsp::stream<float>* _in;
        correct_convolutional* conv = NULL;
        uint8_t convTmp[1024];

        int match = 0;
        int err = 0;
        int readCount = 0;
        int writeCount = 0;
        bool syncing = true;

        int framesWritten = 0;
    };

    class Decoder : public dsp::hier_block {
    public:
        Decoder() {}

        Decoder(dsp::stream<dsp::complex_t>* input, float sampleRate) {
            init(input, sampleRate);
        }

        void init(dsp::stream<dsp::complex_t>* input, float sampleRate) {
            _sampleRate = sampleRate;

            demod.init(input, _sampleRate, KGSSTV_DEVIATION * 2, false);
            rrcTaps = dsp::taps::rootRaisedCosine<float>(31, KGSSTV_RRC_ALPHA, _sampleRate / KGSSTV_BAUDRATE);
            fir.init(&demod.out, rrcTaps);
            recov.init(&fir.out, _sampleRate / KGSSTV_BAUDRATE, 1e-6, 0.01, 0.01);
            doubler.init(&recov.out);

            deframer.init(&doubler.outA);
            ns.init(&deframer.out);
            diagOut = &doubler.outB;

            registerBlock(&demod);
            registerBlock(&fir);
            registerBlock(&recov);
            registerBlock(&doubler);
            registerBlock(&deframer);
            registerBlock(&ns);

            _block_init = true;
        }

        void setInput(dsp::stream<dsp::complex_t>* input) {
            assert(_block_init);
            demod.setInput(input);
        }

        dsp::stream<float>* diagOut = NULL;

    private:
        dsp::demod::FM<float> demod;
        dsp::tap<float> rrcTaps;
        dsp::filter::FIR<float, float> fir;
        dsp::clock_recovery::MM<float> recov;
        dsp::routing::Doubler<float> doubler;

        Deframer deframer;
        dsp::sink::Null<uint8_t> ns;

        float _sampleRate;
    };
}
