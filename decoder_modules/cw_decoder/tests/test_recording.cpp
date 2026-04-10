#include <catch.hpp>
#include <cw/channel.h>
#include <cmath>
#include <vector>
#include <fstream>
#include <cstring>

// Minimal WAV reader for mono 16-bit PCM
struct WavData {
    int sampleRate = 0;
    int channels = 0;
    std::vector<float> samples;  // normalized to [-1, 1]
};

static WavData loadWav(const std::string& path) {
    WavData wav;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return wav;

    char riff[4]; f.read(riff, 4);
    if (std::memcmp(riff, "RIFF", 4) != 0) return wav;

    uint32_t fileSize; f.read((char*)&fileSize, 4);
    char wave[4]; f.read(wave, 4);
    if (std::memcmp(wave, "WAVE", 4) != 0) return wav;

    uint16_t audioFormat = 0, numChannels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0, dataSize = 0;

    while (f.good()) {
        char chunkId[4]; f.read(chunkId, 4);
        uint32_t chunkSize; f.read((char*)&chunkSize, 4);
        if (!f.good()) break;

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            f.read((char*)&audioFormat, 2);
            f.read((char*)&numChannels, 2);
            f.read((char*)&sampleRate, 4);
            uint32_t byteRate; f.read((char*)&byteRate, 4);
            uint16_t blockAlign; f.read((char*)&blockAlign, 2);
            f.read((char*)&bitsPerSample, 2);
            if (chunkSize > 16) f.seekg(chunkSize - 16, std::ios::cur);
        }
        else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataSize = chunkSize;
            int numSamples = dataSize / (bitsPerSample / 8) / numChannels;
            wav.samples.resize(numSamples);
            wav.sampleRate = sampleRate;
            wav.channels = numChannels;

            if (bitsPerSample == 16) {
                std::vector<int16_t> raw(numSamples * numChannels);
                f.read((char*)raw.data(), dataSize);
                for (int i = 0; i < numSamples; i++) {
                    wav.samples[i] = raw[i * numChannels] / 32768.0f;
                }
            }
            break;
        }
        else {
            f.seekg(chunkSize, std::ios::cur);
        }
    }
    return wav;
}

// Convert mono audio (with CW tone at toneFreq) to IQ samples at targetRate.
// The audio contains a CW beat note — we need to create IQ where the channel
// DSP can extract the tone.
static std::vector<dsp::complex_t> audioToIQ(
    const WavData& audio, float toneFreq, float targetRate)
{
    // The audio already contains the demodulated CW tone.
    // To feed it to the CW channel (which expects IQ), we modulate
    // the audio onto a carrier at toneFreq to create synthetic IQ.
    int outLen = (int)((double)audio.samples.size() / audio.sampleRate * targetRate);
    std::vector<dsp::complex_t> iq(outLen);

    double phaseInc = 2.0 * M_PI * toneFreq / targetRate;
    double phase = 0;
    double ratio = (double)audio.sampleRate / targetRate;

    for (int i = 0; i < outLen; i++) {
        // Resample audio
        double srcIdx = i * ratio;
        int idx = (int)srcIdx;
        float frac = (float)(srcIdx - idx);
        float audioSample = 0;
        if (idx < (int)audio.samples.size() - 1) {
            audioSample = audio.samples[idx] * (1.0f - frac) + audio.samples[idx + 1] * frac;
        } else if (idx < (int)audio.samples.size()) {
            audioSample = audio.samples[idx];
        }

        // Modulate onto carrier at toneFreq
        iq[i].re = audioSample * cosf((float)phase);
        iq[i].im = audioSample * sinf((float)phase);
        phase += phaseInc;
        if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
    return iq;
}

TEST_CASE("Recording: decode real CW audio", "[cw][recording]") {
    std::string path = "/Users/zetabit/.config/sdrpp/recordings/audio_7028020Hz_19-08-52_03-04-2026.wav";
    auto audio = loadWav(path);
    if (audio.samples.empty()) {
        WARN("Recording not found: " << path);
        return;
    }

    INFO("Loaded: " << audio.sampleRate << " Hz, " << audio.samples.size() << " samples, "
         << (float)audio.samples.size() / audio.sampleRate << "s");

    float toneFreq = 800.0f;  // CW beat note frequency (~789 Hz from analysis)
    float iqRate = 8000.0f;   // CW decoder expects 8 kHz IQ

    auto iq = audioToIQ(audio, toneFreq, iqRate);
    INFO("IQ samples: " << iq.size() << " (" << iq.size() / iqRate << "s)");

    cw::Channel ch;
    ch.init(0, toneFreq);
    ch.debugLog = true;

    // Track WPM over time
    int blockSize = 512;
    float lastReportTime = 0;
    for (int off = 0; off < (int)iq.size(); off += blockSize) {
        int n = std::min(blockSize, (int)iq.size() - off);
        ch.process(n, &iq[off]);

        float timeSec = (float)(off + n) / iqRate;
        if (timeSec - lastReportTime >= 2.0f) {
            WARN("t=" << timeSec << "s wpm=" << ch.wpm << " snr=" << ch.snr
                 << " frozen=" << (ch.wpm > 0 && ch.snr < 3 ? "maybe" : "no"));
            lastReportTime = timeSec;
        }
    }

    std::string decoded = ch.text.getText();
    WARN("Decoded: '" << decoded << "'");
    WARN("Final WPM: " << ch.wpm << " SNR: " << ch.snr);

    // At minimum, we should decode something
    REQUIRE(decoded.size() > 3);
}
