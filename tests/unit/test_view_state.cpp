#include <catch.hpp>
#include <gui/widgets/waterfall.h>
#include <gui/interfaces/iwaterfall_state.h>
#include <gui/interfaces/iconfig_store.h>
#include <gui/view_state.h>
#include <gui/gui_math.h>

namespace {

class MockWaterfall : public IWaterfallState {
public:
    double bandwidth = 2400000.0;
    double viewBandwidth = 2400000.0;
    double viewOffset = 0.0;
    double centerFrequency = 100000000.0;
    std::string selectedVFO;
    std::map<std::string, ImGui::WaterfallVFO*> vfos;
    bool mouseInFFT = false;
    bool mouseInWaterfall = false;
    double snr = 0.0;
    bool selectedVFOChanged = false;

    double getBandwidth() override { return bandwidth; }
    double getViewBandwidth() override { return viewBandwidth; }
    double getViewOffset() override { return viewOffset; }
    double getCenterFrequency() override { return centerFrequency; }
    void setViewBandwidth(double bw) override { viewBandwidth = bw; }
    void setViewOffset(double offset) override { viewOffset = offset; }
    const std::string& getSelectedVFO() override { return selectedVFO; }
    ImGui::WaterfallVFO* getVFO(const std::string& name) override {
        auto it = vfos.find(name);
        return (it != vfos.end()) ? it->second : nullptr;
    }
    bool isMouseInFFT() override { return mouseInFFT; }
    bool isMouseInWaterfall() override { return mouseInWaterfall; }
    double getSelectedVFOSNR() override { return snr; }
    bool hasSelectedVFOChanged() override { return selectedVFOChanged; }
    void clearSelectedVFOChanged() override { selectedVFOChanged = false; }
    bool hasCenterFreqMoved() override { return false; }
    void clearCenterFreqMoved() override {}
    int getFFTHeight() override { return 300; }
};

class MockConfig : public IConfigStore {
public:
    json data;

    MockConfig() : data(json::object()) {}

    void readConfig(std::function<void(const json&)> fn) override { fn(data); }
    void withConfig(std::function<void(json&)> fn) override { fn(data); }
};

} // namespace

// ── loadFromConfig ───────────────────────────────────────────────────────────

TEST_CASE("ViewState loadFromConfig restores saved zoom", "[view_state]") {
    MockWaterfall wf;
    MockConfig config;
    // Slider and viewBw must be consistent: slider=0.5 → viewBw = 1000 + 0.25 * (8M-1000) = 2000750
    // Using the forward formula to derive consistent values:
    double expectedViewBw = gui_math::zoomSliderToBandwidth(0.5f, wf.bandwidth);
    config.data["bandwidth_slider"] = 0.5f;
    config.data["bandwidth_view"] = expectedViewBw;
    config.data["bandwidth_offset"] = 50000.0;

    ViewStateCoordinator vs(&wf, &config);
    float sliderBw;
    vs.loadFromConfig(sliderBw);

    REQUIRE(sliderBw == Approx(0.5f));
    REQUIRE(wf.viewBandwidth == Approx(expectedViewBw));
    REQUIRE(wf.viewOffset == Approx(50000.0));
}

TEST_CASE("ViewState loadFromConfig skips invalid values", "[view_state]") {
    MockWaterfall wf;
    wf.viewBandwidth = 8000000.0;
    wf.viewOffset = 0.0;

    MockConfig config;
    config.data["bandwidth_slider"] = 0.0f;
    config.data["bandwidth_view"] = 0.0f;
    config.data["bandwidth_offset"] = 0.0;

    ViewStateCoordinator vs(&wf, &config);
    float sliderBw;
    vs.loadFromConfig(sliderBw);

    // viewBw <= 1.0, so nothing should change
    REQUIRE(wf.viewBandwidth == Approx(8000000.0));
}

TEST_CASE("ViewState loadFromConfig restores offset after zoom centers on VFO", "[view_state]") {
    MockWaterfall wf;
    wf.bandwidth = 8000000.0;

    // Simulate a VFO at offset 100000 that zoom would center on
    ImGui::WaterfallVFO vfo = {};
    vfo.centerOffset = 100000.0;
    wf.selectedVFO = "Radio";
    wf.vfos["Radio"] = &vfo;

    MockConfig config;
    config.data["bandwidth_slider"] = 0.5f;
    config.data["bandwidth_view"] = 4000000.0;
    config.data["bandwidth_offset"] = -200000.0; // user had panned away from VFO

    ViewStateCoordinator vs(&wf, &config);
    float sliderBw;
    vs.loadFromConfig(sliderBw);

    // The saved offset should win over VFO centering
    REQUIRE(wf.viewOffset == Approx(-200000.0));
}

// ── onZoomChange ─────────────────────────────────────────────────────────────

