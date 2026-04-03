#include <catch.hpp>
#include <gui/view_state.h>
#include <gui/gui_math.h>
#include <cmath>
#include <string>

// ============================================================
// Mock implementations
// ============================================================

class MockWaterfall : public IWaterfallState {
public:
    double wholeBandwidth = 2400000.0;
    double viewBandwidth = 0;
    double viewOffset = 0;
    double centerFreq = 14070000.0;
    std::string selectedVFO;

    double getBandwidth() override { return wholeBandwidth; }
    double getViewBandwidth() override { return viewBandwidth; }
    double getViewOffset() override { return viewOffset; }
    double getCenterFrequency() override { return centerFreq; }

    void setViewBandwidth(double bw) override { viewBandwidth = bw; }
    void setViewOffset(double offset) override {
        // Mimic waterfall clamping: keep view within whole bandwidth
        if (offset - (viewBandwidth / 2.0) < -(wholeBandwidth / 2.0)) {
            offset = (viewBandwidth / 2.0) - (wholeBandwidth / 2.0);
        }
        if (offset + (viewBandwidth / 2.0) > (wholeBandwidth / 2.0)) {
            offset = (wholeBandwidth / 2.0) - (viewBandwidth / 2.0);
        }
        viewOffset = offset;
    }

    const std::string& getSelectedVFO() override { return selectedVFO; }
    ImGui::WaterfallVFO* getVFO(const std::string&) override { return nullptr; }

    bool isMouseInFFT() override { return false; }
    bool isMouseInWaterfall() override { return false; }
    double getSelectedVFOSNR() override { return 0; }
    bool hasSelectedVFOChanged() override { return false; }
    void clearSelectedVFOChanged() override {}
    bool hasCenterFreqMoved() override { return false; }
    void clearCenterFreqMoved() override {}
    int getFFTHeight() override { return 200; }
};

class MockConfig : public IConfigStore {
public:
    json data;

    void readConfig(std::function<void(const json&)> fn) override { fn(data); }
    void withConfig(std::function<void(json&)> fn) override { fn(data); }
};

// ============================================================
// gui_math round-trip tests
// ============================================================

TEST_CASE("gui_math slider↔bandwidth round-trip", "[zoom][math]") {
    double wholeBw = 2400000.0;

    SECTION("slider=0 maps to minimum bandwidth 1000 Hz") {
        double bw = gui_math::zoomSliderToBandwidth(0.0f, wholeBw);
        REQUIRE(bw == Approx(1000.0));
        float slider = gui_math::bandwidthToZoomSlider(1000.0, wholeBw);
        REQUIRE(slider == Approx(0.0f).margin(1e-6));
    }

    SECTION("slider=1 maps to full bandwidth") {
        double bw = gui_math::zoomSliderToBandwidth(1.0f, wholeBw);
        REQUIRE(bw == Approx(wholeBw));
        float slider = gui_math::bandwidthToZoomSlider(wholeBw, wholeBw);
        REQUIRE(slider == Approx(1.0f).margin(1e-6));
    }

    SECTION("round-trip at multiple slider values") {
        for (float s = 0.0f; s <= 1.0f; s += 0.05f) {
            double bw = gui_math::zoomSliderToBandwidth(s, wholeBw);
            float recovered = gui_math::bandwidthToZoomSlider(bw, wholeBw);
            REQUIRE(recovered == Approx(s).margin(1e-5));
        }
    }

    SECTION("round-trip with logged values: slider=0.213235, wholeBw=2400249") {
        double logWholeBw = 2400249.0;
        float logSlider = 0.213235f;
        double bw = gui_math::zoomSliderToBandwidth(logSlider, logWholeBw);
        REQUIRE(bw == Approx(110080.79).margin(50.0));
        float recovered = gui_math::bandwidthToZoomSlider(bw, logWholeBw);
        REQUIRE(recovered == Approx(logSlider).margin(1e-4));
    }

    SECTION("wholeBw <= 1000 returns slider=1") {
        REQUIRE(gui_math::bandwidthToZoomSlider(500.0, 1000.0) == Approx(1.0f));
        REQUIRE(gui_math::bandwidthToZoomSlider(500.0, 500.0) == Approx(1.0f));
    }

    SECTION("viewBw clamped to [0,1] factor range") {
        // viewBw below minimum
        float s = gui_math::bandwidthToZoomSlider(500.0, 2400000.0);
        REQUIRE(s == Approx(0.0f));
        // viewBw above wholeBw
        s = gui_math::bandwidthToZoomSlider(3000000.0, 2400000.0);
        REQUIRE(s == Approx(1.0f));
    }
}

