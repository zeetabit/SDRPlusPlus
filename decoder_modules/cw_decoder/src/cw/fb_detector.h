#pragma once
#include "tone_detector.h"     // KeyEvent
#include "model_fit.h"
#include <vector>
#include <utility>
#include <algorithm>
#include <cmath>

namespace cw {

    // Streaming forward-backward soft key detector (docs §52, the #42 line).
    //
    // A 2-state HMM over key state (space/mark) on the envelope. Emissions are a
    // Gaussian per state whose {mu,var} are fit online by a trailing-window EM
    // (ModelFitScorer). The forward pass is causal; the backward pass is FIXED-LAG
    // over [t, t+LAG] — validated to reproduce the full-signal batch smoother to
    // <1e-3 CER because within an element the emission LLR pins beta in a few
    // samples (§52 step 1a). This makes it a real online detector with LAG-sample
    // latency, unlike the batch probe it was proven against.
    //
    // Rejection is by EVIDENCE DURATION, not magnitude: a brief noise spike never
    // accumulates enough posterior mass to cross, so the spurious-short-event flood
    // that defeats the Schmitt trigger under noise (§20.8) does not occur. It does
    // NOT squelch below the copy floor — that is a channel-level decision on the
    // integrated model-fit (§52 step 3); this stage only decodes.
    class FBDetector {
    public:
        void init(float internalRate) {
            _rate = internalRate;
            _winW  = std::max(256, (int)(internalRate * 1.2f));   // 1.2 s trailing EM window
            reset();
        }

        void reset() {
            _abs = 0; _sinceRefit = 0; _fill = 0;
            _haveParams = false;
            _muLo = 0; _muHi = 0; _vLo = 1; _vHi = 1; _snrDb = 0; _keyDown = false;
            _a0 = 0.5; _a1 = 0.5;
            _win.assign(_winW, 0.0f); _winHead = 0;
            const int R = LAG + 1;
            _rEnv.assign(R, 0.0f); _rMuLo.assign(R, 0.0f); _rMuHi.assign(R, 0.0f);
            _rVLo.assign(R, 1.0f); _rVHi.assign(R, 1.0f);
            _rA0.assign(R, 0.5); _rA1.assign(R, 0.5);
            _emitState = 0; _rawRun = 0; _rawState = 0;
            _wLo = _wXLo = _wXXLo = _wHi = _wXHi = _wXXHi = 0.0;
        }

        std::vector<KeyEvent> process(const float* env, int count) {
            std::vector<KeyEvent> out;
            const long long blockStart = _abs;
            for (int i = 0; i < count; i++) {
                const float x = env[i];
                // Fixed window only bootstraps the first fit; recursive online-EM
                // (updateEmission) tracks the params thereafter, so QSB fades are
                // followed without the window's long/short noise-vs-fade tradeoff.
                if (!_haveParams) {
                    _win[_winHead] = x; _winHead = (_winHead + 1) % _winW;
                    if (_fill < _winW) { _fill++; }
                    if (++_sinceRefit >= REFIT_STRIDE || _fill >= MIN_FIT) { refit(); }
                }

                forwardStep(x);                       // updates _a0,_a1 + lag ring
                if (_haveParams) { updateEmission(x); }
                if (_fwdOnly) { fwdDecide(blockStart, out); }
                else if (_abs >= LAG) { finalize(_abs - LAG, blockStart, out); }
                _abs++;
            }
            return out;
        }

        float getSNR() const { return _snrDb; }
        bool isKeyDown() const { return _keyDown; }
        void preseed(float level, int count) { (void)level; (void)count; }

    private:
        static constexpr int LAG         = 48;    // fixed smoothing lag (samples)
        static constexpr int REFIT_STRIDE = 128;  // EM refit period (samples)
        static constexpr int MIN_FIT     = 64;    // min samples before a first fit
        static constexpr int MIN_RUN     = 12;    // debounce: min committed run
        static constexpr float SWITCH_P  = 0.01f; // HMM transition prior
        static constexpr float FIT_LLR_MIN = 0.02f; // min bimodality to adopt a refit
        static constexpr double ONLINE_LAM = 0.003;  // online-EM forgetting (~0.33s eff)

