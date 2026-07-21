#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"
#include "cw_bench_stats.h"
#include "cw_parallel.h"
#include "cw_snr.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace cw_test;

// ============================================================
// Real-recording gate — ARRL W1AW code practice (docs §18)
//
// Every other test in this suite scores the decoder against signals this
// repository generated, so it can only confirm the decoder agrees with the
// generator's assumptions (§15). These are real transmissions published with
// their exact text, so they are the only ground truth here that the decoder's
// own model did not produce.
//
// Audio is not committed — it is ARRL's and the text derives from QST. Run
// tests/fetch_recordings.sh to populate the cache.
//
// This test is opt-in ([.]) so the default suite stays offline, and it FAILS
// rather than skips when the cache is missing. The version of this file that
// preceded it returned early on a missing recording, before its only REQUIRE,
// so it would have passed silently had it ever been in the build — which it
// never was (§17.5).
// ============================================================

namespace {

    struct WavData {
        int sampleRate = 0;
        std::vector<float> samples;
    };

    WavData loadWav(const std::string& path) {
        WavData wav;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { return wav; }

        char riff[4]; f.read(riff, 4);
        if (std::memcmp(riff, "RIFF", 4) != 0) { return wav; }
        uint32_t fileSize; f.read((char*)&fileSize, 4);
        char wave[4]; f.read(wave, 4);
        if (std::memcmp(wave, "WAVE", 4) != 0) { return wav; }

        uint16_t channels = 0, bits = 0;
        uint32_t rate = 0;
        while (f.good()) {
            char id[4]; f.read(id, 4);
            if (!f.good()) { break; }
            uint32_t size; f.read((char*)&size, 4);
            if (!f.good()) { break; }

            if (!std::memcmp(id, "fmt ", 4)) {
                uint16_t fmt; f.read((char*)&fmt, 2);
                f.read((char*)&channels, 2);
                f.read((char*)&rate, 4);
                uint32_t byteRate; f.read((char*)&byteRate, 4);
                uint16_t align; f.read((char*)&align, 2);
                f.read((char*)&bits, 2);
                if (size > 16) { f.seekg(size - 16, std::ios::cur); }
            } else if (!std::memcmp(id, "data", 4)) {
                if (bits != 16 || channels < 1) { return wav; }
                const int total = size / (bits / 8);
                wav.samples.reserve(total / channels);
                for (int i = 0; i < total; i++) {
                    int16_t s; f.read((char*)&s, 2);
                    if (i % channels == 0) { wav.samples.push_back(s / 32768.0f); }
                }
                wav.sampleRate = rate;
                break;
            } else {
                f.seekg(size, std::ios::cur);
            }
        }
        return wav;
    }

    // Real audio carries the CW tone on a real-valued signal. Handing the
    // samples over as the I component with Q at zero puts the wanted component
    // at DC once the channel translates by -toneFreq, and leaves the image at
    // -2*toneFreq where the complex low-pass rejects it.
    //
    // The previous version of this file instead computed audio(t)*exp(j*w*t),
    // which places the signal at w±tone; translating by -w returns it to
    // ±tone, outside the ±100 Hz filter for ANY choice of w. Measured against
    // a real recording it decodes noise: SNR 3.0 and "T ET T E ET E EE"
    // where this conversion reaches CER 0.007.
    std::vector<dsp::complex_t> audioToIQ(const WavData& audio) {
        std::vector<dsp::complex_t> iq(audio.samples.size());
        for (size_t i = 0; i < audio.samples.size(); i++) {
            iq[i].re = audio.samples[i];
            iq[i].im = 0.0f;
        }
        return iq;
    }

