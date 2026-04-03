# CW Decoder Module — Architecture

## Overview

Multi-channel CW (Morse code) decoder for SDR++. Captures a 3 kHz VFO bandwidth at 8 kHz sample rate and runs up to 10 parallel decode channels, each tuned to a different tone frequency. Supports automatic tone detection via FFT scanning and manual channel pinning.

V2 module API. Single-file main.cpp delegates all logic to the `cw/` component library.

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

## Per-Channel Processing Pipeline

Each `Channel` owns the entire processing chain from IQ to decoded text. All blocks run inline in the DSP thread callback — no internal threads.

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
│  │  (VOLK dot_prod)  │  ~20 taps, ~200 Hz effective passband                │
│  │                  │  Rejects signals >200 Hz from tone center              │
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
│  │  (VOLK dot_prod)  │  ~20 taps                                            │
│  │                  │  Removes HF noise from magnitude                       │
│  └────────┬─────────┘                                                        │
│           │ float[envCount] @ 1000 Hz (smooth envelope)                      │
│           ▼                                                                  │
│  Output: envBuf[envCount], where envCount = count / 8                        │
│  Settling time: ~44ms total (< 40ms dit at 30 WPM)                          │
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
│    • Ring buffer resets on window size change                                │
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
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
│  ┌─ Dynamic Range Gate ─────────────────────────────────────────────────┐    │
│  │  dynamicRange = signalPeak / noiseFloor                              │    │
│  │  If < 1.8 → skip detection (signal ≈ noise, no CW present)          │    │
│  └──────────────────────────────────────────────────────────────────────┘    │
│                                                                              │
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
│  │  2. Create retroTiming, seed with 5 × (dit, 3*dit) pairs            │    │
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
│  Static binary tree, 63 nodes (depth 6):                                     │
│    root(0) → dit: 2i+1, dah: 2i+2                                           │
│    Covers: A-Z, 0-9, / = ?                                                  │
│                                                                              │
│  ┌─ Multi-Path Beam Search (8 paths) ──────────────────────────────────┐    │
│  │                                                                      │    │
│  │  addElement(DIT|DAH, confidence):                                    │    │
│  │    pClassified = 0.5 + confidence × 0.5                              │    │
│  │    pAlternate  = 1.0 - pClassified                                   │    │
│  │                                                                      │    │
│  │    For each active path:                                              │    │
│  │      Fork into dit-child (prob × pDit) and dah-child (prob × pDah)  │    │
│  │      If at tree boundary (node >= 63): keep at current node          │    │
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
│   ├── architecture.md               this file
│   └── decoding-improvement-plan.md  decoder quality plan + research
├── src/
│   ├── main.cpp                      V2 module: VFO, sink, menu handler
│   └── cw/
│       ├── channel.h                 per-channel pipeline + retroDecode + correction
│       ├── channel_manager.h         multi-channel lifecycle + overlay
│       ├── conversation.h            QSO state machine + context-aware correction
│       ├── corrector.h               post-decode error correction (Levenshtein)
│       ├── dsp.h                     EnvelopeDSP: xlate → decim → BPF → mag → smooth
│       ├── menu.h                    ImGui UI: channel list, controls, text display
│       ├── morse_tree.h              beam search (8/12 paths, 127-node tree, prosigns)
│       ├── text_buffer.h             thread-safe text + in-place replaceLastN
│       ├── timing.h                  Kalman + Bayesian + adaptive gap centers
│       ├── tone_detector.h           CFAR detector + impulse blanker
│       ├── tone_scanner.h            FFT tone scanning + spectral averaging
│       └── vocabulary.h              CW dictionary + callsign/Q-code/RST patterns
└── tests/
    ├── CMakeLists.txt                Catch2 + volk + fftw3f
    ├── cw_test_signals.h             IQ generator + CER/WER + Farnsworth
    └── test_*.cpp                    197 tests, 527 assertions, <0.5s
```

## Constants

| Constant | Value | Location | Rationale |
|----------|-------|----------|-----------|
| `CW_SAMPLERATE` | 8000 Hz | `channel.h` | VFO output rate |
| `CW_INTERNAL_RATE` | 1000 Hz | `channel.h` | Post-decimation rate |
| `CW_VFO_BANDWIDTH` | 3000 Hz | `main.cpp` | 10 channels × 300 Hz spacing |
| `CW_MAX_CHANNELS` | 10 | `channel_manager.h` | Per-channel cost trivial |
| `CW_TONE_MATCH_HZ` | 120 Hz | `channel_manager.h` | > BPF bandwidth / 2 |
| `CW_IDLE_TIMEOUT` | 40 frames | `channel_manager.h` | ~5 seconds |
| BPF cutoff | 100 Hz / 100 Hz | `dsp.h` | ~20 taps, 200 Hz passband |
| Smoothing cutoff | 80 Hz / 100 Hz | `dsp.h` | ~20 taps |
| Noise subsample | every 8th | `tone_detector.h` | 250-element buffer, ~2s window |
| Convergence | ≥ 10 subsamples | `tone_detector.h` | ~80 samples at 1 kHz |
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
| Flush timeout | 4×dit | `channel.h` | Emit pending character |
| Word timeout | 6×dit | `channel.h` | Emit space |

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
