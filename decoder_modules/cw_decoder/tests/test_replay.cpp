// Replay harness: run the REAL ChannelManager over a recorded baseband IQ WAV.
//
// The offline decode tests feed a single core synthetic/AF audio. This drives
// the full front end — ToneScanner detection + channel spawn/prune + decode —
// over a real SDR++ baseband recording, so a signal the operator copies by ear
// but the decoder misses becomes a reproducible, instrumented measurement.
//
// Reproduces the module front end (src/main.cpp): shift the VFO offset to DC,
// then decimate to the 8 kHz / 3 kHz VFO the manager runs at.
//
//   CW_REPLAY_WAV=/path/to/baseband_<centerHz>_...wav \
//   CW_REPLAY_OFFSET_HZ=36545 \            # VFO - recordingCenter (default 36545)
//   ./cw_decoder_tests "[replay]"
//
// Run ONLY by the explicit [replay] tag — never [.] / [cw] wildcards.

#include <catch.hpp>
#include <cw/channel_manager.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <cmath>

namespace {

    struct IQ {
        std::vector<dsp::complex_t> samples;
        double sampleRate = 0;
    };

    // Streamed WAV read of a 2-channel Int16 baseband file, applying the VFO
    // frequency shift during the read (offset -> DC) to avoid a second pass over
    // ~73M samples.
    IQ loadShiftedIQ(const std::string& path, double offsetHz) {
        IQ iq;
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) { return iq; }

