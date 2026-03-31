#include <gui/view_state.h>
#include <gui/widgets/waterfall.h>
#include <gui/gui_math.h>
#include <utils/flog.h>
#include <algorithm>

void ViewStateCoordinator::loadFromConfig(float& sliderBw) {
    float viewBw = 0;
    double viewOffset = 0;
    sliderBw = 0;
    config->readConfig([&](const json& conf) {
        sliderBw = conf.value("bandwidth_slider", 0.0f);
        viewBw = conf.value("bandwidth_view", 0.0f);
        viewOffset = conf.value("bandwidth_offset", 0.0);
    });
    if (sliderBw >= 0 && viewBw > 1.0) {
        flog::info("Loaded zoom: slider={0}, view={1}, offset={2}", sliderBw, viewBw, viewOffset);
        this->onZoomChanged(sliderBw, viewBw, false);
        // Restore saved offset after onZoomChanged (which may override it with VFO center)
        wf->setViewOffset(viewOffset);
        lastSavedViewOffset = viewOffset;
    }
}

void ViewStateCoordinator::onZoomChange(float sliderValue) {
    double finalBw = gui_math::zoomSliderToBandwidth(sliderValue, wf->getBandwidth());
    this->onZoomChanged(sliderValue, finalBw);
}

void ViewStateCoordinator::onZoomChanged(float sliderValue, double viewBandwidthValue, bool saveValues) {
    wf->setViewBandwidth(viewBandwidthValue);

    // Center on selected VFO when zooming
    const auto& selVFO = wf->getSelectedVFO();
    if (!selVFO.empty()) {
        auto* vfo = wf->getVFO(selVFO);
        if (vfo) {
            wf->setViewOffset(vfo->centerOffset);
        }
    }

    if (saveValues) {
        double viewOffset = wf->getViewOffset();
        flog::info("store zoom: slider={0}, view={1}, offset={2}", sliderValue, viewBandwidthValue, viewOffset);
        config->withConfig([&](json& conf) {
            conf["bandwidth_slider"] = sliderValue;
            conf["bandwidth_view"] = viewBandwidthValue;
            conf["bandwidth_offset"] = viewOffset;
        });
    }
}

void ViewStateCoordinator::persistViewOffsetIfChanged() {
    double currentViewOffset = wf->getViewOffset();
    if (currentViewOffset != lastSavedViewOffset) {
        lastSavedViewOffset = currentViewOffset;
        config->withConfig([&](json& conf) { conf["bandwidth_offset"] = currentViewOffset; });
    }
}

void ViewStateCoordinator::persistFrequency() {
    config->withConfig([this](json& conf) { conf["frequency"] = wf->getCenterFrequency(); });
}

void ViewStateCoordinator::persistFrequencyAndVFO(const std::string& vfoName, double vfoOffset) {
    config->withConfig([&](json& conf) {
        conf["frequency"] = wf->getCenterFrequency();
        conf["vfoOffsets"][vfoName] = vfoOffset;
    });
}

void ViewStateCoordinator::persistVFOOffset(const std::string& vfoName, double offset) {
    config->withConfig([&](json& conf) {
        conf["vfoOffsets"][vfoName] = offset;
    });
}
