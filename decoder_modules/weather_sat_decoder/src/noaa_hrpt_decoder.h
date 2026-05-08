#pragma once
#include <sat_decoder.h>
#include <dsp/block.h>
#include <dsp/stream.h>
#include <dsp/demod/fm.h>
#include <dsp/routing/splitter.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/sink/null_sink.h>
#include <gui/widgets/symbol_diagram.h>
#include <gui/widgets/line_push_image.h>
#include <gui/gui.h>
#include <inttypes.h>
#include <algorithm>

#define NOAA_HRPT_VFO_SR 3000000.0f
#define NOAA_HRPT_VFO_BW 2000000.0f

// Bitstream utility
inline uint64_t readBits(int offset, int length, uint8_t* buffer) {
    uint64_t outputValue = 0;
    int lastBit = offset + (length - 1);
    int firstWord = offset / 8;
    int lastWord = lastBit / 8;
    int firstOffset = offset - (firstWord * 8);
    int lastOffset = lastBit - (lastWord * 8);
    int wordCount = (lastWord - firstWord) + 1;

    if (wordCount == 1) {
        return (buffer[firstWord] & (0xFF >> firstOffset)) >> (7 - lastOffset);
    }

    int bitsRead = length;
    for (int i = 0; i < wordCount; i++) {
        if (i == 0) {
            bitsRead -= 8 - firstOffset;
            outputValue |= (uint64_t)(buffer[firstWord] & (0xFF >> firstOffset)) << bitsRead;
            continue;
        }
        if (i == (wordCount - 1)) {
            outputValue |= (uint64_t)buffer[lastWord] >> (7 - lastOffset);
            break;
        }
        bitsRead -= 8;
        outputValue |= (uint64_t)buffer[firstWord + i] << bitsRead;
    }
    return outputValue;
}

// Manchester hamming distance helper
inline int MachesterHammingDistance(float* data, uint8_t* syncBits, int n) {
    int dist = 0;
    for (int i = 0; i < n; i++) {
        if ((data[(2 * i) + 1] > data[2 * i]) != syncBits[i]) { dist++; }
    }
    return dist;
}

const uint8_t NOAAHRPTSyncWord[] = {
    1, 0, 1, 0, 0, 0, 0, 1, 0, 0,
    0, 1, 0, 1, 1, 0, 1, 1, 1, 1,
    1, 1, 0, 1, 0, 1, 1, 1, 0, 0,
    0, 1, 1, 0, 0, 1, 1, 1, 0, 1,
    1, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    0, 0, 1, 0, 0, 1, 0, 1, 0, 1
};

// Module-local ManchesterDeframer ported to dsp::block
class ManchesterDeframer : public dsp::block {
public:
    ManchesterDeframer() {}

    ManchesterDeframer(dsp::stream<float>* in, int frameLen, uint8_t* syncWord, int syncLen) { init(in, frameLen, syncWord, syncLen); }

    ~ManchesterDeframer() {
        if (!_block_init) { return; }
        stop();
        delete[] buffer;
        delete[] _syncword;
        _block_init = false;
    }

    void init(dsp::stream<float>* in, int frameLen, uint8_t* syncWord, int syncLen) {
        _in = in;
        _frameLen = frameLen;
        _syncword = new uint8_t[syncLen];
        _syncLen = syncLen;
        memcpy(_syncword, syncWord, syncLen);

        buffer = new float[STREAM_BUFFER_SIZE + (syncLen * 2)];
        memset(buffer, 0, syncLen * 2 * sizeof(float));
        bufferStart = &buffer[syncLen * 2];

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
        count = _in->read();
        if (count < 0) { return -1; }

        int readable;
        memcpy(bufferStart, _in->readBuf, (count - 1) * sizeof(float));

        for (int i = 0; i < count;) {
            if (bitsRead >= 0) {
                readable = std::min<int>(count - i, _frameLen - bitsRead);
                memcpy(&out.writeBuf[bitsRead], &buffer[i], readable * sizeof(float));
                bitsRead += readable;
                i += readable;
                if (bitsRead >= _frameLen) {
                    out.swap(_frameLen);
                    bitsRead = -1;
                }
                continue;
            }

            if (MachesterHammingDistance(&buffer[i], _syncword, _syncLen) <= 2) {
                bitsRead = 0;
                continue;
            }

            i++;
        }

        memcpy(buffer, &_in->readBuf[count - (_syncLen * 2)], _syncLen * 2 * sizeof(float));

        _in->flush();
        return count;
    }