    // ARRL text files are CRLF with a trailing DOS EOF, and mark prosigns with
    // bare '<' and '>'. The control characters are file artifacts that were
    // never keyed. The angle brackets were: which prosign each denotes is not
    // stated in the files, so rather than guess a mapping to '+'/'*' they are
    // dropped from the reference. A decoder that emits a prosign there scores
    // an insertion, which is the conservative direction.
    std::string cleanReference(const std::string& raw) {
        std::string out;
        for (char c : raw) {
            if (c == '\r' || c == '\x1a' || c == '<' || c == '>') { continue; }
            out += c;
        }
        return out;
    }

    std::string recordingsDir() {
        if (const char* env = std::getenv("CW_RECORDINGS_DIR")) { return env; }
        return std::string(CW_TESTS_DIR) + "/recordings";
    }

    struct RecordingResult {
        DecodeScore score;
        float wpm = 0;
        float snr = 0;
        int decodedChars = 0;
        float durationSec = 0;
    };

    struct LoadedSession {
        std::vector<dsp::complex_t> iq;
        std::string reference;
        float durationSec = 0;
    };

    // Loads once so the core sweep does not re-read a 15 MB WAV per core.
    LoadedSession loadSession(const std::string& stem) {
        const std::string dir = recordingsDir();
        WavData audio = loadWav(dir + "/" + stem + ".wav");
        std::ifstream tf(dir + "/" + stem + ".txt");
        std::string truth((std::istreambuf_iterator<char>(tf)),
                           std::istreambuf_iterator<char>());

        INFO("Recording cache: " << dir);
        INFO("Run tests/fetch_recordings.sh to populate it.");
        REQUIRE_FALSE(audio.samples.empty());
        REQUIRE_FALSE(truth.empty());
        REQUIRE(audio.sampleRate == 8000);

        LoadedSession s;
        s.durationSec = (float)audio.samples.size() / audio.sampleRate;
        s.reference = cleanReference(truth);
        s.iq.resize(audio.samples.size());
        for (size_t i = 0; i < audio.samples.size(); i++) {
            s.iq[i].re = audio.samples[i];
            s.iq[i].im = 0.0f;
        }
        return s;
    }

    DecodeScore scoreWithCore(const LoadedSession& s, const std::string& coreName,
                              float toneFreq) {
        cw::Channel ch;
        ch.init(0, toneFreq, coreName);
        // init() falls back to DEFAULT_CORE for an unknown name rather than
        // failing, so an unchecked sweep would silently measure legacy.
        REQUIRE(ch.coreName() == coreName);
        for (int off = 0; off < (int)s.iq.size(); off += 512) {
            int n = std::min(512, (int)s.iq.size() - off);
            ch.process(n, &s.iq[off]);
        }
        return score(s.reference, ch.text.getText());
    }

    RecordingResult decodeRecording(const std::string& stem, float toneFreq) {
        const std::string dir = recordingsDir();
        const std::string wavPath = dir + "/" + stem + ".wav";
        const std::string txtPath = dir + "/" + stem + ".txt";

        WavData audio = loadWav(wavPath);
        std::ifstream tf(txtPath);
        std::string truth((std::istreambuf_iterator<char>(tf)),
                           std::istreambuf_iterator<char>());

        INFO("Recording cache: " << dir);
        INFO("Run tests/fetch_recordings.sh to populate it.");
        REQUIRE_FALSE(audio.samples.empty());
        REQUIRE_FALSE(truth.empty());
        REQUIRE(audio.sampleRate == 8000);

        auto iq = audioToIQ(audio);

        cw::Channel ch;
        ch.init(0, toneFreq);
        for (int off = 0; off < (int)iq.size(); off += 512) {
            int n = std::min(512, (int)iq.size() - off);
            ch.process(n, &iq[off]);
        }

        RecordingResult r;
        const std::string decoded = ch.text.getText();
        r.score = score(cleanReference(truth), decoded);
        r.wpm = ch.wpm;
        r.snr = ch.snr;
        r.decodedChars = (int)decoded.size();
        r.durationSec = (float)audio.samples.size() / audio.sampleRate;
        return r;
    }