        char riff[4]; f.read(riff, 4);
        uint32_t riffSize; f.read((char*)&riffSize, 4);
        char wave[4]; f.read(wave, 4);
        if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) { return iq; }

        uint16_t channels = 0, bits = 0; uint32_t rate = 0; uint32_t dataSize = 0;
        std::streampos dataPos = -1;
        while (f && dataPos < 0) {
            char id[4]; f.read(id, 4);
            uint32_t sz; f.read((char*)&sz, 4);
            if (!f) { break; }
            if (std::memcmp(id, "fmt ", 4) == 0) {
                uint16_t fmt; f.read((char*)&fmt, 2);
                f.read((char*)&channels, 2);
                f.read((char*)&rate, 4);
                uint32_t byteRate; f.read((char*)&byteRate, 4);
                uint16_t blockAlign; f.read((char*)&blockAlign, 2);
                f.read((char*)&bits, 2);
                f.seekg(sz - 16, std::ios::cur);
            }
            else if (std::memcmp(id, "data", 4) == 0) {
                dataSize = sz;
                dataPos = f.tellg();
            }
            else {
                f.seekg(sz, std::ios::cur);
            }
        }
        if (dataPos < 0 || channels != 2 || bits != 16) { return iq; }

        iq.sampleRate = rate;
        f.clear();
        f.seekg(dataPos);

        const uint64_t nFrames = dataSize / 4;   // 2ch * 2 bytes
        iq.samples.reserve(nFrames);

        const double w = -2.0 * M_PI * offsetHz / (double)rate;
        double phase = 0.0;

        const size_t CHUNK = 1 << 20;            // frames per read
        std::vector<int16_t> buf(CHUNK * 2);
        uint64_t done = 0;
        while (done < nFrames) {
            size_t want = std::min<uint64_t>(CHUNK, nFrames - done);
            f.read((char*)buf.data(), want * 4);
            std::streamsize got = f.gcount() / 4;
            if (got <= 0) { break; }
            for (std::streamsize i = 0; i < got; i++) {
                float re = buf[i * 2] / 32768.0f;
                float im = buf[i * 2 + 1] / 32768.0f;
                float c = cosf((float)phase), s = sinf((float)phase);
                iq.samples.push_back({ re * c - im * s, re * s + im * c });
                phase += w;
                if (phase < -M_PI) { phase += 2.0 * M_PI; }
                if (phase > M_PI) { phase -= 2.0 * M_PI; }
            }
            done += got;
        }
        return iq;
    }

    // Blackman-windowed-sinc low-pass, sized from the transition width.
    std::vector<float> designLPF(double cutoffHz, double stopHz, double fs) {
        double fc = 0.5 * (cutoffHz + stopHz) / fs;      // normalised -6 dB point
        int L = (int)std::ceil(3.3 * fs / (stopHz - cutoffHz));
        if (L % 2 == 0) { L++; }
        if (L < 11) { L = 11; }
        std::vector<float> h(L);
        int c = (L - 1) / 2;
        double sum = 0.0;
        for (int n = 0; n < L; n++) {
            int k = n - c;
            double sinc = (k == 0) ? 2.0 * fc : sin(2.0 * M_PI * fc * k) / (M_PI * k);
            double win = 0.42 - 0.5 * cos(2.0 * M_PI * n / (L - 1)) + 0.08 * cos(4.0 * M_PI * n / (L - 1));
            h[n] = (float)(sinc * win);
            sum += h[n];
        }
        for (auto& v : h) { v /= (float)sum; }
        return h;
    }

    std::vector<dsp::complex_t> decimate(const std::vector<dsp::complex_t>& in, int M,
                                         const std::vector<float>& h) {
        const int L = (int)h.size();
        const long N = (long)in.size();
        std::vector<dsp::complex_t> out(N / M);
        for (size_t m = 0; m < out.size(); m++) {
            long base = (long)m * M - (L - 1);
            float ar = 0, ai = 0;
            for (int k = 0; k < L; k++) {
                long idx = base + k;
                if (idx >= 0 && idx < N) { ar += h[k] * in[idx].re; ai += h[k] * in[idx].im; }
            }
            out[m] = { ar, ai };
        }
        return out;
    }

    double envd(const char* k, double def) {
        const char* v = std::getenv(k);
        return v ? atof(v) : def;
    }

    // Load the recording and reproduce the module front end: shift VFO offset to
    // DC, decimate 2.4 MHz -> 8 kHz. Returns the 8 kHz IQ the manager runs on.
    std::vector<dsp::complex_t> loadDecimated8k(const char* wav, double offset, double& fsOut) {
        IQ iq = loadShiftedIQ(wav, offset);
        if (iq.sampleRate <= 0) { fsOut = 0; return {}; }
        auto s1 = decimate(iq.samples, 10, designLPF(4000, 236000, iq.sampleRate));
        iq.samples.clear(); iq.samples.shrink_to_fit();
        auto s2 = decimate(s1, 6, designLPF(4000, 36000, iq.sampleRate / 10));
        s1.clear(); s1.shrink_to_fit();
        auto s3 = decimate(s2, 5, designLPF(3600, 4000, iq.sampleRate / 60));
        fsOut = iq.sampleRate / 300.0;
        return s3;
    }

    // E-flood proxy: single-element chars (E/T) are valid CW, but a long run of
    // them between real words is the noise-admission signature. Report the count
    // and the longest consecutive run — no ground truth needed to see garbage.
    void garbageStats(const std::string& t, int& single, int& longestRun, int& total) {
        single = 0; longestRun = 0; total = 0; int run = 0;
        for (char c : t) {
            if (c == ' ') { continue; }
            total++;
            if (c == 'E' || c == 'T') { single++; run++; if (run > longestRun) { longestRun = run; } }
            else { run = 0; }
        }
    }

    void dumpState(cw::ChannelManager& mgr, double tSec) {
        printf("  t=%5.1fs | ", tSec);
        for (auto& e : mgr.entries) {
            std::string txt = e.channel->text.getText();
            if (txt.size() > 14) { txt = txt.substr(txt.size() - 14); }
            printf("[%.0fHz snr%.1f wpm%.0f fit%.2f \"%s\"] ",
                   e.channel->toneFreq, e.channel->snr, e.channel->wpm,
                   e.channel->modelFit, txt.c_str());
        }
        printf("\n");
    }

} // namespace

