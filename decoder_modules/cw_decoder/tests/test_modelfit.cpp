#include <catch.hpp>
#include <cw/model_fit.h>
#include <cw/channel.h>
#include "cw_test_signals.h"
#include <vector>
#include <random>
#include <cmath>
#include <cstdio>

// §52 lever #41 — model-fit quality metric, Stage 0/1.
//
// The scorer's whole claim is that its LLR is ORTHOGONAL to SNR: a loud steady
// carrier (high SNR) is unimodal and must score BELOW keyed CW (bimodal), because
// that is precisely the false positive the channel manager's `snr > 6.0` gate
// passes. These probes synthesise controlled envelopes (Rayleigh noise floor,
// Rician-ish mark level) to isolate the discriminator; Stage 2/3 validates on real
// front-end envelopes through the channel manager.

using namespace cw;

namespace {
    // Rayleigh-distributed envelope: magnitude of complex Gaussian, scale sigma.
    float rayleigh(std::mt19937& g, float sigma) {
        std::normal_distribution<float> nd(0.0f, sigma);
        float re = nd(g), im = nd(g);
        return std::sqrt(re * re + im * im);
    }

    // Envelope around a mark tone of amplitude A with additive noise sigma (Rician).
    float rician(std::mt19937& g, float A, float sigma) {
        std::normal_distribution<float> nd(0.0f, sigma);
        float re = A + nd(g), im = nd(g);
        return std::sqrt(re * re + im * im);
    }

    struct Env { std::vector<float> v; float meanSnrDb; };

    // Amplitude "SNR" in dB the way the channel manager reasons about it: ratio of
    // the loud-portion mean to the noise-floor mean. A carrier and keyed CW at the
    // same mark amplitude report the SAME SNR here — the point of the test.
    float snrDb(float markMean, float noiseMean) {
        return 20.0f * std::log10(std::max(markMean, 1e-6f) / std::max(noiseMean, 1e-6f));
    }

    Env genNoise(std::mt19937& g, int n, float sigma) {
        Env e; e.v.reserve(n);
        for (int i = 0; i < n; i++) { e.v.push_back(rayleigh(g, sigma)); }
        e.meanSnrDb = 0.0f;
        return e;
    }

    Env genCarrier(std::mt19937& g, int n, float A, float sigma) {
        Env e; e.v.reserve(n);
        for (int i = 0; i < n; i++) { e.v.push_back(rician(g, A, sigma)); }
        e.meanSnrDb = snrDb(A, sigma * 1.253f);   // Rayleigh mean = sigma*sqrt(pi/2)
        return e;
    }

    // Keyed CW: alternating mark (Rician A) / space (Rayleigh) blocks at ~ditLen.
    Env genCW(std::mt19937& g, int n, float A, float sigma, int ditLen, float duty) {
        Env e; e.v.reserve(n);
        int period = std::max(2, (int)(ditLen / std::max(duty, 0.05f)));
        for (int i = 0; i < n; i++) {
            bool mark = (i % period) < (int)(period * duty);
            e.v.push_back(mark ? rician(g, A, sigma) : rayleigh(g, sigma));
        }
        e.meanSnrDb = snrDb(A, sigma * 1.253f);
        return e;
    }
}