    dsp::stream<float> out;

private:
    float* buffer = nullptr;
    float* bufferStart = nullptr;
    uint8_t* _syncword = nullptr;
    int count;
    int _frameLen;
    int _syncLen;
    int bitsRead = -1;
    dsp::stream<float>* _in;
};

// Module-local ManchesterDecoder ported to dsp::block (float input, uint8_t output)
class ManchesterDecoderBlock : public dsp::block {
public:
    ManchesterDecoderBlock() {}

    ManchesterDecoderBlock(dsp::stream<float>* in, bool inverted) { init(in, inverted); }

    void init(dsp::stream<float>* in, bool inverted) {
        _in = in;
        _inverted = inverted;
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

        if (_inverted) {
            for (int i = 0; i < count; i += 2) {
                out.writeBuf[i / 2] = (_in->readBuf[i + 1] < _in->readBuf[i]);
            }
        }
        else {
            for (int i = 0; i < count; i += 2) {
                out.writeBuf[i / 2] = (_in->readBuf[i + 1] > _in->readBuf[i]);
            }
        }

        _in->flush();
        out.swap(count / 2);
        return count;
    }

    dsp::stream<uint8_t> out;

private:
    dsp::stream<float>* _in;
    bool _inverted;
};

// Module-local BitPacker ported to dsp::block
class BitPacker : public dsp::block {
public:
    BitPacker() {}

    BitPacker(dsp::stream<uint8_t>* in) { init(in); }

    void init(dsp::stream<uint8_t>* in) {
        _in = in;
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
        int count = _in->read();
        if (count < 0) { return -1; }

        for (int i = 0; i < count; i++) {
            if ((i % 8) == 0) { out.writeBuf[i / 8] = 0; }
            out.writeBuf[i / 8] |= (_in->readBuf[i] & 1) << (7 - (i % 8));
        }

        _in->flush();
        out.swap((count / 8) + (((count % 8) == 0) ? 0 : 1));
        return count;
    }

    dsp::stream<uint8_t> out;

private:
    dsp::stream<uint8_t>* _in;
};

// Module-local NOAA HRPT Demux ported to dsp::block
class HRPTDemux : public dsp::block {
public:
    HRPTDemux() {}

    HRPTDemux(dsp::stream<uint8_t>* in) { init(in); }

    void init(dsp::stream<uint8_t>* in) {
        _in = in;
        registerInput(_in);
        registerOutput(&AVHRRChan1Out);
        registerOutput(&AVHRRChan2Out);
        registerOutput(&AVHRRChan3Out);
        registerOutput(&AVHRRChan4Out);
        registerOutput(&AVHRRChan5Out);
        registerOutput(&TIPOut);
        registerOutput(&AIPOut);
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

    inline uint16_t HRPTReadWord(int offset, uint8_t* buf) {
        return (uint16_t)readBits(offset * 10, 10, buf);
    }

    int run() override {
        int count = _in->read();
        if (count < 0) { return -1; }

        int minFrame = readBits(61, 2, _in->readBuf);

        if (minFrame == 0) {
            _in->flush();
            return count;
        }

        if (minFrame == 1) {
            for (int i = 0; i < 5; i++) {
                for (int j = 0; j < 104; j++) {
                    TIPOut.writeBuf[j] = (HRPTReadWord(103 + (i * 104) + j, _in->readBuf) >> 2) & 0xFF;
                }
                if (!TIPOut.swap(104)) { return -1; };
            }
        }
        else if (minFrame == 3) {
            for (int i = 0; i < 5; i++) {
                for (int j = 0; j < 104; j++) {
                    AIPOut.writeBuf[j] = (HRPTReadWord(103 + (i * 104) + j, _in->readBuf) >> 2) & 0xFF;
                }
                if (!AIPOut.swap(104)) { return -1; };
            }
        }

        for (int i = 0; i < 2048; i++) {
            AVHRRChan1Out.writeBuf[i] = HRPTReadWord(750 + (i * 5), _in->readBuf);
            AVHRRChan2Out.writeBuf[i] = HRPTReadWord(750 + (i * 5) + 1, _in->readBuf);
            AVHRRChan3Out.writeBuf[i] = HRPTReadWord(750 + (i * 5) + 2, _in->readBuf);
            AVHRRChan4Out.writeBuf[i] = HRPTReadWord(750 + (i * 5) + 3, _in->readBuf);
            AVHRRChan5Out.writeBuf[i] = HRPTReadWord(750 + (i * 5) + 4, _in->readBuf);
        }
        if (!AVHRRChan1Out.swap(2048)) { return -1; };
        if (!AVHRRChan2Out.swap(2048)) { return -1; };
        if (!AVHRRChan3Out.swap(2048)) { return -1; };
        if (!AVHRRChan4Out.swap(2048)) { return -1; };
        if (!AVHRRChan5Out.swap(2048)) { return -1; };

        _in->flush();
        return count;
    }

    dsp::stream<uint8_t> TIPOut;
    dsp::stream<uint8_t> AIPOut;

    dsp::stream<uint16_t> AVHRRChan1Out;
    dsp::stream<uint16_t> AVHRRChan2Out;
    dsp::stream<uint16_t> AVHRRChan3Out;
    dsp::stream<uint16_t> AVHRRChan4Out;
    dsp::stream<uint16_t> AVHRRChan5Out;

private:
    dsp::stream<uint8_t>* _in;
};

// Module-local TIP Demux ported to dsp::block
class TIPDemux : public dsp::block {
public:
    TIPDemux() {}