    struct RecordingCase {
        const char* stem;
        const char* label;
        float maxCer;
        float maxWer;
        float wpmLow, wpmHigh;   // expected reported speed, not the label speed
    };

    // Thresholds are ratchets measured on the pinned sessions in
    // fetch_recordings.sh. Lower them when a change earns it; never raise one
    // to admit a change.
    //
    // Measured 2026-07-20: 0.0087/0.0044/0.0000/0.0000/0.0089 CER. The 15 and
    // 20 WPM sessions decode a real 6-to-7 minute transmission with zero
    // character errors, so their gate is exact equality — anything above zero
    // is a regression, not noise. Decoding a fixed WAV is deterministic.
    //
    // The 5 and 10 WPM sessions report 12.0 and 13.2 WPM, far above their
    // label, and carry the highest error of the five; the wpm bounds gate that
    // measured behaviour. WHY their character speed exceeds the label is not
    // established: Farnsworth keying would explain it, but the gap ratios in
    // these files have not been measured (docs §18.6.1, ⊘ open).
    const RecordingCase RECORDINGS[] = {
        {"w1aw_5wpm",  "5 WPM (char ~12)",  0.010f, 0.070f, 10.0f, 15.0f},
        {"w1aw_10wpm", "10 WPM (char ~13)", 0.006f, 0.050f, 11.0f, 16.0f},
        {"w1aw_15wpm", "15 WPM",            0.0f,   0.0f,   13.0f, 16.0f},
        {"w1aw_20wpm", "20 WPM",            0.0f,   0.0f,   17.0f, 21.0f},
        {"w1aw_35wpm", "35 WPM",            0.011f, 0.060f, 31.0f, 36.0f},
    };
}

TEST_CASE("Recording: ARRL W1AW code practice", "[cw][.][recording]") {
    // 750 Hz on every pinned session, measured by Goertzel sweep. Hardcoded
    // rather than scanned so a tone-tracking regression shows up as a decode
    // failure here instead of being silently absorbed.
    constexpr float TONE = 750.0f;

    printf("\n=== ARRL W1AW code practice (real audio, published text) ===\n");
    printf("%-22s %7s %7s %7s %7s %8s %8s\n",
           "session", "audio", "chars", "CER", "WER", "wpm", "snr");

    for (const auto& rc : RECORDINGS) {
        RecordingResult r = decodeRecording(rc.stem, TONE);

        printf("%-22s %6.0fs %7d %7.4f %7.4f %8.1f %8.1f\n",
               rc.label, r.durationSec, r.decodedChars,
               r.score.cer, r.score.wer, r.wpm, r.snr);

        INFO("session: " << rc.label << " CER=" << r.score.cer
             << " WER=" << r.score.wer << " wpm=" << r.wpm);

        // A zero gate means the session currently decodes exactly; <= keeps it
        // satisfiable while still failing on the first error introduced.
        CHECK(r.score.cer <= rc.maxCer);
        CHECK(r.score.wer <= rc.maxWer);
        CHECK(r.wpm > rc.wpmLow);
        CHECK(r.wpm < rc.wpmHigh);
        // A collapsed decode can still score well against a short reference if
        // it emits almost nothing; require the output length to be plausible.
        CHECK(r.decodedChars > r.score.refChars * 0.9f);
    }
    printf("\n");
}

TEST_CASE("Recording: punctuation appears in real decoded text", "[cw][.][recording]") {
    // Period and comma were unmapped until 2026-07-20 and no synthetic message
    // contains either, so this is the only test that would notice them being
    // dropped again. ARRL sessions average ~20 of each.
    const std::string dir = recordingsDir();
    WavData audio = loadWav(dir + "/w1aw_20wpm.wav");
    INFO("Run tests/fetch_recordings.sh to populate " << dir);
    REQUIRE_FALSE(audio.samples.empty());

    auto iq = audioToIQ(audio);
    cw::Channel ch;
    ch.init(0, 750.0f);
    for (int off = 0; off < (int)iq.size(); off += 512) {
        int n = std::min(512, (int)iq.size() - off);
        ch.process(n, &iq[off]);
    }

    const std::string decoded = ch.text.getText();
    const int periods = (int)std::count(decoded.begin(), decoded.end(), '.');
    const int commas  = (int)std::count(decoded.begin(), decoded.end(), ',');
    INFO("decoded periods=" << periods << " commas=" << commas);
    CHECK(periods >= 5);
    CHECK(commas >= 5);
}

