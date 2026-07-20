#include <catch.hpp>
#include <cw/morse_tree.h>
#include <cw/timing.h>
#include <cw/tone_detector.h>
#include <cw/text_buffer.h>
#include <cmath>
#include <thread>

// ============================================================
// Morse Tree Tests
// ============================================================

TEST_CASE("MorseDecoder decodes single characters", "[cw][morse]") {
    cw::MorseDecoder decoder;
    decoder.init();

    SECTION("E = dit") {
        decoder.addElement(cw::DIT, 1.0f);
        REQUIRE(decoder.characterBreak() == 'E');
    }

    SECTION("T = dah") {
        decoder.addElement(cw::DAH, 1.0f);
        REQUIRE(decoder.characterBreak() == 'T');
    }

    SECTION("A = dit-dah") {
        decoder.addElement(cw::DIT, 1.0f);
        decoder.addElement(cw::DAH, 1.0f);
        REQUIRE(decoder.characterBreak() == 'A');
    }

    SECTION("M = dah-dah") {
        decoder.addElement(cw::DAH, 1.0f);
        decoder.addElement(cw::DAH, 1.0f);
        REQUIRE(decoder.characterBreak() == 'M');
    }

    SECTION("S = dit-dit-dit") {
        decoder.addElement(cw::DIT, 1.0f);
        decoder.addElement(cw::DIT, 1.0f);
        decoder.addElement(cw::DIT, 1.0f);
        REQUIRE(decoder.characterBreak() == 'S');
    }

    SECTION("O = dah-dah-dah") {
        decoder.addElement(cw::DAH, 1.0f);
        decoder.addElement(cw::DAH, 1.0f);
        decoder.addElement(cw::DAH, 1.0f);
        REQUIRE(decoder.characterBreak() == 'O');
    }
}

TEST_CASE("MorseDecoder decodes full alphabet", "[cw][morse]") {
    cw::MorseDecoder decoder;
    decoder.init();

    struct TC { const char* pattern; char expected; };
    TC cases[] = {
        {".-",    'A'}, {"-...",  'B'}, {"-.-.",  'C'}, {"-..",   'D'},
        {".",     'E'}, {"..-.",  'F'}, {"--.",   'G'}, {"....",  'H'},
        {"..",    'I'}, {".---",  'J'}, {"-.-",   'K'}, {".-..",  'L'},
        {"--",    'M'}, {"-.",    'N'}, {"---",   'O'}, {".--.",  'P'},
        {"--.-",  'Q'}, {".-.",   'R'}, {"...",   'S'}, {"-",     'T'},
        {"..-",   'U'}, {"...-",  'V'}, {".--",   'W'}, {"-..-",  'X'},
        {"-.--",  'Y'}, {"--..",  'Z'},
    };

    for (auto& tc : cases) {
        decoder.reset();
        for (const char* p = tc.pattern; *p; p++) {
            decoder.addElement(*p == '.' ? cw::DIT : cw::DAH, 1.0f);
        }
        INFO("Pattern: " << tc.pattern << " expected: " << tc.expected);
        REQUIRE(decoder.characterBreak() == tc.expected);
    }
}

TEST_CASE("MorseDecoder decodes digits", "[cw][morse]") {
    cw::MorseDecoder decoder;
    decoder.init();

    struct TC { const char* pattern; char expected; };
    TC cases[] = {
        {"-----", '0'}, {".----", '1'}, {"..---", '2'}, {"...--", '3'},
        {"....-", '4'}, {".....", '5'}, {"-....", '6'}, {"--...", '7'},
        {"---..", '8'}, {"----.", '9'},
    };

    for (auto& tc : cases) {
        decoder.reset();
        for (const char* p = tc.pattern; *p; p++) {
            decoder.addElement(*p == '.' ? cw::DIT : cw::DAH, 1.0f);
        }
        INFO("Pattern: " << tc.pattern << " expected: " << tc.expected);
        REQUIRE(decoder.characterBreak() == tc.expected);
    }
}

