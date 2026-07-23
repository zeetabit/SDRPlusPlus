#pragma once
#include "core.h"
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cw {

    // Regime router (docs §52 step 4, option B). Runs `select` and `fb` in parallel
    // and commits to one after a short measurement window, by the input-referred SNR.
    //
    // fb (forward-only HMM + online-EM + matched filter) wins DECISIVELY in heavy
    // broadband noise — the fast+heavy-AWGN regime nothing else copies (noise3-25wpm
    // -0.74 vs legacy). select wins on clean, hand-keyed, fading, interference and
    // Farnsworth — where fb's remaining harm sits. inputSnr is bimodal (clean/
    // hand-keyed/Farnsworth read high, broadband noise reads ~6 dB, §52 step 2 cal),
    // so routing on it captures fb's AWGN wins while sending fb's weak regimes to
    // select — the standing default. Pre-commit, select drives the sink live and fb's
    // output is buffered; if fb wins the commit, the provisional output is cleared and
    // fb's buffer replayed, so no lead-in is lost. Cost is ~2x decode per channel.
    class RegimeRouteCore : public IDecodeCore {
    public:
        RegimeRouteCore(std::unique_ptr<IDecodeCore> select, std::unique_ptr<IDecodeCore> fb)
            : _select(std::move(select)), _fb(std::move(fb)) {}

        void init(float sampleRate, float internalRate, float toneFreq) override {
            _sampleRate = sampleRate;
            _select->init(sampleRate, internalRate, toneFreq);
            _fb->init(sampleRate, internalRate, toneFreq);
            reset();
        }
        void setToneFreq(float f) override { _select->setToneFreq(f); _fb->setToneFreq(f); }
        void reset() override {
            _select->reset(); _fb->reset();
            _committed = false; _useFb = false; _samples = 0; _fbLog.clear(); _selLog.clear();
        }
        void preseed(float level, int count) override {
            _select->preseed(level, count); _fb->preseed(level, count);
        }

        void process(int count, const dsp::complex_t* iq, CharSink& sink) override {
            // Route: pre-commit select drives the sink (and both cores buffer, to score
            // decode quality); post-commit the winner drives the sink, loser discarded.
            _fbSink.bind((_committed && _useFb) ? &sink : nullptr, _committed ? nullptr : &_fbLog);
            _selSink.bind((!_committed || !_useFb) ? &sink : nullptr, _committed ? nullptr : &_selLog);
            _fb->process(count, iq, _fbSink);
            _select->process(count, iq, _selSink);
            _samples += count;

            if (!_committed && _samples >= (long long)(_sampleRate * COMMIT_SEC)) {
                // Decide by DECODE PLAUSIBILITY, not raw SNR: fb and select both read as
                // buried on qsb/qrm/weak-hand-keyed (low inputSnr), but select copies
                // those well while fb only truly wins where select produces garbage
                // (broadband noise). Adopt fb only when it decodes MORE real ham content
                // than select — biased to select (the standing default) on ties.
                const int fbGood = validTokens(_fbLog), selGood = validTokens(_selLog);
                // fb only where it decodes MORE real ham content than select (i.e. select
                // is failing) AND the channel reads as broadband noise. Ties -> select.
                _useFb = (fbGood > selGood) && (_fb->stats().inputSnr < ROUTE_SNR);
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

    private:
        static constexpr float ROUTE_SNR = 8.0f;   // fb only considered below this
        static constexpr float COMMIT_SEC = 6.0f;   // enough decoded words for reliable counts

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
        int validTokens(const std::vector<Op>& log) const {
            int good = 0; std::string w;
            auto flush = [&]{ std::string u; for (char c : w) u += (char)std::toupper((unsigned char)c); if (isValidToken(u)) good++; w.clear(); };
            for (const auto& op : log) {
                if (op.type == 0) { if (op.c == ' ') flush(); else w += op.c; }
                else if (op.type == 1 || op.type == 2) { flush(); }
                else if (op.type == 3) { w.clear(); good = 0; }   // clearEmitted: reset
            }
            flush();
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
    };
}