// Every registry core was judged on synthetic signals (§12-§13). This is the
// first time any of them meets real keying, and the specific question is
// whether legacy+mf reaches the 35 WPM dah-split of §18.6.2 — a defect whose
// mechanism matches the matched-filter resize dropout that +mf repairs, and
// which §12.3 refuted using profiles that contain no such case.
TEST_CASE("Recording: core sweep on real audio", "[cw][.][recording-matrix]") {
    constexpr float TONE = 750.0f;

    std::vector<LoadedSession> sessions;
    std::vector<std::string> labels;
    for (const auto& rc : RECORDINGS) {
        sessions.push_back(loadSession(rc.stem));
        labels.push_back(rc.label);
    }

    printf("\n=== Registry cores vs ARRL real audio (CER) ===\n");
    printf("%-20s", "core");
    for (const auto& l : labels) { printf("%10s", l.c_str()); }
    printf("%10s\n", "mean");

    std::string bestName;
    float bestMean = 1e9f;
    float legacyMean = -1.0f;
    float peakdual16Mean = -1.0f;
    int cores = 0;

    for (const auto& spec : cw::coreRegistry()) {
        printf("%-20s", spec.name.c_str());
        float sum = 0;
        for (size_t i = 0; i < sessions.size(); i++) {
            DecodeScore s = scoreWithCore(sessions[i], spec.name, TONE);
            printf("%10.4f", s.cer);
            sum += s.cer;
        }
        const float mean = sum / sessions.size();
        printf("%10.4f\n", mean);

        if (spec.name == "legacy") { legacyMean = mean; }
        if (spec.name == "legacy+peakdual16") { peakdual16Mean = mean; }
        if (mean < bestMean) { bestMean = mean; bestName = spec.name; }
        cores++;
    }

    printf("\nbest: %s (mean CER %.4f), legacy %.4f\n\n",
           bestName.c_str(), bestMean, legacyMean);

    // `bestMean <= legacyMean` would be tautological — best is a minimum over a
    // set containing legacy. Gate the properties that can actually break: every
    // registry core ran (none silently fell back, which scoreWithCore also
    // guards), legacy reproduces the [recording] gate's mean, and the variants
    // measured as catastrophic on slow real audio stay that way rather than
    // quietly becoming the default.
    CHECK(cores == (int)cw::coreRegistry().size());
    CHECK(legacyMean == Approx(0.0044f).margin(0.0005f));
    CHECK(peakdual16Mean > legacyMean * 10.0f);
}

// ============================================================
// Noise-augmented real audio (docs §20)
//
// Every pinned session is 19-76 dB SNR, and every candidate core that wins on
// real audio loses under synthetic noise (§19). So the one regime that decides
// which core to promote — real keying under heavy noise — has never been
// measured: the recordings supply the keying, the synthetic profiles supply
// the noise, and nothing supplies both. This adds controlled AWGN to the real
// recordings and adjudicates the same way as test_promotion.cpp.
// ============================================================

namespace {

    // The wanted tone's peak amplitude sets what a given noiseAmp means. A WAV
    // is scaled arbitrarily while the synthetic generator keys at amplitude
    // 1.0, so without this a noiseAmp of 2.0 is a different SNR on every
    // recording and not comparable to the synthetic axis. Peak-normalizing to
    // 1.0 makes the two noise scales the same scale.
    void normalizeToPeak(std::vector<dsp::complex_t>& iq) {
        float peak = 0.0f;
        for (const auto& s : iq) {
            const float mag = std::sqrt(s.re * s.re + s.im * s.im);
            if (mag > peak) { peak = mag; }
        }
        if (peak <= 0.0f) { return; }
        const float g = 1.0f / peak;
        for (auto& s : iq) { s.re *= g; s.im *= g; }
    }