TEST_CASE("Replay: real baseband recording through ChannelManager", "[cw][.][replay]") {
    const char* wav = std::getenv("CW_REPLAY_WAV");
    if (!wav) { WARN("set CW_REPLAY_WAV to run"); return; }

    double offset = envd("CW_REPLAY_OFFSET_HZ", 36545.0);
    printf("\n=== Replay %s  (VFO offset %.0f Hz -> DC) ===\n", wav, offset);

    double fs = 0;
    auto s3 = loadDecimated8k(wav, offset, fs);
    REQUIRE(fs == Approx(8000.0));
    printf("  decimated to %.2f ks @ %.0f Hz\n\n", s3.size() / 1e3, fs);

    cw::ChannelManager mgr;
    mgr.init((float)fs);
    mgr.autoDetect = true;
    mgr.scanThreshold = (float)envd("CW_REPLAY_THRESH", 3.0);   // matches UI "Auto Detect 3 dB"
    mgr.maxChannels = (int)envd("CW_REPLAY_CHANNELS", 4);
    mgr.modelFitGate = true;

    const int BLK = 1024;
    double nextDump = 0.0;
    for (size_t p = 0; p + BLK <= s3.size(); p += BLK) {
        mgr.process(&s3[p], BLK);
        mgr.updateChannels();
        double tSec = (double)(p + BLK) / fs;
        if (tSec >= nextDump) { dumpState(mgr, tSec); nextDump += 2.0; }
    }

    printf("\n=== FINAL channels ===\n");
    for (auto& e : mgr.entries) {
        printf("  %+.0f Hz  snr=%.1f wpm=%.0f fit=%.2f  \"%s\"\n",
               e.channel->toneFreq, e.channel->snr, e.channel->wpm, e.channel->modelFit,
               e.channel->text.getText().c_str());
    }
    printf("\n");
}

// Head-to-head: run several cores on the SAME extracted real signal (one pinned
// channel per core at CW_REPLAY_TONE Hz). No reliable ground truth on this
// recording, so compare on the E-flood garbage proxy + eyeball the text.
TEST_CASE("Replay: core A/B on the extracted real signal", "[cw][.][replay-cores]") {
    const char* wav = std::getenv("CW_REPLAY_WAV");
    if (!wav) { WARN("set CW_REPLAY_WAV to run"); return; }
    double offset = envd("CW_REPLAY_OFFSET_HZ", 36545.0);
    float tone = (float)envd("CW_REPLAY_TONE", 1000.0);

    double fs = 0;
    auto s3 = loadDecimated8k(wav, offset, fs);
    REQUIRE(fs == Approx(8000.0));
    printf("\n=== Core A/B on tone %+.0f Hz  (%s) ===\n\n", tone, wav);

    const char* cores[] = { "legacy", "legacy+select", "legacy+fb",
                            "legacy+fb+sel", "legacy+fb+sel+cont", "legacy+route" };
    printf("  %-18s | conf | mfit | sngl/tot | run | text\n", "core");
    printf("  -------------------+------+------+----------+-----+-----\n");
    for (const char* core : cores) {
        cw::Channel ch;
        ch.init(0, tone, core);
        if (ch.coreName() != core) { printf("  %-18s | (unavailable)\n", core); continue; }
        for (int off = 0; off + 1 <= (int)s3.size(); off += 512) {
            int n = std::min(512, (int)s3.size() - off);
            ch.process(n, &s3[off]);
        }
        std::string txt = ch.text.getText();
        int single, run, total;
        garbageStats(txt, single, run, total);
        // conf = decode confidence, mfit = envelope model-fit — candidate router
        // correctness discriminators: does the RIGHT core report higher?
        printf("  %-18s | %.2f | %.2f | %4d/%-3d | %3d | \"%s\"\n",
               core, ch.confidence, ch.modelFit, single, total, run, txt.c_str());
    }
    printf("\n  sngl/tot = single-element (E/T) chars over total; run = longest E/T run.\n"
           "  A cleaner decoder shows a SHORTER longest-run at similar length.\n\n");
}