TEST_CASE("model-fit vetoes carriers/noise, keeps usable-SNR CW (§52 #41)", "[cw][modelfit]") {
    std::mt19937 g(12345);
    ModelFitScorer sc;
    const int N = 4000;
    const float sigma = 1.0f;

    Env noise    = genNoise(g, N, sigma);
    Env carrier  = genCarrier(g, N, 5.0f, sigma);       // 12 dB steady carrier
    Env carrierL = genCarrier(g, N, 15.0f, sigma);      // ~22 dB LOUD het
    Env cw25     = genCW(g, N, 5.0f, sigma, 40, 0.5f);  // usable-SNR keyed CW
    Env cw15     = genCW(g, N, 5.0f, sigma, 67, 0.5f);
    Env cwWeak   = genCW(g, N, 2.5f, sigma, 40, 0.5f);  // 6 dB — at the sensitivity floor

    auto f = [&](Env& e) { return sc.score(e.v.data(), (int)e.v.size()); };
    ModelFit mNoise = f(noise), mCar = f(carrier), mCarL = f(carrierL),
             mCW25 = f(cw25), mCW15 = f(cw15), mWeak = f(cwWeak);

    printf("\n=== §52 #41 model-fit LLR vs SNR (carrier/het veto) ===\n");
    printf("%-10s %8s %10s %8s %8s %8s\n", "signal", "snrDb", "llr", "muLo", "muHi", "wHi");
    auto row = [](const char* nm, Env& e, ModelFit& m) {
        printf("%-10s %8.2f %10.4f %8.3f %8.3f %8.3f\n",
               nm, e.meanSnrDb, m.llr, m.muLo, m.muHi, m.wHi);
    };
    row("noise",    noise,    mNoise);
    row("carrier",  carrier,  mCar);
    row("carrierL", carrierL, mCarL);
    row("cw-25",    cw25,     mCW25);
    row("cw-15",    cw15,     mCW15);
    row("cw-weak",  cwWeak,   mWeak);
    printf("weak-CW floor: at %.1f dB the two-level structure ~= best unimodal; the\n",
           cwWeak.meanSnrDb);
    printf("metric is a carrier VETO, not a weak-signal detector (that stays SNR's job).\n\n");

    const float THRESH = 0.05f;   // any single cut in [~0, 0.28] separates the two classes

    // 1. Usable-SNR keyed CW clears the threshold; carrier and noise do not.
    CHECK(mCW25.llr > THRESH);
    CHECK(mCW15.llr > THRESH);
    CHECK(mCar.llr  < THRESH);
    CHECK(mNoise.llr < THRESH);

    // 2. The orthogonality that matters: a LOUD het (22 dB, sails past `snr > 6.0`)
    //    still scores below the CW threshold. This is the false positive SNR passes.
    CHECK(carrierL.meanSnrDb > 20.0f);   // huge SNR
    CHECK(mCarL.llr < THRESH);           // still vetoed on fit

    // 3. A bimodal keyer recovers a duty estimate near 0.5.
    CHECK(mCW25.wHi > 0.3f);
    CHECK(mCW25.wHi < 0.7f);
}

// A single LLR threshold separates usable-SNR CW from ALL loud non-Morse across
// varied amplitudes — the discrimination SNR cannot make (a 22 dB het outranks
// a 12 dB CW signal on SNR, but not on fit).
TEST_CASE("model-fit LLR threshold beats any SNR cut for het rejection (§52 #41)", "[cw][modelfit]") {
    std::mt19937 g(999);
    ModelFitScorer sc;
    const int N = 4000;

    float maxNonCW = -1e9f, minCW = 1e9f;
    for (int trial = 0; trial < 8; trial++) {
        Env noise   = genNoise(g, N, 1.0f);
        Env carrier = genCarrier(g, N, 4.0f + trial * 1.5f, 1.0f);   // 12..27 dB hets
        Env cw      = genCW(g, N, 4.5f + (trial % 3), 1.0f, 40 + trial, 0.5f);  // usable CW
        maxNonCW = std::max({maxNonCW, sc.score(noise.v.data(), N).llr,
                                       sc.score(carrier.v.data(), N).llr});
        minCW    = std::min(minCW,     sc.score(cw.v.data(), N).llr);
    }
    printf("\n§52 #41 separation: min usable-CW llr=%.4f  >  max non-CW llr=%.4f\n",
           minCW, maxNonCW);
    CHECK(minCW > maxNonCW);   // a clean LLR threshold exists across all trials
}