TEST_CASE("MorseDecoder decodes prosigns", "[cw][morse][prosign]") {
    cw::MorseDecoder decoder;
    decoder.init();

    struct TC { const char* pattern; char expected; const char* name; };
    TC cases[] = {
        {".-.-.",  '+', "AR"},   // end of message
        {"...-.-", '*', "SK"},   // end of contact
        {"-...-",  '=', "BT"},  // break (same as = sign)
    };

    for (auto& tc : cases) {
        decoder.reset();
        for (const char* p = tc.pattern; *p; p++) {
            decoder.addElement(*p == '.' ? cw::DIT : cw::DAH, 1.0f);
        }
        INFO("Prosign: " << tc.name << " pattern: " << tc.pattern << " expected: " << tc.expected);
        REQUIRE(decoder.characterBreak() == tc.expected);
    }
}

TEST_CASE("MorseDecoder decodes punctuation", "[cw][morse][punctuation]") {
    // Added 2026-07-20. Both were unmapped, which made every period decode as
    // '*' (SK, the nearest mapped node) and every comma vanish. The defect
    // survived the whole suite because MSG_CQ and MSG_FULL carry no punctuation —
    // it was only visible against real ARRL code-practice text.
    cw::MorseDecoder decoder;
    decoder.init();

    struct TC { const char* pattern; char expected; const char* name; };
    TC cases[] = {
        {".-.-.-", '.', "period"},
        {"--..--", ',', "comma"},
    };

    for (auto& tc : cases) {
        decoder.reset();
        for (const char* p = tc.pattern; *p; p++) {
            decoder.addElement(*p == '.' ? cw::DIT : cw::DAH, 1.0f);
        }
        INFO("Punctuation: " << tc.name << " pattern: " << tc.pattern);
        REQUIRE(decoder.characterBreak() == tc.expected);
    }
}

TEST_CASE("MorseDecoder punctuation does not shadow its prefixes", "[cw][morse][punctuation]") {
    // Period sits directly below AR (.-.-.) and comma two below Z (--..).
    // A tree edit that captured a parent node would break these instead, and
    // the prosign test alone would not catch the comma/Z case.
    cw::MorseDecoder decoder;
    decoder.init();

    struct TC { const char* pattern; char expected; };
    TC cases[] = {
        {".-.-.",  '+'},   // AR — period's parent
        {"--..",   'Z'},   // Z — comma's grandparent
    };

    for (auto& tc : cases) {
        decoder.reset();
        for (const char* p = tc.pattern; *p; p++) {
            decoder.addElement(*p == '.' ? cw::DIT : cw::DAH, 1.0f);
        }
        INFO("Prefix: " << tc.pattern);
        REQUIRE(decoder.characterBreak() == tc.expected);
    }

    // Comma's parent (--..-, node 56) is unmapped, but characterBreak never
    // returns '\0' while any beam path sits on a mapped node — confidence is
    // clamped to 0.95, so the alternate branch always survives and this
    // resolves to '7' at the neighbouring node. The property that matters is
    // narrower: the deeper comma must not capture the shallower pattern.
    decoder.reset();
    for (const char* p = "--..-"; *p; p++) {
        decoder.addElement(*p == '.' ? cw::DIT : cw::DAH, 1.0f);
    }
    REQUIRE(decoder.characterBreak() != ',');
}

TEST_CASE("MorseDecoder resets after characterBreak", "[cw][morse]") {
    cw::MorseDecoder decoder;
    decoder.init();

    decoder.addElement(cw::DIT, 1.0f);
    REQUIRE(decoder.characterBreak() == 'E');

    decoder.addElement(cw::DAH, 1.0f);
    REQUIRE(decoder.characterBreak() == 'T');
}

TEST_CASE("MorseDecoder wordBreak emits character", "[cw][morse]") {
    cw::MorseDecoder decoder;
    decoder.init();

    decoder.addElement(cw::DIT, 1.0f);
    decoder.addElement(cw::DAH, 1.0f);
    REQUIRE(decoder.wordBreak() == 'A');
}

// ============================================================
// Adaptive Timing Tests
// ============================================================

TEST_CASE("AdaptiveTiming classifies dit and dah after training", "[cw][timing]") {
    cw::AdaptiveTiming timing;
    timing.init(1000.0f);

    // Train with mixed elements so all strategies can build clusters
    for (int i = 0; i < 10; i++) {
        timing.classifyOn(70.0f);
        timing.classifyOn(210.0f);
    }

    // After training, classification should be reliable
    REQUIRE(timing.classifyOn(70.0f).element == cw::DIT);
    REQUIRE(timing.classifyOn(210.0f).element == cw::DAH);
}

