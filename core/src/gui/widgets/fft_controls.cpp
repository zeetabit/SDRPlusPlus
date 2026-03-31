#include <gui/widgets/fft_controls.h>
#include <gui/main_window.h>
#include <gui/gui.h>
#include <gui/gui_math.h>
#include <gui/style.h>
#include <imgui.h>
#include <config.h>
#include <core.h>
#include <algorithm>

void FFTControls::draw(MainWindow& mw) {
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Zoom").x / 2.0));
    ImGui::TextUnformatted("Zoom");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - 10 * style::uiScale);
    ImVec2 wfSliderSize(20.0 * style::uiScale, 150.0 * style::uiScale);
    if (ImGui::VSliderFloat("##_7_", wfSliderSize, &bw, 1.0, 0.0, "")) {
        mw.viewState.onZoomChange(bw);
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Max").x / 2.0));
    ImGui::TextUnformatted("Max");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - 10 * style::uiScale);
    if (ImGui::VSliderFloat("##_8_", wfSliderSize, &fftMax, 0.0, -160.0f, "")) {
        fftMax = gui_math::constrainFFTMax(fftMax, fftMin);
        core::configManager.withConfig([&](json& conf) { conf["max"] = fftMax; });
    }

    ImGui::NewLine();

    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - (ImGui::CalcTextSize("Min").x / 2.0));
    ImGui::TextUnformatted("Min");
    ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2.0) - 10 * style::uiScale);
    ImGui::SetItemUsingMouseWheel();
    if (ImGui::VSliderFloat("##_9_", wfSliderSize, &fftMin, 0.0, -160.0f, "")) {
        fftMin = gui_math::constrainFFTMin(fftMin, fftMax);
        core::configManager.withConfig([&](json& conf) { conf["min"] = fftMin; });
    }

    gui::waterfall.setFFTMin(fftMin);
    gui::waterfall.setFFTMax(fftMax);
    gui::waterfall.setWaterfallMin(fftMin);
    gui::waterfall.setWaterfallMax(fftMax);

    // Persist FFT height changes
    int currentHeight = gui::waterfall.getFFTHeight();
    if (currentHeight != fftHeight) {
        fftHeight = currentHeight;
        core::configManager.withConfig([&](json& conf) { conf["fftHeight"] = fftHeight; });
    }
}
