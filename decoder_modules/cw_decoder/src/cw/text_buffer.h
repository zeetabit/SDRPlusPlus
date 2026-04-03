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
            return text;
        }

        // Get a snapshot of entries for UI rendering (per-char confidence + corrected flag)
        std::vector<CharEntry> getEntries() {
            std::lock_guard<std::mutex> lck(mtx);
            return entries;
        }

        void clear() {
            std::lock_guard<std::mutex> lck(mtx);
            text.clear();
            entries.clear();
        }

    private:
        std::mutex mtx;
        std::string text;
        std::vector<CharEntry> entries;
    };

}
