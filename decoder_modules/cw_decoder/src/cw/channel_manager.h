#pragma once
#include "channel.h"
#include "tone_scanner.h"
#include <config.h>
#include <gui/widgets/waterfall.h>
#include <utils/flog.h>
#include <vector>
#include <memory>
#include <string>
#include <mutex>

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
        void init(float sampleRate) {
            scanner.init(sampleRate, 1024);
        }

        ~ChannelManager() {}

        void addChannel(float toneFreq, bool pinned) {
            std::lock_guard<std::mutex> lck(mtx);
            if ((int)entries.size() >= maxChannels) { return; }
            auto ch = std::make_unique<Channel>();
            ch->init(nextId++, toneFreq);
            ch->debugLog = true;  // TEMP: enable decode logging
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
            for (auto& e : entries) {
                if (e.pinned) { continue; }
                if (e.channel->snr > 6.0f) { e.idleFrames = 0; }
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
                    ch->debugLog = true;  // TEMP: enable decode logging
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

        void rebuildPinnedList() {
            pinnedTones.clear();
            for (auto& e : entries) {
                if (e.pinned) { pinnedTones.push_back(e.channel->toneFreq); }
            }
        }

        void loadConfig(ConfigManager& config, const std::string& name) {
            config.readConfig([&](const json& conf) {
                if (!conf.contains(name)) { return; }
                auto& c = conf[name];
                maxChannels = c.value("maxChannels", CW_MAX_CHANNELS_DEFAULT);
                if (maxChannels < 1) { maxChannels = 1; }
                if (maxChannels > CW_MAX_CHANNELS_HARD) { maxChannels = CW_MAX_CHANNELS_HARD; }
                autoDetect = c.value("autoDetect", true);
                scanThreshold = c.value("scanThreshold", 10.0f);
                if (c.contains("pinnedChannels") && c["pinnedChannels"].is_array()) {
                    pinnedTones.clear();
                    for (auto& ch : c["pinnedChannels"]) {
                        pinnedTones.push_back(ch.value("tone", 700.0f));
                    }
                }
            });
        }

        void saveConfig(ConfigManager& config, const std::string& name) {
            rebuildPinnedList();
            config.withConfig([&](json& conf) {
                conf[name]["maxChannels"] = maxChannels;
                conf[name]["autoDetect"] = autoDetect;
                conf[name]["scanThreshold"] = scanThreshold;
                json pinned = json::array();
                for (auto& t : pinnedTones) { pinned.push_back({{"tone", t}}); }
                conf[name]["pinnedChannels"] = pinned;
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
        std::vector<ChannelEntry> entries;     // Modified ONLY under mtx
        std::vector<StableTone> stableTones;   // Modified ONLY under mtx
        std::vector<float> pinnedTones;
        std::vector<DetectedTone> lastDetected;

    private:
        ToneScanner scanner;
        int nextId = 0;

        std::mutex mtx;         // protects entries vector
        std::mutex detectedMtx; // protects lastDetected + scanReady
        bool scanReady = false;
    };

}