TEST_CASE("ViewState onZoomChange maps slider to bandwidth", "[view_state]") {
    MockWaterfall wf;
    wf.bandwidth = 2400000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.onZoomChange(1.0f);

    REQUIRE(wf.viewBandwidth == Approx(2400000.0));
}

TEST_CASE("ViewState onZoomChange at 0.0 gives minimum bandwidth", "[view_state]") {
    MockWaterfall wf;
    wf.bandwidth = 2400000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.onZoomChange(0.0f);

    REQUIRE(wf.viewBandwidth == Approx(1000.0));
}

// ── onZoomChanged ────────────────────────────────────────────────────────────

TEST_CASE("ViewState onZoomChanged sets bandwidth and saves config", "[view_state]") {
    MockWaterfall wf;
    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.onZoomChanged(0.7f, 1500000.0, true);

    REQUIRE(wf.viewBandwidth == Approx(1500000.0));
    REQUIRE(config.data["bandwidth_slider"] == Approx(0.7f));
    REQUIRE(config.data["bandwidth_view"] == Approx(1500000.0));
    REQUIRE(config.data.contains("bandwidth_offset"));
}

TEST_CASE("ViewState onZoomChanged without save does not write config", "[view_state]") {
    MockWaterfall wf;
    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.onZoomChanged(0.5f, 1000000.0, false);

    REQUIRE(wf.viewBandwidth == Approx(1000000.0));
    REQUIRE_FALSE(config.data.contains("bandwidth_slider"));
}

TEST_CASE("ViewState onZoomChanged centers on selected VFO", "[view_state]") {
    MockWaterfall wf;
    ImGui::WaterfallVFO vfo = {};
    vfo.centerOffset = 300000.0;
    wf.selectedVFO = "Radio";
    wf.vfos["Radio"] = &vfo;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.onZoomChanged(0.5f, 1000000.0, false);

    REQUIRE(wf.viewOffset == Approx(300000.0));
}

TEST_CASE("ViewState onZoomChanged with no VFO leaves offset unchanged", "[view_state]") {
    MockWaterfall wf;
    wf.viewOffset = 42000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.onZoomChanged(0.5f, 1000000.0, false);

    REQUIRE(wf.viewOffset == Approx(42000.0));
}

// ── persistViewOffsetIfChanged ───────────────────────────────────────────────

TEST_CASE("ViewState persistViewOffsetIfChanged saves on change", "[view_state]") {
    MockWaterfall wf;
    wf.viewOffset = 10000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.persistViewOffsetIfChanged();

    REQUIRE(config.data["bandwidth_offset"] == Approx(10000.0));
}

TEST_CASE("ViewState persistViewOffsetIfChanged deduplicates", "[view_state]") {
    MockWaterfall wf;
    wf.viewOffset = 10000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.persistViewOffsetIfChanged();

    // Mutate config to detect if it gets overwritten
    config.data["bandwidth_offset"] = -999.0;
    vs.persistViewOffsetIfChanged(); // offset unchanged, should not write

    REQUIRE(config.data["bandwidth_offset"] == Approx(-999.0));
}

TEST_CASE("ViewState persistViewOffsetIfChanged writes again after offset changes", "[view_state]") {
    MockWaterfall wf;
    wf.viewOffset = 10000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.persistViewOffsetIfChanged();

    wf.viewOffset = 20000.0;
    vs.persistViewOffsetIfChanged();

    REQUIRE(config.data["bandwidth_offset"] == Approx(20000.0));
}

// ── persistFrequency ─────────────────────────────────────────────────────────

TEST_CASE("ViewState persistFrequency writes center frequency", "[view_state]") {
    MockWaterfall wf;
    wf.centerFrequency = 145000000.0;

    MockConfig config;
    config.data = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.persistFrequency();

    REQUIRE(config.data["frequency"] == Approx(145000000.0));
}

// ── persistFrequencyAndVFO ───────────────────────────────────────────────────

TEST_CASE("ViewState persistFrequencyAndVFO writes both", "[view_state]") {
    MockWaterfall wf;
    wf.centerFrequency = 145000000.0;

    MockConfig config;
    config.data["vfoOffsets"] = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.persistFrequencyAndVFO("Radio", 25000.0);

    REQUIRE(config.data["frequency"] == Approx(145000000.0));
    REQUIRE(config.data["vfoOffsets"]["Radio"] == Approx(25000.0));
}

// ── persistVFOOffset ─────────────────────────────────────────────────────────

TEST_CASE("ViewState persistVFOOffset writes named offset", "[view_state]") {
    MockWaterfall wf;

    MockConfig config;
    config.data["vfoOffsets"] = json::object();

    ViewStateCoordinator vs(&wf, &config);
    vs.persistVFOOffset("Radio", -50000.0);

    REQUIRE(config.data["vfoOffsets"]["Radio"] == Approx(-50000.0));
}
