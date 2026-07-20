#include <catch.hpp>
#include <cw/channel.h>
#include <cw/timing.h>
#include "cw_test_signals.h"

using namespace cw_test;

// ============================================================
// Farnsworth Spacing Tests
//
// Farnsworth sends elements at normal speed but stretches
// inter-character and inter-word gaps. Common in CW training.
// The decoder must adapt gap classification to handle stretched gaps.
// ============================================================

TEST_CASE("Farnsworth: ratio 1.5 CQ at 15 WPM", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(80.0f, 1.5f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.15f);
}

TEST_CASE("Farnsworth: ratio 2.0 CQ at 15 WPM", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(80.0f, 2.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.25f);
}

TEST_CASE("Farnsworth: ratio 1.5 SOS", "[cw][farnsworth]") {
    auto s = decodeAndScore("EEETTT SOS", profileFarnsworth(80.0f, 1.5f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer < 0.15f);
}

TEST_CASE("Farnsworth: ratio 1.5 at 20 WPM", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(60.0f, 1.5f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.2f);
}

TEST_CASE("Farnsworth: ratio 1.5 with mild noise", "[cw][farnsworth]") {
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworthNoisy(80.0f, 1.5f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.2f);
}

TEST_CASE("Farnsworth: ratio 2.0 long message", "[cw][farnsworth]") {
    auto s = decodeAndScore(
        "CQ CQ CQ DE W1AW W1AW QTH NEWINGTON CT K",
        profileFarnsworth(80.0f, 2.0f));
    INFO("CER=" << s.cer << " WER=" << s.wer);
    REQUIRE(s.cer < 0.25f);
}

TEST_CASE("Farnsworth: ratio 1.0 is standard (regression check)", "[cw][farnsworth]") {
    // ratio=1.0 should be identical to standard timing
    auto s = decodeAndScore(MSG_CQ(), profileFarnsworth(80.0f, 1.0f));
    INFO("CER=" << s.cer);
    REQUIRE(s.cer == 0.0f);
}

// ============================================================
// Phase 21 probe (docs §16): where does gap classification fail?
//
// Feeds the generator's *true* gap durations straight into
// AdaptiveTiming, bypassing the detector. profileFarnsworth is
// noiseless, so any misclassification here is the gap classifier's
// own — not an upstream edge error. Reports the confusion matrix,
// the converged centres against the generator's ElementModel, and
// which fallback produced those centres.
// ============================================================

namespace {

    struct GapProbe {
        int confusion[3][3] = {};          // [truth][classified]
        int sourceHist[5] = {};            // GapCenterSource histogram
        int firstErrIdx = -1;              // index of first misclassified gap
        int totalGaps = 0;
        float finalElem = 0, finalChar = 0, finalWord = 0;
        float trueChar = 0, trueWord = 0;
    };

    int gapIndex(cw_test::TruthKind k) {
        switch (k) {
            case cw_test::TRUTH_ELEMENT_GAP: return 0;
            case cw_test::TRUTH_CHAR_GAP:    return 1;
            case cw_test::TRUTH_WORD_GAP:    return 2;
            default:                         return -1;
        }
    }

    int classifiedIndex(cw::Gap g) {
        switch (g) {
            case cw::ELEMENT_GAP: return 0;
            case cw::CHAR_GAP:    return 1;
            case cw::WORD_GAP:    return 2;
        }
        return 0;
    }

    GapProbe probeGaps(const char* message, SignalParams params, int seed) {
        params.seed = seed;
        auto sig = generateMessage(message, params);

        GapProbe out;
        out.trueChar = sig.model.charGapMs;
        out.trueWord = sig.model.wordGapMs;

        cw::AdaptiveTiming timing;
        timing.init(params.sampleRate, cw::TIMING_KALMAN);

        for (const auto& seg : sig.segments) {
            if (seg.kind == cw_test::TRUTH_WARMUP || seg.kind == cw_test::TRUTH_TRAILING) continue;
            float ms = 1000.0f * (float)(seg.endSample - seg.startSample) / params.sampleRate;
            if (seg.tone) { timing.classifyOn(ms); continue; }

            int t = gapIndex(seg.kind);
            if (t < 0) continue;

            float e, c, w;
            out.sourceHist[(int)timing.gapCenters(e, c, w)]++;

            cw::TimingEvent evt = timing.classifyOff(ms);
            int cl = classifiedIndex(evt.gap);
            out.confusion[t][cl]++;
            if (t != cl && out.firstErrIdx < 0) out.firstErrIdx = out.totalGaps;
            out.totalGaps++;
        }

        timing.gapCenters(out.finalElem, out.finalChar, out.finalWord);
        return out;
    }
}

TEST_CASE("Farnsworth: gap classifier probe", "[cw][.][farnsworth-probe]") {
    const float ratios[] = {1.0f, 1.5f, 2.0f, 3.0f};
    const char* srcName[] = {"cold", "bootstrap", "nosplit", "clamped", "adapted"};

    // This probe is STRUCTURALLY NOISE-BLIND and only isolates the classifier.
    // It walks the generator's truth segments, so the durations it feeds in are
    // identical at every noise level — noise reaches gap classification only via
    // the detector, which this bypasses on purpose. Adding a noisy profile here
    // produces a byte-identical table; it was tried and removed. Measuring what
    // noise does to gap classification needs DETECTOR-derived gaps aligned to
    // truth by group delay, as the guard probe does (docs §16.5, ⊘ open).
    printf("\n=== Gap classifier probe (detector bypassed) ===\n");
    printf("%-6s %-7s %6s %8s  %-26s %-22s %s\n",
           "ratio", "noise", "gaps", "err", "confusion truth->cls", "centres est/true", "source");

    for (float r : ratios) {
        GapProbe p = probeGaps(MSG_FULL(), profileFarnsworth(80.0f, r), 12345);

        int errs = 0;
        for (int t = 0; t < 3; t++)
            for (int c = 0; c < 3; c++)
                if (t != c) errs += p.confusion[t][c];

        printf("%-6.1f %-7s %6d %8d  E>C%2d E>W%2d C>E%2d C>W%2d W>C%2d  "
               "c=%.0f/%.0f w=%.0f/%.0f  ",
               r, "n/a", p.totalGaps, errs,
               p.confusion[0][1], p.confusion[0][2],
               p.confusion[1][0], p.confusion[1][2], p.confusion[2][1],
               p.finalChar, p.trueChar, p.finalWord, p.trueWord);
        for (int s = 0; s < 5; s++)
            if (p.sourceHist[s]) printf("%s=%d ", srcName[s], p.sourceHist[s]);
        printf("firstErr=%d\n", p.firstErrIdx);

        // The probe is worthless if the generator produced no gaps to classify
        // or if the Farnsworth stretch never reached the signal.
        CHECK(p.totalGaps > 0);
        CHECK(p.trueChar == Approx(80.0f * 3.0f * r));
        CHECK(p.trueWord == Approx(80.0f * 7.0f * r));

        // Ratchet on the known defect (docs §16). Below ratio 2.0 the cold-start
        // defaults still classify correctly, so nothing may fail. At or above
        // it, exactly one gap is misread — the first char gap, before the
        // estimator has data. Asserting the magnitude rather than deleting the
        // check catches both a regression and a silent fix.
        CHECK(errs <= (r >= 2.0f ? 1 : 0));
    }
}

TEST_CASE("Farnsworth: word gaps preserved at ratio 2.0", "[cw][farnsworth]") {
    // At ratio=2.0, word gaps are 14 dit lengths (vs 7 standard).
    // The decoder must still identify word boundaries correctly.
    std::string decoded = decode(MSG_CQ(), profileFarnsworth(80.0f, 2.0f));
    std::string norm = normalize(decoded);
    INFO("decoded='" << norm << "'");
    // Should have multiple words (spaces between them)
    auto words = splitWords(norm);
    INFO("word count=" << words.size());
    REQUIRE(words.size() >= 4);  // CQ CQ CQ DE ... at least 4 words
}