    TIPDemux(dsp::stream<uint8_t>* in) { init(in); }

    void init(dsp::stream<uint8_t>* in) {
        _in = in;
        registerInput(_in);
        registerOutput(&HIRSOut);
        registerOutput(&SEMOut);
        registerOutput(&DCSOut);
        registerOutput(&SBUVOut);
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
        int count = _in->read();
        if (count < 0) { return -1; }

        HIRSOut.writeBuf[0] = _in->readBuf[16];
        HIRSOut.writeBuf[1] = _in->readBuf[17];
        HIRSOut.writeBuf[2] = _in->readBuf[22];
        HIRSOut.writeBuf[3] = _in->readBuf[23];
        HIRSOut.writeBuf[4] = _in->readBuf[26];
        HIRSOut.writeBuf[5] = _in->readBuf[27];
        HIRSOut.writeBuf[6] = _in->readBuf[30];
        HIRSOut.writeBuf[7] = _in->readBuf[31];
        HIRSOut.writeBuf[8] = _in->readBuf[34];
        HIRSOut.writeBuf[9] = _in->readBuf[35];
        HIRSOut.writeBuf[10] = _in->readBuf[38];
        HIRSOut.writeBuf[11] = _in->readBuf[39];
        HIRSOut.writeBuf[12] = _in->readBuf[42];
        HIRSOut.writeBuf[13] = _in->readBuf[43];
        HIRSOut.writeBuf[14] = _in->readBuf[54];
        HIRSOut.writeBuf[15] = _in->readBuf[55];
        HIRSOut.writeBuf[16] = _in->readBuf[58];
        HIRSOut.writeBuf[17] = _in->readBuf[59];
        HIRSOut.writeBuf[18] = _in->readBuf[62];
        HIRSOut.writeBuf[19] = _in->readBuf[63];
        HIRSOut.writeBuf[20] = _in->readBuf[66];
        HIRSOut.writeBuf[21] = _in->readBuf[67];
        HIRSOut.writeBuf[22] = _in->readBuf[70];
        HIRSOut.writeBuf[23] = _in->readBuf[71];
        HIRSOut.writeBuf[24] = _in->readBuf[74];
        HIRSOut.writeBuf[25] = _in->readBuf[75];
        HIRSOut.writeBuf[26] = _in->readBuf[78];
        HIRSOut.writeBuf[27] = _in->readBuf[79];
        HIRSOut.writeBuf[28] = _in->readBuf[82];
        HIRSOut.writeBuf[29] = _in->readBuf[83];
        HIRSOut.writeBuf[30] = _in->readBuf[84];
        HIRSOut.writeBuf[31] = _in->readBuf[85];
        HIRSOut.writeBuf[32] = _in->readBuf[88];
        HIRSOut.writeBuf[33] = _in->readBuf[89];
        HIRSOut.writeBuf[34] = _in->readBuf[92];
        HIRSOut.writeBuf[35] = _in->readBuf[93];
        if (!HIRSOut.swap(36)) { return -1; };

        SEMOut.writeBuf[0] = _in->readBuf[20];
        SEMOut.writeBuf[1] = _in->readBuf[21];
        if (!SEMOut.swap(2)) { return -1; };

        DCSOut.writeBuf[0] = _in->readBuf[18];
        DCSOut.writeBuf[1] = _in->readBuf[19];
        DCSOut.writeBuf[2] = _in->readBuf[24];
        DCSOut.writeBuf[3] = _in->readBuf[25];
        DCSOut.writeBuf[4] = _in->readBuf[28];
        DCSOut.writeBuf[5] = _in->readBuf[29];
        DCSOut.writeBuf[6] = _in->readBuf[32];
        DCSOut.writeBuf[7] = _in->readBuf[33];
        DCSOut.writeBuf[8] = _in->readBuf[40];
        DCSOut.writeBuf[9] = _in->readBuf[41];
        DCSOut.writeBuf[10] = _in->readBuf[44];
        DCSOut.writeBuf[11] = _in->readBuf[45];
        DCSOut.writeBuf[12] = _in->readBuf[52];
        DCSOut.writeBuf[13] = _in->readBuf[53];
        DCSOut.writeBuf[14] = _in->readBuf[56];
        DCSOut.writeBuf[15] = _in->readBuf[57];
        DCSOut.writeBuf[16] = _in->readBuf[60];
        DCSOut.writeBuf[17] = _in->readBuf[61];
        DCSOut.writeBuf[18] = _in->readBuf[64];
        DCSOut.writeBuf[19] = _in->readBuf[65];
        DCSOut.writeBuf[20] = _in->readBuf[68];
        DCSOut.writeBuf[21] = _in->readBuf[69];
        DCSOut.writeBuf[22] = _in->readBuf[72];
        DCSOut.writeBuf[23] = _in->readBuf[73];
        DCSOut.writeBuf[24] = _in->readBuf[76];
        DCSOut.writeBuf[25] = _in->readBuf[77];
        DCSOut.writeBuf[26] = _in->readBuf[86];
        DCSOut.writeBuf[27] = _in->readBuf[87];
        DCSOut.writeBuf[28] = _in->readBuf[90];
        DCSOut.writeBuf[29] = _in->readBuf[91];
        DCSOut.writeBuf[30] = _in->readBuf[94];
        DCSOut.writeBuf[31] = _in->readBuf[95];
        if (!DCSOut.swap(32)) { return -1; };

        SBUVOut.writeBuf[0] = _in->readBuf[36];
        SBUVOut.writeBuf[1] = _in->readBuf[37];
        SBUVOut.writeBuf[2] = _in->readBuf[80];
        SBUVOut.writeBuf[3] = _in->readBuf[81];
        if (!SBUVOut.swap(4)) { return -1; };

        _in->flush();
        return count;
    }