TEST_CASE("AdaptiveTiming adapts to speed", "[cw][timing]") {
    cw::AdaptiveTiming timing;
    timing.init(1000.0f);

    // Feed 20 WPM (dit=60ms, dah=180ms)
    for (int i = 0; i < 20; i++) {
        timing.classifyOn(60.0f);
        timing.classifyOn(180.0f);
    }

    REQUIRE(timing.getWPM() > 15.0f);
    REQUIRE(timing.getWPM() < 25.0f);
    REQUIRE(timing.isLocked());
}

TEST_CASE("AdaptiveTiming classifies gaps", "[cw][timing]") {
    cw::AdaptiveTiming timing;
    timing.init(1000.0f);

    // Gap classification uses ditCenter (default 80ms)
    REQUIRE(timing.classifyOff(60.0f).gap == cw::ELEMENT_GAP);
    REQUIRE(timing.classifyOff(250.0f).gap == cw::CHAR_GAP);
    REQUIRE(timing.classifyOff(600.0f).gap == cw::WORD_GAP);
}

TEST_CASE("AdaptiveTiming confidence is between 0 and 1", "[cw][timing]") {
    cw::AdaptiveTiming timing;
    timing.init(1000.0f);

    for (int i = 0; i < 10; i++) {
        auto dit = timing.classifyOn(70.0f);
        REQUIRE(dit.confidence >= 0.0f);
        REQUIRE(dit.confidence <= 1.0f);
        auto dah = timing.classifyOn(210.0f);
        REQUIRE(dah.confidence >= 0.0f);
        REQUIRE(dah.confidence <= 1.0f);
    }
}

TEST_CASE("AdaptiveTiming reset restores initial state", "[cw][timing]") {
    cw::AdaptiveTiming timing;
    timing.init(1000.0f);

    for (int i = 0; i < 20; i++) {
        timing.classifyOn(30.0f);
        timing.classifyOn(90.0f);
    }
    REQUIRE(timing.isLocked());

    timing.reset();
    REQUIRE_FALSE(timing.isLocked());
    REQUIRE(timing.getDitDuration() == Approx(80.0f));
}

// ============================================================
// Tone Detector Tests
// ============================================================

TEST_CASE("ToneDetector detects key-down from silence", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    std::vector<float> silence(2000, 0.001f);
    detector.process(silence.data(), silence.size());

    std::vector<float> signal(500, 1.0f);
    auto events = detector.process(signal.data(), signal.size());

    bool foundKeyDown = false;
    for (auto& e : events) { if (e.keyDown) { foundKeyDown = true; } }
    REQUIRE(foundKeyDown);
    REQUIRE(detector.isKeyDown());
}

TEST_CASE("ToneDetector detects key-up after signal", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    std::vector<float> silence(2000, 0.001f);
    std::vector<float> signal(500, 1.0f);

    detector.process(silence.data(), silence.size());
    detector.process(signal.data(), signal.size());
    REQUIRE(detector.isKeyDown());

    auto events = detector.process(silence.data(), silence.size());
    bool foundKeyUp = false;
    for (auto& e : events) { if (!e.keyDown) { foundKeyUp = true; } }
    REQUIRE(foundKeyUp);
}

TEST_CASE("ToneDetector works when signal present at startup", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    // Simulate CW already in progress: alternating signal/silence (50% duty)
    // The 25th percentile noise estimator should lock onto the silence level
    std::vector<float> mixed(2000);
    for (int i = 0; i < 2000; i++) {
        mixed[i] = (i % 200 < 100) ? 1.0f : 0.001f;  // 100ms on, 100ms off
    }
    detector.process(mixed.data(), mixed.size());

    // Noise floor should be near the silence level, not the signal level
    REQUIRE(detector.getNoiseFloor() < 0.1f);
    REQUIRE(detector.getSNR() > 5.0f);
}

TEST_CASE("ToneDetector SNR increases with signal", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    std::vector<float> silence(2000, 0.001f);
    detector.process(silence.data(), silence.size());
    float snrBefore = detector.getSNR();

    std::vector<float> signal(500, 1.0f);
    detector.process(signal.data(), signal.size());
    REQUIRE(detector.getSNR() > snrBefore);
}

