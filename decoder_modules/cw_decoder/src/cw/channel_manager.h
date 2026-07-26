#pragma once
#include "channel.h"
#include "tone_scanner.h"
#include <config.h>
#include <module_config.h>
#include <gui/widgets/waterfall.h>
#include <utils/flog.h>
#include <vector>
#include <memory>
#include <string>
#include <mutex>
#include <atomic>

#define CW_MAX_CHANNELS_HARD  20
#define CW_MAX_CHANNELS_DEFAULT 10
#define CW_TONE_MATCH_HZ  120.0f  // Must exceed BPF effective bandwidth (200 Hz) / 2
#define CW_IDLE_TIMEOUT        80   // FFT frames (~10 seconds) for channels with no decode
#define CW_IDLE_TIMEOUT_ACTIVE 240  // ~30 seconds for channels that have decoded text
#define CW_TONE_CONFIRM_FRAMES  3   // frames before a new tone becomes confirmed
#define CW_TONE_DECAY_FRAMES   20   // frames before an unconfirmed tone is dropped

namespace cw {

    struct ChannelEntry {
        std::unique_ptr<Channel> channel;
        bool pinned;
        int idleFrames;
    };

    struct StableTone {
        float frequency;
        float power;        // last seen power
        int seenFrames;     // consecutive frames seen
        int missingFrames;  // consecutive frames NOT seen
        bool confirmed;     // promoted to channel
    };

    // Thread-safe channel manager.
    //
    // Two threads access this:
    //   DSP thread  → process() feeds IQ to scanner + existing channels
    //   UI thread   → updateChannels() adds/removes channels, drawMenu reads state
    //
    // The entries vector is ONLY modified by the UI thread (via updateChannels).
    // The DSP thread reads entries under a shared lock for per-channel processing.
    // A mutex protects entries during structural changes.
    class ChannelManager {
    public:
        // Propagated to every Channel on creation. Toggle via the "debugLog"
        // config key (read once at module init in main.cpp).
        bool debugLog = false;
        // Dictionary word-correction, propagated to every Channel. OFF by default
        // so the raw decode stays visible (correction masks detector/timing errors).
        bool wordCorrection = false;

        void init(float sampleRate) {
            scanner.init(sampleRate, 1024);
        }

        ~ChannelManager() {}

        void addChannel(float toneFreq, bool pinned) {
            std::lock_guard<std::mutex> lck(mtx);
            if ((int)entries.size() >= maxChannels) { return; }
            auto ch = std::make_unique<Channel>();
            ch->init(nextId++, toneFreq);
            ch->debugLog = debugLog;
            ch->wordCorrection = wordCorrection;
            entries.push_back({std::move(ch), pinned, 0});
        }

        void removeChannel(int index) {
            std::lock_guard<std::mutex> lck(mtx);
            if (index >= 0 && index < (int)entries.size()) {
                entries.erase(entries.begin() + index);
            }
        }

        int findChannelNear(float freq, float tolerance) {
            for (int i = 0; i < (int)entries.size(); i++) {
                float dist = fabsf(entries[i].channel->toneFreq - freq);
                if (dist < tolerance) { return i; }
            }
            return -1;
        }

        // Called from DSP thread: feed IQ to scanner + existing channels.
        // Does NOT modify the entries vector.
        void process(dsp::complex_t* data, int count) {
            // Frequency switch (UI thread) requests a decode-state reset; honor it here
            // on the DSP thread so core->reset() never races process(). Text is frozen
            // (kept), timing/detector re-acquire on the new signal.
            if (resetDecodeReq.exchange(false)) {
                std::lock_guard<std::mutex> lck(mtx);
                for (auto& e : entries) { e.channel->resetDecodeState(); }
            }
            bool scanned = scanner.feed(data, count);
            if (scanned && autoDetect) {
                std::lock_guard<std::mutex> lck(detectedMtx);
                lastDetected = scanner.getDetectedTones(scanThreshold, maxChannels);
                scanReady = true;
            }

            // Process existing channels under lock (short hold — just snapshot pointers)
            std::vector<Channel*> channels;
            {
                std::lock_guard<std::mutex> lck(mtx);
                channels.reserve(entries.size());
                for (auto& e : entries) { channels.push_back(e.channel.get()); }
            }
            for (auto* ch : channels) {
                ch->process(count, data);
            }
        }

