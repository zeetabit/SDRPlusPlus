#pragma once
#include <cw/channel.h>
#include "cw_test_signals.h"
#include "cw_bench_stats.h"
#include <chrono>

// Core × profile benchmark matrix.
//
// Every core in cw::coreRegistry() is run over every profile across N seeds.
// Because Channel owns post-processing identically for all cores, differences
// between rows are attributable to decoding.

namespace cw_test {

    struct MatrixCell {
        std::string core;
        std::string profile;

        float cerMean = 0, cerP95 = 0, werMean = 0;
        float insRate = 0, delRate = 0, subRate = 0;   // per reference character
        float realtimeX = 0;                            // audio secs / decode secs
        float ttfoMs = 0;                               // time to first output
        float wpmRmsErr = 0;                            // vs true WPM, after lock
        int   seeds = 0;
    };

    // One decode run instrumented for every metric family.
    struct RunResult {
        std::string text;
        float decodeSecs = 0;
        float audioSecs = 0;
        float ttfoMs = -1;      // <0 = never produced output
        float wpmRmsErr = -1;   // <0 = never locked
    };

    // Built after the signal exists: an oracle core closes over that signal's
    // ground truth, so it cannot be constructed from a registry name.
    using CoreFactory = std::function<std::unique_ptr<cw::IDecodeCore>(const GeneratedSignal&)>;

    inline RunResult runInstrumentedCore(const std::string& message,
                                         const SignalParams& params,
                                         const CoreFactory& makeCore,
                                         const std::string& label) {
        auto sig = generateMessage(message, params);
        cw::Channel ch;
        ch.initWithCore(0, params.toneFreq, makeCore(sig), label);

        RunResult r;
        r.audioSecs = (float)sig.samples.size() / params.sampleRate;
        const float trueWpm = 1200.0f / params.ditMs;

        double sqErr = 0;
        int wpmSamples = 0;
        bool sawOutput = false;

        auto t0 = std::chrono::steady_clock::now();
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);

            if (!sawOutput && !ch.text.getText().empty()) {
                sawOutput = true;
                r.ttfoMs = (float)(off + n) / params.sampleRate * 1000.0f;
            }
            // Sample WPM tracking once locked (wpm > 0 means timing produced an
            // estimate). Compared against the generator's true speed.
            if (ch.wpm > 0.0f) {
                double e = ch.wpm - trueWpm;
                sqErr += e * e;
                wpmSamples++;
            }
        }
        auto t1 = std::chrono::steady_clock::now();

        r.decodeSecs = std::chrono::duration<float>(t1 - t0).count();
        r.text = ch.text.getText();
        if (wpmSamples > 0) { r.wpmRmsErr = (float)std::sqrt(sqErr / wpmSamples); }
        return r;
    }

    inline RunResult runInstrumented(const std::string& message,
                                     const SignalParams& params,
                                     const std::string& coreName) {
        const cw::CoreSpec* spec = cw::findCore(coreName);
        if (!spec) { spec = cw::findCore(cw::DEFAULT_CORE); }
        return runInstrumentedCore(message, params,
                                   [spec](const GeneratedSignal&) { return spec->make(); },
                                   spec->name);
    }

    inline MatrixCell runCellWith(const CoreFactory& makeCore,
                                  const std::string& coreLabel,
                                  const std::string& profileName,
                                  const std::string& message,
                                  SignalParams params,
                                  int nSeeds = 24,
                                  unsigned seedBase = 1000) {
        MatrixCell c;
        c.core = coreLabel;
        c.profile = profileName;
        c.seeds = nSeeds;

        std::vector<float> cers;
        float werSum = 0;
        long insSum = 0, delSum = 0, subSum = 0, refSum = 0;
        float audioSum = 0, decodeSum = 0;
        float ttfoSum = 0; int ttfoN = 0;
        double wpmSq = 0; int wpmN = 0;

        for (int i = 0; i < nSeeds; i++) {
            params.seed = seedBase + (unsigned)i * 7919u;
            auto r = runInstrumentedCore(message, params, makeCore, coreLabel);

            auto s = score(message, r.text);
            cers.push_back(s.cer);
            werSum += s.wer;

            auto e = alignErrors(message, r.text);
            insSum += e.ins; delSum += e.del; subSum += e.sub; refSum += e.refLen;

            audioSum  += r.audioSecs;
            decodeSum += r.decodeSecs;
            if (r.ttfoMs >= 0) { ttfoSum += r.ttfoMs; ttfoN++; }
            if (r.wpmRmsErr >= 0) { wpmSq += (double)r.wpmRmsErr * r.wpmRmsErr; wpmN++; }
        }

        auto st = summarize(cers, werSum);
        c.cerMean = st.mean;
        c.cerP95  = st.p95;
        c.werMean = st.meanWER;

        if (refSum > 0) {
            c.insRate = (float)insSum / refSum;
            c.delRate = (float)delSum / refSum;
            c.subRate = (float)subSum / refSum;
        }
        c.realtimeX = decodeSum > 0 ? audioSum / decodeSum : 0;
        c.ttfoMs    = ttfoN > 0 ? ttfoSum / ttfoN : -1;
        c.wpmRmsErr = wpmN  > 0 ? (float)std::sqrt(wpmSq / wpmN) : -1;
        return c;
    }

    inline MatrixCell runCell(const std::string& coreName,
                              const std::string& profileName,
                              const std::string& message,
                              SignalParams params,
                              int nSeeds = 24,
                              unsigned seedBase = 1000) {
        const cw::CoreSpec* spec = cw::findCore(coreName);
        if (!spec) { spec = cw::findCore(cw::DEFAULT_CORE); }
        return runCellWith([spec](const GeneratedSignal&) { return spec->make(); },
                           coreName, profileName, message, params, nSeeds, seedBase);
    }
}