        void refit() {
            _sinceRefit = 0;
            const int n = std::min(_fill, _winW);
            if (n < MIN_FIT) { return; }
            _fitBuf.resize(n);
            // unroll the ring oldest->newest
            int idx = (_winHead - n + _winW) % _winW;
            for (int k = 0; k < n; k++) { _fitBuf[k] = _win[idx]; idx = (idx + 1) % _winW; }
            ModelFit f = _scorer.score(_fitBuf.data(), n);
            // Only adopt a fit that is genuinely bimodal (#41 llr). A window that is
            // mostly space (long gap) fits a degenerate near-unimodal mixture; adopting
            // it would collapse muHi onto muLo, drop getSNR to ~0 (gating the next
            // word's key-downs via StagedCore sqFactor) and corrupt the emission model
            // for the next mark. Holding the last confident fit through gaps is correct.
            if (f.muHi - f.muLo > 1e-6f && f.llr > FIT_LLR_MIN) {
                float vf = 0.2f * (f.muHi - f.muLo); vf *= vf;
                _muLo = f.muLo; _muHi = f.muHi;
                _vLo = std::max(f.vLo, vf); _vHi = std::max(f.vHi, vf);
                // keying-contrast SNR: mark/noise separation in dB, for the channel gate
                _snrDb = 10.0f * std::log10((f.muHi + 1e-9f) / (f.muLo + 1e-9f));
                // Seed the recursive online-EM sufficient stats (normalised, so
                // wHi+wLo=1) from this one-shot bootstrap fit; online tracking takes
                // over from here (§52 step 4: no fixed window -> fade-robust).
                double wH = std::max(0.05, std::min(0.95, (double)f.wHi)), wL = 1.0 - wH;
                _wHi = wH; _wXHi = wH * _muHi; _wXXHi = wH * ((double)_vHi + (double)_muHi*_muHi);
                _wLo = wL; _wXLo = wL * _muLo; _wXXLo = wL * ((double)_vLo + (double)_muLo*_muLo);
                _haveParams = true;
            }
        }

        // Recursive online EM: exponentially-forgetting, POSTERIOR-WEIGHTED updates
        // of the two-component emission stats. Only mark-posterior mass updates the
        // mark stats, so muHi tracks the fading mark level as marks arrive and HOLDS
        // through gaps (no all-space collapse); muLo tracks the noise floor. One
        // timescale (ONLINE_LAM) but soft-weighted, so it adapts in element-time:
        // fast enough for QSB, noise-robust by averaging over marks.
        void updateEmission(float x) {
            const double lam = ONLINE_LAM, keep = 1.0 - lam;
            const double rHi = _a1, rLo = _a0;
            _wHi  = keep*_wHi  + lam*rHi;       _wLo  = keep*_wLo  + lam*rLo;
            _wXHi = keep*_wXHi + lam*rHi*x;     _wXLo = keep*_wXLo + lam*rLo*x;
            _wXXHi= keep*_wXXHi+ lam*rHi*x*x;   _wXXLo= keep*_wXXLo+ lam*rLo*x*x;
            if (_wHi > 1e-4 && _wLo > 1e-4) {
                double muHi = _wXHi/_wHi, muLo = _wXLo/_wLo;
                if (muHi > muLo + 1e-6) {
                    double vf = 0.2*(muHi-muLo); vf *= vf;
                    _muHi = (float)muHi; _muLo = (float)muLo;
                    _vHi = (float)std::max(_wXXHi/_wHi - muHi*muHi, vf);
                    _vLo = (float)std::max(_wXXLo/_wLo - muLo*muLo, vf);
                    _snrDb = 10.0f * std::log10((float)((muHi + 1e-9) / (muLo + 1e-9)));
                }
            }
        }

        void emitLL(float x, float muLo, float muHi, float vLo, float vHi,
                    double& e0, double& e1) const {
            double d0 = x - muLo, d1 = x - muHi;
            double lb0 = -0.5 * (d0*d0 / vLo + std::log(vLo));
            double lb1 = -0.5 * (d1*d1 / vHi + std::log(vHi));
            double m = std::max(lb0, lb1);
            e0 = std::exp(lb0 - m); e1 = std::exp(lb1 - m);
        }