    dsp::stream<uint8_t> HIRSOut;
    dsp::stream<uint8_t> SEMOut;
    dsp::stream<uint8_t> DCSOut;
    dsp::stream<uint8_t> SBUVOut;

private:
    dsp::stream<uint8_t>* _in;
};

inline uint16_t HIRSSignedToUnsigned(uint16_t n) {
    return (n & 0x1000) ? (0x1000 + (n & 0xFFF)) : (0xFFF - (n & 0xFFF));
}

// Module-local HIRS Demux ported to dsp::block
class HIRSDemux : public dsp::block {
public:
    HIRSDemux() {}

    HIRSDemux(dsp::stream<uint8_t>* in) { init(in); }

    void init(dsp::stream<uint8_t>* in) {
        _in = in;
        registerInput(_in);
        for (int i = 0; i < 20; i++) {
            registerOutput(&radChannels[i]);
        }

        for (int i = 0; i < 20; i++) {
            for (int j = 0; j < 56; j++) { radChannels[i].writeBuf[j] = 0xFFF; }
        }

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
        int count = _in->read();
        if (count < 0) { return -1; }

        int element = readBits(19, 6, _in->readBuf);

        if ((element < lastElement || element > 55) && newImageData) {
            newImageData = false;
            for (int i = 0; i < 20; i++) {
                if (!radChannels[i].swap(56)) { return -1; }
            }
            for (int i = 0; i < 20; i++) {
                for (int j = 0; j < 56; j++) { radChannels[i].writeBuf[j] = 0xFFF; }
            }
        }
        lastElement = element;

        if (element <= 55) {
            newImageData = true;

            radChannels[0].writeBuf[element] = HIRSSignedToUnsigned(readBits(26, 13, _in->readBuf));
            radChannels[1].writeBuf[element] = HIRSSignedToUnsigned(readBits(52, 13, _in->readBuf));
            radChannels[2].writeBuf[element] = HIRSSignedToUnsigned(readBits(65, 13, _in->readBuf));
            radChannels[3].writeBuf[element] = HIRSSignedToUnsigned(readBits(91, 13, _in->readBuf));
            radChannels[4].writeBuf[element] = HIRSSignedToUnsigned(readBits(221, 13, _in->readBuf));
            radChannels[5].writeBuf[element] = HIRSSignedToUnsigned(readBits(208, 13, _in->readBuf));
            radChannels[6].writeBuf[element] = HIRSSignedToUnsigned(readBits(143, 13, _in->readBuf));
            radChannels[7].writeBuf[element] = HIRSSignedToUnsigned(readBits(156, 13, _in->readBuf));
            radChannels[8].writeBuf[element] = HIRSSignedToUnsigned(readBits(273, 13, _in->readBuf));
            radChannels[9].writeBuf[element] = HIRSSignedToUnsigned(readBits(182, 13, _in->readBuf));
            radChannels[10].writeBuf[element] = HIRSSignedToUnsigned(readBits(119, 13, _in->readBuf));
            radChannels[11].writeBuf[element] = HIRSSignedToUnsigned(readBits(247, 13, _in->readBuf));
            radChannels[12].writeBuf[element] = HIRSSignedToUnsigned(readBits(78, 13, _in->readBuf));
            radChannels[13].writeBuf[element] = HIRSSignedToUnsigned(readBits(195, 13, _in->readBuf));
            radChannels[14].writeBuf[element] = HIRSSignedToUnsigned(readBits(234, 13, _in->readBuf));
            radChannels[15].writeBuf[element] = HIRSSignedToUnsigned(readBits(260, 13, _in->readBuf));
            radChannels[16].writeBuf[element] = HIRSSignedToUnsigned(readBits(39, 13, _in->readBuf));
            radChannels[17].writeBuf[element] = HIRSSignedToUnsigned(readBits(104, 13, _in->readBuf));
            radChannels[18].writeBuf[element] = HIRSSignedToUnsigned(readBits(130, 13, _in->readBuf));
            radChannels[19].writeBuf[element] = HIRSSignedToUnsigned(readBits(169, 13, _in->readBuf));
        }

        if (element == 55) {
            newImageData = false;
            for (int i = 0; i < 20; i++) {
                if (!radChannels[i].swap(56)) { return -1; }
            }
            for (int i = 0; i < 20; i++) {
                for (int j = 0; j < 56; j++) { radChannels[i].writeBuf[j] = 0xFFF; }
            }
        }

        _in->flush();
        return count;
    }

