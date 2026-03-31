#pragma once
#include <gui/interfaces/iwaterfall_state.h>
#include <gui/interfaces/iconfig_store.h>
#include <gui/interfaces/ifrequency_control.h>
#include <gui/widgets/waterfall.h>

class InputHandler {
public:
    InputHandler() = default;
    InputHandler(IWaterfallState* wf, IConfigStore* config, IFrequencyControl* freqCtl)
        : wf(wf), config(config), freqCtl(freqCtl) {}

    void inject(IWaterfallState* wf, IConfigStore* config, IFrequencyControl* freqCtl) {
        this->wf = wf; this->config = config; this->freqCtl = freqCtl;
    }

    void process(int tuningMode, ImGui::WaterfallVFO* vfo, bool lockControls);

private:
    IWaterfallState* wf = nullptr;
    IConfigStore* config = nullptr;
    IFrequencyControl* freqCtl = nullptr;
};
