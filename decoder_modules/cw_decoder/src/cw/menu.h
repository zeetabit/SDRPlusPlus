#pragma once
#include <imgui.h>
#include <gui/style.h>
#include <gui/widgets/snr_meter.h>
#include <string>
#include <algorithm>
#include "channel_manager.h"

#define CONCAT(a, b) ((std::string(a) + b).c_str())

namespace cw {

    // Channel slot view — always rendered at full height. Inactive channels are dimmed.
    // Returns true if the entries list was modified (caller must break the loop).
    static inline bool drawChannelSlot(ChannelEntry& entry, float,
                                       ChannelManager& mgr, int ci, bool& configChanged) {
        auto& ch = entry.channel;
        bool active = ch->snr > 3.0f || ch->wpm > 0 || !ch->text.getText().empty();
        ImGui::PushID(ch->id);

        if (!active) { style::beginDisabled(); }

        // Tone slider + pin/remove
        float menuWidth = ImGui::GetContentRegionAvail().x;
        float btnWidth = 95.0f * style::uiScale;
        ImGui::SetNextItemWidth(menuWidth - btnWidth);
        float tone = ch->toneFreq;
        if (ImGui::SliderFloat("##tone", &tone, -1500.0f, 1500.0f, "%.0f Hz")) {
            ch->setToneFreq(tone);
        }

        if (!active) { style::endDisabled(); }

        ImGui::SameLine();
        if (ImGui::SmallButton(entry.pinned ? "Unpin" : "Pin")) {
            entry.pinned = !entry.pinned;
            configChanged = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("X")) {
            ImGui::PopID();
            mgr.removeChannel(ci);
            configChanged = true;
            return true;
        }

        if (!active) { style::beginDisabled(); }

        // WPM + SNR + conversation state
        if (ch->wpm > 0) { ImGui::Text("WPM: %.0f", ch->wpm); }
        else { ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "WPM: --"); }
        ImGui::SameLine();
        ImGui::Text("SNR: %.1f dB", ch->snr);
        {
            auto convState = ch->conversation.getState();
            ImVec4 stateColor;
            const char* stateLabel;
            switch (convState) {
                case ConversationTracker::CQ_CALL:
                    stateLabel = "[CQ]"; stateColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f); break;
                case ConversationTracker::EXCHANGE:
                    stateLabel = "[QSO]"; stateColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); break;
                case ConversationTracker::RST_EXCHANGE:
                    stateLabel = "[RST]"; stateColor = ImVec4(1.0f, 0.8f, 0.3f, 1.0f); break;
                case ConversationTracker::CLOSING:
                    stateLabel = "[73]"; stateColor = ImVec4(1.0f, 0.5f, 0.5f, 1.0f); break;
                default:
                    stateLabel = nullptr; stateColor = {}; break;
            }
            if (stateLabel) {
                ImGui::SameLine();
                ImGui::TextColored(stateColor, "%s", stateLabel);
            }
        }

        // SNR meter
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::SNRMeter(ch->snr);

        // Decoded text — per-char confidence coloring + corrected highlight.
        // High confidence (>0.8): full white. Medium (0.4-0.8): dimmed.
        // Low (<0.4): gray. Corrected chars: light blue tint.
        auto entries = ch->text.getEntries();
        float textHeight = 50.0f * style::uiScale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
        ImGui::BeginChild("##decoded", ImVec2(ImGui::GetContentRegionAvail().x, textHeight), true);
        if (entries.empty()) {
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 0.6f), "(listening...)");
        }
        else {
            float wrapWidth = ImGui::GetContentRegionAvail().x;
            float x = ImGui::GetCursorPosX();
            for (auto& e : entries) {
                // Compute color from confidence + corrected flag
                float alpha;
                if (e.confidence > 0.8f) alpha = 1.0f;
                else if (e.confidence > 0.4f) alpha = 0.6f;
                else alpha = 0.35f;

                ImVec4 color;
                if (e.corrected) {
                    // Light blue tint for corrected characters
                    color = ImVec4(0.5f, 0.8f, 1.0f, alpha);
                } else {
                    color = ImVec4(1.0f, 1.0f, 1.0f, alpha);
                }

                // Word wrap: track x position, only SameLine when staying on same row
                char buf[2] = {e.character, 0};
                float charWidth = ImGui::CalcTextSize(buf).x;
                bool wrap = (x + charWidth > wrapWidth && e.character != ' ');

                if (!wrap && x > ImGui::GetCursorPosX() + 0.1f) {
                    ImGui::SameLine(0, 0);
                }
                if (wrap) {
                    x = ImGui::GetCursorPosX();
                }

                ImGui::TextColored(color, "%c", e.character);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("'%c' conf=%.0f%%%s",
                        e.character, e.confidence * 100.0f,
                        e.corrected ? " (corrected)" : "");
                }
                x += charWidth;
            }
        }
        if (ImGui::GetScrollY() < ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Action buttons
        if (ImGui::SmallButton("Clear")) { ch->text.clear(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) { ImGui::SetClipboardText(ch->text.getText().c_str()); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) { ch->reset(); }

        if (!active) { style::endDisabled(); }

        ImGui::PopID();
        return false;
    }

    // Draw an empty slot placeholder at the same height as an active channel.
    static inline void drawEmptySlot(int slot, float, const std::string& name) {
        ImGui::PushID(slot + 2000);

        // Tone slider placeholder (disabled)
        style::beginDisabled();
        float menuWidth = ImGui::GetContentRegionAvail().x;
        float btnWidth = 95.0f * style::uiScale;
        ImGui::SetNextItemWidth(menuWidth - btnWidth);
        float dummy = 0;
        ImGui::SliderFloat("##tone", &dummy, -1500.0f, 1500.0f, "-- Hz");
        ImGui::SameLine();
        ImGui::SmallButton("Pin");
        ImGui::SameLine();
        ImGui::SmallButton("X");

        // WPM + SNR
        ImGui::Text("WPM: --");
        ImGui::SameLine();
        ImGui::Text("SNR: -- dB");

        // SNR meter
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        float zeroSnr = 0;
        ImGui::SNRMeter(zeroSnr);

        // Text area (matches active channel height)
        float textHeight = 50.0f * style::uiScale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.05f, 1.0f));
        ImGui::BeginChild("##decoded", ImVec2(ImGui::GetContentRegionAvail().x, textHeight), true);
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Buttons
        ImGui::SmallButton("Clear");
        ImGui::SameLine();
        ImGui::SmallButton("Copy");
        ImGui::SameLine();
        ImGui::SmallButton("Reset");
        style::endDisabled();

        ImGui::PopID();
    }

    inline bool drawMenu(ChannelManager& mgr, const std::string& name, bool enabled) {
        bool configChanged = false;

        if (!enabled) { style::beginDisabled(); }

        float menuWidth = ImGui::GetContentRegionAvail().x;

        // ── Channels slider ──
        char chLabel[32];
        snprintf(chLabel, sizeof(chLabel), "Channels %d", mgr.maxChannels);
        ImGui::SetNextItemWidth(menuWidth);
        if (ImGui::SliderInt(CONCAT("##cw_maxch_", name), &mgr.maxChannels, 1, CW_MAX_CHANNELS_DEFAULT, chLabel)) {
            configChanged = true;
        }

        // ── Auto detect + threshold ──
        if (ImGui::Checkbox(CONCAT("Auto Detect##cw_auto_", name), &mgr.autoDetect)) {
            configChanged = true;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::SliderFloat(CONCAT("##cw_thresh_", name), &mgr.scanThreshold, 3.0f, 30.0f, "%.0f dB")) {
            configChanged = true;
        }

        // ── Fixed-slot channel list — stable height, no collapsing ──
        for (int slot = 0; slot < mgr.maxChannels; slot++) {
            ImGui::Separator();
            if (slot < (int)mgr.entries.size()) {
                if (drawChannelSlot(mgr.entries[slot], menuWidth, mgr, slot, configChanged)) {
                    break;
                }
            }
            else {
                drawEmptySlot(slot, menuWidth, name);
            }
        }

        // ── Global controls ──
        if ((int)mgr.entries.size() > 1) {
            ImGui::Separator();
            if (ImGui::SmallButton(CONCAT("Clear All##cw_clr_", name))) {
                for (auto& e : mgr.entries) { e.channel->text.clear(); }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(CONCAT("Copy All##cw_cpy_", name))) {
                std::string all;
                for (auto& e : mgr.entries) {
                    std::string t = e.channel->text.getText();
                    if (!t.empty()) {
                        char hdr[64];
                        snprintf(hdr, sizeof(hdr), "--- Ch %d (%.0f Hz) ---\n", e.channel->id, e.channel->toneFreq);
                        all += hdr;
                        all += t + "\n\n";
                    }
                }
                if (!all.empty()) { ImGui::SetClipboardText(all.c_str()); }
            }
        }

        if (!enabled) { style::endDisabled(); }

        return configChanged;
    }

}