// ============================================================
// ViewStateCoordinator save/restore tests
// ============================================================

TEST_CASE("ViewState save then restore at same bandwidth", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;
    wf.wholeBandwidth = 2400000.0;

    ViewStateCoordinator vsc(&wf, &cfg);

    // Simulate zoom to slider=0.213235
    float slider = 0.213235f;
    vsc.onZoomChange(slider);

    // Verify saved values
    REQUIRE(cfg.data.contains("bandwidth_slider"));
    REQUIRE(cfg.data.contains("bandwidth_view"));
    float savedSlider = cfg.data["bandwidth_slider"].get<float>();
    double savedView = cfg.data["bandwidth_view"].get<double>();
    REQUIRE(savedSlider == Approx(slider).margin(1e-5));
    REQUIRE(savedView == Approx(110080.0).margin(200.0));

    // Set a view offset and persist it
    wf.setViewOffset(994019.0);
    vsc.persistViewOffsetIfChanged();
    REQUIRE(cfg.data["bandwidth_offset"].get<double>() == Approx(994019.0).margin(1.0));

    // Restore at same bandwidth
    float restoredSlider = 0;
    vsc.loadFromConfig(restoredSlider);
    REQUIRE(restoredSlider == Approx(slider).margin(1e-4));
    REQUIRE(wf.viewBandwidth == Approx(savedView).margin(1.0));
    REQUIRE(wf.viewOffset == Approx(994019.0).margin(1.0));
}

TEST_CASE("ViewState restore with smaller total bandwidth", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;

    // Saved at 2.4 MHz
    cfg.data["bandwidth_slider"] = 0.213235f;
    cfg.data["bandwidth_view"] = 110080.79;
    cfg.data["bandwidth_offset"] = 994019.0;

    // Restore at 1 MHz — view fits, but offset needs clamping
    wf.wholeBandwidth = 1000000.0;

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    // View bandwidth should be preserved (110 kHz < 1 MHz)
    REQUIRE(wf.viewBandwidth == Approx(110080.79).margin(1.0));

    // Slider recalculated for new wholeBw
    float expected = gui_math::bandwidthToZoomSlider(110080.79, 1000000.0);
    REQUIRE(slider == Approx(expected).margin(1e-4));
    REQUIRE(slider > 0.213235f); // wider relative to smaller wholeBw → larger slider

    // Offset should be clamped by waterfall (994019 exceeds ±500k range)
    double maxOffset = (wf.wholeBandwidth / 2.0) - (wf.viewBandwidth / 2.0);
    REQUIRE(wf.viewOffset <= maxOffset);
}

TEST_CASE("ViewState restore with larger total bandwidth", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;

    cfg.data["bandwidth_slider"] = 0.213235f;
    cfg.data["bandwidth_view"] = 110080.79;
    cfg.data["bandwidth_offset"] = 994019.0;

    // Restore at 10 MHz
    wf.wholeBandwidth = 10000000.0;

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    // View bandwidth preserved
    REQUIRE(wf.viewBandwidth == Approx(110080.79).margin(1.0));

    // Slider is smaller (same absolute view in a much wider whole)
    REQUIRE(slider < 0.213235f);

    // Offset preserved (994k well within ±5M range)
    REQUIRE(wf.viewOffset == Approx(994019.0).margin(1.0));
}

TEST_CASE("ViewState restore when saved viewBw exceeds new wholeBw", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;

    // Saved at wide bandwidth
    cfg.data["bandwidth_slider"] = 0.5f;
    cfg.data["bandwidth_view"] = 1500000.0;
    cfg.data["bandwidth_offset"] = 0.0;

    // Restore at narrower bandwidth
    wf.wholeBandwidth = 500000.0;

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    // viewBw clamped to wholeBw
    REQUIRE(wf.viewBandwidth == Approx(500000.0));
    // slider=1.0 (fully zoomed out)
    REQUIRE(slider == Approx(1.0f));
}

TEST_CASE("ViewState no restore when saved viewBw is 0", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;
    wf.wholeBandwidth = 2400000.0;

    // No saved zoom (fresh config)
    cfg.data["bandwidth_slider"] = 0.0f;
    cfg.data["bandwidth_view"] = 0.0f;
    cfg.data["bandwidth_offset"] = 0.0;

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    // Should not apply zoom (viewBw <= 1.0 guard)
    REQUIRE(slider == Approx(0.0f));
    REQUIRE(wf.viewBandwidth == 0.0); // untouched
}

