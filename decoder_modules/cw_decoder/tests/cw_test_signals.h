#pragma once
#include <dsp/types.h>
// decode()/decodeAndScore() below construct a cw::Channel. Included here rather
// than left to the includer: relying on include order made adding a new harness
// fail with "no type named 'Channel'" from inside this file.
#include <cw/channel.h>
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

    // Ground truth: what the generator actually keyed, sample-exact.
    //
    // Oracle ablation replaces a pipeline stage with a perfect one to measure
    // that stage's headroom. Detector scoring compares emitted key events
    // against these boundaries. Both need the semantic label, not just the
    // edge, so segments carry their element/gap class.
    enum TruthKind {
        TRUTH_DIT, TRUTH_DAH,
        TRUTH_ELEMENT_GAP, TRUTH_CHAR_GAP, TRUTH_WORD_GAP,
        TRUTH_WARMUP, TRUTH_TRAILING
    };

    struct TruthSegment {
        bool tone;
        long long startSample;   // inclusive, at params.sampleRate
        long long endSample;     // exclusive
        TruthKind kind;
    };

    // The durations the generator actually keys, before jitter. Recorded here
    // rather than re-derived by consumers: weightBias and farnsworthRatio move
    // these away from the nominal 1:3 and 1:3:7 ratios, and an oracle that
    // re-derives them silently drifts when the generator changes.
    struct ElementModel {
        float nominalDitMs = 0;   // 1200/WPM — the speed, unaffected by weighting
        float ditMs = 0;          // weighted
        float dahMs = 0;          // weighted
        float elemGapMs = 0;
        float charGapMs = 0;      // Farnsworth-stretched
        float wordGapMs = 0;      // Farnsworth-stretched
    };

    struct GeneratedSignal {
        std::vector<dsp::complex_t> samples;
        std::string sourceText;  // canonical text that was encoded
        std::vector<TruthSegment> segments;
        ElementModel model;
    };

    inline GeneratedSignal generateMessage(const std::string& text, SignalParams params) {
        CWSignalGenerator gen;
        gen.init(params);

        GeneratedSignal result;
        result.sourceText = "";

        // Taken from the generator itself, so the two cannot disagree.
        result.model.nominalDitMs = params.ditMs;
        result.model.ditMs       = gen.weightedDit();
        result.model.dahMs       = gen.weightedDah();
        result.model.elemGapMs   = params.ditMs;
        result.model.charGapMs   = params.ditMs * 3.0f * params.farnsworthRatio;
        result.model.wordGapMs   = params.ditMs * 7.0f * params.farnsworthRatio;

        // 200ms warmup silence — enough for noise floor to converge
        int warmupSamples = (int)(0.2f * params.sampleRate);
        result.samples.resize(warmupSamples);
        gen.generate(result.samples.data(), warmupSamples, false);
        result.segments.push_back({false, 0, warmupSamples, TRUTH_WARMUP});

        int chunkMax = 65536;
        std::vector<dsp::complex_t> buf(chunkMax);

        // Single append path so tone/silence draw from the RNG in the same
        // order as before the truth recording was added.
        auto append = [&](float ms, bool tone, TruthKind kind) {
            int samples = std::max(1, (int)(ms / 1000.0f * params.sampleRate));
            buf.resize(std::max((int)buf.size(), samples));
            gen.generate(buf.data(), samples, tone);
            long long start = (long long)result.samples.size();
            result.samples.insert(result.samples.end(), buf.data(), buf.data() + samples);
            result.segments.push_back({tone, start, start + samples, kind});
        };

        for (size_t ci = 0; ci < text.size(); ci++) {
            char c = toupper(text[ci]);
            if (c == ' ') {
                // Word gap: 7 dit lengths, stretched by Farnsworth ratio.
                append(gen.jitter(params.ditMs * 7.0f * params.farnsworthRatio), false, TRUTH_WORD_GAP);
                result.sourceText += ' ';
                continue;
            }

            std::string morse = charToMorse(c);
            if (morse.empty()) continue;

            result.sourceText += c;

            for (size_t ei = 0; ei < morse.size(); ei++) {
                bool isDit = (morse[ei] == '.');
                float onMs = isDit ? gen.jitter(gen.weightedDit())
                                   : gen.jitter(gen.weightedDah());
                append(onMs, true, isDit ? TRUTH_DIT : TRUTH_DAH);
                if (ei + 1 < morse.size()) {
                    append(gen.jitter(params.ditMs), false, TRUTH_ELEMENT_GAP);
                }
            }

            // Character gap (3 dit lengths), stretched by Farnsworth ratio.
            if (ci + 1 < text.size() && text[ci + 1] != ' ') {
                append(gen.jitter(params.ditMs * 3.0f * params.farnsworthRatio), false, TRUTH_CHAR_GAP);
            }
        }

        // Trailing word gap to flush decoder
        append(gen.jitter(params.ditMs * 10.0f), false, TRUTH_TRAILING);

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

    // ── Factorial profiles ──
    //
    // The named profiles above vary several parameters at once, which makes
    // them useful as scenarios and useless for attribution. Measured 2026-07-20:
    // the noise* set is 15 WPM / jitter 0 / bias 0 while the handkeyed* set is
    // noiseAmp 0.3 / jitter 0.15 / bias 0.1, so comparing the two families
    // attributes to "noise" a difference that also spans speed, jitter and
    // weight bias. This varies exactly what it names.

    inline SignalParams profileFactorial(float ditMs, float noiseAmp,
                                         float jitterPct = 0.0f,
                                         float weightBias = 0.0f) {
        SignalParams p;
        p.ditMs = ditMs;
        p.noiseAmp = noiseAmp;
        p.jitterPct = jitterPct;
        p.weightBias = weightBias;
        return p;
    }

    // ── Canonical profile set ──
    //
    // Five test files each carried their own hardcoded list with divergent
    // membership and divergent naming, which is how hand-keyed coverage came to
    // stop at 25 WPM in every one of them. Adopted by test_matrix.cpp and
    // test_promotion.cpp — the sweeps that decide promotions. The other lists
    // still differ deliberately (oracle omits qrm; gap-noise carries no
    // message) and are left alone.
    //
    // handkeyed-30/35/40 were added after 40 WPM proved to be where candidates
    // diverge most: 0.17 CER between legacy+edge+log and legacy+mf, invisible
    // to every sweep that stopped at 25.

    struct TestProfile {
        const char* name;
        const char* message;
        SignalParams params;
    };

    inline std::vector<TestProfile> standardProfiles() {
        std::vector<TestProfile> v = {
            {"clean-15",     MSG_FULL(), profileClean(80.0f)},
            {"clean-25",     MSG_FULL(), profileClean(48.0f)},
            {"handkeyed-15", MSG_FULL(), profileHandKeyed(80.0f)},
            {"handkeyed-20", MSG_FULL(), profileHandKeyed(60.0f)},
            {"handkeyed-25", MSG_FULL(), profileHandKeyed(48.0f)},
            {"handkeyed-30", MSG_FULL(), profileHandKeyed(40.0f)},
            {"handkeyed-35", MSG_FULL(), profileHandKeyed(34.3f)},
            {"handkeyed-40", MSG_FULL(), profileHandKeyed(30.0f)},
            {"qsb",          MSG_FULL(), profileQSB(80.0f)},
            {"qrm",          MSG_FULL(), profileQRM(80.0f)},
            {"qrn",          MSG_FULL(), profileQRN(80.0f)},
            {"farnsworth20", MSG_FULL(), profileFarnsworth(80.0f, 2.0f)},
            {"worstcase",    MSG_FULL(), profileWorstCase(80.0f)},
        };
        for (float amp : {2.0f, 3.0f, 4.0f}) {
            SignalParams p = profileClean(80.0f);
            p.noiseAmp = amp;
            v.push_back({amp == 2.0f ? "noise2.0" : (amp == 3.0f ? "noise3.0" : "noise4.0"),
                         MSG_FULL(), p});
        }
        // Fast CW under noise: the noise axis above is all 15 WPM, which hid a
        // detector regression at speed (§25) AND a speed-switch regression at
        // fast×heavy noise (§43) — the cells where this campaign keeps failing.
        // The full 25/30 WPM × noise 2/3/4 grid is gated so no future candidate
        // is adjudicated blind to that regime, even though 3.0/4.0 are garbage
        // cells (CER > 0.9): a candidate that worsens them is misfiring.
        struct FastCell { float dit; float amp; const char* name; };
        const FastCell fast[] = {
            {48.0f, 2.0f, "noise2.0-25wpm"}, {40.0f, 2.0f, "noise2.0-30wpm"},
            {48.0f, 3.0f, "noise3.0-25wpm"}, {40.0f, 3.0f, "noise3.0-30wpm"},
            {48.0f, 4.0f, "noise4.0-25wpm"}, {40.0f, 4.0f, "noise4.0-30wpm"},
        };
        for (const auto& f : fast) {
            SignalParams p = profileClean(f.dit);
            p.noiseAmp = f.amp;
            v.push_back({f.name, MSG_FULL(), p});
        }
        // Weak hand-keyed: jitter 0.15 AND heavy noise. A jitter-only regime gate
        // (§48) would pick log here (jittered) but log loses under heavy noise
        // (§46b), so these gate the SNR condition — jitter-alone must not pass.
        struct WeakHk { float dit; float amp; const char* name; };
        const WeakHk weak[] = {
            {60.0f, 1.5f, "hk20-n1.5"}, {48.0f, 1.5f, "hk25-n1.5"}, {40.0f, 1.5f, "hk30-n1.5"},
        };
        for (const auto& w : weak) {
            SignalParams p = profileHandKeyed(w.dit);
            p.noiseAmp = w.amp;
            v.push_back({w.name, MSG_FULL(), p});
        }
        return v;
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
