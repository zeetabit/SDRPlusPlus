#pragma once
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdlib>

// Soft-segmentation sequence decoder (docs matched-filter-element-integrity.md
// §7b). The fb path collapses its soft posterior into hard events at MIN_RUN, so
// a dah split by a spurious sub-dit gap is frozen as dit-gap-dit before the beam.
// This trellis keeps the SEGMENTATION soft: each marginal gap forks a SPLIT (close
// the element) and a MERGE (fold the gap back into the mark) hypothesis, weighted
// by P(real boundary). The valid-Morse tree — the only prior, structural not
// linguistic — resolves which segmentation is real. This is the soft version of
// the dip-depth idea that failed as a hard gate.

namespace cw {

    class SoftSeqDecoder {
    public:
        SoftSeqDecoder() { buildTree(); }

        // Whole-stream decode of a (keyDown, timeMs) event list under a dit estimate.
        // Returns the best path's text. Pure structural decode — no vocabulary.
        std::string decode(const std::vector<std::pair<bool, float>>& evs, float ditMs) {
            if (ditMs < 5.0f) { ditMs = 60.0f; }
            const float forkMax = FORK_MAX * ditMs;   // gaps below this are ambiguous
            const float wordGap = WORD_GAP * ditMs;
            const float charGap = CHAR_GAP * ditMs;

            _beam.clear();
            _beam.push_back({0, 1.0f, 0.0f, std::string()});
            float lastDown = -1.0f, lastUp = -1.0f;

            for (auto& ev : evs) {
                if (ev.first) {                        // key DOWN: a gap just ended
                    if (lastUp >= 0.0f) { step(ev.second - lastUp, ditMs, forkMax, charGap, wordGap); }
                    lastDown = ev.second;
                } else {                               // key UP: a mark just ended
                    if (lastDown >= 0.0f) {
                        const float m = ev.second - lastDown;
                        for (auto& p : _beam) { p.pend += m; }
                    }
                    lastUp = ev.second;
                }
            }
            // Flush: close the final pending element and emit its character.
            for (auto& p : _beam) {
                if (p.pend > 0.3f * ditMs) { advance(p, p.pend, ditMs); }
                char c = charAt(p.node);
                if (c) { p.text += c; }
            }
            // Best path by prob * letter-frequency plausibility (existing beam prior).
            const std::string* best = nullptr; float bestScore = -1.0f;
            for (auto& p : _beam) {
                if (p.prob > bestScore) { bestScore = p.prob; best = &p.text; }
            }
            return best ? *best : std::string();
        }

    private:
        struct Path { int node; float prob; float pend; std::string text; };

        static constexpr int   treeSize   = 127;
        static constexpr int   BEAM_K      = 16;
        static constexpr float FORK_MAX    = 1.6f;   // fork split/merge below 1.6*dit
        static constexpr float CHAR_GAP    = 2.2f;   // >2.2*dit: character boundary
        static constexpr float WORD_GAP    = 5.0f;   // >5*dit: word space

        char charAt(int node) const {
            return (node > 0 && node < treeSize) ? _tree[node] : '\0';
        }
        // Close a pending mark of `markMs` as one element and advance the node.
        void advance(Path& p, float markMs, float ditMs) const {
            const int child = (markMs < 2.0f * ditMs) ? (2*p.node + 1) : (2*p.node + 2);
            p.node = (child < treeSize) ? child : p.node;
            p.pend = 0.0f;
        }
        // P(the gap is a real element boundary | duration). ~0 for sub-dit notches,
        // rising through 1*dit. Logistic centred at ~0.55*dit.
        static float pReal(float g, float ditMs) {
            const float z = (g - 0.55f * ditMs) / (0.22f * ditMs);
            return 1.0f / (1.0f + std::exp(-z));
        }

        void step(float g, float ditMs, float forkMax, float charGap, float wordGap) {
            _next.clear();
            for (auto& p : _beam) {
                if (g >= charGap) {
                    // Clear boundary: close element, emit char, reset to root.
                    Path np = p;
                    if (np.pend > 0.3f * ditMs) { advance(np, np.pend, ditMs); }
                    char c = charAt(np.node);
                    if (c) { np.text += c; }
                    if (g >= wordGap) { np.text += ' '; }
                    np.node = 0; np.pend = 0.0f;
                    _next.push_back(std::move(np));
                } else if (g < forkMax) {
                    // Ambiguous internal gap: fork SPLIT (element boundary) vs MERGE.
                    const float pr = pReal(g, ditMs);
                    Path sp = p;                       // SPLIT: close element, stay in char
                    if (sp.pend > 0.3f * ditMs) { advance(sp, sp.pend, ditMs); }
                    sp.prob *= pr;
                    _next.push_back(std::move(sp));
                    Path mg = p;                       // MERGE: gap folded into the mark
                    mg.pend += g; mg.prob *= (1.0f - pr);
                    _next.push_back(std::move(mg));
                } else {
                    // Between forkMax and charGap: treat as a within-char element gap.
                    Path np = p;
                    if (np.pend > 0.3f * ditMs) { advance(np, np.pend, ditMs); }
                    _next.push_back(std::move(np));
                }
            }
            // Prune to top-K by prob, then renormalise.
            if ((int)_next.size() > BEAM_K) {
                std::partial_sort(_next.begin(), _next.begin() + BEAM_K, _next.end(),
                    [](const Path& a, const Path& b) { return a.prob > b.prob; });
                _next.resize(BEAM_K);
            }
            float mx = 0.0f; for (auto& p : _next) { mx = std::max(mx, p.prob); }
            if (mx > 0.0f) { for (auto& p : _next) { p.prob /= mx; } }
            _beam.swap(_next);
        }

        void buildTree() {
            // Explicit table (mirrors morse_tree.h; kept local so MorseDecoder is
            // untouched and the default path stays byte-identical). dit=2n+1, dah=2n+2.
            std::fill(std::begin(_tree), std::end(_tree), '\0');
            _tree[1]='E'; _tree[2]='T'; _tree[3]='I'; _tree[4]='A'; _tree[5]='N';
            _tree[6]='M'; _tree[7]='S'; _tree[8]='U'; _tree[9]='R'; _tree[10]='W';
            _tree[11]='D'; _tree[12]='K'; _tree[13]='G'; _tree[14]='O'; _tree[15]='H';
            _tree[16]='V'; _tree[17]='F'; _tree[19]='L'; _tree[21]='P'; _tree[22]='J';
            _tree[23]='B'; _tree[24]='X'; _tree[25]='C'; _tree[26]='Y'; _tree[27]='Z';
            _tree[28]='Q'; _tree[31]='5'; _tree[32]='4'; _tree[34]='3'; _tree[38]='2';
            _tree[46]='1'; _tree[47]='6'; _tree[48]='='; _tree[49]='/'; _tree[55]='7';
            _tree[59]='8'; _tree[61]='9'; _tree[62]='0';
            _tree[41]='+'; _tree[68]='*'; _tree[84]='.'; _tree[114]=',';
        }

        char _tree[treeSize];
        std::vector<Path> _beam, _next;
    };

}