TEST_CASE("ToneDetector hysteresis prevents chatter", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    std::vector<float> silence(2000, 0.001f);
    detector.process(silence.data(), silence.size());

    // Feed signal at midpoint between ON/OFF thresholds — in the dead zone
    float mid = detector.getNoiseFloor() + 0.45f * (detector.getSignalPeak() - detector.getNoiseFloor());
    std::vector<float> borderline(200, mid);
    auto events = detector.process(borderline.data(), borderline.size());
    // Should get at most one transition (into the zone), not chatter
    REQUIRE(events.size() <= 1);
}

TEST_CASE("ToneDetector reset clears state", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    std::vector<float> silence(2000, 0.001f);
    detector.process(silence.data(), silence.size());
    std::vector<float> signal(500, 1.0f);
    detector.process(signal.data(), signal.size());
    REQUIRE(detector.isKeyDown());

    detector.reset();
    REQUIRE_FALSE(detector.isKeyDown());
}

TEST_CASE("ToneDetector percentile noise is robust to outliers", "[cw][detector]") {
    cw::ToneDetector detector;
    detector.init(1000.0f);

    // 90% silence with occasional strong spikes — noise floor should stay low
    std::vector<float> data(2000, 0.001f);
    for (int i = 0; i < 200; i++) { data[i * 10] = 5.0f; }  // 10% spikes
    detector.process(data.data(), data.size());

    REQUIRE(detector.getNoiseFloor() < 0.01f);
}

// ============================================================
// TextBuffer Tests
// ============================================================

TEST_CASE("TextBuffer append and getText", "[cw][textbuf]") {
    cw::TextBuffer buf;
    buf.append('H');
    buf.append('I');
    REQUIRE(buf.getText() == "HI");
}

TEST_CASE("TextBuffer appendSpace", "[cw][textbuf]") {
    cw::TextBuffer buf;
    buf.append('H');
    buf.append('I');
    buf.appendSpace();
    buf.append('O');
    buf.append('M');
    REQUIRE(buf.getText() == "HI OM");
}

TEST_CASE("TextBuffer clear", "[cw][textbuf]") {
    cw::TextBuffer buf;
    buf.append('A');
    buf.clear();
    REQUIRE(buf.getText().empty());
}

TEST_CASE("TextBuffer thread safety", "[cw][textbuf]") {
    cw::TextBuffer buf;

    std::thread writer([&]() {
        for (int i = 0; i < 1000; i++) { buf.append('A' + (i % 26)); }
    });

    std::thread reader([&]() {
        for (int i = 0; i < 100; i++) { buf.getText(); }
    });

    writer.join();
    reader.join();
    REQUIRE(buf.getText().size() == 1000);
}

TEST_CASE("TextBuffer getEntries returns per-char data", "[cw][textbuf]") {
    cw::TextBuffer buf;
    buf.append('C', 0.9f);
    buf.append('Q', 0.85f);

    auto entries = buf.getEntries();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].character == 'C');
    REQUIRE(entries[0].confidence == Approx(0.9f));
    REQUIRE_FALSE(entries[0].corrected);
    REQUIRE(entries[1].character == 'Q');
    REQUIRE_FALSE(entries[1].corrected);
}

TEST_CASE("TextBuffer replaceLastN sets corrected flag", "[cw][textbuf]") {
    cw::TextBuffer buf;
    buf.append('+', 0.3f);
    buf.append('Q', 0.4f);

    // Corrector replaces "+Q" with "CQ"
    buf.replaceLastN(2, "CQ", 0.4f);

    auto entries = buf.getEntries();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].character == 'C');
    REQUIRE(entries[0].corrected == true);
    REQUIRE(entries[1].character == 'Q');
    REQUIRE(entries[1].corrected == true);
    REQUIRE(buf.getText() == "CQ");
}

TEST_CASE("TextBuffer uncorrected chars have corrected=false", "[cw][textbuf]") {
    cw::TextBuffer buf;
    buf.append('W', 0.95f);
    buf.append('1', 0.9f);
    buf.append('A', 0.92f);
    buf.append('W', 0.88f);

    auto entries = buf.getEntries();
    for (auto& e : entries) {
        REQUIRE_FALSE(e.corrected);
    }
}

// ============================================================
// Timing Strategy Comparison Tests
//
// Test all three strategies (K-Means, Median, Bimodal) on the same
// element sequences to measure correctness and speed tracking.
// ============================================================

