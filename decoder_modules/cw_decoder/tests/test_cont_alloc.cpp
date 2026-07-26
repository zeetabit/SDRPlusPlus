#include <catch.hpp>
#include <cw/channel.h>
#include "cw_test_signals.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>

// D (RT-safety) evidence probe. DR-5's blocker was that the continuous re-decode
// ALLOCATED every 0.5 s in process() (2x makeFresh + event/mark vectors + candidate
// strings). D reworked it to pre-allocated, reset()-per-cycle scratch. This test
// MEASURES that the per-cycle re-decode is allocation-free rather than asserting it
// from code reading (Principle 1: evidence, not assumptions).
//
// Method: append trailing SILENCE to a decoded signal. Silence produces no new key
// events, so the window content is frozen, but reDecodeBuffer still fires every 0.5 s
// (paramsReady + period). Idle cycles therefore isolate PURELY cycle-driven allocation
// from event/growth-driven allocation. Before D each idle cycle allocated ~30 objects;
// after D it must allocate ~0. Cross-checked against fb+sel (no re-decode), whose idle
// delta is the alloc-free reference.

namespace {
    std::atomic<bool> g_armed{false};
    std::atomic<long> g_allocs{0};
    inline void countAlloc() {
        if (g_armed.load(std::memory_order_relaxed)) {
            g_allocs.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Global allocator replacement — a correct malloc/free passthrough that only counts
// while armed, so every other test in the binary is byte-for-byte unaffected.
void* operator new(std::size_t n) { countAlloc(); void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { countAlloc(); void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace cw_test;

namespace {
    // Allocations counted over the trailing no-signal tail of `sig` (with `silenceSec`
    // of extra NOISE-ONLY samples appended at the channel's noise floor), after a
    // warmup that lets all capacity growth settle. Noise-only (not pure zeros) keeps
    // the detector's online-EM well-conditioned, so the tail represents a real "no
    // signal, still keying up the decoder" gap rather than a degenerate all-zero input.
    long idleAllocs(const char* core, GeneratedSignal sig, float toneFreq,
                    float silenceSec, float noiseAmp) {
        const int rate = (int)8000.0f;   // signal sample rate (SignalParams::sampleRate)
        const int pad = (int)(rate * silenceSec);
        std::mt19937 rng(777);
        std::normal_distribution<float> nd(0.0f, noiseAmp);
        for (int i = 0; i < pad; i++) {
            sig.samples.push_back(dsp::complex_t{nd(rng), nd(rng)});
        }

        cw::Channel ch; ch.init(0, toneFreq, core); ch.wordCorrection = false;
        const int total = (int)sig.samples.size();
        const int active = total - pad;   // boundary: signal ends, silence begins

        int off = 0;
        // Warmup through ALL of the active signal + the first 1 s of silence so every
        // buffer has reached steady-state capacity before measuring.
        const int warmEnd = std::min(total, active + rate);
        for (; off < warmEnd; off += 512) {
            ch.process(std::min(512, total - off), &sig.samples[off]);
        }
        g_allocs.store(0, std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
        for (; off < total; off += 512) {
            ch.process(std::min(512, total - off), &sig.samples[off]);
        }
        g_armed.store(false, std::memory_order_relaxed);
        return g_allocs.load(std::memory_order_relaxed);
    }
}

TEST_CASE("ContAlloc: idle re-decode cycles are allocation-free", "[cw][.][cont-alloc]") {
    SignalParams p = profileModerateNoise(80.0f);
    p.seed = 1000;
    auto sig = generateMessage(MSG_FULL(), p);

    // 6 s of trailing noise-only tail == ~12 re-decode cycles with frozen content.
    const long baseIdle = idleAllocs("legacy+fb+sel",      sig, p.toneFreq, 6.0f, p.noiseAmp);
    const long contIdle = idleAllocs("legacy+fb+sel+cont", sig, p.toneFreq, 6.0f, p.noiseAmp);

    printf("[cont-alloc] idle-region allocations over 6 s noise tail: "
           "fb+sel=%ld  fb+sel+cont=%ld  (cont-base=%ld)\n",
           baseIdle, contIdle, contIdle - baseIdle);

    // fb+sel has no re-decode; its idle allocation is the alloc-free REFERENCE for the
    // shared streaming path (front end + detector + sink). The continuous core adds a
    // full re-detect + two decodes every 0.5 s on top of that. If that re-decode still
    // called makeFresh/vectors/strings per cycle, contIdle would exceed baseIdle by
    // ~30 x the ~12 idle cycles (hundreds). D makes it reuse pre-allocated scratch, so
    // cont must add only incidental growth over the reference. This is an invariant,
    // not a tuned threshold: a reintroduced per-cycle allocation blows it into the
    // hundreds and fails loudly.
    REQUIRE(contIdle - baseIdle <= 8);
}
