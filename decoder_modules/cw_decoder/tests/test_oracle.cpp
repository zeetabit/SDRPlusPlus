#include <catch.hpp>
#include "cw_oracle.h"
#include "cw_matrix.h"
#include <cstdio>

// Oracle ablation — how much CER is each stage costing?
//
// A 2×2 rather than a cumulative chain, so the interaction is visible: a stage
// can look harmless alone and be the whole problem once the one upstream of it
// is fixed.
//
// Tagged [.] so it stays out of the default run, like the matrix sweeps.

using namespace cw_test;

namespace {

    struct OProfile {
        const char* name;
        const char* message;
        SignalParams params;
    };

    std::vector<OProfile> oracleProfiles() {
        std::vector<OProfile> v;
        v.push_back({"clean-15wpm",     MSG_FULL(), profileClean(80.0f)});
        v.push_back({"handkeyed-15wpm", MSG_FULL(), profileHandKeyed(80.0f)});
        v.push_back({"handkeyed-20wpm", MSG_FULL(), profileHandKeyed(60.0f)});
        v.push_back({"handkeyed-25wpm", MSG_FULL(), profileHandKeyed(48.0f)});
        v.push_back({"qsb",             MSG_FULL(), profileQSB(80.0f)});
        v.push_back({"qrn",             MSG_FULL(), profileQRN(80.0f)});
        v.push_back({"farnsworth-2.0",  MSG_FULL(), profileFarnsworth(80.0f, 2.0f)});
        v.push_back({"worstcase",       MSG_FULL(), profileWorstCase(80.0f)});
        for (float amp : {2.0f, 3.0f, 4.0f}) {
            OProfile p;
            p.name = amp == 2.0f ? "snr-noise2.0" :
                     amp == 3.0f ? "snr-noise3.0" : "snr-noise4.0";
            p.message = MSG_FULL();
            p.params = profileClean(80.0f);
            p.params.noiseAmp = amp;
            v.push_back(p);
        }
        return v;
    }

    CoreFactory oracleFactory(SignalParams params, OracleConfig cfg) {
        return [params, cfg](const GeneratedSignal& sig) {
            return makeOracleCore(sig, params, cfg);
        };
    }

}

TEST_CASE("oracle ablation: stage headroom", "[cw][.][oracle]") {
    constexpr int SEEDS = 24;

    struct Config { const char* label; OracleConfig cfg; };
    const Config configs[] = {
        {"baseline",  {false, false}},
        {"+det",      {true,  false}},
        {"+tim",      {false, true }},
        {"+det+tim",  {true,  true }},
    };

    printf("\n%-18s %10s %10s %10s %10s   %s\n",
           "profile", "baseline", "+det", "+tim", "+det+tim", "headroom(det)");
    printf("%s\n", std::string(84, '-').c_str());

    for (const auto& prof : oracleProfiles()) {
        float cer[4];
        float ins[4], del[4];
        for (int i = 0; i < 4; i++) {
            auto c = runCellWith(oracleFactory(prof.params, configs[i].cfg),
                                 configs[i].label, prof.name, prof.message,
                                 prof.params, SEEDS);
            cer[i] = c.cerMean;
            ins[i] = c.insRate;
            del[i] = c.delRate;
        }

        printf("%-18s %10.4f %10.4f %10.4f %10.4f   %+.4f\n",
               prof.name, cer[0], cer[1], cer[2], cer[3], cer[0] - cer[1]);
        printf("%-18s %10s ins=%.3f del=%.3f  ->  ins=%.3f del=%.3f\n",
               "", "", ins[0], del[0], ins[1], del[1]);

        // The oracle is an upper bound on what fixing that stage can buy.
        // Nothing is asserted about its magnitude — this test reports.
        CHECK(cer[0] >= 0.0f);
    }
    printf("\n");
}

// The oracle detector must reproduce the generator exactly, or every number
// above is measuring the harness rather than the decoder.
TEST_CASE("oracle detector emits exactly the generated key transitions", "[cw][oracle-self]") {
    SignalParams p = profileClean(80.0f);
    p.seed = 7;
    auto sig = generateMessage("PARIS", p);

    auto trans = truthTransitions(sig, p.sampleRate, 1000.0f);

    // "PARIS" = .--. .- .-. .. ... = 4+2+3+2+3 = 14 elements -> 28 transitions
    REQUIRE(trans.size() == 28);

    for (size_t i = 0; i < trans.size(); i++) {
        REQUIRE(trans[i].keyDown == (i % 2 == 0));
        if (i > 0) { REQUIRE(trans[i].sample >= trans[i - 1].sample); }
    }

    OracleDetector det(trans);
    det.init(1000.0f);
    det.reset();

    std::vector<float> dummy(64, 0.0f);
    int emitted = 0;
    long long totalBlocks = (sig.samples.size() / 8) / 64 + 2;
    for (long long b = 0; b < totalBlocks; b++) {
        auto evts = det.process(dummy.data(), 64);
        for (auto& e : evts) {
            REQUIRE(e.sampleOffset >= 0);
            REQUIRE(e.sampleOffset < 64);
            REQUIRE(e.keyDown == (emitted % 2 == 0));
            emitted++;
        }
    }
    REQUIRE(emitted == (int)trans.size());
}

// The recorded element model must match what the generator keys, or the timing
// oracle is measuring its own error. Weight bias and Farnsworth both move these
// off the nominal 1:3 and 1:3:7 ratios.
TEST_CASE("ground-truth element model matches the generator", "[cw][oracle-self]") {
    SECTION("weight bias shifts dit and dah") {
        SignalParams p = profileHandKeyed(80.0f);   // weightBias = 0.1
        auto sig = generateMessage("E", p);
        REQUIRE(sig.model.nominalDitMs == Approx(80.0f));
        REQUIRE(sig.model.ditMs == Approx(80.0f * 1.1f));
        REQUIRE(sig.model.dahMs == Approx(240.0f * 0.95f));
        // The boundary must sit strictly between them, unlike 2×nominal.
        float b = std::sqrt(sig.model.ditMs * sig.model.dahMs);
        REQUIRE(b > sig.model.ditMs);
        REQUIRE(b < sig.model.dahMs);
    }

    SECTION("Farnsworth stretches gaps but not elements") {
        SignalParams p = profileFarnsworth(80.0f, 2.0f);
        auto sig = generateMessage("E", p);
        REQUIRE(sig.model.ditMs == Approx(80.0f));
        REQUIRE(sig.model.elemGapMs == Approx(80.0f));
        REQUIRE(sig.model.charGapMs == Approx(480.0f));
        REQUIRE(sig.model.wordGapMs == Approx(1120.0f));
    }
}

// A clean signal with both oracles must decode perfectly. If this fails the
// oracle plumbing is wrong and no headroom number can be trusted.
TEST_CASE("oracle sanity: clean signal decodes exactly", "[cw][oracle-self]") {
    SignalParams p = profileClean(80.0f);
    p.seed = 11;

    auto r = runInstrumentedCore(MSG_CQ(), p,
                                 oracleFactory(p, OracleConfig{true, true}),
                                 "+det+tim");
    auto s = score(MSG_CQ(), r.text);
    INFO("decoded: '" << r.text << "'");
    CHECK(s.cer == 0.0f);
}
