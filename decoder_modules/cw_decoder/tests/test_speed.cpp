#include <catch.hpp>
#include <cw/channel.h>
#include <cw/dsp.h>
#include <cw/tone_detector.h>
#include "cw_test_signals.h"
#include <chrono>

using namespace cw_test;
using clk = std::chrono::high_resolution_clock;

TEST_CASE("Profile: where time goes for MSG_FULL", "[speed]") {
    auto params = profileClean(80.0f);
    auto sig = generateMessage(MSG_FULL(), params);
    int total = sig.samples.size();

    // 1. Measure IQ generation time (already done above, but measure standalone)
    auto t0 = clk::now();
    auto sig2 = generateMessage(MSG_FULL(), params);
    auto t1 = clk::now();
    long genMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 2. Measure DSP chain only (IQ → envelope)
    cw::EnvelopeDSP dsp;
    dsp.init(700.0f, 8000.0f, 1000.0f);
    std::vector<float> env(total / 8 + 1000);
    t0 = clk::now();
    int envCount = 0;
    for (int off = 0; off < total; off += 512) {
        int n = std::min(512, total - off);
        envCount += dsp.process(n, &sig.samples[off], &env[envCount]);
    }
    t1 = clk::now();
    long dspMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 3. Measure detector only (envelope → key events)
    cw::ToneDetector det;
    det.init(1000.0f);
    det.preseed(0.001f, 500);
    t0 = clk::now();
    auto events = det.process(env.data(), envCount);
    t1 = clk::now();
    long detMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // 4. Measure full channel decode
    t0 = clk::now();
    auto s = decodeAndScore(MSG_FULL(), params);
    t1 = clk::now();
    long fullMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    WARN("MSG_FULL = " << strlen(MSG_FULL()) << " chars, " << total << " IQ samples, " << envCount << " env samples");
    WARN("IQ generation:  " << genMs << "ms");
    WARN("DSP chain:      " << dspMs << "ms");
    WARN("Detector:       " << detMs << "ms (" << events.size() << " events)");
    WARN("Full decode:    " << fullMs << "ms, CER=" << s.cer);
}
