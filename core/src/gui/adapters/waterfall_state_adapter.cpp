#include <gui/adapters/waterfall_state_adapter.h>
#include <gui/gui.h>

double WaterfallStateAdapter::getBandwidth() { return gui::waterfall.getBandwidth(); }
double WaterfallStateAdapter::getViewBandwidth() { return gui::waterfall.getViewBandwidth(); }
double WaterfallStateAdapter::getViewOffset() { return gui::waterfall.getViewOffset(); }
double WaterfallStateAdapter::getCenterFrequency() { return gui::waterfall.getCenterFrequency(); }

void WaterfallStateAdapter::setViewBandwidth(double bw) { gui::waterfall.setViewBandwidth(bw); }
void WaterfallStateAdapter::setViewOffset(double offset) { gui::waterfall.setViewOffset(offset); }

const std::string& WaterfallStateAdapter::getSelectedVFO() { return gui::waterfall.selectedVFO; }
ImGui::WaterfallVFO* WaterfallStateAdapter::getVFO(const std::string& name) {
    auto it = gui::waterfall.vfos.find(name);
    return (it != gui::waterfall.vfos.end()) ? it->second : nullptr;
}

bool WaterfallStateAdapter::isMouseInFFT() { return gui::waterfall.mouseInFFT; }
bool WaterfallStateAdapter::isMouseInWaterfall() { return gui::waterfall.mouseInWaterfall; }
double WaterfallStateAdapter::getSelectedVFOSNR() { return gui::waterfall.selectedVFOSNR; }

bool WaterfallStateAdapter::hasSelectedVFOChanged() { return gui::waterfall.selectedVFOChanged; }
void WaterfallStateAdapter::clearSelectedVFOChanged() { gui::waterfall.selectedVFOChanged = false; }

bool WaterfallStateAdapter::hasCenterFreqMoved() { return gui::waterfall.centerFreqMoved; }
void WaterfallStateAdapter::clearCenterFreqMoved() { gui::waterfall.centerFreqMoved = false; }

int WaterfallStateAdapter::getFFTHeight() { return gui::waterfall.getFFTHeight(); }