    // Same additive complex-Gaussian model the generator uses
    // (cw_test_signals.h): re += amp*N(0,1), im += amp*N(0,1).
    std::vector<dsp::complex_t> withNoise(const std::vector<dsp::complex_t>& iq,
                                          float amp, unsigned seed) {
        std::vector<dsp::complex_t> out = iq;
        if (amp <= 0.0f) { return out; }
        std::mt19937 rng(seed);
        std::normal_distribution<float> n(0.0f, 1.0f);
        for (auto& s : out) {
            s.re += amp * n(rng);
            s.im += amp * n(rng);
        }
        return out;
    }

    float decodeNoisyCER(const std::vector<dsp::complex_t>& iq,
                         const std::string& reference,
                         const std::string& coreName, float toneFreq) {
        cw::Channel ch;
        ch.init(0, toneFreq, coreName);
        for (int off = 0; off < (int)iq.size(); off += 512) {
            int n = std::min(512, (int)iq.size() - off);
            ch.process(n, &iq[off]);
        }
        return score(reference, ch.text.getText()).cer;
    }

    // Per-core CER over nSeeds independent noise realizations of one session.
    std::vector<float> coreCERsOverSeeds(const LoadedSession& s, float amp,
                                         const std::string& coreName,
                                         float toneFreq, int nSeeds) {
        std::vector<float> cers(nSeeds);
        parallelFor(nSeeds, [&](int i) {
            const auto noisy = withNoise(s.iq, amp, 4242u + (unsigned)i * 7919u);
            cers[i] = decodeNoisyCER(noisy, s.reference, coreName, toneFreq);
        });
        return cers;
    }
}

