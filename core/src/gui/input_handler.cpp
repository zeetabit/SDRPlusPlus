#include <gui/input_handler.h>
#include <gui/gui_math.h>
#include <imgui.h>

void InputHandler::process(int tuningMode, ImGui::WaterfallVFO* vfo, bool lockControls) {
    if (lockControls) { return; }

    // Handle arrow keys
    if (vfo != NULL && (wf->isMouseInFFT() || wf->isMouseInWaterfall())) {
        bool freqChanged = false;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && !freqCtl->isDigitHovered()) {
            double nfreq = gui_math::stepFrequency(wf->getCenterFrequency() + vfo->generalOffset, vfo->snapInterval, -1);
            freqCtl->tune(tuningMode, wf->getSelectedVFO(), nfreq);
            freqChanged = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !freqCtl->isDigitHovered()) {
            double nfreq = gui_math::stepFrequency(wf->getCenterFrequency() + vfo->generalOffset, vfo->snapInterval, 1);
            freqCtl->tune(tuningMode, wf->getSelectedVFO(), nfreq);
            freqChanged = true;
        }
        if (freqChanged) {
            config->withConfig([&](json& conf) {
                conf["frequency"] = wf->getCenterFrequency();
                if (vfo != NULL) {
                    conf["vfoOffsets"][wf->getSelectedVFO()] = vfo->generalOffset;
                }
            });
        }
    }

    // Handle scrollwheel
    int wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0 && (wf->isMouseInFFT() || wf->isMouseInWaterfall())) {
        double nfreq;
        if (vfo != NULL) {
            double interval;
            if (ImGui::IsKeyDown(ImGuiKey_LeftShift)) {
                interval = vfo->snapInterval * 10.0;
            }
            else if (ImGui::IsKeyDown(ImGuiKey_LeftAlt)) {
                interval = vfo->snapInterval * 0.1;
            }
            else {
                interval = vfo->snapInterval;
            }
            nfreq = gui_math::stepFrequency(wf->getCenterFrequency() + vfo->generalOffset, interval, wheel);
        }
        else {
            nfreq = gui_math::scrollPanFrequency(wf->getCenterFrequency(), wf->getViewBandwidth(), wheel);
        }
        freqCtl->tune(tuningMode, wf->getSelectedVFO(), nfreq);
        freqCtl->setDisplayFrequency(nfreq);
        config->withConfig([&](json& conf) {
            conf["frequency"] = wf->getCenterFrequency();
            if (vfo != NULL) {
                conf["vfoOffsets"][wf->getSelectedVFO()] = vfo->generalOffset;
            }
        });
    }
}
