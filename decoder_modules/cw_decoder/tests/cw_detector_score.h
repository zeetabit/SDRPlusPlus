#pragma once
#include <cw/staged_core.h>
#include <cw/stages.h>
#include "cw_test_signals.h"
#include "cw_oracle.h"
#include <cmath>
#include <numeric>

// Detector ground-truth scoring.
//
// §11 settled *attribution* — on the AWGN family all the error is at the
// detector. This settles *kind*: is the detector hallucinating events, dropping
// them, or placing them at the wrong time? Those call for different fixes, and
// CER cannot tell them apart.
//
// Specifically it measures the magnitude of the Schmitt asymmetry predicted in
// §2.3(h), which is the most load-bearing unmeasured claim in the docs.

namespace cw_test {

    // ── Capture what the real detector emits, in absolute time ──
    //
    // A decorator rather than a reimplementation of StagedCore's chain: the
    // events must come through the real front end and matched filter, or the
    // group delay and edge shaping being measured are not the ones the decoder
    // actually sees.
    class RecordingDetector : public cw::IDetector {
    public:
        explicit RecordingDetector(std::unique_ptr<cw::IDetector> in) : inner(std::move(in)) {}

        void init(float rate) override { inner->init(rate); }
        void reset() override { inner->reset(); counter = 0; captured.clear(); }

        std::vector<cw::KeyEvent> process(const float* env, int count) override {
            auto evts = inner->process(env, count);
            for (const auto& e : evts) {
                captured.push_back({counter + e.sampleOffset, e.keyDown});
            }
            counter += count;
            return evts;
        }

        float getSNR() const override { return inner->getSNR(); }
        bool isKeyDown() const override { return inner->isKeyDown(); }
        const char* name() const override { return "recording"; }
        void preseed(float l, int c) override { inner->preseed(l, c); }

        const std::vector<TruthTransition>& events() const { return captured; }
        long long processed() const { return counter; }

    private:
        std::unique_ptr<cw::IDetector> inner;
        std::vector<TruthTransition> captured;
        long long counter = 0;
    };

    struct DetectorScore {
        float groupDelayMs   = 0;   // estimated constant latency, removed before scoring
        float falseRate      = 0;   // spurious transitions per true element
        float missRate       = 0;   // true elements with no detected edge
        float onStretchMs    = 0;   // d_up - d_down; positive = ON stretched, gap shortened
        float onStretchPct   = 0;   // as % of the true dit
        float edgeJitterMs   = 0;   // sd of edge delay about its per-polarity mean
        float edgeJitterPct  = 0;
        int trueTransitions  = 0;
        int detectedTransitions = 0;
        int matched          = 0;
    };

    namespace detail {

        inline std::vector<uint8_t> keyStateArray(const std::vector<TruthTransition>& tr,
                                                  long long len) {
            std::vector<uint8_t> s((size_t)len, 0);
            bool cur = false;
            long long pos = 0;
            for (const auto& t : tr) {
                long long end = std::min(t.sample, len);
                for (long long i = pos; i < end; i++) { s[(size_t)i] = cur; }
                if (end >= len) { return s; }
                pos = std::max(0LL, end);
                cur = t.keyDown;
            }
            for (long long i = pos; i < len; i++) { s[(size_t)i] = cur; }
            return s;
        }

        // Constant latency is larger than half a dit at 25 WPM, so windowed
        // matching must know it first. Cross-correlating the two key-state
        // square waves is robust to heavy false detection in a way that
        // matching the first few edges is not.
        inline int estimateLagSamples(const std::vector<uint8_t>& truth,
                                      const std::vector<uint8_t>& det,
                                      int maxLag) {
            long long best = -1;
            int bestLag = 0;
            for (int lag = 0; lag <= maxLag; lag++) {
                long long agree = 0;
                size_t n = std::min(truth.size(), det.size() - (size_t)std::min((size_t)lag, det.size()));
                for (size_t i = 0; i < n; i++) {
                    agree += (truth[i] == det[i + lag]);
                }
                if (agree > best) { best = agree; bestLag = lag; }
            }
            return bestLag;
        }

    }