// Tune/bandwidth sweep: the operator copied this best in USB at 1 kHz BW, and
// the pinned decode is offset-fragile — so sweep the channel tone (and compare
// fb narrow vs wide) to test whether the decoder is simply mistuned / too narrow
// for a drifting signal. Scored against the operator's (uncertain) copy.
TEST_CASE("Replay: tone + bandwidth sweep on the real signal", "[cw][.][replay-sweep]") {
    const char* wav = std::getenv("CW_REPLAY_WAV");
    if (!wav) { WARN("set CW_REPLAY_WAV to run"); return; }
    double offset = envd("CW_REPLAY_OFFSET_HZ", 36545.0);
    double fs = 0;
    auto s3 = loadDecimated8k(wav, offset, fs);
    REQUIRE(fs == Approx(8000.0));

    const std::string GT = "DCOMEOUTONETIMETEMLADT16C=SOEALQQX";
    auto lev = [](const std::string& a, const std::string& b) {
        std::string x, y;
        for (char c : a) { if (c != ' ') x += c; }
        for (char c : b) { if (c != ' ') y += c; }
        int m = x.size(), n = y.size();
        std::vector<int> d(n + 1);
        for (int j = 0; j <= n; j++) { d[j] = j; }
        for (int i = 1; i <= m; i++) {
            int prev = d[0]; d[0] = i;
            for (int j = 1; j <= n; j++) {
                int cur = d[j];
                d[j] = std::min({ d[j] + 1, d[j - 1] + 1, prev + (x[i-1] != y[j-1]) });
                prev = cur;
            }
        }
        return d[n];
    };
    auto anchors = [](const std::string& t) {
        std::string v; for (char c : t) { if (c != ' ') v += c; }
        std::string h;
        for (const char* a : { "OME", "ONE", "TIO", "TIM", "OUT", "16C" }) {
            if (v.find(a) != std::string::npos) { h += a; h += " "; }
        }
        return h.empty() ? std::string("-") : h;
    };

    float lo = (float)envd("CW_SWEEP_LO", 880);
    float hi = (float)envd("CW_SWEEP_HI", 1120);
    float step = (float)envd("CW_SWEEP_STEP", 20);
    const char* only = std::getenv("CW_SWEEP_CORE");
    const char* allCores[] = { "legacy+fb", "legacy+fb+wide", "legacy+select" };
    std::vector<const char*> cores;
    if (only) { cores.push_back(only); } else { for (auto c : allCores) { cores.push_back(c); } }
    for (const char* core : cores) {
        printf("\n=== %s : tone sweep ===\n  tone | CER  | anchors | text\n  -----+------+---------+----\n", core);
        int bestD = 1e9; float bestT = 0;
        for (float tone = lo; tone <= hi + 0.5f; tone += step) {
            cw::Channel ch;
            ch.init(0, tone, core);
            if (ch.coreName() != core) { printf("  (unavailable)\n"); break; }
            for (int off = 0; off + 1 <= (int)s3.size(); off += 512) {
                int n = std::min(512, (int)s3.size() - off);
                ch.process(n, &s3[off]);
            }
            std::string txt = ch.text.getText();
            int d = lev(GT, txt);
            float cer = (float)d / 34.0f;
            if (d < bestD) { bestD = d; bestT = tone; }
            printf("  %4.0f | %.2f | %-8s| \"%s\"\n", tone, cer, anchors(txt).c_str(), txt.c_str());
        }
        printf("  best CER %.2f at tone %.0f Hz\n", bestD / 34.0f, bestT);
    }
    printf("\n");
}

