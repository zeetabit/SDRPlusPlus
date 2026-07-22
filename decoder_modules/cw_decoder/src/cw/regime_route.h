#pragma once
#include "core.h"
#include <cstdint>
#include <memory>
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
            _committed = false; _useFb = false; _samples = 0; _fbLog.clear();
        }
        void preseed(float level, int count) override {
            _select->preseed(level, count); _fb->preseed(level, count);
        }

        void process(int count, const dsp::complex_t* iq, CharSink& sink) override {
            // Route: pre-commit select drives the sink and fb buffers; post-commit the
            // winner drives the sink and the loser is discarded.
            _fbSink.bind((_committed && _useFb) ? &sink : nullptr, _committed ? nullptr : &_fbLog);
            _selSink.bind((!_committed || !_useFb) ? &sink : nullptr, nullptr);
            _fb->process(count, iq, _fbSink);
            _select->process(count, iq, _selSink);
            _samples += count;

            if (!_committed && _samples >= (long long)(_sampleRate * COMMIT_SEC)) {
                _useFb = (_fb->stats().inputSnr < ROUTE_SNR);
                _committed = true;
                if (_useFb) { sink.clearEmitted(); replay(sink); }   // adopt fb's lead-in
                _fbLog.clear(); _fbLog.shrink_to_fit();
            }
        }

        CoreStats stats() const override {
            return (_committed && _useFb ? _fb : _select)->stats();
        }
        const float* diagnostic(int& n) const override {
            return (_committed && _useFb ? _fb : _select)->diagnostic(n);
        }

    private:
        static constexpr float ROUTE_SNR = 8.0f;   // < this (broadband noise) -> fb
        static constexpr float COMMIT_SEC = 1.5f;   // inputSnr is bimodal, robust by here

        struct Op { uint8_t type; char c; float conf; };  // 0=char 1=flushWord 2=wordGap 3=clear
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
        std::vector<Op> _fbLog;
        bool _committed = false, _useFb = false;
        long long _samples = 0;
        float _sampleRate = 8000.0f;
    };
}
