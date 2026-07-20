# CW Decoder Module — Architecture

## Overview

Multi-channel CW (Morse code) decoder for SDR++. Captures a 3 kHz VFO bandwidth at 8 kHz sample rate and runs up to 10 parallel decode channels, each tuned to a different tone frequency. Supports automatic tone detection via FFT scanning and manual channel pinning.

V2 module API. Single-file main.cpp delegates all logic to the `cw/` component library.

**Decoding is pluggable.** A `Channel` hosts one `IDecodeCore` selected by name from
a registry; post-processing (spelling correction, conversation tracking, text
buffering) lives in `Channel` and is identical for every core. See
[Pluggable Decode Cores](#pluggable-decode-cores) below — that section is the
entry point for anyone changing decoding behaviour. The Stage 1–8 pipeline
documented further down describes the **`legacy` core specifically**, which
remains the production default.

## Top-Level Data Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            SDR++ Runtime                                    │
│                                                                             │
│  Source IQ (e.g. RTL-SDR @ 2.4 MHz, 2.4 MSPS)                             │
│      │                                                                      │
│      ▼                                                                      │
│  VFO (3 kHz BW, 8 kHz output rate)                                         │
│      │                                                                      │
│      ▼                                                                      │
│  Handler<complex_t> sink ─── callback on DSP thread                         │
└──────┬──────────────────────────────────────────────────────────────────────┘
       │
       │ complex_t[], count        DSP THREAD
       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                        ChannelManager.process()                              │
│                                                                              │
│  ┌──────────────┐   Tone discovery (parallel to channels)                    │
│  │ ToneScanner   │   1024-pt FFT → spectral avg → peak detect               │
│  │ (FFT-based)   │   Output: vector<DetectedTone> {freq, power}              │
│  └──────┬────────┘                                                           │
│         │ detected tones buffered for UI thread                              │
│         │                                                                    │
│  ┌──────┴───────────────────────────────────────────────────────────────┐    │
│  │               Per-Channel Processing (0..N-1)                        │    │
│  │                                                                      │    │
│  │  for each active Channel:                                            │    │
│  │      channel.process(count, data)                                    │    │
│  │                                                                      │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
       │
       │ UI THREAD (menuHandler, every frame)
       ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                      ChannelManager.updateChannels()                          │
│                                                                              │
│  • Promote confirmed tones → addChannel()                                    │
│  • Remove idle auto-channels (40 frames / ~5s inactivity)                    │
│  • Pinned channels survive idle timeout                                      │
│  • Push markers to waterfall VFO overlay                                     │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Pluggable Decode Cores

Two levels, because two kinds of decoder must coexist.

```
Channel  (owns post-processing, implements CharSink)
    │
    ├── IDecodeCore ─── IQ in, characters out. The outer contract.
    │     │
    │     ├── StagedCore ─── composes independently swappable stages
    │     │      IFrontEnd       EnvelopeFrontEnd (filter geometry parameterized)
    │     │      IDetector       SchmittDetector
    │     │      ITiming         AdaptiveTimingStage (7 strategies)
    │     │      ISymbolDecoder  BeamSymbolDecoder
    │     │      + inline sequencing: pre-lock capture, retroDecode, flush, freeze
    │     │
    │     ├── BellCore   (future) ─── implements IDecodeCore DIRECTLY
    │     └── MillsCore  (future)
    │
    └── corrector + conversation + TextBuffer   (shared by ALL cores)
```

**Why Bell/Mills-style decoders cannot be stages.** Bell 1977 is "a set of linear
Kalman filters operating on a dynamically evolving trellis" — detection, timing
and symbol decoding are one inseparable computation. Routing it through
`IDetector`→`ITiming`→`ISymbolDecoder` would destroy the joint estimation that
makes it work. Such cores implement `IDecodeCore` directly and sit beside
`StagedCore` as peers. `IDecodeCore::process()` therefore takes **raw IQ**, not
an envelope.

**Why sequencing is inline rather than a fifth stage.** A jointly-estimating core
replaces that logic wholesale, so there is no second implementation to validate
an `ISequencer` interface against. Extract it when one exists.

**Why post-processing sits outside every core.** A core reaches decoded text only
through `CharSink` (`emitChar` / `flushWord` / `emitWordGap` / `clearEmitted`).
Correction and conversation tracking are therefore identical across cores, so the
benchmark matrix measures *decoding*, not post-processing.

**Why front-end bandwidth is a declared core parameter.** Pre-detection bandwidth
alone moves CER by up to 38× (`decoder-investigation-2026-07.md` §4). If cores
could quietly narrow their own filter, one could "win" the benchmark without
decoding better. Declaring it keeps the comparison honest.

### Registry

`core_registry.h` is the single list of benchmarkable configurations. The config
UI lists it; the benchmark matrix iterates it. Adding a core or a stage
combination is one entry. Names are persisted in module config — keep them
stable. Unknown names fall back to `legacy` rather than failing.

| name | configuration | status |
|---|---|---|
| `legacy` | Schmitt + Kalman V1 + beam search | **production default** |
| `legacy+kmeans` | K-means timing | worse on jitter and QRM/QRN |
| `legacy+median` | median-split timing | worst overall |
| `legacy+bimodal` | bimodal-histogram timing | best on handkeyed-25; poor at high noise |
| `legacy+kalman2` | corrected dah gain, confidence-gated learning | better on 10/13, blocked on noise4.0 |
| `legacy+log` | log-duration Kalman (multiplicative jitter) | best on 5, blocked on noise3.0/4.0 |
| `legacy+logrobust` | log timing + Huberised state update | ≈ `+log`, no blocker relief |
| `legacy+mf` | matched filter preserved across resize | 17× fewer spurious events, CER unchanged, 3 profiles worse |
| `legacy+mf+log` | preserved matched filter + log timing | combination baseline |
| `legacy+sym` | symmetric Schmitt thresholds (0.45/0.45) | refuted: barely moves the bias, hysteresis loss is catastrophic |
| `legacy+edge` | release edge corrected for modelled stretch | better on 6, over-corrects at low SNR |
| `legacy+edge+log` | edge correction + log timing | comparison baseline |
| `legacy+edge+mf` | edge correction + preserved matched filter | comparison baseline |
| `legacy+peak` | threshold referenced to windowed 90th percentile | **best handkeyed-15 measured** (0.1579 → 0.0628); handkeyed-25 0.0898; `qsb` regresses 43× |
| `legacy+peak+edge` | percentile reference + edge correction | combination baseline |
| `legacy+peakdual` | dual-window peak reference, 500 ms short | **best spread measured** — 7 profiles better, `qsb` and `qrn` worse |
| `legacy+peakdual16` | dual-window, 250 ms short + persistence | handkeyed-15 0.0640 (2nd best; `+peak` is 0.0628); `qsb`/noise3/4 worse |
| `legacy+peakgate` | instant attack, gated on confirmed key-down | 1 better / 8 worse. Refuted its own premise: ON stretch identical to `legacy`, misses 45× on noise2.0 (§13.8) |
| `legacy+bpf40` | 40/50 BPF, 64 Hz ENBW | breaks clean-25 WPM |
| `legacy+bpf30` | 30/40 BPF, 48 Hz ENBW | best on noise3.0; worse on hand-keyed |
| `legacy+bpf20` | 20/30 BPF, 31 Hz ENBW | |

**Promotion rule:** a variant becomes the default only when it is no worse on
*every* profile.

> ⚠ **The rule is known-broken in three ways, all unresolved.** It has no notion
> of *operating regime* (`+kalman2` is blocked by `noise4.0` 0.9032 vs 0.9888 —
> both total failure), no notion of *significance* (`+peakdual` is blocked partly
> by a difference of ~1 character across 24 seeds), and it is CER-based while CER
> weights insertions and deletions equally — by the skimmer criterion, where a
> false spot costs more than a missed one, `legacy+mf` is an improvement.
> Details: investigation §7, §13.6, §12.4. This is why 20 variants have produced
> zero promotions. "Better on average" is not sufficient — a profile that decoded
better before and worse after is a regression. Variants that lose are kept, not
deleted: they are the comparison baseline for future cores.

**What the registry deliberately excludes.** Oracle cores — a perfect detector
or a perfect timing model — live in `tests/cw_oracle.h`, never here. They need
ground truth only the signal generator has, so they cannot run on a real signal
and must not reach the config UI. `Channel::initWithCore()` exists to host a
core the registry cannot construct; `init(id, tone, coreName)` is a wrapper over
it.

## Per-Channel Processing Pipeline (the `legacy` core)

This section describes the **`legacy`** core — the production default. Other
registry variants differ only in the stage noted in the table above.

`Channel` owns the front-end-to-text chain via its core. All blocks run inline in
the DSP thread callback — no internal threads.

```
Channel.process(count, complex_t* iq)
    │
    │ complex_t[count] @ 8000 Hz
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 1: Envelope Extraction (EnvelopeDSP)                                  │
│                                                                              │
│  ┌─────────────────┐  complex_t[count] @ 8 kHz                              │
│  │  FreqXlator      │  VOLK rotator: shift tone frequency to DC             │
│  │  (volk_32fc_...   │  Phase-continuous across calls                        │
│  │   rotator)        │  Delta = exp(-j * 2π * toneFreq / sampleRate)        │
│  └────────┬──────────┘                                                       │
│           │ complex_t[count] @ 8 kHz (tone now at DC)                        │
│           ▼                                                                  │
│  ┌─────────────────┐                                                         │
│  │  Decimate 8:1    │  Average-and-decimate (box-car anti-alias)             │
│  │                  │  8000 Hz → 1000 Hz internal rate                       │
│  │                  │  count/8 output samples                                │
│  └────────┬─────────┘                                                        │
│           │ complex_t[count/8] @ 1000 Hz                                     │
│           ▼                                                                  │
│  ┌─────────────────┐                                                         │
│  │  Narrow BPF      │  Complex FIR lowpass: 100 Hz cutoff, 100 Hz trans      │
│  │  (VOLK dot_prod)  │  38 taps, 167.6 Hz ENBW (measured, not the 200 Hz     │
│  │                  │  nominal — the Nuttall window rolls off the corners)   │
│  │                  │  Group delay 18.5 ms                                   │
│  └────────┬─────────┘                                                        │
│           │ complex_t[count/8] @ 1000 Hz (narrowband)                        │
│           ▼                                                                  │
│  ┌─────────────────┐                                                         │
│  │  Magnitude       │  volk_32fc_magnitude_32f                               │
│  │                  │  complex → float: |I + jQ|                             │
│  └────────┬─────────┘                                                        │
│           │ float[count/8] @ 1000 Hz (raw envelope)                          │
│           ▼                                                                  │
│  ┌─────────────────┐                                                         │
│  │  Smoothing LPF   │  Float FIR lowpass: 80 Hz cutoff, 100 Hz trans        │
│  │                  │  Post-detection: narrowing this HURTS (measured) —     │
│  │  (VOLK dot_prod)  │  magnitude is nonlinear, so it cannot recover         │
│  │                  │  pre-detection SNR. Leave it alone.                    │
│  └────────┬─────────┘                                                        │
│           │ float[envCount] @ 1000 Hz (smooth envelope)                      │
│           ▼                                                                  │
│  Output: envBuf[envCount], where envCount = count / 8                        │
│  Group delay: MEASURED end-to-end 57–85 ms (investigation §12), not the      │
│    ~37 ms the component figures suggest. EXCEEDS one dit at 25 WPM (48 ms).  │
│    Previously documented as "~44ms total (< 40ms dit at 30 WPM)" — both      │
│    self-contradictory and about half the true value.                         │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ float[envCount] @ 1000 Hz
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 2: Matched Filter                                                     │
│                                                                              │
│  Moving-average ring buffer filter:                                          │
│    • Pre-lock:  window = 25 samples (25ms)                                   │
│    • Post-lock: window = 0.4 × ditSamples (adapts to WPM)                   │
│    • Ring buffer ZEROED on window size change — injects a dropout            │
│      mid-element. Measured defect, deliberately retained; see the             │
│      known-defects table and `legacy+mf`.                                     │
│    • Smooths keying transitions, suppresses impulse noise                    │
│                                                                              │
│  Output: mfBuf[envCount]                                                     │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ float[envCount] @ 1000 Hz (filtered envelope)
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 3: Tone Detection (ToneDetector)                                      │
│                                                                              │
│  Input:  float[envCount]                                                     │
│  Output: vector<KeyEvent> { keyDown: bool, sampleOffset: int }               │
│  Side:   updates noiseFloor, signalPeak, SNR                                │
│                                                                              │
│  ┌─ Noise Floor ────────────────────────────────────────────────────────┐    │
│  │  Subsampled 25th percentile:                                         │    │
│  │    • Every 8th sample → 250-element circular buffer                  │    │
│  │    • nth_element for O(1) amortized percentile                       │    │
│  │    • Robust to signal presence in window (only 25% threshold)        │    │
│  │    • Convergence guard: skip detection until 10+ subsamples          │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Peak Tracker ───────────────────────────────────────────────────────┐    │
│  │  Instant attack (v > peak → peak = v)                                │    │
│  │  Exponential decay: peak -= alpha * (peak - v), tau = 0.5s           │    │
│  │  Drives impulse blanker + getSNR. The THRESHOLD reference is          │    │
│  │  selectable (PeakTracker): instant / percentile / slow-attack /      │    │
│  │  dual-window. Instant attack chases the rising edge — measured        │    │
│  │  +9.8% ON stretch. See known defects and investigation §13.          │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Dynamic Range Gate ─────────────────────────────────────────────────┐    │
│  │  dynamicRange = signalPeak / noiseFloor                              │    │
│  │  If < 1.8 → skip detection (signal ≈ noise, no CW present)          │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  (EdgeBias selects raw / symmetric-thresholds / event-time correction)        │
│  ┌─ SNR-Adaptive Schmitt Trigger ───────────────────────────────────────┐    │
│  │  range = signalPeak - noiseFloor                                     │    │
│  │  High SNR (>10 dB):   onThresh = noise + 0.55 * range               │    │
│  │                        offThresh = noise + 0.35 * range              │    │
│  │  Medium (4-10 dB):    onThresh = noise + 0.60 * range               │    │
│  │                        offThresh = noise + 0.30 * range              │    │
│  │  Low (<4 dB):         onThresh = noise + 0.65 * range               │    │
│  │                        offThresh = noise + 0.25 * range              │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Adaptive Debounce ──────────────────────────────────────────────────┐    │
│  │  rawState must persist for debounceLen samples before emitting       │    │
│  │  debounceLen = max(5ms, 8% of estimated dit), capped at 25ms        │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Impulse Blanker (QRN) ───────────────────────────────────────────────┐    │
│  │  Activates after first key cycle (estimatedDitSamples > 0)           │    │
│  │  When key is UP and sample > signalPeak × 2.5:                       │    │
│  │    Hold previous value for up to debounce samples                    │    │
│  │  If sustained beyond debounce: real signal, stop blanking            │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Dit Duration Estimator ─────────────────────────────────────────────┐    │
│  │  Tracks ON durations in sliding window (20 elements)                 │    │
│  │  Lower-median: sort → pick element at (size/2)/2                     │    │
│  │  Feeds back to debounce and matched filter sizing                    │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ vector<KeyEvent> + SNR
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 4: Soft Squelch + Event Routing                                       │
│                                                                              │
│  sqFactor = clamp((snr - 3.0) / 7.0, 0, 1)                                 │
│    • SNR < 3 dB:   sqFactor = 0 → events suppressed                         │
│    • SNR 3-10 dB:  sqFactor = 0..1 → confidence scaled                      │
│    • SNR > 10 dB:  sqFactor = 1 → full confidence                           │
│                                                                              │
│  ┌─ Pre-Lock Path (timingWasLocked == false) ───────────────────────────┐    │
│  │  • Save event to preLockEvents[] buffer                              │    │
│  │  • Feed ON durations to timing.classifyOn() (seeds Kalman)           │    │
│  │  • Feed OFF durations to timing.classifyOff() (seeds gap model)      │    │
│  │  • Do NOT emit text (defer to retroDecode)                           │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Post-Lock Path (timingWasLocked == true) ───────────────────────────┐    │
│  │  • ON events → timing.classifyOn() → morseDecoder.addElement()       │    │
│  │  • OFF events → timing.classifyOff() → char/word gap emit            │    │
│  │  • Confidence scaled by sqFactor                                     │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ (timing locks after 7 elements)
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 5: Timing Lock + RetroDecoding                                        │
│                                                                              │
│  Trigger: timing.isLocked() == true (Kalman: seedCount + 4 = 7 elements)    │
│                                                                              │
│  ┌─ retroDecode() ──────────────────────────────────────────────────────┐    │
│  │                                                                      │    │
│  │  1. Get lockedDit from main timing (e.g. 80ms for 15 WPM)           │    │
│  │                                                                      │    │
│  │  2. Create retroTiming, seed with 8 × (dit, 3*dit) pairs            │    │
│  │                                                                      │    │
│  │  3. Replay preLockEvents[] through retroTiming + retroMorse:         │    │
│  │     ┌────────────────────────────────────────────────────────┐       │    │
│  │     │  for each SavedEvent {keyDown, timeMs}:                │       │    │
│  │     │    keyDown → classifyOff(gap) → char/word break?       │       │    │
│  │     │    keyUp   → classifyOn(elemMs) → addElement(dit/dah)  │       │    │
│  │     └────────────────────────────────────────────────────────┘       │    │
│  │                                                                      │    │
│  │  4. Emit completed characters to retroText                           │    │
│  │                                                                      │    │
│  │  5. Transfer state to main decoder:                                  │    │
│  │     • text = retroText (replaces any pre-lock text)                  │    │
│  │     • morseDecoder = retroMorse (preserves in-progress char)         │    │
│  │     • lastKeyDown/lastKeyUp persist for seamless gap computation     │    │
│  │                                                                      │    │
│  │  6. Clear preLockEvents[], envHistory[]                              │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  Pre-lock flush: if timing never locks but long silence detected             │
│  (> 4 × ditDuration), retroDecode fires with best-effort timing,            │
│  then morseDecoder.characterBreak() flushes the last character.              │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ Element events (DIT/DAH + confidence)
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 6: Adaptive Timing (AdaptiveTiming → KalmanTiming)                    │
│                                                                              │
│  Input:  ON duration (ms) or OFF duration (ms)                               │
│  Output: TimingEvent { element: DIT|DAH, confidence } or                     │
│          TimingEvent { gap: ELEMENT|CHAR|WORD, confidence }                  │
│                                                                              │
│  ┌─ Element Classification (classifyOn) ────────────────────────────────┐    │
│  │                                                                      │    │
│  │  Kalman Filter:                                                      │    │
│  │    State: ditEst (ms)         P: state covariance                    │    │
│  │    Q = ditEst² × 0.001       (process noise: slow drift)            │    │
│  │    R adapts from residuals    (measurement noise)                    │    │
│  │                                                                      │    │
│  │  Predict:  P += Q                                                    │    │
│  │  Classify: ditLik = N(d; ditEst, P+R) × 1.2  (dit prior)           │    │
│  │            dahLik = N(d; 3×ditEst, 9P+R)                            │    │
│  │            pick max → DIT or DAH                                     │    │
│  │  Update:   K = P / (P + R)                                          │    │
│  │            ditEst += K × (measurement - ditEst)                      │    │
│  │            P *= (1 - K)                                              │    │
│  │  Clamp:    ditEst to [34, 150] ms (8-35 WPM)                        │    │
│  │                                                                      │    │
│  │  Confidence = max(ditLik, dahLik) / (ditLik + dahLik)                │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Gap Classification (classifyOff) ───────────────────────────────────┐    │
│  │                                                                      │    │
│  │  Bayesian Gaussian Mixture:                                          │    │
│  │    elemMean = dit         elemSigma = max(0.5σ, 0.2×dit)            │    │
│  │    charMean = 3×dit       charSigma = max(1.2σ, 0.5×dit)            │    │
│  │    wordMean = 7×dit       wordSigma = max(2.0σ, 1.0×dit)            │    │
│  │                                                                      │    │
│  │    σ = jitter sigma estimated from dit-duration variance             │    │
│  │                                                                      │    │
│  │    P(type|d) ∝ N(d; mean, sigma) × prior                            │    │
│  │    Priors: element=5.0  char=2.0  word=0.5                           │    │
│  │    Pick argmax → ELEMENT_GAP, CHAR_GAP, or WORD_GAP                  │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ DIT/DAH elements + CHAR/WORD gap events
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 7: Morse Decoding (MorseDecoder)                                      │
│                                                                              │
│  Static binary tree, 127 nodes (depth 7):                                    │
│    root(0) → dit: 2i+1, dah: 2i+2                                           │
│    Covers: A-Z, 0-9, / = ? and prosigns AR, SK                              │
│    ~40 of 127 nodes carry a character; the rest emit '\0' (see defects)      │
│                                                                              │
│  ┌─ Multi-Path Beam Search (8 paths) ──────────────────────────────────┐    │
│  │                                                                      │    │
│  │  addElement(DIT|DAH, confidence):                                    │    │
│  │    pClassified = 0.5 + confidence × 0.5                              │    │
│  │    pAlternate  = 1.0 - pClassified                                   │    │
│  │                                                                      │    │
│  │    For each active path:                                              │    │
│  │      Fork into dit-child (prob × pDit) and dah-child (prob × pDah)  │    │
│  │      If at tree boundary (node >= 127): keep at current node         │    │
│  │                                                                      │    │
│  │    Prune to top 8 paths by probability                                │    │
│  │    Normalize probabilities (prevent underflow)                        │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Character Emission (characterBreak) ────────────────────────────────┐    │
│  │                                                                      │    │
│  │  Triggered by: CHAR_GAP, WORD_GAP, or silence timeout                │    │
│  │                                                                      │    │
│  │  For each path with a valid character at its tree node:               │    │
│  │    score = path.prob × (1 + 0.1 × letterPrior[char])                 │    │
│  │    Letter frequency prior: E=13, T=9.1, A=8.2, ..., Z=0.07          │    │
│  │                                                                      │    │
│  │  Emit char with highest score                                         │    │
│  │  Reset all paths to root                                              │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────────────────────┘
    │
    │ char + confidence
    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│  STAGE 8: Text Output + Post-Decode Correction                               │
│                                                                              │
│  Characters emitted immediately via emitChar() → text.append()               │
│  (real-time display, per-char confidence stored)                             │
│                                                                              │
│  On word boundary (WORD_GAP or silence flush):                               │
│    flushWord() → corrector::correctWord(currentWord, avgConfidence)          │
│    If corrected ≠ original: text.replaceLastN() overwrites in-place          │
│                                                                              │
│  ┌─ Corrector Pipeline (corrector.h) ───────────────────────────────────┐    │
│  │  1. Known word (dictionary/callsign/RST/Q-code) → keep              │    │
│  │  2. High confidence (>0.8) → trust decode                            │    │
│  │  3. Short words (≤2 chars): only prosign/digit corrections           │    │
│  │  4. Edit distance 1 from dictionary word → correct                   │    │
│  │     (context-expected words get +10 score boost from conversation)   │    │
│  │  5. Almost-callsign with digit/letter swap → fix (B↔6, S↔5, 0↔O)   │    │
│  │  6. Almost-RST with prosign→digit → fix (+→5, *→9)                  │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Conversation Tracker (conversation.h) ──────────────────────────────┐    │
│  │  State machine: IDLE → CQ_CALL → EXCHANGE → RST_EXCHANGE → CLOSING  │    │
│  │  Transitions on decoded words (CQ, DE, callsign, RST, 73, SK)       │    │
│  │  isExpectedWord(): boosts correction for context-appropriate words   │    │
│  │  New conversation detection:                                         │    │
│  │    High-confidence CQ (>80%) in unexpected state → reset             │    │
│  │    Only if WPM within 30% of locked operator (rejects cross-channel) │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Vocabulary (vocabulary.h) ──────────────────────────────────────────┐    │
│  │  Dictionary: CQ, DE, RST, QTH, 73, TEST, K, BK, SK, AR, ...        │    │
│  │  Callsign:   [A-Z]{1-2} [0-9] [A-Z]{1-4} (W1AW, K1ABC, VE3NEA)   │    │
│  │  RST report: [1-5][1-9][1-9] (599, 579, 339)                       │    │
│  │  Q-code:     Q[A-Z][A-Z] (QTH, QSO, QRM)                           │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  TextBuffer: mutex-protected, append + replaceLastN for in-place correction  │
│  Stores: vector<CharEntry> {character, confidence, corrected} + string       │
│  getEntries() provides snapshot for per-char UI rendering                    │
│                                                                              │
│  ┌─ UI Rendering (menu.h) ──────────────────────────────────────────────┐    │
│  │  Per-character colored text based on confidence:                      │    │
│  │    confidence > 0.8:  full white (alpha=1.0)                         │    │
│  │    confidence 0.4-0.8: dimmed (alpha=0.6)                            │    │
│  │    confidence < 0.4:  gray (alpha=0.35)                              │    │
│  │  Corrected chars: light blue tint (0.5, 0.8, 1.0)                   │    │
│  │  Hover tooltip: "'C' conf=85%" or "'Q' conf=40% (corrected)"        │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  Silence flush: after 4×dit → characterBreak + flushWord                     │
│                 after 6×dit → emitWordGap (space + correction)               │
└──────────────────────────────────────────────────────────────────────────────┘
```

## Block Interfaces

### Data Types Between Stages

| From → To | Type | Rate | Description |
|-----------|------|------|-------------|
| VFO → Channel | `complex_t[]` | 8000 Hz | Raw IQ from VFO output |
| EnvelopeDSP out | `float[]` | 1000 Hz | Smooth CW envelope (envBuf) |
| MatchedFilter out | `float[]` | 1000 Hz | Impulse-filtered envelope (mfBuf) |
| ToneDetector out | `vector<KeyEvent>` | event-driven | `{keyDown: bool, sampleOffset: int}` |
| Soft Squelch | `float sqFactor` | per-buffer | 0..1 confidence scale |
| AdaptiveTiming (ON) | `TimingEvent` | per-element | `{element: DIT\|DAH, confidence, durationMs}` |
| AdaptiveTiming (OFF) | `TimingEvent` | per-gap | `{gap: ELEMENT\|CHAR\|WORD, confidence, durationMs}` |
| MorseDecoder out | `char` | per-character | Decoded ASCII character |
| TextBuffer | `string` | on-demand | Accumulated decoded text |

### State Machines

**Channel Decode State** (`timingWasLocked`, `flushed`):
```
                    ┌─────────────────┐
                    │   PRE-LOCK       │
                    │                  │
     (init) ──────►│  save events     │
                    │  feed timing     │
                    │  no text emit    │
                    │                  │
                    └────────┬─────────┘
                             │
              timing.isLocked() == true
              OR silence > 4×dit (pre-lock flush)
                             │
                             ▼
                    ┌─────────────────┐
                    │   RETRO-DECODE   │──── replay preLockEvents with
                    │                  │     locked timing, emit text
                    └────────┬─────────┘
                             │
                             ▼
                    ┌─────────────────┐
                    │   POST-LOCK      │
                    │                  │◄──── classify elements and gaps
                    │  decode inline   │      emit chars on gap events
                    │                  │
                    └────────┬─────────┘
                             │
              silence > 4×dit, !flushed
                             │
                             ▼
                    ┌─────────────────┐
                    │   FLUSHED        │──── characterBreak() emits
                    │                  │     pending character
                    └──────────────────┘
                             │
              new keyDown event
                             │
                             ▼
                    (back to POST-LOCK, flushed = false)
```

**ToneDetector State** (`currentState`, `stableSamples`):
```
     ┌──────────┐   v > onThresh, stable >= debounce   ┌──────────┐
     │  KEY UP   │ ───────────────────────────────────► │  KEY DOWN │
     │           │ ◄─────────────────────────────────── │           │
     └──────────┘   v < offThresh, stable >= debounce   └──────────┘

     Debounce: state change requires stableSamples >= debounceLen
               (5ms base, up to 8% of dit estimate, max 25ms)
```

## Tone Scanner Pipeline

Runs in parallel with per-channel processing on the DSP thread.

```
IQ (8 kHz) from VFO
    │
    ▼
┌─────────────────────────────────────────────────────┐
│  ToneScanner.feed(data, count)                       │
│                                                      │
│  Accumulate into 1024-sample ring buffer             │
│  When full:                                          │
│    1. Apply Hann window                              │
│    2. 1024-point FFT (FFTW)                          │
│    3. Magnitude² of each bin                         │
│    4. Exponential spectral averaging (α=0.2)         │
│    5. Median noise floor across all bins             │
│    6. Peak detection:                                │
│       • Local max across ±2 bins                     │
│       • SNR = 10·log10(peak/median) > threshold      │
│    7. Store detected tones: {frequency, power}       │
│                                                      │
│  Bin resolution: 8000 / 1024 ≈ 7.8 Hz               │
│  Update rate: 8000 / 1024 ≈ 7.8 frames/sec          │
└─────────────────────────────────────────────────────┘
    │
    │ detectedTones (buffered for UI thread)
    ▼
┌─────────────────────────────────────────────────────┐
│  updateChannels() (UI thread)                        │
│                                                      │
│  Tone confirmation pipeline:                         │
│    Candidate → 3 consecutive frames → Confirmed      │
│    Missing for 20 frames → Dropped                   │
│                                                      │
│  Channel matching:                                   │
│    Confirmed tone within 120 Hz of existing → skip   │
│    Otherwise → addChannel(freq, pinned=false)        │
│                                                      │
│  Idle removal:                                       │
│    Auto channel with 0 decoded chars for 40 frames   │
│    → removeChannel() (pinned channels exempt)        │
└─────────────────────────────────────────────────────┘
```

## Threading Model

```
┌────────────────────────────────────┐
│          DSP Thread                 │
│                                    │
│  iqHandler() callback:             │
│    ChannelManager.process()        │
│      ToneScanner.feed()            │
│      Channel[0..N].process()       │
│        EnvelopeDSP.process()       │
│        MatchedFilter               │
│        ToneDetector.process()      │
│        Timing + Decode             │
│        TextBuffer.append()  ◄──┐   │
│                                │   │
└────────────────────────────────┼───┘
                                 │ mutex
┌────────────────────────────────┼───┐
│          UI Thread              │   │
│                                 │   │
│  menuHandler() per frame:       │   │
│    syncVFOs()                   │   │
│    updateChannels()             │   │
│    drawMenu()                   │   │
│      TextBuffer.getText()  ◄───┘   │
│      SNR, WPM display             │
│      Channel controls             │
│                                    │
└────────────────────────────────────┘
```

Shared state protection:
- `TextBuffer`: mutex on append/getText/clear
- `ChannelManager.entries`: mutex on add/remove (UI thread only modifies structure)
- `Channel.diagBuf`: mutex for envelope waveform display
- `ChannelManager.detectedTones`: mutex between DSP feed and UI updateChannels

## File Structure

```
decoder_modules/cw_decoder/
├── CMakeLists.txt
├── docs/
│   ├── architecture.md                    this file — what the code does
│   ├── decoding-improvement-plan.md       historical plan + research (see caveats inside)
│   └── decoder-investigation-2026-07.md   audit, experiments, measured baselines, proposal
├── src/
│   ├── main.cpp                      V2 module: VFO, sink, menu handler
│   └── cw/
│       ├── core.h                    IDecodeCore, stage interfaces, CharSink, registry types
│       ├── core_registry.h           the single list of benchmarkable configurations
│       ├── stages.h                  adapters binding concrete components to stage interfaces
│       ├── staged_core.h             pipeline as composed stages + sequencing/retroDecode
│       ├── channel.h                 hosts a core; implements CharSink; owns correction
│       ├── channel_manager.h         multi-channel lifecycle + overlay
│       ├── conversation.h            QSO state machine + context-aware correction
│       ├── corrector.h               post-decode error correction (Levenshtein)
│       ├── dsp.h                     EnvelopeDSP: xlate → decim → BPF → mag → smooth
│       ├── menu.h                    ImGui UI: channel list, controls, text display
│       ├── morse_tree.h              beam search (8/12 paths, 127-node tree, prosigns)
│       ├── text_buffer.h             thread-safe text + in-place replaceLastN
│       ├── timing.h                  7 strategies: kmeans/median/bimodal/kalman/kalman2/log/logrobust
│       ├── tone_detector.h           Schmitt detector + impulse blanker
│       ├── tone_scanner.h            FFT tone scanning + spectral averaging
│       └── vocabulary.h              CW dictionary + callsign/Q-code/RST patterns
└── tests/
    ├── CMakeLists.txt                Catch2 + volk + fftw3f
    ├── cw_test_signals.h             IQ generator + ground truth + CER/WER + Farnsworth
    ├── cw_bench_stats.h              multi-seed stats + ins/del/sub alignment
    ├── cw_matrix.h                   core × profile matrix harness
    ├── cw_oracle.h                   test-only oracle stages (perfect detector / timing)
    ├── cw_detector_score.h           detector vs ground truth: false/miss/bias/jitter
    ├── test_benchmark_multiseed.cpp  24-seed regression gates (always run)
    ├── test_matrix.cpp               core comparison sweep (on demand, tagged [.])
    ├── test_oracle.cpp               per-stage headroom ablation + instrument self-checks
    ├── test_detector_metrics.cpp     detector characterisation, diagnostics, mf-fix
    └── test_*.cpp                    221 tests, 1616 assertions, ~17s
```

## Running the benchmarks

```bash
mkdir -p decoder_modules/cw_decoder/tests/build
cd decoder_modules/cw_decoder/tests/build && cmake .. && make -j8

./cw_decoder_tests                       # full suite incl. 24-seed gates (~17s)
./cw_decoder_tests "[characterize]" -s    # per-profile CER distribution table
./cw_decoder_tests "[matrix-timing]" -s   # timing strategies head-to-head
./cw_decoder_tests "[matrix-bpf]"    -s   # front-end bandwidths head-to-head
./cw_decoder_tests "[matrix-all]"    -s   # every registry core (~2 min)
./cw_decoder_tests "[oracle]"        -s   # per-stage headroom ablation (~16s)
./cw_decoder_tests "[detector-metrics]" -s # detector vs ground truth (~4s)
./cw_decoder_tests "[mf-fix]"        -s   # legacy vs legacy+mf
./cw_decoder_tests "[detector-diag]" -s   # spurious-event locations, resize test
./cw_decoder_tests "[edge-fix]"      -s   # edge-bias and peak-reference variants
./cw_decoder_tests "[attack-sweep]"  -s   # peak attack constant (refuted, kept for history)
./cw_decoder_tests "[window-sweep]"  -s   # percentile window length
./cw_decoder_tests "[dual-window]"   -s   # dual-window switch threshold
./cw_decoder_tests "[dual-refine]"   -s   # short-window length x persistence
./cw_decoder_tests "[peak-gate]"     -s   # transition-gated peak (refuted, kept for history)
./cw_decoder_tests "[guard-probe]"   -s   # dynamic-range guard: rejection during key-down
./cw_decoder_tests "[guard-sweep]"   -s   # guard constant sweep (~2 min)
```

The matrix and oracle sweeps are tagged `[.]` so Catch2 hides them from the
default run: the always-on suite keeps the `legacy` regression gates, the
exploratory sweeps are opt-in. The oracle *self-checks* (`[oracle-self]`) are
deliberately not hidden — if the instrument breaks, every headroom number it
produces is measuring the harness.

**Comparison vs attribution.** The matrix ranks cores against each other; it
cannot express how close any of them is to what is achievable. Oracle ablation
replaces a stage with a perfect one and reads off that stage's headroom. Both
are needed: the matrix gates changes, the oracle decides where to spend effort.
Results in `decoder-investigation-2026-07.md` §11.

## Benchmark profiles

Referenced constantly across all three docs and defined in
`tests/cw_test_signals.h`. Speed is `1200 / ditMs`, so 80 / 60 / 48 ms = 15 / 20
/ 25 WPM. All use `MSG_FULL()` (71 chars) unless noted.

| profile | composition |
|---|---|
| `clean-N` | no degradation |
| `mild-noise` / `moderate-noise` | noiseAmp 0.5 / 1.5 |
| `handkeyed-N` | jitter 15%, weightBias +0.1, noiseAmp 0.3 |
| `qsb` | fade 0.3 Hz (3.3 s period), depth 0.7, noiseAmp 0.5 |
| `qrm` | interferer at 900 Hz, amp 0.6, noiseAmp 0.3 |
| `qrn` | impulses rate 0.001, amp 3.0, noiseAmp 0.3 |
| `contest-20wpm` | jitter 10%, noiseAmp 0.8, QRM 850 Hz amp 0.3, `MSG_CONTEST()` |
| `farnsworth-R` | gaps stretched by R, elements at normal speed |
| `worstcase` | noiseAmp 1.5 + jitter 20% + weightBias 0.15 + QSB 0.5 Hz/0.5 + QRM 850 Hz/0.4 + QRN 0.0005/2.0 |
| `snr-noiseN` | clean 15 WPM plus noiseAmp N — the axis filter bandwidth acts on |

Two timescales worth holding in mind, because several defects turn on them: a
keying edge is ~32 ms (set by the matched filter at `0.4 × dit`), and the `qsb`
fade cycle is ~3300 ms.

## Working practices

Learned the hard way; each one has an incident behind it.

| practice | why |
|---|---|
| Test after **each** change, never batch | Attribution is impossible once two changes are in flight. |
| Run each suite **once** per iteration, save the output, post-process the file | Re-running to extract a different field doubles the wait for no information. |
| **Thresholds are ratchets** — lower when earned, never raise to admit a change | Raising one converts a regression into a "pass". |
| A profile that decoded better before and worse after is a **regression, not a trade** | The one exception that proves it: `worstcase` was called a "physical limit" for exactly this reason, then improved 8×. |
| Keep losing variants in the registry | They are the comparison baseline for future cores, and re-deriving them costs more than the entry. |
| `legacy` must stay **byte-identical**; verify with the characterization table before/after | A "pure refactor" that changes behaviour is the hardest bug class to find later. |
| Mark unmeasured claims (⊘ / ⚠ / ✗ / 📎) | Of 17 predictions made from code reading, 8 were wrong. Unmarked plausible claims have roughly even odds. |
| Close the loop back to **CER** before treating a stage metric as a target | Cutting detector false events 17× moved CER by nothing; the ON-stretch metric misled twice. |
| Before "fixing" an apparent bug, check what currently **absorbs its error** | Four separate mechanisms in this codebase are load-bearing defects. |
| Subagents must **never** run `git checkout` / `stash` / `restore` | The working tree carries uncommitted work; one such call destroyed an unrelated edit. Back up with `cp`, restore with `cp`. |

**Measurement discipline.** Every degradation profile is scored over 24
independent seeds and asserted on the **mean**. Single-seed CER is quantized to
1/refChars and carries no variance estimate, so it cannot distinguish a real
change from a lucky draw — the pre-2026-07 benchmark table was single-seed
(`seed = 42`) and materially understated hand-keyed error. Thresholds are
ratchets: lower them when a change earns it, never raise them to admit one.

## Constants

> ⊘ **EVIDENCE NEEDED — the Rationale column is design intent, not
> measurement.** Only the filter geometry (measured in
> `decoder-investigation-2026-07.md` §4.1), the benchmark seed count, the
> learn/Huber thresholds (§8–9), and the dynamic-range guard (§13.10) have been
> measured. Every other threshold here was chosen by reasoning and has never
> been swept. Several are known to be
> load-bearing in ways their rationale does not describe — see the defects
> table below.

| Constant | Value | Location | Rationale |
|----------|-------|----------|-----------|
| `CW_SAMPLERATE` | 8000 Hz | `channel.h` | VFO output rate |
| `CW_INTERNAL_RATE` | 1000 Hz | `channel.h` | Post-decimation rate |
| `CW_VFO_BANDWIDTH` | 3000 Hz | `main.cpp` | 10 channels × 300 Hz spacing |
| `CW_MAX_CHANNELS` | 10 | `channel_manager.h` | Per-channel cost trivial |
| `CW_TONE_MATCH_HZ` | 120 Hz | `channel_manager.h` | > BPF bandwidth / 2 |
| `CW_IDLE_TIMEOUT` | 40 frames | `channel_manager.h` | ~5 seconds |
| BPF cutoff | 100 Hz / 100 Hz | `dsp.h` | 38 taps, 167.6 Hz ENBW, 18.5 ms delay (measured) |
| Smoothing cutoff | 80 Hz / 100 Hz | `dsp.h` | narrowing measured to *hurt* — do not tune |
| Noise subsample | every 8th | `tone_detector.h` | 250-element buffer, ~2s window |
| Convergence | ≥ 10 subsamples | `tone_detector.h` | ~80 samples at 1 kHz |
| Dynamic-range guard | 1.8 | `tone_detector.h` | Below this the sample is skipped and detection is off. **Swept and confirmed optimal** (§13.10): no value in 1.0–3.0 dominates it, and disabling it makes high-noise CER worse. |
| Soft squelch | 3-10 dB ramp | `channel.h` | Linear confidence scaling |
| Debounce | 5-25ms | `tone_detector.h` | 8% of dit estimate |
| Kalman seed | 3 elements | `timing.h` | Bootstrap initial estimate |
| Timing lock | 7 elements | `timing.h` | seed + 4 |
| Dit clamp | 34-150ms | `timing.h` | 8-35 WPM |
| Beam paths | 8 (12 low-conf) | `morse_tree.h` | Expands under heavy degradation |
| Tree size | 127 nodes | `morse_tree.h` | Depth 7: supports prosigns (AR, SK) |
| Impulse threshold | 2.5× signalPeak | `tone_detector.h` | QRN spike rejection |
| Min element (good SNR) | 0.3×dit | `channel.h` | Rejects phantom elements |
| Min element (low SNR) | 0.15×dit | `channel.h` | Relaxed when edges are fuzzy |
| Correction threshold | confidence < 0.8 | `corrector.h` | Only correct uncertain words |
| Short word skip | ≤2 chars | `corrector.h` | Unless prosign/digit garble |
| Context boost | +10 score | `corrector.h` | For conversation-expected words |
| New QSO detection | CQ conf > 0.8 | `conversation.h` | Resets state in unexpected position |
| WPM consistency | ±30% | `conversation.h` | Rejects cross-channel CQ |
| Flush timeout | 4×dit | `staged_core.h` | Emit pending character |
| Word timeout | 6×dit | `staged_core.h` | Emit space |
| Timing freeze | 30×dit silence | `staged_core.h` | Prevents noise-induced drift |
| Learn threshold | conf ≥ 0.60 | `timing.h` | kalman2/log: skip state update below this |
| Huber k | 2.0 σ | `timing.h` | logrobust only: downweight outlier state updates |
| Benchmark seeds | 24 | `cw_bench_stats.h` | Mean CER stderr ≈ 0.01–0.05 |
| Peak percentile | 90th | `tone_detector.h` | non-default reference; window spans many key cycles |
| Peak window (long) | 2000 ms | `tone_detector.h` | 250 entries at 1 kHz / 8× subsample |
| Peak window (short) | 250–500 ms | `tone_detector.h` | dual-window only; 500 ms halves estimator SE vs 250 |
| Dual switch threshold | 0.05 relative | `tone_detector.h` | disagreement above this = level is moving |
| Dual persistence | 1–16 subsamples | `tone_detector.h` | consecutive disagreements required; filters estimator noise |
| Edge width factor | 0.4 × dit | `tone_detector.h` | `EDGE_COMPENSATE` only; must track `computeFilterWindow()` |

### Known defects deliberately left in place

Documented in `decoder-investigation-2026-07.md`; each has a code comment. These
look like bugs and are load-bearing — do not "fix" them without re-measuring.

| Location | Defect | Why it stays |
|---|---|---|
| `timing.h` KalmanTiming | R adapts on the **posterior residual**, not the innovation | Correcting it regresses hand-keyed 0.158 → 0.223. The low R keeps the gain high, compensating for the linear-ms coordinate being wrong. Fix only with the log-domain move. |
| `timing.h` KalmanTiming | Seed dead branch: ambiguous seed assumed all-dits | Deferring the seed also defers timing lock and retroDecode, breaking clean decoding (WPM sweep 0.01 → 0.364). Correct fix needs gap durations, which `classifyOn()` cannot see. |
| `timing.h` LogTiming | No outlier gate on learning | Three variants tried (3σ relative, ln2 absolute, Huber). All fail: learning from outliers inflates R, which widens acceptance and keeps the filter tolerant. Rejecting them makes it brittle (qrm 0.002 → 0.393). |
| `morse_tree.h` | Unmapped tree nodes emit `'\0'`, silently dropping the character | Real defect, not yet addressed — deletions cost the same as substitutions in CER but give the operator no cue. ⊘ magnitude never measured. |
| `timing.h` adaptive gap centres | Farnsworth char/word split fails at ratio 2.0 | Measured, not yet fixed: oracle ablation puts the entire `farnsworth-2.0` error here (0.0141 → 0.0000 with ideal gap boundaries, detector irrelevant). Isolated and cheap. |
| `staged_core.h` matched filter | Ring buffer zeroed on window resize — dropout mid-element, 14 spurious transitions on a *noiseless* 25 WPM signal | Fixing it (`legacy+mf`) cuts spurious events 17× and CER does not follow: `qsb`, `worstcase` and `noise2.0` all regress. The artifacts fall below `minElementMs()` and are already discarded. |
| `tone_detector.h` | Every ON is stretched by ~9.8% of a dit, distorting the observed dah:dit ratio to 2.82 | Confirmed on a noiseless signal. **Cause is unknown.** Two attributions have been refuted: the threshold ratio gap (by `legacy+sym`, 11%) and instant-attack peak tracking (by `legacy+peakgate`, §13.8 — removing key-up attack entirely reproduces the stretch to 4 s.f.). `PEAK_PERCENTILE` does cut it 57%, but the mechanism for that is now ⊘ unexplained. Correcting it is worth 0.095 CER on hand-keyed; no variant yet avoids regressing `qsb`. |
| `tone_detector.h` | `dynamicRange < 1.8` skips the sample, so while it holds detection is **off**, not merely biased | **Not a defect — swept and confirmed (§13.10).** Disabling it takes blindness to 0% and makes noise4.0 CER *worse* (0.9032 → 0.9173): it reports the SNR limit rather than causing it, and 1.8 is Pareto-optimal. Separately load-bearing (§13.9): legacy's key-up instant attack keeps `peakRef` above it, so 100% of legacy's rejections on noise2.0 land in key-up where they cost nothing. Remove that prop and detection goes blind for 40–65% of key-down time. |
| `tone_detector.h` | `signalPeak` instant attack: the threshold reference chases the rising edge but holds on the falling one | The asymmetry above. `PEAK_PERCENTILE` removes it and reaches the oracle-detector bound on hand-keyed, but a ~2 s window cannot follow QSB. **Four replacement estimators measured (§13); none promotable** — within-element stability and fade tracking are in direct opposition for windowed estimators. Instant attack's fade tracking (`qsb` 0.0117) is still the best measured. A fifth, gating the update on key state rather than replacing the estimator, was also refuted: it reproduced legacy's ON stretch exactly, so the asymmetry above is **not** the cause of the stretch and its mechanism is now unexplained (§13.8). |

## Build

```bash
# Module (as part of SDR++)
cmake --build build --target cw_decoder

# Tests (standalone)
cd decoder_modules/cw_decoder/tests/build
cmake ..
make -j8
./cw_decoder_tests
```

Requires: volk, fftw3f, Catch2 v2 (single-header, in `tests/catch2/`).