struct TimingTestResult {
    int correct;
    int total;
    float finalWpm;
    float accuracy() const { return total > 0 ? (float)correct / total : 0; }
};

// Feed a sequence of dit/dah durations and check classification accuracy.
static TimingTestResult testTimingStrategy(cw::TimingStrategy strategy,
                                           float ditMs, int count) {
    cw::AdaptiveTiming t;
    t.init(1000.0f, strategy);

    float dahMs = ditMs * 3.0f;
    TimingTestResult r = {0, 0, 0};

    // Alternating dit-dah pattern (like "A" repeated)
    for (int i = 0; i < count; i++) {
        auto dit = t.classifyOn(ditMs);
        if (dit.element == cw::DIT) { r.correct++; }
        r.total++;

        auto dah = t.classifyOn(dahMs);
        if (dah.element == cw::DAH) { r.correct++; }
        r.total++;
    }

    r.finalWpm = t.getWPM();
    return r;
}

TEST_CASE("Timing strategies: 15 WPM (dit=80ms)", "[cw][timing][strategy]") {
    float ditMs = 80.0f;
    float expectedWpm = 1200.0f / ditMs;  // 15

    auto km = testTimingStrategy(cw::TIMING_KMEANS, ditMs, 20);
    auto med = testTimingStrategy(cw::TIMING_MEDIAN, ditMs, 20);
    auto bi = testTimingStrategy(cw::TIMING_BIMODAL, ditMs, 20);

    INFO("K-Means:  accuracy=" << km.accuracy() << " wpm=" << km.finalWpm);
    INFO("Median:   accuracy=" << med.accuracy() << " wpm=" << med.finalWpm);
    INFO("Bimodal:  accuracy=" << bi.accuracy() << " wpm=" << bi.finalWpm);

    // All should achieve >90% accuracy at standard speed
    REQUIRE(km.accuracy() > 0.9f);
    REQUIRE(med.accuracy() > 0.9f);
    REQUIRE(bi.accuracy() > 0.9f);

    // WPM should be within 30% of expected
    REQUIRE(km.finalWpm == Approx(expectedWpm).margin(expectedWpm * 0.3f));
    REQUIRE(med.finalWpm == Approx(expectedWpm).margin(expectedWpm * 0.3f));
    REQUIRE(bi.finalWpm == Approx(expectedWpm).margin(expectedWpm * 0.3f));
}

TEST_CASE("Timing strategies: 25 WPM (dit=48ms)", "[cw][timing][strategy]") {
    float ditMs = 48.0f;

    auto km = testTimingStrategy(cw::TIMING_KMEANS, ditMs, 20);
    auto med = testTimingStrategy(cw::TIMING_MEDIAN, ditMs, 20);
    auto bi = testTimingStrategy(cw::TIMING_BIMODAL, ditMs, 20);

    INFO("K-Means:  accuracy=" << km.accuracy() << " wpm=" << km.finalWpm);
    INFO("Median:   accuracy=" << med.accuracy() << " wpm=" << med.finalWpm);
    INFO("Bimodal:  accuracy=" << bi.accuracy() << " wpm=" << bi.finalWpm);

    // K-Means fails at 25 WPM (seed=80ms, 48ms dit classified as "short dah")
    // Median and Bimodal adapt from first principles
    REQUIRE(med.accuracy() > 0.9f);
    REQUIRE(bi.accuracy() > 0.9f);
    REQUIRE(bi.accuracy() >= km.accuracy());  // Bimodal should be at least as good
}

TEST_CASE("Timing strategies: 5 WPM slow (dit=240ms)", "[cw][timing][strategy]") {
    // This is where K-Means struggles due to fixed 80ms seed
    float ditMs = 240.0f;
    float expectedWpm = 1200.0f / ditMs;  // 5

    auto km = testTimingStrategy(cw::TIMING_KMEANS, ditMs, 20);
    auto med = testTimingStrategy(cw::TIMING_MEDIAN, ditMs, 20);
    auto bi = testTimingStrategy(cw::TIMING_BIMODAL, ditMs, 20);

    INFO("K-Means:  accuracy=" << km.accuracy() << " wpm=" << km.finalWpm);
    INFO("Median:   accuracy=" << med.accuracy() << " wpm=" << med.finalWpm);
    INFO("Bimodal:  accuracy=" << bi.accuracy() << " wpm=" << bi.finalWpm);

    // Median and Bimodal should do well; K-Means may struggle
    REQUIRE(med.accuracy() > 0.85f);
    REQUIRE(bi.accuracy() > 0.85f);
    // K-Means: no strict requirement — documenting its behavior
    INFO("K-Means at 5 WPM accuracy: " << km.accuracy());
}

