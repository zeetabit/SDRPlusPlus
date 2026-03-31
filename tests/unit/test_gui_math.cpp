#include <catch.hpp>
#include <gui/gui_math.h>

// ── Zoom slider → bandwidth mapping ──────────────────────────────────────────

TEST_CASE("zoomSliderToBandwidth at extremes", "[gui_math][zoom]") {
    double wholeBw = 2400000.0; // 2.4 MHz

    SECTION("slider=1.0 returns full bandwidth") {
        REQUIRE(gui_math::zoomSliderToBandwidth(1.0f, wholeBw) == Approx(wholeBw));
    }

    SECTION("slider=0.0 returns minimum 1000 Hz") {
        REQUIRE(gui_math::zoomSliderToBandwidth(0.0f, wholeBw) == Approx(1000.0));
    }
}

TEST_CASE("zoomSliderToBandwidth is monotonically increasing", "[gui_math][zoom]") {
    double wholeBw = 10000000.0; // 10 MHz
    double prev = gui_math::zoomSliderToBandwidth(0.0f, wholeBw);
    for (float s = 0.01f; s <= 1.0f; s += 0.01f) {
        double cur = gui_math::zoomSliderToBandwidth(s, wholeBw);
        REQUIRE(cur >= prev);
        prev = cur;
    }
}

TEST_CASE("zoomSliderToBandwidth quadratic midpoint", "[gui_math][zoom]") {
    double wholeBw = 8000000.0;
    // At slider=0.5, factor=0.25, bw = 1000 + 0.25*(8M-1000) = ~2000750
    double result = gui_math::zoomSliderToBandwidth(0.5f, wholeBw);
    REQUIRE(result == Approx(1000.0 + 0.25 * (wholeBw - 1000.0)));
}

TEST_CASE("zoomSliderToBandwidth never exceeds whole bandwidth", "[gui_math][zoom]") {
    double wholeBw = 500000.0;
    for (float s = 0.0f; s <= 2.0f; s += 0.1f) {
        REQUIRE(gui_math::zoomSliderToBandwidth(s, wholeBw) <= wholeBw);
    }
}

TEST_CASE("zoomSliderToBandwidth with small bandwidth", "[gui_math][zoom]") {
    SECTION("bandwidth smaller than 1000 Hz returns bandwidth") {
        double wholeBw = 500.0;
        // factor*delta is negative, so result = min(1000 + negative, 500) = 500
        REQUIRE(gui_math::zoomSliderToBandwidth(1.0f, wholeBw) == Approx(wholeBw));
    }

    SECTION("bandwidth exactly 1000 Hz") {
        REQUIRE(gui_math::zoomSliderToBandwidth(0.5f, 1000.0) == Approx(1000.0));
    }
}

// ── VFO offset clamping ──────────────────────────────────────────────────────

TEST_CASE("clampVFOOffset within view is unchanged", "[gui_math][vfo]") {
    REQUIRE(gui_math::clampVFOOffset(1000.0, 0.0, 10000.0) == Approx(1000.0));
    REQUIRE(gui_math::clampVFOOffset(-2000.0, 0.0, 10000.0) == Approx(-2000.0));
    REQUIRE(gui_math::clampVFOOffset(0.0, 0.0, 10000.0) == Approx(0.0));
}

TEST_CASE("clampVFOOffset clamps to view bounds", "[gui_math][vfo]") {
    // viewOffset=0, viewBW=10000 → visible range [-5000, 5000]
    SECTION("above upper bound") {
        REQUIRE(gui_math::clampVFOOffset(6000.0, 0.0, 10000.0) == Approx(5000.0));
    }
    SECTION("below lower bound") {
        REQUIRE(gui_math::clampVFOOffset(-6000.0, 0.0, 10000.0) == Approx(-5000.0));
    }
}

TEST_CASE("clampVFOOffset with non-zero view offset", "[gui_math][vfo]") {
    // viewOffset=2000, viewBW=4000 → visible range [0, 4000]
    REQUIRE(gui_math::clampVFOOffset(-100.0, 2000.0, 4000.0) == Approx(0.0));
    REQUIRE(gui_math::clampVFOOffset(5000.0, 2000.0, 4000.0) == Approx(4000.0));
    REQUIRE(gui_math::clampVFOOffset(2000.0, 2000.0, 4000.0) == Approx(2000.0));
}

TEST_CASE("clampVFOOffset at exact boundaries", "[gui_math][vfo]") {
    REQUIRE(gui_math::clampVFOOffset(5000.0, 0.0, 10000.0) == Approx(5000.0));
    REQUIRE(gui_math::clampVFOOffset(-5000.0, 0.0, 10000.0) == Approx(-5000.0));
}

// ── Frequency stepping ───────────────────────────────────────────────────────

