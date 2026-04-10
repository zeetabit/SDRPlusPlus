#pragma once
#include "../demod.h"
#include <dsp/demod/ssb.h>

namespace demod {
    class LSB : public Demodulator {
    public:
        LSB() {}

        LSB(std::string name, ModuleConfig* cfg, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, cfg, input, bandwidth, audioSR);
        }

        ~LSB() {
            stop();
        }

        void init(std::string name, ModuleConfig* cfg, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _cfg = cfg;

            // Load config
            _cfg->read([&](const json& conf) {
                if (conf.contains(getName()) && conf[getName()].contains("agcAttack")) { agcAttack = conf[getName()]["agcAttack"]; }
                if (conf.contains(getName()) && conf[getName()].contains("agcDecay")) { agcDecay = conf[getName()]["agcDecay"]; }
            });

            // Define structure
            demod.init(input, dsp::demod::SSB<dsp::stereo_t>::Mode::LSB, bandwidth, getIFSampleRate(), agcAttack / getIFSampleRate(), agcDecay / getIFSampleRate());
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            float menuWidth = ImGui::GetContentRegionAvail().x;
            ImGui::LeftLabel("AGC Attack");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_lsb_agc_attack_" + name).c_str(), &agcAttack, 1.0f, 200.0f)) {
                demod.setAGCAttack(agcAttack / getIFSampleRate());
                _cfg->with([&](json& conf) { conf[getName()]["agcAttack"] = agcAttack; });
            }
            ImGui::LeftLabel("AGC Decay");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_lsb_agc_decay_" + name).c_str(), &agcDecay, 1.0f, 20.0f)) {
                demod.setAGCDecay(agcDecay / getIFSampleRate());
                _cfg->with([&](json& conf) { conf[getName()]["agcDecay"] = agcDecay; });
            }
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "LSB"; }
        double getIFSampleRate() { return 24000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 2800.0; }
        double getMinBandwidth() { return 500.0; }
        double getMaxBandwidth() { return getIFSampleRate() / 2.0; }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 100.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_UPPER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return true; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        dsp::demod::SSB<dsp::stereo_t> demod;

        ModuleConfig* _cfg = NULL;

        float agcAttack = 50.0f;
        float agcDecay = 5.0f;

        std::string name;
    };
}