    // Aligns detected transitions to truth allowing insertions and deletions,
    // then reports where the surviving matches sit in time.
    inline DetectorScore scoreDetector(const std::vector<TruthTransition>& truth,
                                       const std::vector<TruthTransition>& det,
                                       long long lenSamples,
                                       float internalRate,
                                       float ditMs,
                                       std::vector<TruthTransition>* falseOut = nullptr,
                                       std::vector<TruthTransition>* missOut = nullptr) {
        DetectorScore s;
        s.trueTransitions = (int)truth.size();
        s.detectedTransitions = (int)det.size();
        if (truth.empty()) { return s; }

        const float msPerSample = 1000.0f / internalRate;

        auto tState = detail::keyStateArray(truth, lenSamples);
        auto dState = detail::keyStateArray(det, lenSamples);
        const int maxLag = (int)(0.200f * internalRate);
        const int lag = detail::estimateLagSamples(tState, dState, maxLag);
        s.groupDelayMs = lag * msPerSample;

        // DP alignment: match / false-detection / miss.
        const float window = std::max(4.0f, 0.4f * ditMs) / msPerSample;
        const int n = (int)truth.size(), m = (int)det.size();
        const float INF = 1e9f;
        std::vector<std::vector<float>> cost(n + 1, std::vector<float>(m + 1, INF));
        std::vector<std::vector<char>> back(n + 1, std::vector<char>(m + 1, 0));
        cost[0][0] = 0;
        for (int i = 1; i <= n; i++) { cost[i][0] = i; back[i][0] = 'd'; }
        for (int j = 1; j <= m; j++) { cost[0][j] = j; back[0][j] = 'i'; }

        for (int i = 1; i <= n; i++) {
            for (int j = 1; j <= m; j++) {
                float bestC = cost[i-1][j] + 1.0f; char bestB = 'd';
                if (cost[i][j-1] + 1.0f < bestC) { bestC = cost[i][j-1] + 1.0f; bestB = 'i'; }
                float dt = (float)(det[j-1].sample - lag - truth[i-1].sample);
                bool compatible = truth[i-1].keyDown == det[j-1].keyDown
                                  && std::fabs(dt) <= window;
                if (compatible && cost[i-1][j-1] < bestC) {
                    bestC = cost[i-1][j-1]; bestB = 'm';
                }
                cost[i][j] = bestC; back[i][j] = bestB;
            }
        }

        std::vector<float> downDelays, upDelays;
        int i = n, j = m, misses = 0, falses = 0;
        while (i > 0 || j > 0) {
            char b = back[i][j];
            if (i > 0 && j > 0 && b == 'm') {
                float dtMs = (float)(det[j-1].sample - lag - truth[i-1].sample) * msPerSample;
                (truth[i-1].keyDown ? downDelays : upDelays).push_back(dtMs);
                i--; j--;
            } else if (j > 0 && b == 'i') {
                falses++;
                if (falseOut) { falseOut->push_back(det[j-1]); }
                j--;
            } else if (i > 0) {
                misses++;
                if (missOut) { missOut->push_back(truth[i-1]); }
                i--;
            } else { j--; }
        }

        s.matched = (int)(downDelays.size() + upDelays.size());
        const float trueElements = (float)truth.size() / 2.0f;
        s.falseRate = falses / trueElements;
        s.missRate  = misses / trueElements;

        auto meanOf = [](const std::vector<float>& v) {
            return v.empty() ? 0.0f : std::accumulate(v.begin(), v.end(), 0.0f) / v.size();
        };
        const float mDown = meanOf(downDelays), mUp = meanOf(upDelays);

        // Differential, not absolute: the front end's constant group delay
        // cancels in the decode path (timing consumes only differences), so
        // only the asymmetry between edges biases the dit/dah ratio.
        s.onStretchMs = mUp - mDown;
        s.onStretchPct = ditMs > 0 ? 100.0f * s.onStretchMs / ditMs : 0;

        double sq = 0; int cnt = 0;
        for (float d : downDelays) { sq += (d - mDown) * (d - mDown); cnt++; }
        for (float d : upDelays)   { sq += (d - mUp)   * (d - mUp);   cnt++; }
        s.edgeJitterMs = cnt > 1 ? (float)std::sqrt(sq / cnt) : 0.0f;
        s.edgeJitterPct = ditMs > 0 ? 100.0f * s.edgeJitterMs / ditMs : 0;

        return s;
    }