    dsp::stream<uint16_t> radChannels[20];

private:
    dsp::stream<uint8_t>* _in;
    int lastElement = 0;
    bool newImageData = false;
};

class NOAAHRPTDecoder : public SatDecoder {
public:
    NOAAHRPTDecoder(VFOManager::VFO* vfo, std::string name) : avhrrRGBImage(2048, 256), avhrr1Image(2048, 256), avhrr2Image(2048, 256), avhrr3Image(2048, 256), avhrr4Image(2048, 256), avhrr5Image(2048, 256), symDiag(0.5f) {
        _vfo = vfo;
        _name = name;

        // Core DSP - PM demod mapped to FM demod with bandwidth = deviation * 2
        demod.init(vfo->output, NOAA_HRPT_VFO_SR, 665400.0f * 2.0f, false);

        split.init(&demod.out);
        split.bindStream(&dataStream);
        split.bindStream(&visStream);

        reshape.init(&visStream, 1024, (NOAA_HRPT_VFO_SR / 30) - 1024);
        visSink.init(&reshape.out, visHandler, this);

        deframe.init(&dataStream, 11090 * 10 * 2, (uint8_t*)NOAAHRPTSyncWord, 60);
        manDec.init(&deframe.out, false);
        packer.init(&manDec.out);
        demux.init(&packer.out);
        tipDemux.init(&demux.TIPOut);
        hirsDemux.init(&tipDemux.HIRSOut);

        avhrr1Sink.init(&demux.AVHRRChan1Out, avhrr1Handler, this);
        avhrr2Sink.init(&demux.AVHRRChan2Out, avhrr2Handler, this);
        avhrr3Sink.init(&demux.AVHRRChan3Out, avhrr3Handler, this);
        avhrr4Sink.init(&demux.AVHRRChan4Out, avhrr4Handler, this);
        avhrr5Sink.init(&demux.AVHRRChan5Out, avhrr5Handler, this);

        sbuvSink.init(&tipDemux.SBUVOut);
        dcsSink.init(&tipDemux.DCSOut);
        semSink.init(&tipDemux.SEMOut);

        aipSink.init(&demux.AIPOut);

        hirs1Sink.init(&hirsDemux.radChannels[0], hirs1Handler, this);
        hirs2Sink.init(&hirsDemux.radChannels[1], hirs2Handler, this);
        hirs3Sink.init(&hirsDemux.radChannels[2], hirs3Handler, this);
        hirs4Sink.init(&hirsDemux.radChannels[3], hirs4Handler, this);
        hirs5Sink.init(&hirsDemux.radChannels[4], hirs5Handler, this);
        hirs6Sink.init(&hirsDemux.radChannels[5], hirs6Handler, this);
        hirs7Sink.init(&hirsDemux.radChannels[6], hirs7Handler, this);
        hirs8Sink.init(&hirsDemux.radChannels[7], hirs8Handler, this);
        hirs9Sink.init(&hirsDemux.radChannels[8], hirs9Handler, this);
        hirs10Sink.init(&hirsDemux.radChannels[9], hirs10Handler, this);
        hirs11Sink.init(&hirsDemux.radChannels[10], hirs11Handler, this);
        hirs12Sink.init(&hirsDemux.radChannels[11], hirs12Handler, this);
        hirs13Sink.init(&hirsDemux.radChannels[12], hirs13Handler, this);
        hirs14Sink.init(&hirsDemux.radChannels[13], hirs14Handler, this);
        hirs15Sink.init(&hirsDemux.radChannels[14], hirs15Handler, this);
        hirs16Sink.init(&hirsDemux.radChannels[15], hirs16Handler, this);
        hirs17Sink.init(&hirsDemux.radChannels[16], hirs17Handler, this);
        hirs18Sink.init(&hirsDemux.radChannels[17], hirs18Handler, this);
        hirs19Sink.init(&hirsDemux.radChannels[18], hirs19Handler, this);
        hirs20Sink.init(&hirsDemux.radChannels[19], hirs20Handler, this);
    }