        // Called from UI thread: add/remove channels based on scan results.
        // This is the ONLY place entries are structurally modified at runtime.
        //
        // Tone lifecycle: raw scanner detections → stableTones (with hysteresis) → channels.
        // Existing tones get priority — new detections only replace decayed slots.
        // This prevents channel churn during pileups.
        void updateChannels() {
            if (!autoDetect) { return; }

            // Grab latest scan results
            std::vector<DetectedTone> detected;
            {
                std::lock_guard<std::mutex> lck(detectedMtx);
                if (!scanReady) { return; }
                detected = lastDetected;
                scanReady = false;
            }

            std::lock_guard<std::mutex> lck(mtx);

            // ── Step 1: Update stable tones from raw detections ──
            // Mark all stable tones as "not seen this frame"
            for (auto& st : stableTones) { st.missingFrames++; }

            // Match raw detections to existing stable tones
            for (auto& tone : detected) {
                if (tone.power < scanThreshold) { continue; }
                int bestIdx = -1;
                float bestDist = CW_TONE_MATCH_HZ;
                for (int i = 0; i < (int)stableTones.size(); i++) {
                    float dist = fabsf(stableTones[i].frequency - tone.frequency);
                    if (dist < bestDist) { bestDist = dist; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    // Update existing stable tone
                    auto& st = stableTones[bestIdx];
                    st.frequency = 0.8f * st.frequency + 0.2f * tone.frequency; // smooth drift
                    st.power = tone.power;
                    st.seenFrames++;
                    st.missingFrames = 0;
                    if (!st.confirmed && st.seenFrames >= CW_TONE_CONFIRM_FRAMES) {
                        st.confirmed = true;
                    }
                }
                else if ((int)stableTones.size() < maxChannels) {
                    // New tone — add as unconfirmed
                    stableTones.push_back({tone.frequency, tone.power, 1, 0, false});
                }
            }

            // Dedup stable tones: loop until no more merges
            bool merged = true;
            while (merged) {
                merged = false;
                for (int i = (int)stableTones.size() - 1; i >= 1; i--) {
                    for (int j = 0; j < i; j++) {
                        if (fabsf(stableTones[i].frequency - stableTones[j].frequency) < CW_TONE_MATCH_HZ) {
                            int remove = (stableTones[i].seenFrames < stableTones[j].seenFrames) ? i : j;
                            stableTones.erase(stableTones.begin() + remove);
                            merged = true;
                            break;
                        }
                    }
                    if (merged) { break; }
                }
            }

            // Decay: remove unconfirmed tones that disappeared quickly
            for (int i = (int)stableTones.size() - 1; i >= 0; i--) {
                auto& st = stableTones[i];
                if (!st.confirmed && st.missingFrames > CW_TONE_DECAY_FRAMES) {
                    stableTones.erase(stableTones.begin() + i);
                }
            }

            // ── Step 2: Update idle counters on existing channels ──
            // §52.3 #41: the keying-contrast SNR idles out real CW under moderate
            // noise (n1.0-1.5 report snr 4-5 < 6) — a recall bug, not precision.
            // The model-fit gate rescues those: keep alive if EITHER the SNR is high
            // OR the envelope has clear keyed two-level structure (modelFit > keep).
            // Measured [modelfit-gate-bench]: recall 1/4->3/4 at 0 junk cost; the
            // OR form can never drop what snr>6 already keeps. Amplitude-only fit
            // cannot separate the heaviest-noise CW from a warble (the #42 boundary).
            for (auto& e : entries) {
                if (e.pinned) { continue; }
                const bool active = e.channel->snr > 6.0f ||
                                    (modelFitGate && e.channel->modelFit > modelFitKeep);
                if (active) { e.idleFrames = 0; }
                else { e.idleFrames++; }
            }

            // ── Step 3: Create channels for confirmed stable tones ──
            for (auto& st : stableTones) {
                if (!st.confirmed) { continue; }
                int idx = findChannelNear(st.frequency, CW_TONE_MATCH_HZ);
                if (idx >= 0) {
                    entries[idx].channel->setToneFreq(st.frequency);
                }
                else if ((int)entries.size() < maxChannels) {
                    auto ch = std::make_unique<Channel>();
                    ch->init(nextId++, st.frequency);
                    ch->debugLog = debugLog;
                    ch->wordCorrection = wordCorrection;
                    entries.push_back({std::move(ch), false, 0});
                    flog::info("CW confirmed tone at {0:.0f} Hz ({1:.1f} dB)", st.frequency, st.power);
                }
            }

            // ── Step 4: Dedup — merge channels that are too close ──
            // Loop until no more merges — handles clusters of 3+ channels.
            merged = true;
            while (merged) {
                merged = false;
                for (int i = (int)entries.size() - 1; i >= 1; i--) {
                    if (entries[i].pinned) { continue; }
                    for (int j = 0; j < i; j++) {
                        float dist = fabsf(entries[i].channel->toneFreq - entries[j].channel->toneFreq);
                        if (dist < CW_TONE_MATCH_HZ) {
                            int remove = (entries[i].channel->snr < entries[j].channel->snr) ? i : j;
                            if (entries[remove].pinned) { continue; }
                            entries.erase(entries.begin() + remove);
                            merged = true;
                            break;
                        }
                    }
                    if (merged) { break; }
                }
            }

            // ── Step 5: Remove idle auto channels + their stable tone ──
            for (int i = (int)entries.size() - 1; i >= 0; i--) {
                bool hasDecoded = !entries[i].channel->text.getText().empty();
                int timeout = hasDecoded ? CW_IDLE_TIMEOUT_ACTIVE : CW_IDLE_TIMEOUT;
                if (!entries[i].pinned && entries[i].idleFrames > timeout) {
                    float freq = entries[i].channel->toneFreq;
                    flog::info("CW removing idle channel at {0:.0f} Hz", freq);
                    entries.erase(entries.begin() + i);
                    // Also remove the stable tone so the slot opens for new detections
                    for (int j = (int)stableTones.size() - 1; j >= 0; j--) {
                        if (fabsf(stableTones[j].frequency - freq) < CW_TONE_MATCH_HZ) {
                            stableTones.erase(stableTones.begin() + j);
                            break;
                        }
                    }
                }
            }
        }

        // Called from UI thread
        void clearEntries() {
            std::lock_guard<std::mutex> lck(mtx);
            entries.clear();
            stableTones.clear();
        }

        // UI thread: the operator retuned — request a decode-state reset (honored on
        // the DSP thread in process()). Decoded text is kept (frozen); timing/detector
        // re-acquire on the new signal instead of dragging the old estimates.
        void requestResetDecode() { resetDecodeReq = true; }

        void rebuildPinnedList() {
            pinnedTones.clear();
            for (auto& e : entries) {
                if (e.pinned) { pinnedTones.push_back(e.channel->toneFreq); }
            }
        }

        void loadConfig(ModuleConfig* cfg) {
            cfg->read([&](const json& c) {
                maxChannels = c.value("maxChannels", CW_MAX_CHANNELS_DEFAULT);
                if (maxChannels < 1) { maxChannels = 1; }
                if (maxChannels > CW_MAX_CHANNELS_HARD) { maxChannels = CW_MAX_CHANNELS_HARD; }
                autoDetect = c.value("autoDetect", true);
                scanThreshold = c.value("scanThreshold", 10.0f);
                modelFitGate = c.value("modelFitGate", true);          // §52.3 #41
                modelFitKeep = c.value("modelFitKeep", 0.25f);
                if (c.contains("pinnedChannels") && c["pinnedChannels"].is_array()) {
                    pinnedTones.clear();
                    for (auto& ch : c["pinnedChannels"]) {
                        pinnedTones.push_back(ch.value("tone", 700.0f));
                    }
                }
            });
        }

        void saveConfig(ModuleConfig* cfg) {
            rebuildPinnedList();
            cfg->with([&](json& conf) {
                conf["maxChannels"] = maxChannels;
                conf["autoDetect"] = autoDetect;
                conf["scanThreshold"] = scanThreshold;
                conf["modelFitGate"] = modelFitGate;       // §52.3 #41
                conf["modelFitKeep"] = modelFitKeep;
                json pinned = json::array();
                for (auto& t : pinnedTones) { pinned.push_back({{"tone", t}}); }
                conf["pinnedChannels"] = pinned;
            });
        }

        // Build VFO markers from current channel state (call from UI thread)
        std::vector<ImGui::WaterfallVFO::Marker> getMarkers() {
            std::lock_guard<std::mutex> lck(mtx);
            std::vector<ImGui::WaterfallVFO::Marker> markers;
            for (auto& e : entries) {
                ImGui::WaterfallVFO::Marker m;
                m.offset = e.channel->toneFreq;

                if (e.channel->snr > 6.0f && e.channel->wpm > 0) {
                    m.color = IM_COL32(0, 255, 100, 200);
                }
                else if (e.channel->snr > 3.0f) {
                    m.color = IM_COL32(0, 180, 255, 160);
                }
                else {
                    m.color = IM_COL32(128, 128, 128, 100);
                }

                if (e.channel->wpm > 0) {
                    snprintf(m.label, sizeof(m.label), "%d %.0fw", e.channel->id, e.channel->wpm);
                }
                else {
                    snprintf(m.label, sizeof(m.label), "%d", e.channel->id);
                }
                markers.push_back(m);
            }
            return markers;
        }

        void createPinnedChannels() {
            for (int i = 0; i < (int)pinnedTones.size() && i < maxChannels; i++) {
                addChannel(pinnedTones[i], true);
            }
        }

        // Public state (UI thread reads, DSP thread writes atomically-safe fields)
        int maxChannels = CW_MAX_CHANNELS_DEFAULT;
        bool autoDetect = true;
        float scanThreshold = 10.0f;
        bool modelFitGate = true;      // §52.3 #41: keep marginal real CW alive on model fit
        float modelFitKeep = 0.25f;    // fit threshold for the OR keep-alive (measured)
        std::vector<ChannelEntry> entries;     // Modified ONLY under mtx
        std::vector<StableTone> stableTones;   // Modified ONLY under mtx
        std::vector<float> pinnedTones;
        std::vector<DetectedTone> lastDetected;

    private:
        ToneScanner scanner;
        int nextId = 0;

        std::atomic<bool> resetDecodeReq{false};  // UI->DSP freq-switch reset request
        std::mutex mtx;         // protects entries vector
        std::mutex detectedMtx; // protects lastDetected + scanReady
        bool scanReady = false;
    };

}
