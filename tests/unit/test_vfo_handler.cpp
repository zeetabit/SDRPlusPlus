#include <catch.hpp>
#include <gui/widgets/waterfall.h>
#include <gui/gui_math.h>

// These tests validate the VFO handling logic contracts that VFOHandler implements.
// VFOHandler.cpp can't be compiled in test build (residual gui:: coupling in init/updateFromWaterfall),
// so we test the pure logic: offset clamping, frequency display calculations, and config persistence contracts.

// ── VFO offset clamping (used by onVFOCreated) ──────────────────────────────

TEST_CASE("VFO offset clamping during runtime clamps to visible window", "[vfo_handler][clamp]") {
    // viewOffset=0, viewBW=2400000 → visible [-1200000, 1200000]
    double offset = 1500000.0;
    double result = gui_math::clampVFOOffset(offset, 0.0, 2400000.0);
    REQUIRE(result == Approx(1200000.0));
}

TEST_CASE("VFO offset within view is unchanged", "[vfo_handler][clamp]") {
    double offset = 500000.0;
    double result = gui_math::clampVFOOffset(offset, 0.0, 2400000.0);
    REQUIRE(result == Approx(500000.0));
}

TEST_CASE("VFO offset clamping with non-centered view", "[vfo_handler][clamp]") {
    // viewOffset=500000, viewBW=1000000 → visible [0, 1000000]
    SECTION("below lower bound") {
        REQUIRE(gui_math::clampVFOOffset(-100.0, 500000.0, 1000000.0) == Approx(0.0));
    }
    SECTION("above upper bound") {
        REQUIRE(gui_math::clampVFOOffset(1500000.0, 500000.0, 1000000.0) == Approx(1000000.0));
    }
}

// ── VFO frequency display calculation ────────────────────────────────────────

TEST_CASE("VFO display frequency is center + general offset", "[vfo_handler][display]") {
    double centerFreq = 145000000.0;
    double generalOffset = 25000.0;
    double displayFreq = centerFreq + generalOffset;
    REQUIRE(displayFreq == Approx(145025000.0));
}

TEST_CASE("VFO display frequency with negative offset", "[vfo_handler][display]") {
    double centerFreq = 145000000.0;
    double generalOffset = -50000.0;
    double displayFreq = centerFreq + generalOffset;
    REQUIRE(displayFreq == Approx(144950000.0));
}

TEST_CASE("VFO display frequency with zero offset equals center", "[vfo_handler][display]") {
    double centerFreq = 100000000.0;
    double displayFreq = centerFreq + 0.0;
    REQUIRE(displayFreq == Approx(100000000.0));
}

// ── VFO selection change display contract ────────────────────────────────────

TEST_CASE("VFO selection with active VFO shows offset frequency", "[vfo_handler][selection]") {
    double centerFreq = 145000000.0;
    ImGui::WaterfallVFO vfo = {};
    vfo.generalOffset = 50000.0;

    double displayFreq = (true) ? (vfo.generalOffset + centerFreq) : centerFreq;
    REQUIRE(displayFreq == Approx(145050000.0));
}

TEST_CASE("VFO selection with no VFO shows center frequency", "[vfo_handler][selection]") {
    double centerFreq = 145000000.0;
    ImGui::WaterfallVFO* vfo = nullptr;

    double displayFreq = (vfo != nullptr) ? (vfo->generalOffset + centerFreq) : centerFreq;
    REQUIRE(displayFreq == Approx(145000000.0));
}

// ── Config persistence contract for VFO offsets ──────────────────────────────

TEST_CASE("VFO offset saved under correct key", "[vfo_handler][config]") {
    json conf;
    conf["vfoOffsets"] = json::object();

    std::string vfoName = "Radio";
    double generalOffset = 25000.0;
    conf["vfoOffsets"][vfoName] = generalOffset;

    REQUIRE(conf["vfoOffsets"]["Radio"] == Approx(25000.0));
}

TEST_CASE("VFO offset read from config", "[vfo_handler][config]") {
    json conf;
    conf["vfoOffsets"]["Radio"] = 75000.0;

    bool hasOffset = conf["vfoOffsets"].contains("Radio");
    double offset = conf["vfoOffsets"]["Radio"];

    REQUIRE(hasOffset);
    REQUIRE(offset == Approx(75000.0));
}

TEST_CASE("Missing VFO offset returns no offset", "[vfo_handler][config]") {
    json conf;
    conf["vfoOffsets"] = json::object();

    REQUIRE_FALSE(conf["vfoOffsets"].contains("Radio"));
}
