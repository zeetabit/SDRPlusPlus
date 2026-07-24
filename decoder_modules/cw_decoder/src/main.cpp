#include <module.h>
#include <module_config.h>
#include <module_manifest.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <dsp/sink/handler_sink.h>

#include "cw/channel_manager.h"
#include "cw/menu.h"

#define CW_VFO_BANDWIDTH 3000.0f
#define CW_SNAP_INTERVAL 10.0  // 10 Hz — fine tuning for CW

ConfigManager config;

SDRPP_MOD_INFO{
    "cw_decoder", "Multi-channel CW Decoder", "zetabit", 1, 0, 0, -1
};

SDRPP_MOD_INFO_V2{
    "cw_decoder", "Multi-channel CW Decoder", "zetabit", 1, 0, 0, -1,
    SDRPP_API_VERSION, MOD_CAP_DECODER, 0, nullptr,
    R"({"autoDetect":true,"scanThreshold":10,"listenMode":false})",
    "cw_decoder_config.json"
};

SDRPP_MOD_CONFIG(config);

class CWDecoderModule : public ModuleManager::Instance {
public:
    CWDecoderModule(std::string name, ModuleConfig* cfg) : name(name), cfg(cfg) {
        mgr.init(CW_SAMPLERATE);
        mgr.debugLog = cfg->get<bool>("debugLog", false);
        mgr.wordCorrection = cfg->get<bool>("wordCorrection", false);   // OFF: raw decode visible

        listenMode = cfg->get<bool>("listenMode", false);
        mgr.loadConfig(cfg);

        sink.init(NULL, iqHandler, this);
        gui::menu.registerEntry(name, menuHandler, this, this);
        enable();
    }

    ~CWDecoderModule() {
        if (enabled) { disable(); }
        gui::menu.removeEntry(name);
    }

    void postInit() {}

    void enable() {
        if (enabled) { return; }
        enabled = true;
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER,
            0, CW_VFO_BANDWIDTH, CW_SAMPLERATE, CW_VFO_BANDWIDTH, CW_VFO_BANDWIDTH, true);
        if (vfo) {
            vfo->setSnapInterval(CW_SNAP_INTERVAL);
        }
        mgr.createPinnedChannels();
        sink.setInput(vfo->output);
        sink.start();

        if (listenMode) { applyListenMode(true); }
    }

    void disable() {
        if (!enabled) { return; }
        enabled = false;  // Guard iqHandler first — stops processing immediately
        sink.stop();
        mgr.clearEntries();
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); vfo = nullptr; }
    }

    bool isEnabled() { return enabled; }

private:
    void applyListenMode(bool entering) {
        if (!vfo) { return; }
        if (entering) {
            // Hide CW VFO — radio VFO is the only one visible
            vfo->clearMarkers();
            vfo->setVisible(false);
            vfo->setZOrder(-1);
            vfo->setBandwidthLimits(CW_VFO_BANDWIDTH, CW_SAMPLERATE, false);
            auto [tName, tVfo] = findTargetVFO();
            if (tVfo) {
                vfo->setCenterOffset(tVfo->centerOffset);
                lastTargetOffset = tVfo->centerOffset;
                double bw = std::min(std::max(tVfo->bandwidth, (double)CW_VFO_BANDWIDTH), (double)CW_SAMPLERATE);
                vfo->setBandwidth(bw);
                lastTargetBw = bw;
            }
        }
        else {
            // Clear markers from target VFO before restoring CW VFO
            auto [tName, tVfo] = findTargetVFO();
            if (tVfo) { tVfo->markers.clear(); }
            vfo->setVisible(true);
            vfo->setZOrder(0);
            vfo->setBandwidth(CW_VFO_BANDWIDTH);
            vfo->setBandwidthLimits(CW_VFO_BANDWIDTH, CW_VFO_BANDWIDTH, true);
            lastTargetBw = CW_VFO_BANDWIDTH;
        }
    }

    // Find the VFO to follow: the currently selected VFO on the waterfall.
    // In listen mode our VFO is hidden, so selectedVFO is always the radio's.
    std::pair<std::string, ImGui::WaterfallVFO*> findTargetVFO() {
        std::string sel = gui::waterfall.selectedVFO;
        if (sel.empty() || sel == name) { return {"", nullptr}; }
        auto it = gui::waterfall.vfos.find(sel);
        if (it == gui::waterfall.vfos.end()) { return {"", nullptr}; }
        return {sel, it->second};
    }

    // Called every UI frame: track the target VFO center position.
    void syncVFOs() {
        if (!listenMode || !enabled || !vfo) { return; }

        auto [targetName, targetWtf] = findTargetVFO();
        if (!targetWtf) { return; }

        double targetCenter = targetWtf->centerOffset;
        double targetBw = targetWtf->bandwidth;

        // Track center position
        if (fabs(targetCenter - lastTargetOffset) > 1.0) {
            vfo->setCenterOffset(targetCenter);
            lastTargetOffset = targetCenter;
        }

        // Track bandwidth — CW decoder VFO should cover at least the radio's BW
        // but not exceed our max sample rate capability
        double cwBw = std::min(std::max(targetBw, (double)CW_VFO_BANDWIDTH), (double)CW_SAMPLERATE);
        if (fabs(cwBw - lastTargetBw) > 1.0) {
            vfo->setBandwidth(cwBw);
            lastTargetBw = cwBw;
        }
    }

    static void iqHandler(dsp::complex_t* data, int count, void* ctx) {
        auto* _this = (CWDecoderModule*)ctx;
        if (!_this->enabled) { return; }
        _this->mgr.process(data, count);
    }

    static void menuHandler(void* ctx) {
        auto* _this = (CWDecoderModule*)ctx;

        _this->syncVFOs();
        _this->mgr.updateChannels();

        // Push channel markers to the visible VFO for waterfall display
        {
            auto markers = _this->mgr.getMarkers();
            if (_this->listenMode) {
                // CW VFO is hidden — put markers on the target (radio) VFO
                auto [tName, tVfo] = _this->findTargetVFO();
                if (tVfo) { tVfo->markers = markers; }
            }
            else if (_this->vfo) {
                _this->vfo->setMarkers(markers);
            }
        }

        bool configChanged = false;

        if (ImGui::Checkbox(("Listen Mode##cw_listen_" + _this->name).c_str(), &_this->listenMode)) {
            _this->applyListenMode(_this->listenMode);
            configChanged = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("CW decoder follows the radio VFO.\nRadio VFO stays on top for tuning.");
        }

        if (cw::drawMenu(_this->mgr, _this->name, _this->enabled)) {
            configChanged = true;
        }

        if (configChanged) {
            _this->mgr.saveConfig(_this->cfg);
            _this->cfg->set("listenMode", _this->listenMode);
        }
    }

    std::string name;
    ModuleConfig* cfg = nullptr;
    bool enabled = false;
    bool listenMode = false;
    double lastTargetOffset = 0;
    double lastTargetBw = CW_VFO_BANDWIDTH;
    VFOManager::VFO* vfo = nullptr;
    dsp::sink::Handler<dsp::complex_t> sink;
    cw::ChannelManager mgr;
};

MOD_EXPORT void _INIT_() {
    sdrppInitModuleConfig(config, "cw_decoder_config.json");
}

SDRPP_CREATE_INSTANCE_V2(CWDecoderModule)

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* inst) { delete (CWDecoderModule*)inst; }

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
