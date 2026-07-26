#pragma once
#include <string>
#include <mutex>
#include <vector>

namespace cw {

    struct CharEntry {
        char character;
        float confidence;
        bool corrected = false;  // true if post-decode corrector changed this char
    };

    class TextBuffer {
    public:
        void append(char c, float confidence = 1.0f) {
            std::lock_guard<std::mutex> lck(mtx);
            entries.push_back({c, confidence, false});
            text += c;
        }

        void appendSpace() { append(' '); }

        // Replace the last N characters with a corrected string.
        // Replacement chars are flagged as corrected for UI highlighting.
        void replaceLastN(int n, const std::string& replacement, float confidence = 0.5f) {
            std::lock_guard<std::mutex> lck(mtx);
            if (n > (int)text.size()) n = text.size();
            text.erase(text.size() - n);
            while (n > 0 && !entries.empty()) { entries.pop_back(); n--; }
            for (char c : replacement) {
                entries.push_back({c, confidence, true});
                text += c;
            }
        }

        std::string getText() {
            std::lock_guard<std::mutex> lck(mtx);
            return frozenText.empty() ? text : (frozenText + text);
        }

        // Get a snapshot of entries for UI rendering (per-char confidence + corrected flag).
        // Frozen (retained from before a frequency switch) precede the live decode.
        std::vector<CharEntry> getEntries() {
            std::lock_guard<std::mutex> lck(mtx);
            if (frozen.empty()) { return entries; }
            std::vector<CharEntry> all = frozen;
            all.insert(all.end(), entries.begin(), entries.end());
            return all;
        }

        // Number of leading entries that are frozen history (dimmed in the UI).
        size_t frozenCount() {
            std::lock_guard<std::mutex> lck(mtx);
            return frozen.size();
        }

        // Clear the LIVE decode only — frozen history is retained. Used by the cont
        // core's clearEmitted() (it rewrites the live window each re-decode cycle).
        void clear() {
            std::lock_guard<std::mutex> lck(mtx);
            text.clear();
            entries.clear();
        }

        // Clear everything, frozen history included (the UI "Clear"/"Reset" buttons).
        void clearAll() {
            std::lock_guard<std::mutex> lck(mtx);
            text.clear();
            entries.clear();
            frozenText.clear();
            frozen.clear();
        }

        // Move the live decode into frozen history (retained across a frequency
        // switch) so a decode-state reset keeps the already-copied text visible.
        // A single separator space marks the boundary between sessions.
        void freeze() {
            std::lock_guard<std::mutex> lck(mtx);
            if (entries.empty()) { return; }
            if (!frozen.empty()) { frozen.push_back({' ', 0.0f, false}); frozenText += ' '; }
            frozen.insert(frozen.end(), entries.begin(), entries.end());
            frozenText += text;
            text.clear();
            entries.clear();
        }

    private:
        std::mutex mtx;
        std::string text;
        std::vector<CharEntry> entries;
        std::string frozenText;              // retained decode from before freq switches
        std::vector<CharEntry> frozen;
    };

}
