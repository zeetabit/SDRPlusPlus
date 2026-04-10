#pragma once
#include "../demod.h"
#include <dsp/demod/am.h>

namespace demod {
    class AM : public Demodulator {
    public:
        AM() {}

        AM(std::string name, ModuleConfig* cfg, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, cfg, input, bandwidth, audioSR);
        }

        ~AM() { stop(); }

        void init(std::string name, ModuleConfig* cfg, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _cfg = cfg;

            // Load config
            _cfg->read([&](const json& conf) {
                if (conf.contains(getName()) && conf[getName()].contains("agcAttack")) { agcAttack = conf[getName()]["agcAttack"]; }
                if (conf.contains(getName()) && conf[getName()].contains("agcDecay")) { agcDecay = conf[getName()]["agcDecay"]; }
                if (conf.contains(getName()) && conf[getName()].contains("carrierAgc")) { carrierAgc = conf[getName()]["carrierAgc"]; }
            });

            // Define structure
            demod.init(input, carrierAgc ? dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::AM<dsp::stereo_t>::AGCMode::AUDIO, bandwidth, agcAttack / getIFSampleRate(), agcDecay / getIFSampleRate(), 100.0 / getIFSampleRate(), getIFSampleRate());
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            float menuWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Checkbox(("Carrier AGC##_radio_am_carrier_agc_" + name).c_str(), &carrierAgc)) {
                demod.setAGCMode(carrierAgc ? dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::AM<dsp::stereo_t>::AGCMode::AUDIO);
                _cfg->with([&](json& conf) { conf[getName()]["carrierAgc"] = carrierAgc; });
            }
            ImGui::LeftLabel("AGC Attack");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_am_agc_attack_" + name).c_str(), &agcAttack, 1.0f, 200.0f)) {
                demod.setAGCAttack(agcAttack / getIFSampleRate());
                _cfg->with([&](json& conf) { conf[getName()]["agcAttack"] = agcAttack; });
            }
            ImGui::LeftLabel("AGC Decay");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_am_agc_decay_" + name).c_str(), &agcDecay, 1.0f, 20.0f)) {
                demod.setAGCDecay(agcDecay / getIFSampleRate());
                _cfg->with([&](json& conf) { conf[getName()]["agcDecay"] = agcDecay; });
            }
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "AM"; }
        double getIFSampleRate() { return 15000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 10000.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate(); }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 1000.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return false; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        dsp::demod::AM<dsp::stereo_t> demod;

        ModuleConfig* _cfg = NULL;

        float agcAttack = 50.0f;
        float agcDecay = 5.0f;
        bool carrierAgc = false;

        std::string name;
    };
}