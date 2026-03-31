#include <gui/vfo_handler.h>
#include <gui/view_state.h>
#include <gui/gui.h>
#include <gui/gui_math.h>
#include <gui/tuner.h>
#include <signal_path/signal_path.h>
#include <algorithm>

void VFOHandler::init() {
    vfoCreatedHandler.handler = onVFOCreated;
    vfoCreatedHandler.ctx = this;
    sigpath::vfoManager.onVfoCreated.bindHandler(&vfoCreatedHandler);
}

void VFOHandler::processFrame(int tuningMode, ImGui::WaterfallVFO* vfo) {
    // Handle VFO movement
    if (vfo != NULL) {
        if (vfo->centerOffsetChanged) {
            if (tuningMode == tuner::TUNER_MODE_CENTER) {
                freqCtl->tune(tuner::TUNER_MODE_CENTER, wf->getSelectedVFO(), wf->getCenterFrequency() + vfo->generalOffset);
            }
            freqCtl->setDisplayFrequency(wf->getCenterFrequency() + vfo->generalOffset);
            freqCtl->clearFrequencyChanged();
            config->withConfig([&](json& conf) {
                conf["vfoOffsets"][wf->getSelectedVFO()] = vfo->generalOffset;
            });
        }
    }

    vfoMgr->updateFromWaterfall(&gui::waterfall);

    // Handle selection of another VFO
    if (wf->hasSelectedVFOChanged()) {
        freqCtl->setDisplayFrequency((vfo != NULL) ? (vfo->generalOffset + wf->getCenterFrequency()) : wf->getCenterFrequency());
        wf->clearSelectedVFOChanged();
        freqCtl->clearFrequencyChanged();
    }

    // Handle change in selected frequency
    if (freqCtl->hasFrequencyChanged()) {
        freqCtl->clearFrequencyChanged();
        freqCtl->tune(tuningMode, wf->getSelectedVFO(), freqCtl->getDisplayFrequency());
        if (vfo != NULL) {
            vfo->centerOffsetChanged = false;
            vfo->lowerOffsetChanged = false;
            vfo->upperOffsetChanged = false;
        }
        if (viewState) {
            if (vfo != NULL) {
                viewState->persistFrequencyAndVFO(wf->getSelectedVFO(), vfo->generalOffset);
            }
            else {
                viewState->persistFrequency();
            }
        }
    }

    // Handle dragging the frequency scale
    if (wf->hasCenterFreqMoved()) {
        wf->clearCenterFreqMoved();
        freqCtl->tuneSource(wf->getCenterFrequency());
        if (vfo != NULL) {
            freqCtl->setDisplayFrequency(wf->getCenterFrequency() + vfo->generalOffset);
        }
        else {
            freqCtl->setDisplayFrequency(wf->getCenterFrequency());
        }
        if (viewState) {
            viewState->persistFrequency();
        }
    }
}

void VFOHandler::onVFOCreated(VFOManager::VFO* vfo, void* ctx) {
    VFOHandler* _this = (VFOHandler*)ctx;
    std::string name = vfo->getName();
    bool hasOffset = false;
    double offset = 0;
    _this->config->readConfig([&](const json& conf) {
        if (conf["vfoOffsets"].contains(name)) {
            hasOffset = true;
            offset = conf["vfoOffsets"][name];
        }
    });
    if (!hasOffset) { return; }

    double viewBW = _this->wf->getViewBandwidth();
    double viewOffset = _this->wf->getViewOffset();

    double newOffset = gui_math::clampVFOOffset(offset, viewOffset, viewBW);

    _this->vfoMgr->setCenterOffset(name, _this->initComplete ? newOffset : offset);
}
