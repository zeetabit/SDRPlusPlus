#pragma once
#include <dsp/types.h>
#include <cmath>
#include <vector>
#include <string>
#include <random>
#include <algorithm>
#include <map>

namespace cw_test {

    // Morse lookup table
    inline const std::map<char, std::string>& morseTable() {
        static const std::map<char, std::string> table = {
            {'A', ".-"},   {'B', "-..."}, {'C', "-.-."}, {'D', "-.."},
            {'E', "."},    {'F', "..-."}, {'G', "--."},  {'H', "...."},
            {'I', ".."},   {'J', ".---"}, {'K', "-.-"},  {'L', ".-.."},
            {'M', "--"},   {'N', "-."},   {'O', "---"},  {'P', ".--."},
            {'Q', "--.-"}, {'R', ".-."},  {'S', "..."},  {'T', "-"},
            {'U', "..-"},  {'V', "...-"}, {'W', ".--"},  {'X', "-..-"},
            {'Y', "-.--"}, {'Z', "--.."},
            {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
            {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
            {'8', "---.."}, {'9', "----."},
            {'/', "-..-."}, {'=', "-...-"}, {'?', "..--.."},
            {'+', ".-.-."}, {'*', "...-.-"},  // prosigns: AR, SK
        };
        return table;
    }

    inline std::string charToMorse(char c) {
        c = toupper(c);
        auto& table = morseTable();
        auto it = table.find(c);
        return (it != table.end()) ? it->second : "";
    }

    // ── Signal degradation parameters ──

    struct SignalParams {
        float toneFreq = 700.0f;
        float sampleRate = 8000.0f;
        float amplitude = 1.0f;
        float ditMs = 80.0f;        // base dit duration (15 WPM)
        float riseMs = 5.0f;        // keying rise/fall time

        // Degradation
        float noiseAmp = 0.0f;      // additive gaussian noise amplitude
        float jitterPct = 0.0f;     // timing jitter as fraction (0.15 = ±15%)
        float weightBias = 0.0f;    // dit/dah weight deviation (-0.2 = light dits, +0.2 = heavy dits)
        float qsbHz = 0.0f;         // QSB (fading) rate in Hz (0 = no fading)
        float qsbDepth = 0.0f;      // QSB depth: 0 = none, 1.0 = full (amplitude goes to 0)
        float qrmFreq = 0.0f;       // QRM interferer frequency (0 = no QRM)
        float qrmAmp = 0.0f;        // QRM interferer amplitude
        float qrnRate = 0.0f;       // QRN impulse noise probability per sample (0-1)
        float qrnAmp = 0.0f;        // QRN impulse amplitude

        // Farnsworth: stretches inter-character and inter-word gaps.
        // 1.0 = standard timing. 2.0 = gaps are 2× normal (common in training).
        float farnsworthRatio = 1.0f;

        unsigned seed = 42;
    };

    // ── Signal generator ──

    class CWSignalGenerator {
    public:
        void init(const SignalParams& p) {
            params = p;
            phase = 0;
            qsbPhase = 0;
            qrmPhase = 0;
            envState = 0;
            totalSamples = 0;
            rng.seed(p.seed);
            noiseDist = std::normal_distribution<float>(0.0f, 1.0f);
            uniformDist = std::uniform_real_distribution<float>(0.0f, 1.0f);
        }

        // Generate IQ samples for a key-down or key-up segment
        void generate(dsp::complex_t* buf, int count, bool keyDown) {
            float omega = 2.0f * M_PI * params.toneFreq / params.sampleRate;
            int riseSamples = std::max(1, (int)(params.riseMs / 1000.0f * params.sampleRate));
            float target = keyDown ? 1.0f : 0.0f;
            float qrmOmega = 2.0f * M_PI * params.qrmFreq / params.sampleRate;

            for (int i = 0; i < count; i++) {
                // Smooth keying envelope
                float alpha = 1.0f / riseSamples;
                envState += alpha * (target - envState);

                // QSB: amplitude fading
                float qsbMul = 1.0f;
                if (params.qsbHz > 0) {
                    qsbMul = 1.0f - params.qsbDepth * 0.5f * (1.0f + sinf(qsbPhase));
                    qsbPhase += 2.0f * M_PI * params.qsbHz / params.sampleRate;
                    if (qsbPhase > M_PI) qsbPhase -= 2.0f * M_PI;
                }

                float env = envState * params.amplitude * qsbMul;
                float re = env * cosf(phase);
                float im = env * sinf(phase);

                // QRM: continuous interfering tone
                if (params.qrmFreq > 0 && params.qrmAmp > 0) {
                    re += params.qrmAmp * cosf(qrmPhase);
                    im += params.qrmAmp * sinf(qrmPhase);
                    qrmPhase += qrmOmega;
                    if (qrmPhase > M_PI) qrmPhase -= 2.0f * M_PI;
                }

                // Gaussian noise
                if (params.noiseAmp > 0) {
                    re += params.noiseAmp * noiseDist(rng);
                    im += params.noiseAmp * noiseDist(rng);
                }

                // QRN: impulse noise
                if (params.qrnRate > 0 && uniformDist(rng) < params.qrnRate) {
                    float impulse = params.qrnAmp * (uniformDist(rng) > 0.5f ? 1.0f : -1.0f);
                    re += impulse;
                    im += impulse * (uniformDist(rng) > 0.5f ? 1.0f : -1.0f);
                }

                buf[i].re = re;
                buf[i].im = im;
                phase += omega;
                if (phase > M_PI) phase -= 2.0f * M_PI;
                totalSamples++;
            }
        }

        // Apply jitter to a duration in ms
        float jitter(float ms) {
            if (params.jitterPct <= 0) return ms;
            float variation = params.jitterPct * ms * noiseDist(rng);
            return std::max(ms * 0.3f, ms + variation);
        }

        // Apply weight bias to dit/dah durations
        // Positive bias = heavier dits (longer), negative = lighter dits
        float weightedDit() {
            return params.ditMs * (1.0f + params.weightBias);
        }
        float weightedDah() {
            return params.ditMs * 3.0f * (1.0f - params.weightBias * 0.5f);
        }

        SignalParams params;
    private:
        float phase = 0;
        float qsbPhase = 0;
        float qrmPhase = 0;
        float envState = 0;
        long long totalSamples = 0;
        std::mt19937 rng;
        std::normal_distribution<float> noiseDist;
        std::uniform_real_distribution<float> uniformDist;
    };

    // ── Full message generator: text → IQ samples ──

    struct GeneratedSignal {
        std::vector<dsp::complex_t> samples;
        std::string sourceText;  // canonical text that was encoded
    };

    inline GeneratedSignal generateMessage(const std::string& text, SignalParams params) {
        CWSignalGenerator gen;
        gen.init(params);

        GeneratedSignal result;
        result.sourceText = "";

        // 200ms warmup silence — enough for noise floor to converge
        int warmupSamples = (int)(0.2f * params.sampleRate);
        result.samples.resize(warmupSamples);
        gen.generate(result.samples.data(), warmupSamples, false);

        int chunkMax = 65536;
        std::vector<dsp::complex_t> buf(chunkMax);

        auto appendSilence = [&](float ms) {
            int samples = std::max(1, (int)(ms / 1000.0f * params.sampleRate));
            buf.resize(std::max((int)buf.size(), samples));
            gen.generate(buf.data(), samples, false);
            result.samples.insert(result.samples.end(), buf.data(), buf.data() + samples);
        };

        auto appendTone = [&](float ms) {
            int samples = std::max(1, (int)(ms / 1000.0f * params.sampleRate));
            buf.resize(std::max((int)buf.size(), samples));
            gen.generate(buf.data(), samples, true);
            result.samples.insert(result.samples.end(), buf.data(), buf.data() + samples);
        };

        for (size_t ci = 0; ci < text.size(); ci++) {
            char c = toupper(text[ci]);
            if (c == ' ') {
                // Word gap: 7 dit lengths, stretched by Farnsworth ratio.
                appendSilence(gen.jitter(params.ditMs * 7.0f * params.farnsworthRatio));
                result.sourceText += ' ';
                continue;
            }

            std::string morse = charToMorse(c);
            if (morse.empty()) continue;

            result.sourceText += c;

            for (size_t ei = 0; ei < morse.size(); ei++) {
                float onMs = (morse[ei] == '.') ? gen.jitter(gen.weightedDit())
                                                 : gen.jitter(gen.weightedDah());
                appendTone(onMs);
                if (ei + 1 < morse.size()) {
                    appendSilence(gen.jitter(params.ditMs));  // element gap
                }
            }

            // Character gap (3 dit lengths), stretched by Farnsworth ratio.
            if (ci + 1 < text.size() && text[ci + 1] != ' ') {
                appendSilence(gen.jitter(params.ditMs * 3.0f * params.farnsworthRatio));
            }
        }

        // Trailing word gap to flush decoder
        appendSilence(gen.jitter(params.ditMs * 10.0f));

        return result;
    }

    // ── CER / WER scoring ──

    // Levenshtein edit distance
    inline int editDistance(const std::string& a, const std::string& b) {
        int m = a.size(), n = b.size();
        std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
        for (int i = 0; i <= m; i++) dp[i][0] = i;
        for (int j = 0; j <= n; j++) dp[0][j] = j;
        for (int i = 1; i <= m; i++) {
            for (int j = 1; j <= n; j++) {
                int cost = (a[i-1] != b[j-1]) ? 1 : 0;
                dp[i][j] = std::min({dp[i-1][j] + 1, dp[i][j-1] + 1, dp[i-1][j-1] + cost});
            }
        }
        return dp[m][n];
    }

    // Normalize string: uppercase, collapse whitespace, trim
    inline std::string normalize(const std::string& s) {
        std::string out;
        bool lastSpace = true;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\n') {
                if (!lastSpace) { out += ' '; lastSpace = true; }
            } else {
                out += toupper(c);
                lastSpace = false;
            }
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }

    // Split into words
    inline std::vector<std::string> splitWords(const std::string& s) {
        std::vector<std::string> words;
        std::string word;
        for (char c : s) {
            if (c == ' ') {
                if (!word.empty()) { words.push_back(word); word.clear(); }
            } else {
                word += c;
            }
        }
        if (!word.empty()) words.push_back(word);
        return words;
    }

    struct DecodeScore {
        float cer;        // Character Error Rate: edit_distance / reference_length
        float wer;        // Word Error Rate: word_edit_distance / reference_word_count
        int refChars;     // reference character count
        int refWords;     // reference word count
        int charErrors;   // character-level edit distance
        int wordErrors;   // word-level edit distance
    };

    inline DecodeScore score(const std::string& reference, const std::string& decoded) {
        std::string ref = normalize(reference);
        std::string dec = normalize(decoded);

        DecodeScore s;
        s.refChars = ref.size();
        s.charErrors = editDistance(ref, dec);
        s.cer = s.refChars > 0 ? (float)s.charErrors / s.refChars : (dec.empty() ? 0.0f : 1.0f);

        auto refWords = splitWords(ref);
        auto decWords = splitWords(dec);
        s.refWords = refWords.size();

        // Word-level edit distance
        int m = refWords.size(), n = decWords.size();
        std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));
        for (int i = 0; i <= m; i++) dp[i][0] = i;
        for (int j = 0; j <= n; j++) dp[0][j] = j;
        for (int i = 1; i <= m; i++) {
            for (int j = 1; j <= n; j++) {
                int cost = (refWords[i-1] != decWords[j-1]) ? 1 : 0;
                dp[i][j] = std::min({dp[i-1][j] + 1, dp[i][j-1] + 1, dp[i-1][j-1] + cost});
            }
        }
        s.wordErrors = dp[m][n];
        s.wer = s.refWords > 0 ? (float)s.wordErrors / s.refWords : (decWords.empty() ? 0.0f : 1.0f);