    void select() {
        _vfo->setSampleRate(NOAA_HRPT_VFO_SR, NOAA_HRPT_VFO_BW);
        _vfo->setReference(ImGui::WaterfallVFO::REF_CENTER);
        _vfo->setBandwidthLimits(NOAA_HRPT_VFO_BW, NOAA_HRPT_VFO_BW, true);
    };

    void start() {
        demod.start();

        split.start();
        reshape.start();
        visSink.start();

        deframe.start();
        manDec.start();
        packer.start();
        demux.start();
        tipDemux.start();
        hirsDemux.start();

        avhrr1Sink.start();
        avhrr2Sink.start();
        avhrr3Sink.start();
        avhrr4Sink.start();
        avhrr5Sink.start();

        sbuvSink.start();
        dcsSink.start();
        semSink.start();

        aipSink.start();

        hirs1Sink.start();
        hirs2Sink.start();
        hirs3Sink.start();
        hirs4Sink.start();
        hirs5Sink.start();
        hirs6Sink.start();
        hirs7Sink.start();
        hirs8Sink.start();
        hirs9Sink.start();
        hirs10Sink.start();
        hirs11Sink.start();
        hirs12Sink.start();
        hirs13Sink.start();
        hirs14Sink.start();
        hirs15Sink.start();
        hirs16Sink.start();
        hirs17Sink.start();
        hirs18Sink.start();
        hirs19Sink.start();
        hirs20Sink.start();

        compositeThread = std::thread(&NOAAHRPTDecoder::avhrrCompositeWorker, this);
    };

    void stop() {
        compositeIn1.stopReader();
        compositeIn1.stopWriter();
        compositeIn2.stopReader();
        compositeIn2.stopWriter();

        demod.stop();

        split.stop();
        reshape.stop();
        visSink.stop();

        deframe.stop();
        manDec.stop();
        packer.stop();
        demux.stop();
        tipDemux.stop();
        hirsDemux.stop();

        avhrr1Sink.stop();
        avhrr2Sink.stop();
        avhrr3Sink.stop();
        avhrr4Sink.stop();
        avhrr5Sink.stop();

        sbuvSink.stop();
        dcsSink.stop();
        semSink.stop();

        aipSink.stop();

        hirs1Sink.stop();
        hirs2Sink.stop();
        hirs3Sink.stop();
        hirs4Sink.stop();
        hirs5Sink.stop();
        hirs6Sink.stop();
        hirs7Sink.stop();
        hirs8Sink.stop();
        hirs9Sink.stop();
        hirs10Sink.stop();
        hirs11Sink.stop();
        hirs12Sink.stop();
        hirs13Sink.stop();
        hirs14Sink.stop();
        hirs15Sink.stop();
        hirs16Sink.stop();
        hirs17Sink.stop();
        hirs18Sink.stop();
        hirs19Sink.stop();
        hirs20Sink.stop();

        if (compositeThread.joinable()) {
            compositeThread.join();
        }

        compositeIn1.clearReadStop();
        compositeIn1.clearWriteStop();
        compositeIn2.clearReadStop();
        compositeIn2.clearWriteStop();
    };

