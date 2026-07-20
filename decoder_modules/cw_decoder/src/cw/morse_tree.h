#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>
#include "timing.h"

namespace cw {

    struct MorseNode {
        char character;   // '\0' if no character at this node
    };

    // Static Morse binary tree. Index 0 = root.
    // Left child (dit) = 2*i+1, Right child (dah) = 2*i+2
    // Depth 6 → 63 nodes covers A-Z, 0-9, punctuation.
    //
    // Multi-path beam search decoder:
    //   - Maintains N best paths through the tree simultaneously
    //   - Each element forks every path into dit/dah branches weighted by confidence
    //   - Character break selects the highest-probability valid character
    //   - Handles ambiguous dit/dah classification from timing jitter
    class MorseDecoder {
    public:
        void init() {
            memset(tree, 0, sizeof(tree));
            tree[1].character = 'E';   // .
            tree[2].character = 'T';   // -
            tree[3].character = 'I';   // ..
            tree[4].character = 'A';   // .-
            tree[5].character = 'N';   // -.
            tree[6].character = 'M';   // --
            tree[7].character = 'S';   // ...
            tree[8].character = 'U';   // ..-
            tree[9].character = 'R';   // .-.
            tree[10].character = 'W';  // .--
            tree[11].character = 'D';  // -..
            tree[12].character = 'K';  // -.-
            tree[13].character = 'G';  // --.
            tree[14].character = 'O';  // ---
            tree[15].character = 'H';  // ....
            tree[16].character = 'V';  // ...-
            tree[17].character = 'F';  // ..-.
            tree[19].character = 'L';  // .-..
            tree[21].character = 'P';  // .--.
            tree[22].character = 'J';  // .---
            tree[23].character = 'B';  // -...
            tree[24].character = 'X';  // -..-
            tree[25].character = 'C';  // -.-.
            tree[26].character = 'Y';  // -.--
            tree[27].character = 'Z';  // --..
            tree[28].character = 'Q';  // --.-
            tree[31].character = '5';  // .....
            tree[32].character = '4';  // ....-
            tree[34].character = '3';  // ...--
            tree[38].character = '2';  // ..---
            tree[46].character = '1';  // .----
            tree[47].character = '6';  // -....
            tree[48].character = '=';  // -...-
            tree[49].character = '/';  // -..-.
            tree[55].character = '7';  // --...
            tree[59].character = '8';  // ---..
            tree[61].character = '9';  // ----.
            tree[62].character = '0';  // -----

            // Prosigns (depth 5-6)
            tree[41].character = '+';  // .-.-. (AR — end of message)
            tree[68].character = '*';  // ...-.- (SK — end of contact)
            // BT (-...-) is same as '=' at tree[48], already mapped

            // Punctuation. Absent until 2026-07-20, which made every period
            // decode as '*' (nearest mapped node, SK) and every comma vanish.
            // Invisible to the synthetic suite: MSG_CQ and MSG_FULL are the
            // only messages it sends and neither contains punctuation. Real
            // ARRL code practice text is 23 periods and 21 commas per session.
            tree[84].character  = '.';  // .-.-.- (AR at tree[41] plus a dah)
            tree[114].character = ',';  // --..-- (Z at tree[27] plus two dahs)

            // English letter frequency prior (relative, used for tie-breaking)
            memset(letterPrior, 0, sizeof(letterPrior));
            letterPrior['E'-'A'] = 13.0f; letterPrior['T'-'A'] = 9.1f;
            letterPrior['A'-'A'] = 8.2f;  letterPrior['O'-'A'] = 7.5f;
            letterPrior['I'-'A'] = 7.0f;  letterPrior['N'-'A'] = 6.7f;
            letterPrior['S'-'A'] = 6.3f;  letterPrior['H'-'A'] = 6.1f;
            letterPrior['R'-'A'] = 6.0f;  letterPrior['D'-'A'] = 4.3f;
            letterPrior['L'-'A'] = 4.0f;  letterPrior['C'-'A'] = 2.8f;
            letterPrior['U'-'A'] = 2.8f;  letterPrior['M'-'A'] = 2.4f;
            letterPrior['W'-'A'] = 2.4f;  letterPrior['F'-'A'] = 2.2f;
            letterPrior['G'-'A'] = 2.0f;  letterPrior['Y'-'A'] = 2.0f;
            letterPrior['P'-'A'] = 1.9f;  letterPrior['B'-'A'] = 1.5f;
            letterPrior['V'-'A'] = 1.0f;  letterPrior['K'-'A'] = 0.8f;
            letterPrior['J'-'A'] = 0.15f; letterPrior['X'-'A'] = 0.15f;
            letterPrior['Q'-'A'] = 0.10f; letterPrior['Z'-'A'] = 0.07f;

            paths.clear();
            paths.push_back({0, 1.0f});
        }

