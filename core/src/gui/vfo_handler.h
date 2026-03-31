#pragma once
#include <gui/interfaces/iwaterfall_state.h>
#include <gui/interfaces/iconfig_store.h>
#include <gui/interfaces/ifrequency_control.h>
#include <gui/interfaces/ivfo_manager.h>
#include <signal_path/vfo_manager.h>

class ViewStateCoordinator;

class VFOHandler {
public:
    VFOHandler() = default;
    VFOHandler(IWaterfallState* wf, IConfigStore* config, IFrequencyControl* freqCtl, IVFOManager* vfoMgr, ViewStateCoordinator* viewState = nullptr)
        : wf(wf), config(config), freqCtl(freqCtl), vfoMgr(vfoMgr), viewState(viewState) {}

    void inject(IWaterfallState* wf, IConfigStore* config, IFrequencyControl* freqCtl, IVFOManager* vfoMgr, ViewStateCoordinator* viewState = nullptr) {
        this->wf = wf; this->config = config; this->freqCtl = freqCtl; this->vfoMgr = vfoMgr; this->viewState = viewState;
    }

    void init();
    void processFrame(int tuningMode, ImGui::WaterfallVFO* vfo);

    void setInitComplete(bool complete) { initComplete = complete; }

private:
    static void onVFOCreated(VFOManager::VFO* vfo, void* ctx);
    EventHandler<VFOManager::VFO*> vfoCreatedHandler;
    bool initComplete = false;

    IWaterfallState* wf = nullptr;
    IConfigStore* config = nullptr;
    IFrequencyControl* freqCtl = nullptr;
    IVFOManager* vfoMgr = nullptr;
    ViewStateCoordinator* viewState = nullptr;
};
