#include <catch.hpp>
#include "cw_matrix.h"
#include <cstdio>

using namespace cw_test;

// ============================================================
// Core × profile benchmark matrix — ON DEMAND ONLY.
//
// Tagged [.] so Catch2 hides it from the default run: the always-on suite keeps
// the legacy-only regression gates in test_benchmark_multiseed.cpp, and this
// exploratory sweep is opt-in.
//
//   ./cw_decoder_tests "[matrix]" -s          full sweep
//   ./cw_decoder_tests "[matrix-timing]" -s   timing strategies only
// ============================================================

namespace {
    constexpr int SEEDS = 24;

    struct Prof { const char* name; const char* msg; SignalParams p; };

    std::vector<Prof> profiles() {
        std::vector<Prof> v;
        v.push_back({"clean-15",     MSG_FULL(), profileClean(80.0f)});
        v.push_back({"clean-25",     MSG_FULL(), profileClean(48.0f)});
        v.push_back({"handkeyed-15", MSG_FULL(), profileHandKeyed(80.0f)});
        v.push_back({"handkeyed-20", MSG_FULL(), profileHandKeyed(60.0f)});
        v.push_back({"handkeyed-25", MSG_FULL(), profileHandKeyed(48.0f)});
        v.push_back({"qsb",          MSG_FULL(), profileQSB(80.0f)});
        v.push_back({"qrm",          MSG_FULL(), profileQRM(80.0f)});
        v.push_back({"qrn",          MSG_FULL(), profileQRN(80.0f)});
        v.push_back({"farnsworth20", MSG_FULL(), profileFarnsworth(80.0f, 2.0f)});
        v.push_back({"worstcase",    MSG_FULL(), profileWorstCase(80.0f)});
        for (float amp : {2.0f, 3.0f, 4.0f}) {
            Prof pr;
            pr.name = amp == 2.0f ? "noise2.0" : (amp == 3.0f ? "noise3.0" : "noise4.0");
            pr.msg = MSG_FULL();
            pr.p = profileClean(80.0f);
            pr.p.noiseAmp = amp;
            v.push_back(pr);
        }
        return v;
    }

    void header() {
        printf("\n%-16s %-14s %7s %7s %7s %7s %7s %7s %8s %8s %8s\n",
               "core", "profile", "CER", "p95", "WER", "ins", "del", "sub",
               "rt(x)", "ttfo_ms", "wpmRMS");
        printf("%s\n", std::string(118, '-').c_str());
    }

    void row(const MatrixCell& c) {
        printf("%-16s %-14s %7.4f %7.4f %7.4f %7.4f %7.4f %7.4f %8.0f %8.0f %8.2f\n",
               c.core.c_str(), c.profile.c_str(),
               c.cerMean, c.cerP95, c.werMean,
               c.insRate, c.delRate, c.subRate,
               c.realtimeX, c.ttfoMs, c.wpmRmsErr);
    }

    void sweep(const std::vector<std::string>& cores) {
        header();
        for (const auto& core : cores) {
            for (auto& pr : profiles()) {
                row(runCell(core, pr.name, pr.msg, pr.p, SEEDS));
            }
            printf("\n");
        }
    }
}

TEST_CASE("Matrix: timing strategies", "[cw][.][matrix][matrix-timing]") {
    sweep({"legacy", "legacy+kmeans", "legacy+median", "legacy+bimodal"});
    SUCCEED();
}

TEST_CASE("Matrix: front-end bandwidth", "[cw][.][matrix][matrix-bpf]") {
    sweep({"legacy", "legacy+bpf40", "legacy+bpf30", "legacy+bpf20"});
    SUCCEED();
}

TEST_CASE("Matrix: all registered cores", "[cw][.][matrix][matrix-all]") {
    std::vector<std::string> all;
    for (const auto& s : cw::coreRegistry()) { all.push_back(s.name); }
    sweep(all);
    SUCCEED();
}
