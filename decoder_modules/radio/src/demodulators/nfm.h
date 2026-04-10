#pragma once
#include "../demod.h"
#include <dsp/demod/fm.h>

namespace demod {
    class NFM : public Demodulator {
    public:
        NFM() {}

        NFM(std::string name, ModuleConfig* cfg, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, cfg, input, bandwidth, audioSR);
        }

        ~NFM() { stop(); }

        void init(std::string name, ModuleConfig* cfg, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            this->_cfg = cfg;

            // Load config
            _cfg->read([&](const json& conf) {
                if (conf.contains(getName()) && conf[getName()].contains("lowPass")) { _lowPass = conf[getName()]["lowPass"]; }
            });


            // Define structure
            demod.init(input, getIFSampleRate(), bandwidth, _lowPass);
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            if (ImGui::Checkbox(("Low Pass##_radio_wfm_lowpass_" + name).c_str(), &_lowPass)) {
                demod.setLowPass(_lowPass);
                _cfg->with([&](json& conf) { conf[getName()]["lowPass"] = _lowPass; });
            }
        }

        void setBandwidth(double bandwidth) {
            demod.setBandwidth(bandwidth);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "FM"; }
        double getIFSampleRate() { return 50000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 12500.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 2500.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return true; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return true; }
        bool getNBAllowed() { return false; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        dsp::demod::FM<dsp::stereo_t> demod;

        ModuleConfig* _cfg = NULL;

        bool _lowPass = true;

        std::string name;
    };
}