    void setVFO(VFOManager::VFO* vfo) {
        _vfo = vfo;
        demod.setInput(_vfo->output);
    };

    virtual bool canRecord() {
        return false;
    }

    void drawMenu(float menuWidth) {
        ImGui::SetNextItemWidth(menuWidth);
        symDiag.draw();

        if (showWindow) {
            gui::mainWindow.lockWaterfallControls = true;
            ImGui::Begin("NOAA HRPT Decoder");
            ImGui::BeginTabBar("NOAAHRPTTabs");

            if (ImGui::BeginTabItem("AVHRR RGB(221)")) {
                ImGui::BeginChild("AVHRRRGBChild");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrrRGBImage.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("AVHRR 1")) {
                ImGui::BeginChild("AVHRR1Child");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrr1Image.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("AVHRR 2")) {
                ImGui::BeginChild("AVHRR2Child");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrr2Image.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("AVHRR 3")) {
                ImGui::BeginChild("AVHRR3Child");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrr3Image.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("AVHRR 4")) {
                ImGui::BeginChild("AVHRR4Child");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrr4Image.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("AVHRR 5")) {
                ImGui::BeginChild("AVHRR5Child");
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                avhrr5Image.draw();
                ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("HIRS")) {
                ImGui::BeginChild("HIRSChild");

                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
            ImGui::End();
        }

        ImGui::Checkbox("Show Image", &showWindow);
    };

private:
    // AVHRR Data Handlers
    void avhrrCompositeWorker() {
        compositeIn1.flush();
        compositeIn2.flush();
        while (true) {
            if (compositeIn1.read() < 0) { return; }
            if (compositeIn2.read() < 0) { return; }

            uint8_t* buf = avhrrRGBImage.acquireNextLine();
            float rg, b;
            for (int i = 0; i < 2048; i++) {
                b = ((float)compositeIn1.readBuf[i] * 255.0f) / 1024.0f;
                rg = ((float)compositeIn2.readBuf[i] * 255.0f) / 1024.0f;
                buf[(i * 4)] = rg;
                buf[(i * 4) + 1] = rg;
                buf[(i * 4) + 2] = b;
                buf[(i * 4) + 3] = 255;
            }
            avhrrRGBImage.releaseNextLine();

            compositeIn1.flush();
            compositeIn2.flush();
        }
    }

    static void avhrr1Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        uint8_t* buf = _this->avhrr1Image.acquireNextLine();
        float val;
        for (int i = 0; i < 2048; i++) {
            val = ((float)data[i] * 255.0f) / 1024.0f;
            buf[(i * 4)] = val;
            buf[(i * 4) + 1] = val;
            buf[(i * 4) + 2] = val;
            buf[(i * 4) + 3] = 255;
        }
        _this->avhrr1Image.releaseNextLine();

        memcpy(_this->compositeIn1.writeBuf, data, count * sizeof(uint16_t));
        _this->compositeIn1.swap(count);
    }

    static void avhrr2Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        uint8_t* buf = _this->avhrr2Image.acquireNextLine();
        float val;
        for (int i = 0; i < 2048; i++) {
            val = ((float)data[i] * 255.0f) / 1024.0f;
            buf[(i * 4)] = val;
            buf[(i * 4) + 1] = val;
            buf[(i * 4) + 2] = val;
            buf[(i * 4) + 3] = 255;
        }
        _this->avhrr2Image.releaseNextLine();

        memcpy(_this->compositeIn2.writeBuf, data, count * sizeof(uint16_t));
        _this->compositeIn2.swap(count);
    }

