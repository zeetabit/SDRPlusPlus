#include <catch.hpp>
#include <cw/tone_scanner.h>
#include <cmath>
#include <random>
#include <cstdio>
#include <vector>
#include <algorithm>

static void generateTone(dsp::complex_t* buf, int count, float freq, float sampleRate, float amplitude, float& phase) {
    float omega = 2.0f * M_PI * freq / sampleRate;
    for (int i = 0; i < count; i++) {
        buf[i].re = amplitude * cosf(phase);
        buf[i].im = amplitude * sinf(phase);
        phase += omega;
        if (phase > M_PI) { phase -= 2.0f * M_PI; }
    }
}

TEST_CASE("ToneScanner detects single tone", "[cw][scanner]") {
    cw::ToneScanner scanner;
    scanner.init(8000.0f, 1024);

    // Generate 700 Hz tone for several FFT frames
    float phase = 0;
    dsp::complex_t buf[1024];
    for (int frame = 0; frame < 10; frame++) {
        generateTone(buf, 1024, 700.0f, 8000.0f, 1.0f, phase);
        scanner.feed(buf, 1024);
    }

    auto tones = scanner.getDetectedTones(6.0f, 10);
    REQUIRE(!tones.empty());

    // The strongest tone should be near 700 Hz
    float detectedFreq = tones[0].frequency;
    REQUIRE(detectedFreq == Approx(700.0f).margin(20.0f));
    REQUIRE(tones[0].power > 6.0f);
}

TEST_CASE("ToneScanner detects multiple tones", "[cw][scanner]") {
    cw::ToneScanner scanner;
    scanner.init(8000.0f, 1024);

    float phase1 = 0, phase2 = 0;
    dsp::complex_t buf1[1024], buf2[1024], combined[1024];

    for (int frame = 0; frame < 10; frame++) {
        generateTone(buf1, 1024, 500.0f, 8000.0f, 1.0f, phase1);
        generateTone(buf2, 1024, 900.0f, 8000.0f, 0.8f, phase2);
        for (int i = 0; i < 1024; i++) {
            combined[i].re = buf1[i].re + buf2[i].re;
            combined[i].im = buf1[i].im + buf2[i].im;
        }
        scanner.feed(combined, 1024);
    }

    auto tones = scanner.getDetectedTones(6.0f, 10);
    REQUIRE(tones.size() >= 2);

    // Both tones should be detected (order by power)
    std::vector<float> freqs;
    for (auto& t : tones) { freqs.push_back(t.frequency); }
    std::sort(freqs.begin(), freqs.end());

    bool found500 = false, found900 = false;
    for (float f : freqs) {
        if (fabsf(f - 500.0f) < 20.0f) { found500 = true; }
        if (fabsf(f - 900.0f) < 20.0f) { found900 = true; }
    }
    REQUIRE(found500);
    REQUIRE(found900);
}

TEST_CASE("ToneScanner reports no tones on noise", "[cw][scanner]") {
    cw::ToneScanner scanner;
    scanner.init(8000.0f, 1024);

    // Feed low-level noise
    dsp::complex_t buf[1024];
    srand(42);
    for (int frame = 0; frame < 10; frame++) {
        for (int i = 0; i < 1024; i++) {
            buf[i].re = ((float)rand() / RAND_MAX - 0.5f) * 0.001f;
            buf[i].im = ((float)rand() / RAND_MAX - 0.5f) * 0.001f;
        }
        scanner.feed(buf, 1024);
    }

    auto tones = scanner.getDetectedTones(10.0f, 10);
    REQUIRE(tones.empty());
}

TEST_CASE("ToneScanner merges nearby peaks into one", "[cw][scanner]") {
    cw::ToneScanner scanner;
    scanner.init(8000.0f, 1024);

    // Two tones 50 Hz apart — within 80 Hz merge radius, should become one
    float phase1 = 0, phase2 = 0;
    dsp::complex_t buf1[1024], buf2[1024], combined[1024];

    for (int frame = 0; frame < 10; frame++) {
        generateTone(buf1, 1024, 700.0f, 8000.0f, 1.0f, phase1);
        generateTone(buf2, 1024, 740.0f, 8000.0f, 0.5f, phase2);
        for (int i = 0; i < 1024; i++) {
            combined[i].re = buf1[i].re + buf2[i].re;
            combined[i].im = buf1[i].im + buf2[i].im;
        }
        scanner.feed(combined, 1024);
    }

    auto tones = scanner.getDetectedTones(6.0f, 10);
    // Should be merged into one tone near 700 Hz (the stronger one)
    int count = 0;
    for (auto& t : tones) {
        if (fabsf(t.frequency - 700.0f) < 80.0f) { count++; }
    }
    REQUIRE(count == 1);
}

TEST_CASE("ToneScanner keeps distant tones separate", "[cw][scanner]") {
    cw::ToneScanner scanner;
    scanner.init(8000.0f, 1024);

    // Two tones 300 Hz apart — well beyond merge radius
    float phase1 = 0, phase2 = 0;
    dsp::complex_t buf1[1024], buf2[1024], combined[1024];

    for (int frame = 0; frame < 10; frame++) {
        generateTone(buf1, 1024, 500.0f, 8000.0f, 1.0f, phase1);
        generateTone(buf2, 1024, 800.0f, 8000.0f, 1.0f, phase2);
        for (int i = 0; i < 1024; i++) {
            combined[i].re = buf1[i].re + buf2[i].re;
            combined[i].im = buf1[i].im + buf2[i].im;
        }
        scanner.feed(combined, 1024);
    }

    auto tones = scanner.getDetectedTones(6.0f, 10);
    bool found500 = false, found800 = false;
    for (auto& t : tones) {
        if (fabsf(t.frequency - 500.0f) < 30.0f) { found500 = true; }
        if (fabsf(t.frequency - 800.0f) < 30.0f) { found800 = true; }
    }
    REQUIRE(found500);
    REQUIRE(found800);
}