    // Runs the real pipeline once and scores its detector against ground truth.
    inline DetectorScore measureDetector(const std::string& message, SignalParams params,
                                         cw::MatchedFilterResize mfResize = cw::MF_RESET,
                                         cw::EdgeBias edgeBias = cw::EDGE_RAW,
                                         cw::PeakTracker peak = cw::PEAK_INSTANT_ATTACK,
                                         float peakAttackMs = 300.0f) {
        auto sig = generateMessage(message, params);

        auto rec = std::make_unique<RecordingDetector>(
                       std::make_unique<cw::SchmittDetector>(edgeBias, peak, peakAttackMs));
        RecordingDetector* probe = rec.get();

        auto core = std::make_unique<cw::StagedCore>(
            std::make_unique<cw::EnvelopeFrontEnd>(),
            std::move(rec),
            std::make_unique<cw::AdaptiveTimingStage>(cw::TIMING_KALMAN),
            std::make_unique<cw::BeamSymbolDecoder>(),
            mfResize);

        cw::Channel ch;
        ch.initWithCore(0, params.toneFreq, std::move(core), "probe");
        for (int off = 0; off < (int)sig.samples.size(); off += 512) {
            int n = std::min(512, (int)sig.samples.size() - off);
            ch.process(n, &sig.samples[off]);
        }

        const float internalRate = 1000.0f;
        return scoreDetector(truthTransitions(sig, params.sampleRate, internalRate),
                             probe->events(), probe->processed(),
                             internalRate, sig.model.ditMs);
    }

    struct DetectorStats {
        DetectorScore mean;
        int seeds = 0;
    };

    inline DetectorStats measureDetectorMulti(const std::string& message,
                                              SignalParams params,
                                              int nSeeds = 24,
                                              unsigned seedBase = 1000,
                                              cw::MatchedFilterResize mfResize = cw::MF_RESET,
                                              cw::EdgeBias edgeBias = cw::EDGE_RAW,
                                              cw::PeakTracker peak = cw::PEAK_INSTANT_ATTACK,
                                              float peakAttackMs = 300.0f) {
        DetectorStats out;
        out.seeds = nSeeds;
        DetectorScore acc;
        for (int i = 0; i < nSeeds; i++) {
            params.seed = seedBase + (unsigned)i * 7919u;
            auto s = measureDetector(message, params, mfResize, edgeBias, peak, peakAttackMs);
            acc.groupDelayMs  += s.groupDelayMs;
            acc.falseRate     += s.falseRate;
            acc.missRate      += s.missRate;
            acc.onStretchMs   += s.onStretchMs;
            acc.onStretchPct  += s.onStretchPct;
            acc.edgeJitterMs  += s.edgeJitterMs;
            acc.edgeJitterPct += s.edgeJitterPct;
            acc.trueTransitions += s.trueTransitions;
            acc.detectedTransitions += s.detectedTransitions;
            acc.matched += s.matched;
        }
        const float k = 1.0f / nSeeds;
        out.mean.groupDelayMs  = acc.groupDelayMs  * k;
        out.mean.falseRate     = acc.falseRate     * k;
        out.mean.missRate      = acc.missRate      * k;
        out.mean.onStretchMs   = acc.onStretchMs   * k;
        out.mean.onStretchPct  = acc.onStretchPct  * k;
        out.mean.edgeJitterMs  = acc.edgeJitterMs  * k;
        out.mean.edgeJitterPct = acc.edgeJitterPct * k;
        out.mean.trueTransitions = acc.trueTransitions / nSeeds;
        out.mean.detectedTransitions = acc.detectedTransitions / nSeeds;
        out.mean.matched = acc.matched / nSeeds;
        return out;
    }
}
