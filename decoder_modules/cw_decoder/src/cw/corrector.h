#pragma once
#include <string>
#include <algorithm>
#include <vector>
#include "vocabulary.h"
#include "conversation.h"

namespace cw {
namespace corrector {

    // Levenshtein edit distance between two strings
    inline int editDistance(const std::string& a, const std::string& b) {
        int m = a.size(), n = b.size();
        std::vector<int> prev(n + 1), curr(n + 1);
        for (int j = 0; j <= n; j++) prev[j] = j;
        for (int i = 1; i <= m; i++) {
            curr[0] = i;
            for (int j = 1; j <= n; j++) {
                int cost = (a[i-1] != b[j-1]) ? 1 : 0;
                curr[j] = std::min({prev[j] + 1, curr[j-1] + 1, prev[j-1] + cost});
            }
            std::swap(prev, curr);
        }
        return prev[n];
    }

    // Try to correct a word against the CW vocabulary.
    // Only corrects when:
    //   1. Confidence is below threshold (decode was uncertain)
    //   2. Edit distance is exactly 1 from a known word
    //   3. The correction is unambiguous (only one candidate)
    inline std::string correctWord(const std::string& word, float confidence,
                                    const ConversationTracker* conv = nullptr) {
        if (word.empty()) return word;

        // Already known — keep as-is
        if (vocabulary::isKnownWord(word)) return word;
        if (vocabulary::isCallsign(word)) return word;
        if (vocabulary::isRSTReport(word)) return word;

        // High confidence — trust the decode even if not in dictionary
        if (confidence > 0.8f) return word;

        // Short words (1-2 chars): only correct obvious decode artifacts.
        // Letter↔letter corrections at this length cause false positives (MY↔BK).
        if (word.size() <= 2) {
            // Try prosign→letter substitutions: each prosign could be
            // a misclassified letter (+ shares elements with A,R,C; * with S,V)
            static const char prosignSubs[][3] = {
                {'+', 'A', 'C'},  // AR prosign .-.-. → A(.-)  or C(-.-.)
                {'*', 'S', 'V'},  // SK prosign ...-.- → S(...) or V(..-)
            };
            for (auto& ps : prosignSubs) {
                for (int sub = 1; sub <= 2; sub++) {
                    std::string fixed = word;
                    for (auto& ch : fixed) { if (ch == ps[0]) ch = ps[sub]; }
                    if (fixed != word && vocabulary::isKnownWord(fixed)) return fixed;
                }
            }

            // Leading digit before letter is garble (9Q → CQ) — fall through to dictionary scan.
            // All other 2-letter words: return unchanged.
            bool hasLeadingDigitGarble = (word.size() == 2 && std::isdigit(word[0]) && std::isalpha(word[1]));
            if (!hasLeadingDigitGarble) return word;
        }

        // Try dictionary correction (edit distance 1).
        // When multiple candidates tie, prefer:
        //   1. Context-expected words (from conversation tracker)
        //   2. Longer words (more specific)
        std::string bestMatch;
        int bestDist = 2;
        int bestScore = 0;  // context-expected words get score boost

        for (const auto& dictWord : vocabulary::dictionary()) {
            int lenDiff = (int)word.size() - (int)dictWord.size();
            if (lenDiff > 1 || lenDiff < -1) continue;

            int dist = editDistance(word, dictWord);
            if (dist >= bestDist && dist > 1) continue;

            int score = (int)dictWord.size();
            if (conv && conv->isExpectedWord(dictWord)) score += 10;  // strong context boost

            if (dist < bestDist || (dist == bestDist && score > bestScore)) {
                bestDist = dist;
                bestMatch = dictWord;
                bestScore = score;
            }
        }

        if (bestDist == 1 && !bestMatch.empty()) {
            return bestMatch;
        }

        // Try callsign correction: only if the word already has a mix of
        // letters and digits (it ALMOST looks like a callsign with one error).
        bool hasDigit = false, hasLetter = false;
        for (char c : word) { if (std::isdigit(c)) hasDigit = true; if (std::isalpha(c)) hasLetter = true; }
        if (word.size() >= 4 && word.size() <= 7 && hasDigit && hasLetter) {
            for (int i = 0; i < (int)word.size(); i++) {
                std::string candidate = word;
                char c = word[i];

                // Try digit→letter and letter→digit substitutions
                static const char confusions[][2] = {
                    {'B', '6'}, {'6', 'B'},
                    {'D', '='}, {'S', '5'}, {'5', 'S'},
                    {'H', '5'}, {'5', 'H'},
                    {'0', 'O'}, {'O', '0'},
                    {'1', 'I'}, {'I', '1'},
                    {'9', 'N'}, {'N', '9'},
                    {'+', '5'}, {'*', 'S'},  // prosign→char
                };

                for (auto& cf : confusions) {
                    if (c == cf[0]) {
                        candidate[i] = cf[1];
                        if (vocabulary::isCallsign(candidate)) {
                            return candidate;
                        }
                        candidate[i] = c;  // revert
                    }
                }
            }
        }

        // Try RST correction: if 3 chars, try making it a valid RST
        if (word.size() == 3) {
            std::string candidate = word;
            // Replace non-digits with likely digit confusions
            for (int i = 0; i < 3; i++) {
                char c = candidate[i];
                if (!std::isdigit(c)) {
                    if (c == '+' || c == 'S') candidate[i] = '5';
                    else if (c == 'N' || c == '*') candidate[i] = '9';
                    else if (c == 'O') candidate[i] = '0';
                    else if (c == 'I' || c == 'T') candidate[i] = '1';
                }
            }
            if (vocabulary::isRSTReport(candidate) && editDistance(word, candidate) <= 1) {
                return candidate;
            }
        }

        return word;  // no correction found
    }

}}