TEST_CASE("ToneScanner noise floor is stable", "[cw][scanner]") {
    cw::ToneScanner scanner;
    scanner.init(8000.0f, 1024);

    // Feed tone then silence — the tone should disappear from detections
    float phase = 0;
    dsp::complex_t buf[1024];

    // Tone frames
    for (int i = 0; i < 5; i++) {
        generateTone(buf, 1024, 600.0f, 8000.0f, 1.0f, phase);
        scanner.feed(buf, 1024);
    }
    auto withTone = scanner.getDetectedTones(6.0f, 10);
    REQUIRE(!withTone.empty());

    // Silence frames (averaged spectrum decays)
    for (int i = 0; i < 20; i++) {
        for (int j = 0; j < 1024; j++) {
            buf[j].re = ((float)rand() / RAND_MAX - 0.5f) * 0.001f;
            buf[j].im = ((float)rand() / RAND_MAX - 0.5f) * 0.001f;
        }
        scanner.feed(buf, 1024);
    }
    auto afterSilence = scanner.getDetectedTones(6.0f, 10);
    // After enough silence frames, the tone peak should fade below threshold
    // (or at least be weaker)
    if (!afterSilence.empty()) {
        REQUIRE(afterSilence[0].power < withTone[0].power);
    }
}

// ── Diagnostic: does the detection gate penalise KEYED (intermittent) CW? ──
//
// Every test above feeds a STEADY tone. Real weak CW the operator copies by ear
// is intermittent — the ear integrates key-down SNR + rhythm. The scanner gates
// on a time-averaged, single-bin power spectrum (ToneScanner::getDetectedTones),
// which dilutes an intermittent signal by its duty cycle AND spreads its energy
// across keying sidebands. This probe measures that penalty directly, at the
// signal bin the gate itself reads, keyed vs steady at identical key-down SNR.
//
// Run ONLY as:  ./cw_decoder_tests "[scanner-sens]"   (never [.] / [cw] wildcards)
namespace {
    float signalBinSNRdb(cw::ToneScanner& sc, float freqHz) {
        const std::vector<float>& spec = sc.getPowerSpectrum();
        int n = sc.getFFTSize();
        float bw = sc.getBinWidth();
        int center = (int)lroundf(freqHz / bw) + n / 2;
        float peak = 0.0f;
        for (int b = center - 5; b <= center + 5; b++) {
            if (b >= 0 && b < n && spec[b] > peak) { peak = spec[b]; }
        }
        std::vector<float> s(spec.begin(), spec.end());
        std::nth_element(s.begin(), s.begin() + n / 2, s.end());
        float floor = std::max(s[n / 2], 1e-12f);
        return 10.0f * log10f(std::max(peak, 1e-12f) / floor);
    }
}

TEST_CASE("ToneScanner: keyed-vs-steady detection gap", "[cw][.][scanner-sens]") {
    const float fs = 8000.0f, freq = 700.0f;
    const int ditSamples = 480;      // ~20 WPM at 8 kHz
    const int frames = 60;           // ~7.7 s — many keying cycles, EMA settled
    std::mt19937 rng(12345);
    std::normal_distribution<float> g(0.0f, 1.0f);   // sigma=1 per component

    printf("\n  keydown | steady det | keyed det | penalty  (dB, at 700 Hz signal bin)\n");
    printf("  --------+------------+-----------+--------\n");
    for (float snr = 0.0f; snr <= 12.001f; snr += 2.0f) {
        float A = sqrtf(2.0f * powf(10.0f, snr / 10.0f));   // key-down amplitude for target SNR

        cw::ToneScanner st; st.init(fs, 1024);
        cw::ToneScanner kd; kd.init(fs, 1024);
        dsp::complex_t buf[1024];
        float phS = 0, phK = 0; long clk = 0;
        const float w = 2.0f * (float)M_PI * freq / fs;

        for (int f = 0; f < frames; f++) {
            for (int i = 0; i < 1024; i++) {
                buf[i].re = A * cosf(phS) + g(rng);
                buf[i].im = A * sinf(phS) + g(rng);
                phS += w; if (phS > (float)M_PI) { phS -= 2.0f * (float)M_PI; }
            }
            st.feed(buf, 1024);
        }
        for (int f = 0; f < frames; f++) {
            for (int i = 0; i < 1024; i++) {
                bool on = ((clk / ditSamples) % 5) < 2;    // 40% duty
                float a = on ? A : 0.0f;
                buf[i].re = a * cosf(phK) + g(rng);
                buf[i].im = a * sinf(phK) + g(rng);
                phK += w; if (phK > (float)M_PI) { phK -= 2.0f * (float)M_PI; }
                clk++;
            }
            kd.feed(buf, 1024);
        }

        float sSNR = signalBinSNRdb(st, freq);
        float kSNR = signalBinSNRdb(kd, freq);
        printf("  %6.1f  |  %8.2f  |  %7.2f  | %6.2f\n", snr, sSNR, kSNR, sSNR - kSNR);
    }
    printf("\n  A positive penalty = keyed CW reads WEAKER than a steady tone of the\n"
           "  same key-down SNR, so the averaged-power gate misses it first.\n\n");
}