        // Add an element with confidence-weighted beam search.
        // High confidence: strong preference for the classified element.
        // Low confidence: both dit and dah branches survive.
        void addElement(Element elem, float confidence) {
            // Clamp confidence to avoid killing branches entirely
            confidence = std::max(0.1f, std::min(confidence, 0.95f));

            float pClassified = 0.5f + confidence * 0.5f;   // P(classified element)
            float pAlternate = 1.0f - pClassified;           // P(other element)

            std::vector<PathState> newPaths;
            newPaths.reserve(paths.size() * 2);

            for (auto& p : paths) {
                int ditChild = 2 * p.node + 1;
                int dahChild = 2 * p.node + 2;

                float ditProb = (elem == DIT) ? pClassified : pAlternate;
                float dahProb = (elem == DAH) ? pClassified : pAlternate;

                bool anyChild = false;
                if (ditChild < treeSize) {
                    newPaths.push_back({ditChild, p.prob * ditProb});
                    anyChild = true;
                }
                if (dahChild < treeSize) {
                    newPaths.push_back({dahChild, p.prob * dahProb});
                    anyChild = true;
                }
                // If at tree boundary, keep path at current node (clamped)
                if (!anyChild) {
                    newPaths.push_back({p.node, p.prob * 0.5f});
                }
            }

            // Prune: keep top N paths. Expand beam under low confidence
            // to keep more hypotheses alive under heavy degradation.
            int beamWidth = (confidence < 0.5f) ? maxPathsWide : maxPaths;
            if ((int)newPaths.size() > beamWidth) {
                std::partial_sort(newPaths.begin(), newPaths.begin() + beamWidth, newPaths.end(),
                    [](const PathState& a, const PathState& b) { return a.prob > b.prob; });
                newPaths.resize(beamWidth);
            }

            // Normalize probabilities to prevent underflow
            float maxProb = 0;
            for (auto& p : newPaths) { maxProb = std::max(maxProb, p.prob); }
            if (maxProb > 0) {
                for (auto& p : newPaths) { p.prob /= maxProb; }
            }

            paths = std::move(newPaths);
        }

        // Character break: select the best character from all paths.
        // Applies letter frequency prior for tie-breaking.
        char characterBreak() {
            char bestChar = '\0';
            float bestScore = -1.0f;

            for (auto& p : paths) {
                if (p.node <= 0 || p.node >= treeSize) continue;
                char c = tree[p.node].character;
                if (c == '\0') continue;

                float score = p.prob;

                // Apply mild letter frequency prior (logarithmic to avoid dominating)
                if (c >= 'A' && c <= 'Z') {
                    float prior = letterPrior[c - 'A'];
                    if (prior > 0) {
                        score *= (1.0f + 0.1f * prior);  // mild boost, max ~2.3x for 'E'
                    }
                }

                if (score > bestScore) {
                    bestScore = score;
                    bestChar = c;
                }
            }

            // Reset all paths to root
            paths.clear();
            paths.push_back({0, 1.0f});

            return bestChar;
        }

        char wordBreak() {
            return characterBreak();
        }

        void reset() {
            paths.clear();
            paths.push_back({0, 1.0f});
        }

    private:
        struct PathState {
            int node;
            float prob;
        };

        static constexpr int maxPaths = 8;
        static constexpr int maxPathsWide = 12;  // expanded beam for low-confidence elements
        static constexpr int treeSize = 127;  // depth 7: supports 6-element prosigns
        MorseNode tree[treeSize] = {};
        float letterPrior[26] = {};
        std::vector<PathState> paths;
    };

}
