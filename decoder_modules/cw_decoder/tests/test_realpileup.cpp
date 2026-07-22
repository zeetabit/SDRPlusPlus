#include <catch.hpp>
#include <cw/channel.h>
#include <cw/core_registry.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ============================================================
// Real 40m CW pileup benchmark — TRUTHLESS (docs §52 step: real-data gate)
//
// Every synthetic benchmark can only confirm the decoder agrees with the
// generator's assumptions (§15). The W1AW gate ([recording]) adds real audio
// WITH published truth, but it is single-signal clean copy. This gate adds a
// real 2.4 MHz IQ capture of a 40m contest pileup (~20 CW signals, 28 dB down
// to 11 dB) with NO ground truth — the adversarial case the whole #42 line is
// meant for.
//
// No truth => no CER. Instead: (1) the STRONG signals (>=25 dB) are a de-facto
// correctness anchor — a working decoder must produce clean plausible ham text
// there, and disagreement between cores flags a harness bug; (2) ham CW is
// self-checking (callsigns, CQ/DE/TU/73/5NN), so token PLAUSIBILITY is a
// reference-free quality proxy; (3) the metric that ranks cores is plausible-
// token YIELD down the SNR gradient — the core that keeps extracting real ham
// content at 12 dB while the other emits garbage is better.
//
// Fixtures are produced by scratchpad/extract.py from the raw IQ (not committed,
// too large). Point CW_PILEUP_DIR at the fixtures dir; the test FAILS (not
// skips) if it is set but unreadable, and is opt-in [.] so the default suite
// stays offline.
// ============================================================

using namespace cw;

namespace {

    std::vector<dsp::complex_t> loadIqWav(const std::string& path, int& rate) {
        std::vector<dsp::complex_t> iq;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { return iq; }
        char id[4]; uint32_t sz;
        f.read(id, 4); if (std::memcmp(id, "RIFF", 4)) { return iq; }
        f.read((char*)&sz, 4); f.read(id, 4); if (std::memcmp(id, "WAVE", 4)) { return iq; }
        uint16_t channels = 0, bits = 0; uint32_t r = 0;
        while (f.good()) {
            f.read(id, 4); if (!f.good()) { break; }
            f.read((char*)&sz, 4); if (!f.good()) { break; }
            if (!std::memcmp(id, "fmt ", 4)) {
                uint16_t fmt, align; uint32_t byteRate;
                f.read((char*)&fmt, 2); f.read((char*)&channels, 2);
                f.read((char*)&r, 4); f.read((char*)&byteRate, 4);
                f.read((char*)&align, 2); f.read((char*)&bits, 2);
                if (sz > 16) { f.seekg(sz - 16, std::ios::cur); }
            } else if (!std::memcmp(id, "data", 4)) {
                if (bits != 16 || channels != 2) { return iq; }
                const int n = sz / 4;                // 2ch * 2 bytes
                iq.resize(n);
                for (int i = 0; i < n; i++) {
                    int16_t I, Q; f.read((char*)&I, 2); f.read((char*)&Q, 2);
                    iq[i].re = I / 32768.0f; iq[i].im = Q / 32768.0f;
                }
                rate = r; break;
            } else { f.seekg(sz, std::ios::cur); }
        }
        return iq;
    }

    struct Fixture { std::string name; long rf; float snr; float pitch; };