TEST_CASE("stepFrequency steps and snaps correctly", "[gui_math][freq]") {
    SECTION("step up by 1 kHz from 100 MHz") {
        double result = gui_math::stepFrequency(100000000.0, 1000.0, 1);
        REQUIRE(result == Approx(100001000.0));
    }
    SECTION("step down by 1 kHz from 100 MHz") {
        double result = gui_math::stepFrequency(100000000.0, 1000.0, -1);
        REQUIRE(result == Approx(99999000.0));
    }
}

TEST_CASE("stepFrequency snaps to grid", "[gui_math][freq]") {
    // 100000500 Hz with 1 kHz snap, step +1 → 100001500 → rounds to 100002000
    // Actually: (100000500 + 1000) / 1000 = 100001.5, round = 100002, *1000 = 100002000
    double result = gui_math::stepFrequency(100000500.0, 1000.0, 1);
    REQUIRE(result == Approx(100002000.0));
}

TEST_CASE("stepFrequency with scroll wheel multiple steps", "[gui_math][freq]") {
    SECTION("3 steps up") {
        double result = gui_math::stepFrequency(100000000.0, 1000.0, 3);
        REQUIRE(result == Approx(100003000.0));
    }
    SECTION("5 steps down") {
        double result = gui_math::stepFrequency(100000000.0, 500.0, -5);
        REQUIRE(result == Approx(99997500.0));
    }
}

TEST_CASE("stepFrequency with fractional snap interval", "[gui_math][freq]") {
    // 0.1 Hz snap (used with Alt modifier)
    double result = gui_math::stepFrequency(100000000.0, 100.0, 1);
    REQUIRE(result == Approx(100000100.0));
}

TEST_CASE("stepFrequency zero direction returns snapped original", "[gui_math][freq]") {
    double result = gui_math::stepFrequency(100000000.0, 1000.0, 0);
    REQUIRE(result == Approx(100000000.0));
}

// ── FFT min/max constraints ──────────────────────────────────────────────────

TEST_CASE("constrainFFTMax enforces minimum gap", "[gui_math][fft]") {
    SECTION("max already above min+gap is unchanged") {
        REQUIRE(gui_math::constrainFFTMax(-50.0f, -70.0f) == Approx(-50.0f));
    }
    SECTION("max too close to min gets raised") {
        REQUIRE(gui_math::constrainFFTMax(-65.0f, -70.0f) == Approx(-60.0f));
    }
    SECTION("max below min gets raised to min+gap") {
        REQUIRE(gui_math::constrainFFTMax(-80.0f, -70.0f) == Approx(-60.0f));
    }
    SECTION("max exactly at min+gap is unchanged") {
        REQUIRE(gui_math::constrainFFTMax(-60.0f, -70.0f) == Approx(-60.0f));
    }
}

TEST_CASE("constrainFFTMin enforces minimum gap", "[gui_math][fft]") {
    SECTION("min already below max-gap is unchanged") {
        REQUIRE(gui_math::constrainFFTMin(-70.0f, -50.0f) == Approx(-70.0f));
    }
    SECTION("min too close to max gets lowered") {
        REQUIRE(gui_math::constrainFFTMin(-55.0f, -50.0f) == Approx(-60.0f));
    }
    SECTION("min above max gets lowered to max-gap") {
        REQUIRE(gui_math::constrainFFTMin(-40.0f, -50.0f) == Approx(-60.0f));
    }
}

TEST_CASE("constrainFFT custom gap", "[gui_math][fft]") {
    REQUIRE(gui_math::constrainFFTMax(-68.0f, -70.0f, 5.0f) == Approx(-65.0f));
    REQUIRE(gui_math::constrainFFTMin(-63.0f, -60.0f, 5.0f) == Approx(-65.0f));
}

// ── Scroll pan frequency ─────────────────────────────────────────────────────

TEST_CASE("scrollPanFrequency shifts center frequency", "[gui_math][scroll]") {
    double centerFreq = 100000000.0;
    double viewBw = 2000000.0;

    SECTION("scroll up shifts frequency down") {
        double result = gui_math::scrollPanFrequency(centerFreq, viewBw, 1);
        REQUIRE(result == Approx(centerFreq - viewBw / 20.0));
    }
    SECTION("scroll down shifts frequency up") {
        double result = gui_math::scrollPanFrequency(centerFreq, viewBw, -1);
        REQUIRE(result == Approx(centerFreq + viewBw / 20.0));
    }
    SECTION("zero wheel does nothing") {
        REQUIRE(gui_math::scrollPanFrequency(centerFreq, viewBw, 0) == Approx(centerFreq));
    }
    SECTION("multiple scroll steps") {
        double result = gui_math::scrollPanFrequency(centerFreq, viewBw, 3);
        REQUIRE(result == Approx(centerFreq - 3.0 * viewBw / 20.0));
    }
}
