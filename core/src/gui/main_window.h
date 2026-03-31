#pragma once
#include <imgui/imgui.h>
#include <dsp/types.h>
#include <dsp/stream.h>
#include <signal_path/vfo_manager.h>
#include <string>
#include <utils/event.h>
#include <gui/tuner.h>
#include <gui/input_handler.h>
#include <gui/widgets/fft_controls.h>
#include <gui/top_bar.h>
#include <gui/menu_panel.h>
#include <gui/fft_manager.h>
#include <gui/view_state.h>
#include <gui/vfo_handler.h>
#include <gui/adapters/waterfall_state_adapter.h>
#include <gui/adapters/config_store_adapter.h>
#include <gui/adapters/fft_buffer_adapter.h>
#include <gui/adapters/frequency_control_adapter.h>
#include <gui/adapters/vfo_manager_adapter.h>

#define WINDOW_FLAGS ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBackground

class MainWindow {
public:
    void init();
    ImGui::WaterfallVFO* getSelectedVFO();
    void draw();
    void setViewBandwidthSlider(float bandwidth);
    bool sdrIsRunning();
    void setFirstMenuRender();

    // TODO: Replace with it's own class
    void setVFO(double freq);

    void setPlayState(bool _playing);
    bool isPlaying();

    friend class TopBar;

    bool lockWaterfallControls = false;
    bool playButtonLocked = false;

    Event<bool> onPlayStateChange;

    ViewStateCoordinator viewState;

private:
    WaterfallStateAdapter waterfallAdapter;
    ConfigStoreAdapter configAdapter;
    FFTBufferAdapter fftBufferAdapter;
    FrequencyControlAdapter freqCtlAdapter;
    VFOManagerAdapter vfoMgrAdapter;
    FFTManager fftManager;

    // GUI Variables
    bool playing = false;
    int tuningMode = tuner::TUNER_MODE_NORMAL;
    dsp::stream<dsp::complex_t> dummyStream;
    MenuPanel menuPanel;

    VFOHandler vfoHandler;

    InputHandler inputHandler;
    FFTControls fftControls;
    TopBar topBar;
};