        return s;
    }

    // ── Predefined test messages ──

    inline const char* MSG_CQ()      { return "CQ CQ CQ DE W1AW W1AW K"; }
    inline const char* MSG_RST()     { return "UR RST 599 599 BK"; }
    inline const char* MSG_QTH()     { return "QTH NEWINGTON CT"; }
    inline const char* MSG_73()      { return "TNX FER QSO 73 SK"; }
    inline const char* MSG_SHORT()   { return "SOS"; }
    inline const char* MSG_MIXED()   { return "CQ TEST DE K1ABC 5NN"; }
    inline const char* MSG_CONTEST() { return "CQ CQ TEST K1ABC K1ABC TEST"; }
    inline const char* MSG_FULL()    { return "CQ CQ CQ DE W1AW W1AW QTH NEWINGTON CT UR RST 599 599 TNX FER QSO 73 SK"; }

    // ── Predefined degradation profiles ──

    inline SignalParams profileClean(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        return p;
    }

    inline SignalParams profileMildNoise(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.noiseAmp = 0.5f;
        return p;
    }

    inline SignalParams profileModerateNoise(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.noiseAmp = 1.5f;
        return p;
    }

    inline SignalParams profileHandKeyed(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.jitterPct = 0.15f;
        p.weightBias = 0.1f;
        p.noiseAmp = 0.3f;
        return p;
    }

    inline SignalParams profileQSB(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.qsbHz = 0.3f;
        p.qsbDepth = 0.7f;
        p.noiseAmp = 0.5f;
        return p;
    }

    inline SignalParams profileQRM(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.qrmFreq = 900.0f;
        p.qrmAmp = 0.6f;
        p.noiseAmp = 0.3f;
        return p;
    }

    inline SignalParams profileQRN(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.qrnRate = 0.001f;
        p.qrnAmp = 3.0f;
        p.noiseAmp = 0.3f;
        return p;
    }

    inline SignalParams profileContest(float ditMs = 60.0f) {
        // Fast CW, hand-keyed, moderate noise, some QRM
        SignalParams p;
        p.ditMs = ditMs;
        p.jitterPct = 0.10f;
        p.noiseAmp = 0.8f;
        p.qrmFreq = 850.0f;
        p.qrmAmp = 0.3f;
        return p;
    }

    inline SignalParams profileWorstCase(float ditMs = 80.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.noiseAmp = 1.5f;
        p.jitterPct = 0.20f;
        p.weightBias = 0.15f;
        p.qsbHz = 0.5f;
        p.qsbDepth = 0.5f;
        p.qrmFreq = 850.0f;
        p.qrmAmp = 0.4f;
        p.qrnRate = 0.0005f;
        p.qrnAmp = 2.0f;
        return p;
    }

    inline SignalParams profileFarnsworth(float ditMs = 80.0f, float ratio = 1.5f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.farnsworthRatio = ratio;
        return p;
    }

    inline SignalParams profileFarnsworthNoisy(float ditMs = 80.0f, float ratio = 1.5f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.farnsworthRatio = ratio;
        p.noiseAmp = 0.3f;
        return p;
    }

    // ── Decode helpers (full IQ path, no mocks) ──

    inline std::string decode(const std::string& message, const SignalParams& params) {
        auto sig = generateMessage(message, params);
        cw::Channel ch;
        ch.init(0, params.toneFreq);
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        return ch.text.getText();
    }

    inline DecodeScore decodeAndScore(const std::string& message, const SignalParams& params) {
        auto sig = generateMessage(message, params);
        cw::Channel ch;
        ch.init(0, params.toneFreq);
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }
        std::string decoded = ch.text.getText();
        return score(sig.sourceText, decoded);
    }
}