// Why did the router pick select on the real signal? Dump the arbitration terms.
#include <cw/regime_route.h>
TEST_CASE("Replay: router arbitration on the real signal", "[cw][.][route-why]") {
    const char* wav = std::getenv("CW_REPLAY_WAV");
    if (!wav) { WARN("set CW_REPLAY_WAV to run"); return; }
    double offset = envd("CW_REPLAY_OFFSET_HZ", 36545.0);
    float tone = (float)envd("CW_REPLAY_TONE", 1000.0);
    double fs = 0;
    auto s3 = loadDecimated8k(wav, offset, fs);
    REQUIRE(fs == Approx(8000.0));

    const char* rcore = std::getenv("CW_ROUTE_CORE"); if (!rcore) rcore = "legacy+route";
    cw::RegimeRouteCore::debug = true;
    printf("\n=== router arbitration (%s) @ %+.0f Hz  (gate: fbGood>=3, selRatio<0.6, "
           "fbRatio>selRatio+0.25, fbContrast<5.5, inputSnr<8) ===\n", rcore, tone);
    cw::Channel ch;
    ch.init(0, tone, rcore);
    for (int off = 0; off + 1 <= (int)s3.size(); off += 512) {
        int n = std::min(512, (int)s3.size() - off);
        ch.process(n, &s3[off]);
    }
    cw::RegimeRouteCore::debug = false;
    printf("  route text: \"%s\"\n\n", ch.text.getText().c_str());
}