// §52.3 Stage 2a — does the scorer survive the REAL front end? Stage 0/1 used
// hand-built envelopes; the ~200 Hz BPF could blur the two-level structure. This
// pushes CW / carrier / noise-only IQ through a real Channel and scores the actual
// diagnostic envelope (getDiagramData, the 512-sample window the UI/manager sees).
namespace {
    // Real front-end envelope tail for a raw IQ stream.
    std::vector<float> realEnvelope(const std::vector<dsp::complex_t>& iq) {
        cw::Channel ch;
        ch.init(0, 700.0f);
        int off = 0, chunk = 4096;
        while (off < (int)iq.size()) {
            int n = std::min(chunk, (int)iq.size() - off);
            ch.process(n, iq.data() + off);
            off += n;
        }
        return ch.getDiagramData(4096);   // capped at the internal diag window
    }

    std::vector<dsp::complex_t> continuousTone(bool keyDown, int samples) {
        cw_test::SignalParams p = cw_test::profileClean(80.0f);
        cw_test::CWSignalGenerator gen; gen.init(p);
        std::vector<dsp::complex_t> buf(samples);
        gen.generate(buf.data(), samples, keyDown);   // keyDown=true carrier, false noise floor
        return buf;
    }
}

TEST_CASE("model-fit discriminates on the REAL front-end envelope (§52.3 #41)", "[cw][modelfit]") {
    cw::ModelFitScorer sc;
    const int SAMP = (int)(3.0f * 8000.0f);   // 3 s at 8 kHz

    auto cwSig  = cw_test::generateMessage("CQ CQ DE W1AW W1AW", cw_test::profileClean(48.0f));
    auto envCW  = realEnvelope(cwSig.samples);
    auto envCar = realEnvelope(continuousTone(true,  SAMP));
    auto envNoi = realEnvelope(continuousTone(false, SAMP));

    auto mCW  = sc.score(envCW.data(),  (int)envCW.size());
    auto mCar = sc.score(envCar.data(), (int)envCar.size());
    auto mNoi = sc.score(envNoi.data(), (int)envNoi.size());

    printf("\n=== §52.3 real front-end envelope (n=%d samples) ===\n", (int)envCW.size());
    printf("%-10s %10s %8s %8s %8s\n", "signal", "llr", "muLo", "muHi", "wHi");
    printf("%-10s %10.4f %8.3f %8.3f %8.3f\n", "cw-25",   mCW.llr,  mCW.muLo,  mCW.muHi,  mCW.wHi);
    printf("%-10s %10.4f %8.3f %8.3f %8.3f\n", "carrier", mCar.llr, mCar.muLo, mCar.muHi, mCar.wHi);
    printf("%-10s %10.4f %8.3f %8.3f %8.3f\n", "noise",   mNoi.llr, mNoi.muLo, mNoi.muHi, mNoi.wHi);
    printf("\n");

    // Same veto property must hold on real envelopes: keyed CW above carrier/noise.
    CHECK(mCW.llr > mCar.llr);
    CHECK(mCW.llr > mNoi.llr);
    CHECK(mCW.llr > 0.02f);        // clears a usable-CW threshold
    CHECK(mCar.llr < mCW.llr);     // carrier vetoed relative to CW
}

// §52.3 Stage 2b — the wired path: Channel.modelFit is populated during process()
// (throttled re-scoring) and reflects CW vs carrier, without perturbing decode.
TEST_CASE("Channel.modelFit is populated and discriminates (§52.3 #41)", "[cw][modelfit]") {
    auto feed = [](const std::vector<dsp::complex_t>& iq) {
        cw::Channel ch; ch.init(0, 700.0f);
        int off = 0, chunk = 4096;
        while (off < (int)iq.size()) {
            int n = std::min(chunk, (int)iq.size() - off);
            ch.process(n, iq.data() + off);
            off += n;
        }
        return ch.modelFit;
    };

    auto cwSig  = cw_test::generateMessage("CQ CQ DE W1AW W1AW", cw_test::profileClean(48.0f));
    float fitCW  = feed(cwSig.samples);
    float fitCar = feed(continuousTone(true,  (int)(3.0f * 8000.0f)));

    printf("\n§52.3 wired Channel.modelFit: cw=%.4f  carrier=%.4f\n", fitCW, fitCar);
    CHECK(fitCW > 0.1f);        // real CW populates a clear positive fit
    CHECK(fitCW > fitCar);      // carrier does not
}
