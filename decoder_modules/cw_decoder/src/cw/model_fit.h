#pragma once
#include <vector>
#include <algorithm>
#include <cmath>

namespace cw {

    // Passive envelope-quality scorer (SparkGap ITILA, lever #41, docs §52).
    //
    // Fits a two-component 1-D Gaussian mixture to the detector's real envelope and
    // returns a per-sample log-likelihood ratio of that keyed model against a
    // single-Gaussian null. The number is ORTHOGONAL to SNR: a steady carrier or a
    // het is loud (high SNR) but UNIMODAL (LLR ~ 0); keyed CW is BIMODAL (noise
    // floor vs mark level) and scores high. That is exactly the false positive the
    // channel manager's `snr > 6.0` gate passes.
    //
    // Scores only — never decides key state, so it cannot alter a decode. The
    // Rayleigh/Rician envelope law is approximated by Gaussians here; the full
    // forward-backward HMM (lever #42) is the eventual replacement and reuses this
    // mixture as its measurement primitive.
    struct ModelFit {
        float llr   = 0.0f;   // per-sample (logL_2comp - logL_1comp); >0 => bimodal/keyed
        float muLo  = 0.0f;   // fitted noise-floor mean
        float muHi  = 0.0f;   // fitted mark-level mean
        float wHi   = 0.0f;   // fitted mark occupancy (duty-cycle estimate)
    };

    class ModelFitScorer {
    public:
        void setIterations(int n)   { iters = n; }
        void setVarFloorFrac(float f) { varFloorFrac = f; }

        ModelFit score(const float* env, int n) const {
            ModelFit r;
            if (!env || n < minSamples) { return r; }

            double sum = 0.0, sumSq = 0.0;
            float lo = env[0], hi = env[0];
            for (int i = 0; i < n; i++) {
                float v = env[i];
                sum += v; sumSq += (double)v * v;
                lo = std::min(lo, v); hi = std::max(hi, v);
            }
            const double mean = sum / n;
            const double var1  = std::max(sumSq / n - mean * mean, 1e-12);
            if (hi - lo < 1e-9f) { return r; }   // constant signal: no structure to fit

            // Variance floor keeps a component from collapsing onto a single sample.
            const double varFloor = varFloorFrac * varFloorFrac * var1 + 1e-12;

            // Init at the amplitude quartiles so the two components start separated.
            double muLo = mean - 0.5 * std::sqrt(var1);
            double muHi = mean + 0.5 * std::sqrt(var1);
            double vLo = var1, vHi = var1, wHi = 0.5;

            for (int it = 0; it < iters; it++) {
                double sLoW = 0, sLoX = 0, sLoXX = 0;
                double sHiW = 0, sHiX = 0, sHiXX = 0;
                for (int i = 0; i < n; i++) {
                    double x = env[i];
                    double lLo = logGauss(x, muLo, vLo) + std::log(1.0 - wHi + 1e-12);
                    double lHi = logGauss(x, muHi, vHi) + std::log(wHi + 1e-12);
                    double m = std::max(lLo, lHi);
                    double rHi = std::exp(lHi - m) / (std::exp(lLo - m) + std::exp(lHi - m));
                    double rLo = 1.0 - rHi;
                    sLoW += rLo; sLoX += rLo * x; sLoXX += rLo * x * x;
                    sHiW += rHi; sHiX += rHi * x; sHiXX += rHi * x * x;
                }
                if (sLoW < 1e-6 || sHiW < 1e-6) { break; }   // a component emptied out
                muLo = sLoX / sLoW; muHi = sHiX / sHiW;
                vLo = std::max(sLoXX / sLoW - muLo * muLo, varFloor);
                vHi = std::max(sHiXX / sHiW - muHi * muHi, varFloor);
                wHi = sHiW / n;
            }

            // Enforce lo<hi ordering so muLo/muHi/wHi are interpretable regardless
            // of which component EM settled into.
            if (muLo > muHi) {
                std::swap(muLo, muHi); std::swap(vLo, vHi); wHi = 1.0 - wHi;
            }

            // Composite unimodal null: the BETTER of a single Gaussian (explains a
            // carrier/het) or a single Rayleigh (explains the noise floor), each
            // ML-fit. LLR then rewards only genuinely two-level data — a Rayleigh
            // floor plus separated Rician marks, i.e. keyed CW. A signal that any
            // single unimodal law already explains (carrier OR noise) scores ~0.
            const double rayVar = std::max(sumSq / n * 0.5, 1e-12);
            double ll2 = 0.0, llGauss = 0.0, llRay = 0.0;
            for (int i = 0; i < n; i++) {
                double x = env[i];
                double lLo = logGauss(x, muLo, vLo) + std::log(1.0 - wHi + 1e-12);
                double lHi = logGauss(x, muHi, vHi) + std::log(wHi + 1e-12);
                double m = std::max(lLo, lHi);
                ll2     += m + std::log(std::exp(lLo - m) + std::exp(lHi - m));
                llGauss += logGauss(x, mean, var1);
                llRay   += logRayleigh(x, rayVar);
            }

            r.llr  = (float)((ll2 - std::max(llGauss, llRay)) / n);
            r.muLo = (float)muLo;
            r.muHi = (float)muHi;
            r.wHi  = (float)wHi;
            return r;
        }

    private:
        static double logGauss(double x, double mu, double var) {
            const double d = x - mu;
            return -0.5 * (std::log(2.0 * M_PI * var) + d * d / var);
        }

        // Rayleigh log-pdf: log(x/sigma^2) - x^2/(2 sigma^2), sigma^2 = var.
        static double logRayleigh(double x, double var) {
            if (x <= 0.0) { return -1e9; }
            return std::log(x / var) - x * x / (2.0 * var);
        }

        int   iters        = 20;
        int   minSamples   = 32;
        float varFloorFrac = 0.05f;   // component var floored to (frac^2)*global var
    };

}