TEST_CASE("Recording: candidates under added noise", "[cw][.][recording-noise]") {
    constexpr float TONE = 750.0f;
    constexpr int SEEDS = 24;
    constexpr float TOL = 0.005f;
    const float noises[] = {0.0f, 1.0f, 2.0f, 3.0f};

    // 20 WPM is the cleanest session (CER 0.0000) and the most punctuation-rich
    // real text, so added noise is the only degradation and the reference is
    // the hardest available. One session keeps a full core x noise x seed sweep
    // affordable; the finding is the noise response, not cross-session spread.
    LoadedSession session = loadSession("w1aw_20wpm");
    normalizeToPeak(session.iq);

    // legacy+bpfauto is the WPM-locked noise-aware BPF promotion candidate (§30);
    // legacy+edge+log is the known-bad canary (emits CER > 1.0 at heavy synthetic
    // noise, §25) that proves the harness can register a real regression here.
    // bpfauto is the runtime candidate; bpf20/bpf30 are FIXED narrow filters
    // (narrow from the first sample, no lock dependency) — the decisive diagnostic
    // for whether narrowing helps real audio at all, or only bpfauto's trigger is
    // broken. edge+log is the known-bad canary.
    const char* candidates[] = {"legacy+bpfauto", "legacy+bpf20", "legacy+bpf30", "legacy+edge+log"};

    // The recordings are peak-normalized to 1.0, the same scale the synthetic
    // axis uses, so the dB label is comparable to [bpf-snr-sweep]. Caveat: the
    // WAV is a REAL tone (im = 0), so its key-down power is ~A²/2, not the
    // synthetic complex tone's A² — the real SNR is ~3 dB below this label. It is
    // the synthetic-EQUIVALENT SNR, exact for cross-referencing the synthetic
    // gate, approximate as an absolute figure for real audio.
    auto dbLabel = [](float amp) {
        return amp <= 0 ? 99.0f : noiseAmpToSnrDb(amp, REF_BW_SSB, 1.0f, 8000.0f);
    };

    // [candidate][noiseAmp] -> paired delta vs legacy, for the assertions below.
    std::map<std::string, std::map<float, PairedDelta>> res;
    for (float amp : noises) {
        auto baseCERs = coreCERsOverSeeds(session, amp, "legacy", TONE, SEEDS);
        const auto baseStats = summarize(baseCERs, 0.0f);

        printf("\n=== w1aw_20wpm + noiseAmp %.1f (~%+.1f dB/2500Hz synth-eq, paired, n=%d) ===\n",
               amp, dbLabel(amp), SEEDS);
        printf("legacy CER %.4f (sd %.4f)\n", baseStats.mean, baseStats.stddev);
        printf("%-18s %9s %10s %10s %7s %10s  %s\n",
               "candidate", "cand", "delta", "stderr", "t", "worstcase", "verdict");

        for (const char* cand : candidates) {
            auto candCERs = coreCERsOverSeeds(session, amp, cand, TONE, SEEDS);
            auto d = comparePaired(baseCERs, candCERs);
            const auto cs = summarize(candCERs, 0.0f);
            const bool harm = d.harmful(TOL);
            printf("%-18s %9.4f %+10.4f %10.4f %7.2f %+10.4f  %s%s\n",
                   cand, cs.mean, d.meanDelta, d.stderrDelta, d.t,
                   d.worstCaseDelta(), d.verdict(), harm ? " HARM" : "");
            CHECK(d.nSeeds == SEEDS);
            res[cand][amp] = d;
        }
    }

    // Instrument self-checks: the noise axis must actually degrade, or the test
    // proves nothing. Clean is exact; heavy noise must move legacy off zero.
    auto clean = coreCERsOverSeeds(session, 0.0f, "legacy", TONE, SEEDS);
    auto heavy = coreCERsOverSeeds(session, 3.0f, "legacy", TONE, SEEDS);
    CHECK(summarize(clean, 0.0f).mean == Approx(0.0f).margin(1e-6));
    CHECK(summarize(heavy, 0.0f).mean > 0.05f);

    // Findings on real audio (docs §32), asserted so they cannot silently rot:
    //
    // 1. The thesis TRANSFERS: a fixed narrow BPF is a large, real win on real
    //    keying at moderate noise — bpf20 cuts noiseAmp-1.0 CER ~6x. Narrowing
    //    the pre-detection bandwidth helps real audio, not only synthetic.
    CHECK(res["legacy+bpf20"][1.0f].significant());
    CHECK(res["legacy+bpf20"][1.0f].meanDelta < -0.05f);
    // 2. But a FIXED narrow filter is not the answer: at the heaviest noise it
    //    over-narrows the 20 WPM keying and emits garbage (CER > 1.0), worse than
    //    legacy — so the bandwidth genuinely must adapt.
    CHECK(res["legacy+bpf20"][3.0f].meanDelta > 0.0f);
    // 3. The runtime rule bpfauto FAILS to capture the win: its getSNR trigger,
    //    calibrated on synthetic 15 WPM, does not fire on real 20 WPM audio
    //    (post-BPF getSNR reads too high), so at noiseAmp 1.0 — where bpf20 wins
    //    6x — bpfauto is identical to legacy. This is the promotion blocker.
    CHECK_FALSE(res["legacy+bpfauto"][1.0f].significant());
    // 4. The canary can register a real regression: edge+log emits CER > 1.0 at
    //    heavy synthetic-equivalent noise on real audio too.
    CHECK(res["legacy+edge+log"][2.0f].significant());
    CHECK(res["legacy+edge+log"][2.0f].meanDelta > 0.0f);

    printf("\n  Finding (§32): narrowing helps real audio (bpf20 -6x at moderate "
           "noise), but bpfauto's getSNR trigger does not fire on real audio — "
           "promotion blocked, trigger needs redesign.\n");
}
