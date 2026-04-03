#include <catch.hpp>
#include <cw/tone_scanner.h>
#include <cmath>

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