TEST_CASE("ViewState auto-persists corrected offset on load when clamped", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;
    wf.wholeBandwidth = 1000000.0;

    // Saved with out-of-range offset
    cfg.data["bandwidth_slider"] = 0.3f;
    cfg.data["bandwidth_view"] = 110000.0;
    cfg.data["bandwidth_offset"] = 994019.0; // way out of range for 1 MHz

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    // After restore, waterfall clamped the offset
    double actualOffset = wf.viewOffset;
    REQUIRE(actualOffset < 994019.0); // was clamped

    // loadFromConfig should have auto-persisted the corrected values
    double persistedOffset = cfg.data["bandwidth_offset"].get<double>();
    REQUIRE(persistedOffset == Approx(actualOffset).margin(1.0));

    // lastSavedViewOffset should match actual, so no-op on next persist call
    vsc.persistViewOffsetIfChanged();
    REQUIRE(cfg.data["bandwidth_offset"].get<double>() == Approx(actualOffset).margin(1.0));
}

TEST_CASE("ViewState auto-persists corrected viewBw on load when clamped", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;

    // Saved at wide zoom
    cfg.data["bandwidth_slider"] = 0.5f;
    cfg.data["bandwidth_view"] = 2000000.0;
    cfg.data["bandwidth_offset"] = 0.0;

    // Restore at narrower bandwidth — viewBw gets clamped
    wf.wholeBandwidth = 500000.0;

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    // Config should reflect the clamped viewBw
    double persistedView = cfg.data["bandwidth_view"].get<double>();
    REQUIRE(persistedView == Approx(500000.0));
    float persistedSlider = cfg.data["bandwidth_slider"].get<float>();
    REQUIRE(persistedSlider == Approx(1.0f));
}

TEST_CASE("ViewState onZoomChange stores correct values", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;
    wf.wholeBandwidth = 2400000.0;

    ViewStateCoordinator vsc(&wf, &cfg);

    // Zoom to various levels and verify consistency
    for (float s : {0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        vsc.onZoomChange(s);
        double expectedBw = gui_math::zoomSliderToBandwidth(s, wf.wholeBandwidth);
        REQUIRE(wf.viewBandwidth == Approx(expectedBw).margin(1.0));
        REQUIRE(cfg.data["bandwidth_slider"].get<float>() == Approx(s).margin(1e-5));
        REQUIRE(cfg.data["bandwidth_view"].get<double>() == Approx(expectedBw).margin(1.0));
    }
}

TEST_CASE("ViewState slider must match waterfall after restore", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;
    wf.wholeBandwidth = 2400000.0;

    // Pre-populate config with zoom state
    cfg.data["bandwidth_slider"] = 0.213235f;
    cfg.data["bandwidth_view"] = 110080.79;
    cfg.data["bandwidth_offset"] = 500000.0;

    // Simulate MainWindow init sequence:
    // 1. Set default slider to 1.0 (fully zoomed out)
    float sliderBw = 1.0f;
    // 2. Restore zoom from config
    ViewStateCoordinator vsc(&wf, &cfg);
    vsc.loadFromConfig(sliderBw);

    // The slider value returned must NOT be 1.0 — it must reflect the restored zoom
    REQUIRE(sliderBw != Approx(1.0f));
    REQUIRE(sliderBw == Approx(0.213235f).margin(1e-3));

    // The slider and waterfall must be consistent:
    // applying sliderBw to the formula should yield the waterfall's viewBandwidth
    double expectedBw = gui_math::zoomSliderToBandwidth(sliderBw, wf.wholeBandwidth);
    REQUIRE(wf.viewBandwidth == Approx(expectedBw).margin(1.0));
}

TEST_CASE("ViewState exact logged values round-trip", "[zoom][viewstate]") {
    MockWaterfall wf;
    MockConfig cfg;

    // The exact values from the user's log
    cfg.data["bandwidth_slider"] = 0.213235f;
    cfg.data["bandwidth_view"] = 110080.792416;
    cfg.data["bandwidth_offset"] = 994019.066635;

    // Back-calculate the original wholeBw from slider→viewBw mapping
    // slider^2 * (whole - 1000) + 1000 = 110080.79
    // 0.213235^2 * (whole - 1000) = 109080.79
    // whole = 109080.79 / 0.045469 + 1000 ≈ 2400249
    wf.wholeBandwidth = 2400249.0;

    ViewStateCoordinator vsc(&wf, &cfg);
    float slider = 0;
    vsc.loadFromConfig(slider);

    REQUIRE(slider == Approx(0.213235f).margin(1e-3));
    REQUIRE(wf.viewBandwidth == Approx(110080.79).margin(1.0));
    REQUIRE(wf.viewOffset == Approx(994019.07).margin(1.0));
}