    static void avhrr3Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        uint8_t* buf = _this->avhrr3Image.acquireNextLine();
        float val;
        for (int i = 0; i < 2048; i++) {
            val = ((float)data[i] * 255.0f) / 1024.0f;
            buf[(i * 4)] = val;
            buf[(i * 4) + 1] = val;
            buf[(i * 4) + 2] = val;
            buf[(i * 4) + 3] = 255;
        }
        _this->avhrr3Image.releaseNextLine();
    }

    static void avhrr4Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        uint8_t* buf = _this->avhrr4Image.acquireNextLine();
        float val;
        for (int i = 0; i < 2048; i++) {
            val = ((float)data[i] * 255.0f) / 1024.0f;
            buf[(i * 4)] = val;
            buf[(i * 4) + 1] = val;
            buf[(i * 4) + 2] = val;
            buf[(i * 4) + 3] = 255;
        }
        _this->avhrr4Image.releaseNextLine();
    }

    static void avhrr5Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        uint8_t* buf = _this->avhrr5Image.acquireNextLine();
        float val;
        for (int i = 0; i < 2048; i++) {
            val = ((float)data[i] * 255.0f) / 1024.0f;
            buf[(i * 4)] = val;
            buf[(i * 4) + 1] = val;
            buf[(i * 4) + 2] = val;
            buf[(i * 4) + 3] = 255;
        }
        _this->avhrr5Image.releaseNextLine();
    }

    // HIRS Data Handlers
    static void hirs1Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs2Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs3Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs4Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs5Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs6Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs7Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs8Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs9Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs10Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs11Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs12Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs13Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs14Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs15Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs16Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs17Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs18Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs19Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void hirs20Handler(uint16_t* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
    }

    static void visHandler(float* data, int count, void* ctx) {
        NOAAHRPTDecoder* _this = (NOAAHRPTDecoder*)ctx;
        memcpy(_this->symDiag.acquireBuffer(), data, 1024 * sizeof(float));
        _this->symDiag.releaseBuffer();
    }

    std::string _name;
    std::string _recPath;
    VFOManager::VFO* _vfo;

    // DSP
    dsp::demod::FM<float> demod;

    dsp::stream<float> visStream;
    dsp::stream<float> dataStream;
    dsp::routing::Splitter<float> split;

    dsp::buffer::Reshaper<float> reshape;

    ManchesterDeframer deframe;
    ManchesterDecoderBlock manDec;
    BitPacker packer;
    HRPTDemux demux;
    TIPDemux tipDemux;
    HIRSDemux hirsDemux;

    // AHVRR Handlers
    dsp::sink::Handler<uint16_t> avhrr1Sink;
    dsp::sink::Handler<uint16_t> avhrr2Sink;
    dsp::sink::Handler<uint16_t> avhrr3Sink;
    dsp::sink::Handler<uint16_t> avhrr4Sink;
    dsp::sink::Handler<uint16_t> avhrr5Sink;

    // (at the moment) Unused TIP handlers
    dsp::sink::Null<uint8_t> sbuvSink;
    dsp::sink::Null<uint8_t> dcsSink;
    dsp::sink::Null<uint8_t> semSink;

    // (at the moment) Unused AIP handlers
    dsp::sink::Null<uint8_t> aipSink;

    // HIRS Handlers
    dsp::sink::Handler<uint16_t> hirs1Sink;
    dsp::sink::Handler<uint16_t> hirs2Sink;
    dsp::sink::Handler<uint16_t> hirs3Sink;
    dsp::sink::Handler<uint16_t> hirs4Sink;
    dsp::sink::Handler<uint16_t> hirs5Sink;
    dsp::sink::Handler<uint16_t> hirs6Sink;
    dsp::sink::Handler<uint16_t> hirs7Sink;
    dsp::sink::Handler<uint16_t> hirs8Sink;
    dsp::sink::Handler<uint16_t> hirs9Sink;
    dsp::sink::Handler<uint16_t> hirs10Sink;
    dsp::sink::Handler<uint16_t> hirs11Sink;
    dsp::sink::Handler<uint16_t> hirs12Sink;
    dsp::sink::Handler<uint16_t> hirs13Sink;
    dsp::sink::Handler<uint16_t> hirs14Sink;
    dsp::sink::Handler<uint16_t> hirs15Sink;
    dsp::sink::Handler<uint16_t> hirs16Sink;
    dsp::sink::Handler<uint16_t> hirs17Sink;
    dsp::sink::Handler<uint16_t> hirs18Sink;
    dsp::sink::Handler<uint16_t> hirs19Sink;
    dsp::sink::Handler<uint16_t> hirs20Sink;

    dsp::sink::Handler<float> visSink;

    ImGui::LinePushImage avhrrRGBImage;
    ImGui::LinePushImage avhrr1Image;
    ImGui::LinePushImage avhrr2Image;
    ImGui::LinePushImage avhrr3Image;
    ImGui::LinePushImage avhrr4Image;
    ImGui::LinePushImage avhrr5Image;

    ImGui::SymbolDiagram symDiag;

    dsp::stream<uint16_t> compositeIn1;
    dsp::stream<uint16_t> compositeIn2;
    std::thread compositeThread;

    bool showWindow = false;
};
