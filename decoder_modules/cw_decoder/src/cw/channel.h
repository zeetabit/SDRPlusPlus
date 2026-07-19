#pragma once
#include <cstdio>
#include <dsp/types.h>
#include "dsp.h"
#include "tone_detector.h"
#include "timing.h"
#include "morse_tree.h"
#include "text_buffer.h"
#include "corrector.h"
#include "conversation.h"
#include "core.h"
#include "core_registry.h"

#define CW_SAMPLERATE    8000.0f
#define CW_INTERNAL_RATE 1000.0f
#define CW_MAX_ENVELOPE  65536

namespace cw {

    // Hosts one pluggable decode core and owns everything around it:
    // text buffering, spelling correction and conversation tracking are
    // identical for every core, so a benchmark across cores measures decoding.
    class Channel : public CharSink {
    public:
        int id = 0;
        float toneFreq = 700.0f;
        bool active = true;
        // Verbose decode trace to stderr. Stable, runtime-switchable via
        // the "debugLog" config key. Uses fprintf intentionally — flog takes
        // a global mutex that would stall the DSP thread.
        bool debugLog = false;

        float snr = 0;
        float wpm = 0;
        float confidence = 0;
        TextBuffer text;
        ConversationTracker conversation;

        // coreName selects a decoder from coreRegistry(); unknown names fall
        // back to DEFAULT_CORE rather than failing, so a stale config value
        // cannot break the module.
        void init(int channelId, float tone, const std::string& coreName = DEFAULT_CORE) {
            const CoreSpec* spec = findCore(coreName);
            if (!spec) { spec = findCore(DEFAULT_CORE); }
            initWithCore(channelId, tone, spec->make(), spec->name);
        }

        // Host a core that the registry cannot construct. Oracle cores close
        // over per-signal ground truth, so they are built by the caller and
        // deliberately kept out of coreRegistry(): a core that cannot run on a
        // real signal must never appear in the config UI.
        void initWithCore(int channelId, float tone, std::unique_ptr<IDecodeCore> c,
                          const std::string& label = "custom") {
            id = channelId;
            toneFreq = tone;
            activeCoreName = label;
            core = std::move(c);
            core->init(CW_SAMPLERATE, CW_INTERNAL_RATE, toneFreq);
        }

        const std::string& coreName() const { return activeCoreName; }

        void process(int count, const dsp::complex_t* iq) {
            if (!active) { return; }
            core->process(count, iq, *this);

            int diagN = 0;
            const float* env = core->diagnostic(diagN);
            if (env && diagN > 0) {
                std::lock_guard<std::mutex> lck(diagMtx);
                int toCopy = std::min(diagN, (int)diagBuf.size());
                diagBuf.erase(diagBuf.begin(), diagBuf.begin() + toCopy);
                diagBuf.insert(diagBuf.end(), env, env + toCopy);
            }

            auto st = core->stats();
            snr = st.snr;
            wpm = st.wpm;
            confidence = st.confidence;
        }


        void setToneFreq(float freq) {
            toneFreq = freq;
            core->setToneFreq(freq);
        }

        void preseed(float noiseLevel = 0.001f, float durationMs = 500.0f) {
            int samples = (int)(durationMs / 1000.0f * CW_INTERNAL_RATE);
            core->preseed(noiseLevel, samples);
        }

        // ── CharSink: the only route from a core to decoded text ──

        // Emit a decoded character immediately, and track the current word for
        // post-correction on the word boundary.
        void emitChar(char c, float conf) override {
            if (c == ' ' || c == '\0') return;
            text.append(c, conf);
            currentWord += c;
            currentWordConfSum += conf;
            currentWordCharCount++;
        }

        // On word boundary: correct the current word in-place if needed,
        // then feed the (corrected) word to the conversation tracker.
        void flushWord() override {
            if (currentWord.empty()) return;
            float avgConf = currentWordCharCount > 0 ? currentWordConfSum / currentWordCharCount : 0.5f;
            std::string corrected = corrector::correctWord(currentWord, avgConf, &conversation);
            if (corrected != currentWord) {
                text.replaceLastN(currentWord.size(), corrected, avgConf);
            }
            conversation.feedWord(corrected, avgConf, wpm);
            currentWord.clear();
            currentWordConfSum = 0;
            currentWordCharCount = 0;
        }

        // Emit a word gap (space). Corrects the current word first.
        void emitWordGap() override {
            flushWord();
            text.appendSpace();
        }

        // A core replacing its provisional output with a retro-decoded
        // reconstruction discards everything emitted so far.
        void clearEmitted() override {
            text.clear();
            currentWord.clear();
            currentWordConfSum = 0;
            currentWordCharCount = 0;
        }

        void reset() {
            if (core) { core->reset(); }
            conversation.reset();
            text.clear();
            snr = 0;
            wpm = 0;
            confidence = 0;
            currentWord.clear();
            currentWordConfSum = 0;
            currentWordCharCount = 0;
        }

        std::vector<float> getDiagramData(int maxSamples) {
            std::lock_guard<std::mutex> lck(diagMtx);
            if ((int)diagBuf.size() > maxSamples) {
                return std::vector<float>(diagBuf.end() - maxSamples, diagBuf.end());
            }
            return diagBuf;
        }

    private:
        std::unique_ptr<IDecodeCore> core;
        std::string activeCoreName = DEFAULT_CORE;

        std::string currentWord;
        float currentWordConfSum = 0;
        int currentWordCharCount = 0;

        std::mutex diagMtx;
        std::vector<float> diagBuf = std::vector<float>(512, 0.0f);
    };
}