        void forwardStep(float x) {
            const double stay = 1.0 - SWITCH_P, cross = SWITCH_P;
            double e0, e1; emitLL(x, _muLo, _muHi, _vLo, _vHi, e0, e1);
            double x0 = (_a0*stay + _a1*cross) * e0;
            double x1 = (_a1*stay + _a0*cross) * e1;
            double s = 1.0 / (x0 + x1 + 1e-300);
            _a0 = x0*s; _a1 = x1*s;
            _keyDown = _a1 > _a0;
            const int r = (int)(_abs % (LAG + 1));
            _rEnv[r] = x; _rMuLo[r] = _muLo; _rMuHi[r] = _muHi; _rVLo[r] = _vLo; _rVHi[r] = _vHi;
            _rA0[r] = _a0; _rA1[r] = _a1;
        }

        // Forward-only (ZERO smoothing lag): decide from the causal forward posterior
        // directly. The low transition prior (SWITCH_P) already makes the forward
        // filter sticky, so it resists spurious flips without a backward pass; the
        // min-run debounce adds a small CONSTANT detection delay (cancels in timing).
        // No backward => none of the fixed-lag/windowed-emission boundary drift that
        // char-splits high-SNR signals under narrow smoothing (§52 step 4, user).
        void fwdDecide(long long blockStart, std::vector<KeyEvent>& out) {
            uint8_t raw = (_a1 > _a0) ? 1 : 0;
            if (raw == _rawState) { _rawRun++; }
            else { _rawState = raw; _rawRun = 1; }
            if (_rawState != _emitState && _rawRun >= MIN_RUN) {
                _emitState = _rawState;
                KeyEvent ev; ev.keyDown = (_emitState == 1);
                ev.sampleOffset = (int)(_abs - blockStart);
                out.push_back(ev);
            }
        }

        // Fixed-lag backward over [T, T+LAG]; MAP at T -> streaming debounce -> emit.
        void finalize(long long T, long long blockStart, std::vector<KeyEvent>& out) {
            const double stay = 1.0 - SWITCH_P, cross = SWITCH_P;
            double b0 = 1.0, b1 = 1.0;
            const long long hi = T + LAG;            // == _abs (current head)
            for (long long u = hi; u > T; u--) {
                int r = (int)(u % (LAG + 1));
                double e0, e1; emitLL(_rEnv[r], _rMuLo[r], _rMuHi[r], _rVLo[r], _rVHi[r], e0, e1);
                double nb0 = stay*e0*b0 + cross*e1*b1;
                double nb1 = stay*e1*b1 + cross*e0*b0;
                double s = 1.0 / (nb0 + nb1 + 1e-300); b0 = nb0*s; b1 = nb1*s;
            }
            int rt = (int)(T % (LAG + 1));
            uint8_t map = (_rA1[rt]*b1 > _rA0[rt]*b0) ? 1 : 0;

            // Streaming min-run debounce (approximates the batch 3-pass min-run):
            // commit a flip only after MIN_RUN consecutive raw samples agree.
            if (map == _rawState) { _rawRun++; }
            else { _rawState = map; _rawRun = 1; }
            if (_rawState != _emitState && _rawRun >= MIN_RUN) {
                _emitState = _rawState;
                KeyEvent ev; ev.keyDown = (_emitState == 1);
                // Report at the current head (offset in [0,count)); this shifts every
                // event by a constant LAG, which cancels in timing differences, and
                // avoids negative offsets that reference a prior block (§52 step 4:
                // negative offsets break StagedCore's cross-block gap accounting).
                ev.sampleOffset = (int)(_abs - blockStart);
                out.push_back(ev);
            }
        }

        bool _fwdOnly = true;     // §52 step 4: forward-only, zero-lag (user)
        float _rate = 1000.0f;
        int _winW = 1200;
        ModelFitScorer _scorer;

        long long _abs = 0;
        int _sinceRefit = 0, _fill = 0;
        bool _haveParams = false, _keyDown = false;
        float _muLo = 0, _muHi = 0, _vLo = 1, _vHi = 1, _snrDb = 0;
        double _a0 = 0.5, _a1 = 0.5;

        std::vector<float> _win; int _winHead = 0;
        std::vector<float> _fitBuf;
        std::vector<float> _rEnv, _rMuLo, _rMuHi, _rVLo, _rVHi;
        std::vector<double> _rA0, _rA1;

        uint8_t _emitState = 0, _rawState = 0; int _rawRun = 0;
        // Recursive online-EM sufficient statistics (posterior-weighted, forgetting).
        double _wLo = 0, _wXLo = 0, _wXXLo = 0, _wHi = 0, _wXHi = 0, _wXXHi = 0;
    };

}