// Characterize the fb fragmentation to a MECHANISM: run the real front end + fb
// detector directly and dump the mark/space DURATION stream (1 kHz internal ->
// 1 sample = 1 ms). A healthy dah is one contiguous ~3-dit mark; a SPLIT dah is
// mark / short-gap / mark. Seeing the durations tells split-vs-merge, and where.
#include <cw/stages.h>
TEST_CASE("Replay: fb element/gap durations on the real signal", "[cw][.][replay-events]") {
    const char* wav = std::getenv("CW_REPLAY_WAV");
    if (!wav) { WARN("set CW_REPLAY_WAV to run"); return; }
    double offset = envd("CW_REPLAY_OFFSET_HZ", 36545.0);
    float tone = (float)envd("CW_REPLAY_TONE", 1000.0);
    double t0 = envd("CW_EV_T0", 0.0), t1 = envd("CW_EV_T1", 1e9);
    double fs = 0;
    auto s3 = loadDecimated8k(wav, offset, fs);
    REQUIRE(fs == Approx(8000.0));

    // Match the fb CORE's front end (core_registry makeFB: 140/140/88/100), NOT
    // the EnvelopeFrontEnd default — the earlier 100/100 mismatch gave contrast/
    // dip numbers that did not represent what the core's fb actually sees.
    cw::EnvelopeFrontEnd fe(140.0f, 140.0f, 88.0f, 100.0f);
    fe.init(tone, (float)fs, 1000.0f);
    cw::FBDetector det;
    det.init(1000.0f);

    std::vector<float> env(1024);
    std::vector<float> allEnv;                       // full envelope, for dip-depth
    std::vector<std::pair<bool,long long>> edges;   // (keyDown, absEnvSample)
    long long envAbs = 0;
    int snrBins[16] = {0}; int snrN = 0; double snrSum = 0;
    for (int off = 0; off + 1 <= (int)s3.size(); off += 512) {
        int n = std::min(512, (int)s3.size() - off);
        int m = fe.process(n, &s3[off], env.data());
        auto evs = det.process(env.data(), m);
        for (auto& e : evs) { edges.push_back({ e.keyDown, envAbs + e.sampleOffset }); }
        allEnv.insert(allEnv.end(), env.data(), env.data() + m);
        envAbs += m;
        float sn = det.getSNR();
        if (sn > 0) { snrBins[std::min(15, (int)sn)]++; snrN++; snrSum += sn; }
    }
    // CLOCK-RECOVERY test (synchronous-detection direction): can a dit-grid be
    // recovered from the FADING envelope? Two standard measures over the window
    // (and, to test QSB robustness, over its strong first third vs faded regions).
    auto clockProbe = [](const std::vector<float>& e, int lo, int hi, const char* tag) {
        const int N = hi - lo;
        if (N < 600) { return; }
        double mean = 0; for (int i = lo; i < hi; i++) { mean += e[i]; } mean /= N;
        // (1) Normalised autocorrelation of the mean-removed envelope, 0..280 ms.
        double r0 = 0; for (int i = lo; i < hi; i++) { double d = e[i]-mean; r0 += d*d; }
        r0 = std::max(r0, 1e-12);
        printf("  [%s] envelope autocorr R(tau)/R(0), tau=0..280ms (step 10):\n   ", tag);
        float acf[29];
        for (int L = 0; L <= 28; L++) {
            int lag = L*10; double s = 0;
            for (int i = lo; i < hi-lag; i++) { s += (double)(e[i]-mean)*(e[i+lag]-mean); }
            acf[L] = (float)(s / r0);
            printf("%+.2f ", acf[L]);
        }
        printf("\n");
        // first local MAX after the zero-lag lobe dips below 0.2 -> keying scale
        int firstMax = -1; bool dipped = false;
        for (int L = 1; L <= 27; L++) {
            if (acf[L] < 0.2f) dipped = true;
            if (dipped && acf[L] > acf[L-1] && acf[L] >= acf[L+1] && acf[L] > 0.1f) { firstMax = L*10; break; }
        }
        // (2) Grid-fold clock search: for trial dit T (+best phase), fraction of
        // dit-cells that are UNAMBIGUOUS (mean near floor or near peak). Peaks at
        // the true dit if the keying aligns to a recoverable grid.
        std::vector<float> se(e.begin()+lo, e.begin()+hi); std::sort(se.begin(), se.end());
        float fl = se[se.size()/20], pk = se[se.size()*19/20], sp = std::max(1e-6f, pk-fl);
        float bestT = 0, bestScore = 0;
        for (float T = 35; T <= 120; T += 1.0f) {
            float tBest = 0;
            for (int ph = 0; ph < (int)T; ph += std::max(1,(int)T/6)) {
                int nc = (N-ph)/(int)T, unamb = 0;
                for (int c = 0; c < nc; c++) {
                    int a = lo+ph+c*(int)T; double m=0; for (int k=0;k<(int)T;k++) m+=e[a+k]; m/=(int)T;
                    float nz=((float)m-fl)/sp; if (nz<0.30f||nz>0.70f) unamb++;
                }
                float s = nc? (float)unamb/nc : 0; if (s>tBest) tBest=s;
            }
            if (tBest > bestScore) { bestScore = tBest; bestT = T; }
        }
        printf("  [%s] autocorr firstPeak=%dms   grid-fold best dit=%.0fms (unamb %.2f)\n",
               tag, firstMax, bestT, bestScore);
    };
    clockProbe(allEnv, 0, (int)allEnv.size(), "ALL");
    clockProbe(allEnv, 0, (int)allEnv.size()/3, "1st-3rd");
    clockProbe(allEnv, (int)allEnv.size()*2/3, (int)allEnv.size(), "last-3rd");

    // Dip-depth test: for each GAP, the envelope minimum during it, normalised
    // between the global noise floor (5th pct) and mark peak (95th pct). A real
    // inter-element gap reaches the FLOOR (~0); a fade notch inside a mark dips
    // only PARTWAY (>0). If short gaps sit high and long gaps sit at floor, then
    // dip-depth discriminates splits from real gaps — a zero-regression fix.
    {
        std::vector<float> sortedEnv = allEnv;
        std::sort(sortedEnv.begin(), sortedEnv.end());
        float floorLvl = sortedEnv[sortedEnv.size()/20];
        float peakLvl  = sortedEnv[sortedEnv.size()*19/20];
        float span = std::max(1e-6f, peakLvl - floorLvl);
        // Bucket gap dip-depth by gap-duration class (short <40ms vs long >=40ms).
        int shortN = 0, longN = 0; double shortDepth = 0, longDepth = 0;
        int sBins[11] = {0}, lBins[11] = {0};
        for (size_t i = 1; i < edges.size(); i++) {
            if (edges[i-1].first) continue;          // want SPACE intervals
            long long a = edges[i-1].second, b = edges[i].second;
            if (a < 0 || b > (long long)allEnv.size()) continue;
            float mn = 1e30f;
            for (long long k = a; k < b; k++) { mn = std::min(mn, allEnv[k]); }
            float depth = (mn - floorLvl) / span;    // 0 = at floor, 1 = at peak
            int dur = (int)(b - a);
            int bin = std::max(0, std::min(10, (int)(depth * 10)));
            if (dur < 40) { shortN++; shortDepth += depth; sBins[bin]++; }
            else          { longN++;  longDepth  += depth; lBins[bin]++; }
        }
        printf("\n=== gap DIP-DEPTH (0=noise floor, 1=mark peak) ===\n");
        printf("  SHORT gaps (<40ms, n=%d) mean depth %.2f  hist[.0..1.0]:",
               shortN, shortN ? shortDepth/shortN : 0.0);
        for (int b = 0; b <= 10; b++) printf(" %d", sBins[b]);
        printf("\n  LONG  gaps (>=40ms, n=%d) mean depth %.2f  hist[.0..1.0]:",
               longN, longN ? longDepth/longN : 0.0);
        for (int b = 0; b <= 10; b++) printf(" %d", lBins[b]);
        printf("\n");
    }
    printf("\n=== fb keying-contrast SNR (dB): mean=%.2f  hist[1dB bins 0..11]:",
           snrN ? snrSum/snrN : 0.0);
    for (int b = 0; b < 12; b++) { printf(" %d:%d", b, snrBins[b]); }
    printf(" ===\n");

    // Durations between consecutive edges = element (mark) / gap (space) lengths.
    // At 1 kHz, sample count == ms. Estimate dit as the 20th-pct of mark lengths.
    // A key-DOWN edge opens a MARK interval; a key-UP edge opens a SPACE interval.
    std::vector<int> markDur;
    for (size_t i = 1; i < edges.size(); i++) {
        if (edges[i-1].first) markDur.push_back((int)(edges[i].second - edges[i-1].second));
    }
    int dit = 60;
    if (!markDur.empty()) {
        std::vector<int> s = markDur; std::sort(s.begin(), s.end());
        dit = std::max(20, s[s.size()/5]);            // 20th percentile mark ~ dit
    }
    // Duration histograms (20 ms bins) expose the populations: fragments vs dit
    // vs dah for marks; false-split vs element vs char vs word for gaps.
    auto hist = [](const char* lbl, std::vector<int>& v) {
        int bins[16] = {0};
        for (int d : v) { int b = std::min(15, d / 20); bins[b]++; }
        printf("  %s (20ms bins):", lbl);
        for (int b = 0; b < 12; b++) { printf(" %d:%d", b*20, bins[b]); }
        printf("\n");
    };
    std::vector<int> gapDur;
    for (size_t i = 1; i < edges.size(); i++) {
        if (!edges[i-1].first) gapDur.push_back((int)(edges[i].second - edges[i-1].second));
    }
    hist("MARK", markDur);
    hist("GAP ", gapDur);

    printf("\n=== fb events @ %+.0f Hz  dit~%dms  (M=mark S=space, [t0=%.1f t1=%.1f]) ===\n",
           tone, dit, t0, t1);
    auto cls = [&](bool mark, int d) -> const char* {
        if (mark) return d < 2*dit ? "." : "-";
        if (d < 2*dit) return "|";                    // element gap (~1 dit)
        if (d < 5*dit) return " / ";                  // char gap (~3 dit)
        return " // ";                                // word gap (~7 dit)
    };
    for (size_t i = 1; i < edges.size(); i++) {
        bool mark = edges[i-1].first;                 // interval prev..cur
        int d = (int)(edges[i].second - edges[i-1].second);
        double ts = edges[i-1].second / 1000.0;
        if (ts < t0 || ts > t1) continue;
        printf("  %6.2fs %s%-4d %s\n", ts, mark ? "M" : "S", d, cls(mark, d));
    }
    printf("\n");
}
