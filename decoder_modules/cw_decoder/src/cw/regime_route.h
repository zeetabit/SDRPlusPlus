#pragma once
#include "core.h"
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cw {

    // Regime router (docs §52 step 4, option B). Runs `select` and `fb` in parallel
    // and commits to one after a measurement window, by DECODE PLAUSIBILITY (§53).
    //
    // fb (forward-only HMM + online-EM + matched filter) wins DECISIVELY in heavy
    // broadband noise — the fast+heavy-AWGN regime nothing else copies (noise3-25wpm
    // 0.98 -> 0.41 vs select). select wins on clean, hand-keyed, fading, interference
    // and Farnsworth. The three inputs that separate them (§53): the valid/total token
    // RATIO (select's collapses to garbage in AWGN, stays high where it copes), fb's
    // keying CONTRAST (buried AWGN ~4 dB vs strong hand-keyed ~6 dB — the discriminator
    // inputSnr cannot make), and the pre-BPF inputSnr (broadband gate). Pre-commit,
    // select drives the sink live and fb's output is buffered; if fb wins the commit,
    // the provisional output is cleared and fb's buffer replayed, so no lead-in is lost.
    // Cost is ~2x decode per channel for the commit window, then 1x (winner only).
    class RegimeRouteCore : public IDecodeCore {
    public:
        // selConfGate>0 enables the correctness discriminator (§7c): route to fb when
        // SELECT's committed confidence is below the gate — select's confidence
        // collapses (<0.15) on the buried/valid-but-wrong regimes where fb wins, and
        // stays high (>0.6) where select copies. 0 = disabled (byte-identical legacy).
        RegimeRouteCore(std::unique_ptr<IDecodeCore> select, std::unique_ptr<IDecodeCore> fb,
                        float selConfGate = 0.0f)
            : _select(std::move(select)), _fb(std::move(fb)), _selConfGate(selConfGate) {}

        void init(float sampleRate, float internalRate, float toneFreq) override {
            _sampleRate = sampleRate;
            _select->init(sampleRate, internalRate, toneFreq);
            _fb->init(sampleRate, internalRate, toneFreq);
            reset();
        }
        void setToneFreq(float f) override { _select->setToneFreq(f); _fb->setToneFreq(f); }
        void reset() override {
            _select->reset(); _fb->reset();
            _committed = false; _useFb = false; _samples = 0; _selConfMax = 0.0f;
            _fbLog.clear(); _selLog.clear();
        }
        void preseed(float level, int count) override {
            _select->preseed(level, count); _fb->preseed(level, count);
        }

        void process(int count, const dsp::complex_t* iq, CharSink& sink) override {
            // Route: pre-commit select drives the sink (and both cores buffer, to score
            // decode quality); post-commit the winner drives the sink, loser discarded.
            _fbSink.bind((_committed && _useFb) ? &sink : nullptr, _committed ? nullptr : &_fbLog);
            _selSink.bind((!_committed || !_useFb) ? &sink : nullptr, _committed ? nullptr : &_selLog);
            // Pre-commit both run (to score); post-commit only the winner runs, so the
            // 2x cost is confined to the ~6s acquisition window, not steady state.
            if (!_committed || _useFb)  { _fb->process(count, iq, _fbSink); }
            if (!_committed || !_useFb) { _select->process(count, iq, _selSink); }
            _samples += count;

            // Track the PEAK of select's confidence over the pre-commit window. The
            // confidence gate must fire only when select NEVER showed competence
            // (persistent failure) — a single-snapshot read at commit misfires on the
            // TRANSIENT dips that clean/handkeyed/contest show while re-locking, which
            // recover to high confidence (§7c gate regression, 2026-07-26).
            if (!_committed) {
                _selConfMax = std::max(_selConfMax, _select->stats().confidence);
            }

            if (!_committed && _samples >= (long long)(_sampleRate * COMMIT_SEC)) {
                // Decide by DECODE PLAUSIBILITY, not raw SNR: fb and select both read as
                // buried on qsb/qrm/weak-hand-keyed (low inputSnr), but select copies
                // those well while fb only truly wins where select produces garbage
                // (broadband noise). The signal is the valid/total token RATIO plus fb's
                // keying contrast — see the gate below (§53).
                int fbTot = 0, selTot = 0;
                const int fbGood = validTokens(_fbLog, &fbTot), selGood = validTokens(_selLog, &selTot);
                const float inSnr = _fb->stats().inputSnr;
                // Garbage discriminator: the valid/total RATIO, not the raw count. In
                // heavy AWGN select emits many words but few valid (ratio -> 0) while fb
                // copies clean (ratio ~0.5-0.8); where select COPES (mild noise, qsb,
                // qrm, contest) its ratio stays 0.75-1.0. So route to fb only when select
                // is producing garbage (selRatio low) AND fb is decisively cleaner
                // (fbRatio margin) AND fb itself copies enough (fbGood) AND it is broad-
                // band (inputSnr). This is gate-safe aggression: every select-wins regime
                // has a high selRatio and is excluded, unlike the raw-count gate which
                // stalled on select's 1-2 garbage tokens (§53).
                const float selRatio = selTot > 0 ? (float)selGood / selTot : 1.0f;
                const float fbRatio  = fbTot  > 0 ? (float)fbGood  / fbTot  : 0.0f;
                // fbContrast (fb's keying-contrast SNR, muHi/muLo in dB) is the buried-
                // vs-strong discriminator inputSnr cannot make: fb wins only on BURIED
                // signals (contrast ~4 dB — muHi barely over the noise), and LOSES on
                // strong-but-jittered hand-keyed (contrast ~6 dB, signal present but
                // irregular). Both read ~6 dB inputSnr pre-BPF, but the narrow matched
                // filter separates them post-detection. Gating contrast < CONTRAST_MAX
                // keeps every AWGN win and excludes weak-hand-keyed, whose few clean
                // tokens otherwise fool the valid/total ratio (§53).
                const float fbContrast = _fb->stats().snr;
                const bool ratioGate = (selRatio < SEL_RATIO_MAX)
                                       && (fbRatio > selRatio + RATIO_MARGIN)
                                       && (fbContrast < CONTRAST_MAX)
                                       && (inSnr < ROUTE_SNR);
                // §7c correctness discriminator: select confidently WRONG (low conf,
                // valid-but-wrong copy) where the token RATIO cannot tell (selRatio~1).
                // confGate fires when select's PEAK confidence stayed low — it never
                // copied (persistent failure), not a transient re-lock dip. Stands
                // alone: the cont fb sub-core batches its re-decode so fbGood is ~0 at
                // commit; the fbGood ratio-guard would wrongly veto it. fb-cont is the
                // trusted fallback where select genuinely never copies.
                const bool confGate = (_selConfGate > 0.0f) && (_selConfMax < _selConfGate);
                _useFb = (fbGood >= FB_ABS && ratioGate) || confGate;
                static const bool dbgEnv = getenv("ROUTE_DBG") != nullptr;
                if (debug || dbgEnv) fprintf(stderr, "[ROUTE] sel=%d/%d(%.2f) fb=%d/%d(%.2f) inputSnr=%.1f fbContrast=%.1f selConfMax=%.3f gate=%.2f confG=%d ratioG=%d -> %s\n",
                                   selGood, selTot, selRatio, fbGood, fbTot, fbRatio, inSnr, _fb->stats().snr, _selConfMax, _selConfGate, confGate?1:0, ratioGate?1:0, _useFb ? "FB" : "select");
                _committed = true;
                if (_useFb) { sink.clearEmitted(); replay(sink); }   // adopt fb's lead-in
                _fbLog.clear(); _fbLog.shrink_to_fit();
                _selLog.clear(); _selLog.shrink_to_fit();
            }
        }

        CoreStats stats() const override {
            return (_committed && _useFb ? _fb : _select)->stats();
        }
        const float* diagnostic(int& n) const override {
            return (_committed && _useFb ? _fb : _select)->diagnostic(n);
        }

        inline static bool debug = false;   // §53 arbitration instrumentation (test-only)

    private:
        static constexpr float ROUTE_SNR = 8.0f;   // fb only considered below this
        static constexpr float COMMIT_SEC = 12.0f;   // longer window: token counts separate AWGN (select fails) from qsb/qrm/mild (select copes)
        static constexpr int   FB_ABS  = 3;         // fb must ITSELF copy >= this valid tokens
        static constexpr float SEL_RATIO_MAX = 0.6f;// select is "failing" below this valid/total ratio
        static constexpr float RATIO_MARGIN  = 0.25f;// fb must beat select's ratio by this margin
        static constexpr float CONTRAST_MAX  = 5.5f; // fb wins only BURIED signals; excludes strong hand-keyed

        struct Op { uint8_t type; char c; float conf; };  // 0=char 1=flushWord 2=wordGap 3=clear

        // Reference-free decode quality: count valid ham tokens (callsign / Q-abbr /
        // numeric) in the buffered output. Real copy is rich in them; noise garbage
        // is not — so it separates "select copes" (qsb/qrm/hk) from "select fails"
        // (broadband noise), where inputSnr alone cannot.
        static bool isValidToken(const std::string& t) {
            // Length >= 2 only: single chars (E/T/I/R/K...) are exactly what noise
            // garbage emits, so counting them would let select's garbage score as high
            // as fb's real copy and defeat the arbitration.
            if (t.size() < 2) return false;
            static const char* codes[] = {"CQ","DE","TU","TNX","UR","RST","599","5NN","73",
                "KN","AR","SK","PSE","QRZ","QSL","QTH","NAME","FER","QSO","GM","GA","GE","FB"};
            for (auto c : codes) if (t == c) return true;
            bool allDigit = true; int digits = 0, letters = 0;
            for (char c : t) { if (std::isdigit((unsigned char)c)) digits++; else { allDigit = false; if (std::isalpha((unsigned char)c)) letters++; } }
            if (allDigit) return t.size() >= 2;                                 // RST / serial
            return t.size() >= 3 && t.size() <= 7 && digits >= 1 && letters >= 2;   // callsign
        }
        // Count valid ham tokens AND the total tokens emitted. The RATIO (valid/total)
        // is the garbage discriminator the raw count misses: heavy-AWGN garbage emits
        // MANY words with few valid (low ratio); real copy is mostly valid (high ratio).
        // A word must have >= 2 chars to count toward the total (single-char noise
        // fragments are not "words"); this matches isValidToken's own length floor.
        int validTokens(const std::vector<Op>& log, int* totalOut = nullptr) const {
            int good = 0, total = 0; std::string w;
            auto flush = [&]{
                if (w.size() >= 2) {
                    total++;
                    std::string u; for (char c : w) u += (char)std::toupper((unsigned char)c);
                    if (isValidToken(u)) good++;
                }
                w.clear();
            };
            for (const auto& op : log) {
                if (op.type == 0) { if (op.c == ' ') flush(); else w += op.c; }
                else if (op.type == 1 || op.type == 2) { flush(); }
                else if (op.type == 3) { w.clear(); good = 0; total = 0; }   // clearEmitted: reset
            }
            flush();
            if (totalOut) *totalOut = total;
            return good;
        }

        struct RouteSink : public CharSink {
            CharSink* out = nullptr; std::vector<Op>* log = nullptr;
            void bind(CharSink* o, std::vector<Op>* l) { out = o; log = l; }
            void emitChar(char c, float cf) override { if (out) out->emitChar(c, cf); if (log) log->push_back({0, c, cf}); }
            void flushWord() override { if (out) out->flushWord(); if (log) log->push_back({1, 0, 0}); }
            void emitWordGap() override { if (out) out->emitWordGap(); if (log) log->push_back({2, 0, 0}); }
            void clearEmitted() override { if (out) out->clearEmitted(); if (log) log->push_back({3, 0, 0}); }
        };
        void replay(CharSink& sink) {
            for (const auto& op : _fbLog) {
                switch (op.type) {
                    case 0: sink.emitChar(op.c, op.conf); break;
                    case 1: sink.flushWord(); break;
                    case 2: sink.emitWordGap(); break;
                    case 3: sink.clearEmitted(); break;
                }
            }
        }

        std::unique_ptr<IDecodeCore> _select, _fb;
        RouteSink _fbSink, _selSink;
        std::vector<Op> _fbLog, _selLog;
        bool _committed = false, _useFb = false;
        long long _samples = 0;
        float _sampleRate = 8000.0f;
        float _selConfGate = 0.0f;   // §7c: route-to-fb-when-select-unsure threshold (0=off)
        float _selConfMax = 0.0f;    // peak select confidence over the pre-commit window
    };
}