TEST_CASE("Timing strategies: 35 WPM fast (dit=34ms)", "[cw][timing][strategy]") {
    float ditMs = 34.0f;

    auto km = testTimingStrategy(cw::TIMING_KMEANS, ditMs, 20);
    auto med = testTimingStrategy(cw::TIMING_MEDIAN, ditMs, 20);
    auto bi = testTimingStrategy(cw::TIMING_BIMODAL, ditMs, 20);

    INFO("K-Means:  accuracy=" << km.accuracy() << " wpm=" << km.finalWpm);
    INFO("Median:   accuracy=" << med.accuracy() << " wpm=" << med.finalWpm);
    INFO("Bimodal:  accuracy=" << bi.accuracy() << " wpm=" << bi.finalWpm);

    // K-Means struggles far from seed; Median/Bimodal should adapt
    REQUIRE(med.accuracy() > 0.9f);
    REQUIRE(bi.accuracy() > 0.9f);
}

TEST_CASE("Timing strategies: speed change 15→25 WPM", "[cw][timing][strategy]") {
    // Test adaptation when speed changes mid-stream
    auto testSpeedChange = [](cw::TimingStrategy strategy) -> float {
        cw::AdaptiveTiming t;
        t.init(1000.0f, strategy);

        // Phase 1: 15 WPM
        for (int i = 0; i < 15; i++) {
            t.classifyOn(80.0f);   // dit
            t.classifyOn(240.0f);  // dah
        }
        float wpm1 = t.getWPM();

        // Phase 2: 25 WPM
        int correct = 0, total = 0;
        for (int i = 0; i < 15; i++) {
            if (t.classifyOn(48.0f).element == cw::DIT) correct++;
            total++;
            if (t.classifyOn(144.0f).element == cw::DAH) correct++;
            total++;
        }

        return (float)correct / total;
    };

    float km  = testSpeedChange(cw::TIMING_KMEANS);
    float med = testSpeedChange(cw::TIMING_MEDIAN);
    float bi  = testSpeedChange(cw::TIMING_BIMODAL);

    INFO("Speed change accuracy — K-Means: " << km << " Median: " << med << " Bimodal: " << bi);

    // Bimodal should handle speed changes best (histogram rebuilds)
    // Median's sliding window adapts but slowly
    REQUIRE(bi > 0.7f);
    REQUIRE(bi >= km);
    REQUIRE(bi >= med);
}

TEST_CASE("Timing strategies: noisy durations (±20% jitter)", "[cw][timing][strategy]") {
    srand(42);
    auto testWithJitter = [](cw::TimingStrategy strategy, float ditMs) -> float {
        cw::AdaptiveTiming t;
        t.init(1000.0f, strategy);

        float dahMs = ditMs * 3.0f;
        int correct = 0, total = 0;

        for (int i = 0; i < 30; i++) {
            float jitter = 0.8f + 0.4f * ((float)rand() / RAND_MAX); // 0.8-1.2
            auto dit = t.classifyOn(ditMs * jitter);
            if (dit.element == cw::DIT) correct++;
            total++;

            jitter = 0.8f + 0.4f * ((float)rand() / RAND_MAX);
            auto dah = t.classifyOn(dahMs * jitter);
            if (dah.element == cw::DAH) correct++;
            total++;
        }

        return (float)correct / total;
    };

    float km  = testWithJitter(cw::TIMING_KMEANS, 80.0f);
    float med = testWithJitter(cw::TIMING_MEDIAN, 80.0f);
    float bi  = testWithJitter(cw::TIMING_BIMODAL, 80.0f);

    INFO("Jitter accuracy — K-Means: " << km << " Median: " << med << " Bimodal: " << bi);

    // All should handle ±20% jitter well
    REQUIRE(km > 0.85f);
    REQUIRE(med > 0.85f);
    REQUIRE(bi > 0.85f);
}