    std::vector<Fixture> loadManifest(const std::string& dir) {
        std::vector<Fixture> out;
        std::ifstream f(dir + "/manifest.txt");
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line); Fixture fx; long ns;
            if (ss >> fx.name >> fx.rf >> fx.snr >> fx.pitch >> ns) { out.push_back(fx); }
        }
        return out;
    }

    // Reference-free ham-CW plausibility: fraction of tokens that look like real
    // on-air content. A token counts if it is a callsign, a common Q/abbr code, or
    // a numeric (RST/serial). Deliberately permissive on the "known good" side so
    // the rate tracks copy quality, not vocabulary coverage.
    bool isCallsign(const std::string& t) {
        if (t.size() < 3 || t.size() > 7) { return false; }
        for (char c : t) { if (!std::isalnum((unsigned char)c)) { return false; } }
        int digits = 0, letters = 0;
        for (char c : t) { if (std::isdigit((unsigned char)c)) { digits++; } else { letters++; } }
        // callsigns carry >=1 digit and >=1 letter and a digit not at both ends only
        bool hasMidDigit = false;
        for (size_t i = 1; i + 1 < t.size(); i++) { if (std::isdigit((unsigned char)t[i])) { hasMidDigit = true; } }
        return digits >= 1 && letters >= 2 && (hasMidDigit || std::isdigit((unsigned char)t[0]));
    }
    bool isCode(const std::string& t) {
        static const char* codes[] = {"CQ","DE","TU","TNX","TKS","GM","GA","GE","GN","UR","RST",
            "599","5NN","57N","73","88","K","KN","AR","SK","BK","BT","PSE","QRZ","QSL","QTH","QRL",
            "QSB","QRM","QRN","QSY","NAME","OP","RIG","ANT","PWR","WX","HR","HW","GL","DX","TEST",
            "R","RR","FB","OM","YL","ES","WPM","AGN","CFM","DR","GUD","NW","NR","CL","EE"};
        for (auto c : codes) { if (t == c) { return true; } }
        return false;
    }
    bool isNumeric(const std::string& t) {
        if (t.empty()) { return false; }
        for (char c : t) { if (!std::isdigit((unsigned char)c)) { return false; } }
        return true;
    }
    // Distinct callsigns decoded >=2x. A near-truth correctness signal: token
    // plausibility can be inflated by a chatty decoder, but the SAME callsign
    // appearing repeatedly is extremely unlikely from noise — in a QSO/pileup a
    // real station sends its call several times, so this counts genuine copy.
    int repeatedCallsigns(const std::string& text) {
        std::map<std::string,int> freq;
        std::istringstream ss(text); std::string tok;
        while (ss >> tok) {
            std::string u; for (char c : tok) { u += (char)std::toupper((unsigned char)c); }
            if (isCallsign(u)) { freq[u]++; }
        }
        int rep = 0; for (auto& kv : freq) { if (kv.second >= 2) { rep++; } }
        return rep;
    }
    struct Plaus { int chars = 0; int tokens = 0; int good = 0; int rpt = 0; };
    Plaus plausibility(const std::string& text) {
        Plaus p; p.chars = (int)text.size();
        std::istringstream ss(text); std::string tok;
        while (ss >> tok) {
            std::string u; for (char c : tok) { u += (char)std::toupper((unsigned char)c); }
            p.tokens++;
            if (isCallsign(u) || isCode(u) || isNumeric(u)) { p.good++; }
        }
        p.rpt = repeatedCallsigns(text);
        return p;
    }

    std::string pileupDir() {
        const char* e = std::getenv("CW_PILEUP_DIR");
        return e ? std::string(e) : std::string();
    }

    std::string decode(const std::vector<dsp::complex_t>& iq, float pitch, const std::string& core) {
        Channel ch; ch.init(0, pitch, core);
        REQUIRE(ch.coreName() == core);            // init falls back to DEFAULT on unknown
        for (int off = 0; off < (int)iq.size(); off += 512) {
            int n = std::min(512, (int)iq.size() - off);
            ch.process(n, &iq[off]);
        }
        return ch.text.getText();
    }

}

TEST_CASE("real 40m pileup — truthless core benchmark", "[cw][.][realpileup]") {
    const std::string dir = pileupDir();
    if (dir.empty()) { WARN("set CW_PILEUP_DIR to the extract.py fixtures dir"); return; }
    auto fixtures = loadManifest(dir);
    REQUIRE_FALSE(fixtures.empty());

    const std::vector<std::string> cores = {"legacy", "legacy+select", "legacy+mf", "legacy+bpfauto+ab"};

    printf("\n=== real 40m pileup — plausible-token yield (truthless) ===\n");
    printf("%-22s %5s", "fixture", "dB");
    for (auto& c : cores) { printf(" | %-16s", c.c_str()); }
    printf("\n%-22s %5s", "", "");
    for (size_t i = 0; i < cores.size(); i++) { printf(" | %4s %5s %3s", "good", "rate", "rpt"); }
    printf("\n");

    std::vector<int> sumGood(cores.size(), 0), sumRpt(cores.size(), 0);
    for (auto& fx : fixtures) {
        int rate = 0;
        auto iq = loadIqWav(dir + "/" + fx.name, rate);
        if (iq.empty()) { WARN("unreadable fixture: " << fx.name); continue; }
        printf("%-22s %5.0f", fx.name.c_str(), fx.snr);
        for (size_t ci = 0; ci < cores.size(); ci++) {
            Plaus p = plausibility(decode(iq, fx.pitch, cores[ci]));
            float goodRate = p.tokens ? (float)p.good / p.tokens : 0.0f;
            printf(" | %4d %5.2f %3d", p.good, goodRate, p.rpt);
            sumGood[ci] += p.good; sumRpt[ci] += p.rpt;
        }
        printf("\n");
    }
    printf("%-22s %5s", "TOTAL", "");
    for (size_t ci = 0; ci < cores.size(); ci++) { printf(" | %4d %5s %3d", sumGood[ci], "", sumRpt[ci]); }
    printf("\n\nrpt = distinct callsigns copied >=2x (near-truth). good = plausible tokens. Rank by rpt then good.\n");
}

TEST_CASE("real pileup — strong-signal decoded text (eyeball anchor)", "[cw][.][realpileup-text]") {
    const std::string dir = pileupDir();
    if (dir.empty()) { WARN("set CW_PILEUP_DIR to the extract.py fixtures dir"); return; }
    auto fixtures = loadManifest(dir);
    REQUIRE_FALSE(fixtures.empty());
    printf("\n=== strong-signal decoded text (legacy vs default) ===\n");
    int shown = 0;
    for (auto& fx : fixtures) {
        if (fx.snr < 18.0f || shown >= 4) { continue; }
        shown++;
        int rate = 0;
        auto iq = loadIqWav(dir + "/" + fx.name, rate);
        if (iq.empty()) { continue; }
        printf("\n[%s  %.0fdB]\n", fx.name.c_str(), fx.snr);
        printf("  legacy: %s\n", decode(iq, fx.pitch, "legacy").c_str());
        printf("  select: %s\n", decode(iq, fx.pitch, "legacy+select").c_str());
    }
    printf("\n");
}
