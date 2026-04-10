# CW Decoder — Decoding Improvement Plan

## Current State (after Phase 1-10 implementation)

### Implemented

**Signal Processing:**
- **Kalman filter timing** — state = dit duration + uncertainty. Gaussian likelihood ratio for dit/dah classification. Measurement noise adapts from residuals. R floor widened for faster WPM (ditEst * 0.05). Clamped to 34-150ms (8-35 WPM).
- **Bayesian gap classification** — three Gaussians (element, char, word) with adaptive centers from observed gap durations. Jitter sigma estimated from element duration variance (floor 15% of dit). Priors: 5x element, 2x char, 0.5x word.
- **Adaptive gap centers** — 2-means clustering on OFF durations replaces hardcoded 1:3:7 ratios. Supports Farnsworth spacing where char/word gaps are stretched.
- **Multi-path beam search decoder** — 8 paths (12 under low confidence) through 127-node binary tree (depth 7). Confidence-weighted branching, English letter frequency prior.
- **Event-replay retroDecode** — saves key events during timing convergence, replays with locked timing. 8 dit/dah seed pairs for fast retro timing lock.
- **Soft squelch** — linear ramp 3-10 dB SNR, scales element confidence.
- **Subsampled percentile noise floor** — every 8th sample, 250-element buffer, 25th percentile.
- **Convergence guard** — detection disabled until noiseWinCount >= 10.
- **SNR-adaptive Schmitt thresholds** — 0.55/0.35 at high SNR, 0.60/0.30 at medium, 0.65/0.25 at low.
- **Impulse blanker** — rejects QRN spikes exceeding 2.5× signal peak during key-up. Sample-and-hold for sub-debounce durations. Only activates after first key cycle (estimatedDitSamples > 0).
- **Adaptive minimum element filter** — rejects phantom elements shorter than 0.3×dit (at SNR>6dB) or 0.15×dit (at low SNR). Prevents noise-induced B→6 type confusions.
- **Adaptive debounce** — 5-25ms scaled with dit estimate.

**Decoding:**
- **Prosigns** — AR (.-.-. = '+') and SK (...-.- = '*') in expanded 127-node tree. BT (-...- = '=') was already mapped.
- **Post-decode error correction** — vocabulary dictionary (CW words, Q-codes, callsign patterns, RST reports), Levenshtein distance-1 correction for low-confidence words. In-place correction via `replaceLastN` in text buffer. Short words (≤2 chars) only corrected when containing prosign characters or leading-digit garble — prevents false corrections like MY→BK.
- **Farnsworth spacing** — signal generator supports `farnsworthRatio` parameter. Gap classifier adapts from observed data instead of fixed ratios.
- **Conversation state tracking** — QSO state machine (IDLE→CQ_CALL→EXCHANGE→RST_EXCHANGE→CLOSING) provides context-aware correction boost. Expected words in each state get +10 score in dictionary scan. High-confidence CQ (>80%) in unexpected state triggers new-conversation detection with WPM consistency check (within 30% of locked operator speed) to reject cross-channel pollution.

**UI:**
- **Confidence highlighting** — per-character text coloring based on decode confidence: full white (>0.8), dimmed (0.4-0.8), gray (<0.4). Corrected characters shown in light blue. Hover tooltip displays confidence percentage and "(corrected)" flag.

### Benchmark Results

| Profile | CER | Notes |
|---------|-----|-------|
| Clean 15 WPM | 0.0 | Perfect across 8-35 WPM range |
| Mild noise (0.5) | 0.0 | Perfect |
| Moderate noise (1.5) | 0.0 | Perfect |
| Hand-keyed 15 WPM (15% jitter) | 0.0 | Perfect |
| Hand-keyed 20 WPM | 0.043 | 1 error in 23 chars |
| Hand-keyed 25 WPM | 0.043 | |
| Hand-keyed+QRN 20 WPM | 0.087 | Combined jitter + impulse |
| QRM (+200Hz, 0.6 amp) | 0.0 | BPF rejects interference |
| QRN impulse (3.0 amp) | **0.0** | Corrector fixes first-char garble |
| QRN heavy rate (5× rate) | 0.0 | Mid-message intact |
| QRN strong (amp=5) | 0.0 | Mid-message intact |
| QSB fading (0.3Hz, 70%) | 0.0 | Soft squelch through fades |
| Contest (20 WPM, mixed) | **0.0** | B/6 confusion fixed |
| Farnsworth ratio 1.5 | 0.0 | Perfect |
| Farnsworth ratio 2.0 | 0.043 | Adaptive gap centers |
| Worst case (all combined) | 0.57 | Improved from 0.61 by corrector (+Q→CQ) |
| Worst case long message | 0.45 | More data → better timing |

**Test suite: 197 tests, 527 assertions, <0.5s runtime.**

### Key Insights

#### RetroDecoding is the Single Biggest Improvement

The naive approach — run detector, feed timing, emit text as you go — fails because timing needs ~7 elements to lock. Before lock, element classification uses the default estimate (often wrong), garbling the first 2-3 characters.

**Solution: event replay.** During the pre-lock phase, the main detector runs normally and feeds the timing model. Key events are saved but NOT decoded. When timing locks, saved events are replayed with correct timing parameters. This produces perfect classification from the very first element.

#### Impulse Blanker Design

QRN (atmospheric noise) produces brief high-amplitude spikes that can corrupt keying edges. The blanker compares each sample against `signalPeak × 2.5` — impulses that far exceed the known signal level are replaced with the previous sample value (sample-and-hold). Key constraints:
- Only activates when key is UP (won't clip real signal)
- Only activates after first key cycle (estimatedDitSamples > 0) to avoid interfering with initial signal detection
- Hold duration limited to debounce length (real signals persist beyond this)

#### B/6 Confusion Fix

B(`-...`) at tree node 23, 6(`-....`) at node 47 = dit-child of B. A noise-induced phantom dit after B traverses to 6. Fix: adaptive minimum element duration (0.3×dit after timing locks) rejects elements too short to be real. SNR-dependent: relaxed to 0.15×dit at low SNR where edges are less precise.

#### Farnsworth Spacing

Standard CW: element gaps = 1×dit, char gaps = 3×dit, word gaps = 7×dit. Farnsworth: elements at standard speed, gaps stretched by ratio (1.5-3×). At ratio 2.0, char gaps = 6×dit — indistinguishable from word gaps under hardcoded 3:7 model.

Fix: track observed gap durations in a 40-element sliding window. Split into short (elements) and long (char+word) clusters. Within long cluster, find the largest gap to split char from word. Use observed centers as Gaussian means instead of hardcoded ratios.

#### Noise+QSB is a Physical Limit

Diagnostic breakdown of worst-case profile (noiseAmp=1.5, jitter=20%, QSB 0.5Hz/50%, QRM, QRN):

| Degradation | CER | Notes |
|-------------|-----|-------|
| Noise 1.5 alone | 0.0 | Perfect |
| Jitter 20% alone | 0.48 | Timing errors |
| QSB alone | 0.0 | Perfect |
| QRM alone | 0.0 | Perfect |
| QRN alone | 0.0 | Perfect |
| **Noise + QSB** | **0.61** | **The killer combination** |
| Noise + jitter | 0.04 | Noise helps timing stability |
| Full worst case | 0.65 | Dominated by noise+QSB |

During QSB dips with noise=1.5: signal ≈ 0.5, noise ≈ 1.5 → **-9.5 dB SNR**. AG1LE's research (2013) shows fldigi achieves CER=0.01 at -10 dB with 35 Hz matched filter — but that's static SNR, not oscillating QSB. The oscillation adds re-acquisition overhead every 2 seconds.

**Approaches tried and reverted:**
- Slow peak decay (tau 0.5→2s): thresholds stay high during fade, noise triggers false events
- Signal presence persistence (lower squelch during recent activity): lets MORE noise through
- SNR-adaptive matched filter (wider at low SNR): smears edges, regressions on other profiles
- Static smoothing filter tightening (80→50 Hz): helps QRN but hurts jittered worst-case
- Dynamic smoothing filter (runtime tap change): transient artifacts at switchover point

Sources: [AG1LE SNR vs CER](http://ag1le.blogspot.com/2013/01/morse-decoder-snr-vs-cer-testing.html), [PA3FWM digital mode SNR](http://www.pa3fwm.nl/technotes/tn09b.html), [fldigi CW source](https://github.com/jamescoxon/dl-fldigi/blob/master/src/cw_rtty/cw.cxx)

#### Post-Decode Correction

Corrector runs on word boundaries (WORD_GAP or silence flush). Characters are emitted immediately to the text buffer for real-time display. On word boundary, the corrector checks the completed word:

1. Already known (dictionary/callsign/RST/Q-code) → keep
2. High confidence (>0.8) → trust the decode
3. Edit distance 1 from dictionary word → correct in-place via `replaceLastN`
4. Almost-callsign with digit/letter confusion (B↔6, S↔5, 0↔O) → fix
5. Almost-RST with prosign→digit confusion (+→5, *→9) → fix

Key fix: QRN garble "+Q" → "CQ" (edit distance 1 from dictionary word "CQ", low confidence).

### What Was Tried and Didn't Work

- **Multi-tone diversity (3-channel max-combine)**: Amplifies QRM. Net negative.
- **Multi-tone voting**: Timing misalignment between channels.
- **Hysteretic squelch**: Keeps squelch open during noise.
- **EMA noise floor**: Doesn't converge for bimodal CW signals.
- **Min-hold noise floor**: Floor climbs through signal.
- **Preamble-only scoring ("|" separator)**: Masked real bugs. Removed.
- **Slow peak decay for QSB**: False events from elevated thresholds.
- **Dynamic smoothing filter**: Transient artifacts at tap switchover.
- **Static narrow smoothing (50-60 Hz)**: Helps QRN, hurts jitter.

## Research: How Others Handle CW

### fldigi (source: cw.cxx)
- `agc_peak`: fast attack (factor 20) / slow decay (factor 800) envelope follower
- Dual thresholds: `CWupper` / `CWlower` with state machine (RS_IDLE → RS_IN_TONE → RS_AFTER_TONE)
- `cw_adaptive_receive_threshold`: tracks 2×dit duration via tracking filter
- FFT sin(x)/x filter — very steep lowpass, optimum for CW in white noise
- SOM (Self-Organizing Map) decoder gives ~5% better CER than legacy at -13 dB
- Matched filter automatically sets bandwidth optimal for CW speed

### AG1LE CER vs SNR Data (fldigi, 20 WPM)
| SNR | CER (35Hz filter) | CER (68Hz filter) |
|-----|-------------------|-------------------|
| 0 dB | 0.00001 | 0.00001 |
| -10 dB | 0.010 | 0.208 |
| -13 dB | 0.154 | 0.665 |
| -15 dB | 0.466 | 0.718 |
| -20 dB | 0.680 | 0.754 |

Key finding: **filter bandwidth is the dominant factor** — 35 Hz vs 68 Hz = 20× improvement at -10 dB.

### PA3FWM SNR Requirements
- CW by ear (10 WPM): -18 dB (in 2500 Hz BW)
- RSCW software (12 WPM): -12 dB
- Shannon limit: -1.59 dB Eb/N0 (theoretical floor)

### RSCW (PA3FWM)
- Dibit encoding, Viterbi trellis decoding
- Optimal for machine CW only (too rigid for hand-keyed)

### NATO ML Decoder (2024)
- CNN-LSTM-CTC model, trained on 5.2 hours of Morse audio
- CER 0.1% at positive SNR, accurate down to -3 dB
- Performs well until about -12 dB, then drops dramatically

### Approaches Matrix

| # | Approach | Jitter | Noise | Complexity | Status |
|---|---------|--------|-------|-----------|--------|
| 1 | Kalman filter timing | High | Medium | Medium | **Done** |
| 2 | Bayesian gap classification | High | Medium | Medium | **Done** |
| 3 | Multi-path beam search | High | Medium | Medium | **Done** |
| 4 | Soft squelch | Medium | High | Low | **Done** |
| 5 | Event-replay retroDecode | High | High | Medium | **Done** |
| 6 | Impulse blanker | Medium | High (QRN) | Low | **Done** |
| 7 | Minimum element filter | High | Medium | Low | **Done** |
| 8 | Adaptive gap centers | High (Farnsworth) | Medium | Medium | **Done** |
| 9 | Post-decode correction | Medium | Medium | Medium | **Done** |
| 10 | Dynamic beam width | Medium | High | Low | **Done** |
| 11 | Prosigns (AR, SK) | N/A | N/A | Low | **Done** |
| 12 | Confidence UI highlighting | N/A | N/A | Low | **Done** |
| 13 | Conversation state tracking | Medium | Medium | Medium | **Done** |
| 14 | ATC (W7AY) | Medium | High | Medium | Candidate |
| 15 | Coherent detection (PLL) | Medium | Very High | High | Candidate |
| 16 | LSTM RNN | Very High | Very High | Very High | Future |

## Phase Details

### Phase 1: Test Infrastructure (DONE)
- IQ signal generator with jitter, QSB, QRM, QRN, weight bias, Farnsworth ratio
- CER/WER scoring via Levenshtein edit distance
- 9+ degradation profiles with full-message scoring

### Phase 2: Detector Overhaul (DONE)
- Subsampled percentile noise floor (96x speedup)
- Convergence guard (noiseWinCount >= 10)
- SNR-adaptive Schmitt thresholds
- Adaptive debounce (5-25ms)

### Phase 3: Timing (DONE)
- 4 strategies: K-Means, Median, Bimodal, Kalman (default)
- Kalman: process noise 0.001, R floor max(ditEst×0.05, 4.0)
- Bayesian gap classification with adaptive centers from observed data
- Jitter sigma floor raised to 15% of dit

### Phase 4: Multi-Path Decoder (DONE)
- 127-node binary tree (depth 7, supports 6-element prosigns)
- 8 paths default, 12 under low confidence
- English letter frequency prior for tie-breaking
- Tree boundary clamping

### Phase 5: RetroDecoding + Soft Squelch (DONE)
- Event-replay retroDecode (8 seed pairs)
- Pre-lock: save events, feed timing, don't emit text
- Post-lock: retroDecode replays events, transfers morse state
- Soft squelch: linear ramp 3-10 dB

### Phase 6: Signal Robustness (DONE)
- Impulse blanker: sample-and-hold for spikes > 2.5× signalPeak
- Minimum element filter: 0.3×dit (good SNR) / 0.15×dit (low SNR)
- Wider Kalman R floor for faster WPM
- Dynamic beam width under low confidence

### Phase 7: Farnsworth + Prosigns (DONE)
- farnsworthRatio parameter in signal generator
- Adaptive gap centers from 2-means clustering on observed gaps
- AR (.-.-.) and SK (...-.-) in expanded 127-node tree

### Phase 8: Post-Decode Correction (DONE)
- vocabulary.h: CW word dictionary, callsign/Q-code/RST patterns
- corrector.h: Levenshtein correction, callsign digit/letter fix, RST fix
- In-place correction via replaceLastN on word boundaries
- Characters displayed immediately, corrected on word gap
- Short words (≤2 chars): only prosign→letter and leading-digit corrections (prevents MY→BK false positives)

### Phase 9: Confidence UI + Conversation Tracking (DONE)
- Per-character confidence highlighting in ImGui: white (>0.8), dimmed (0.4-0.8), gray (<0.4)
- Corrected characters: light blue tint for visual distinction
- Hover tooltip: confidence %, "(corrected)" flag
- CharEntry gained `corrected` bool, TextBuffer gained `getEntries()` and `replaceLastN` sets corrected=true
- conversation.h: QSO state machine (IDLE→CQ_CALL→EXCHANGE→RST_EXCHANGE→CLOSING)
- Context-aware correction: `isExpectedWord()` boosts dictionary matches by +10 score in current state
- New conversation detection: high-confidence CQ (>80%) in unexpected state resets state machine
- WPM consistency check: only resets if WPM within 30% of locked operator — rejects cross-channel pollution
- Corrector accepts optional `ConversationTracker*` for context-boosted matching

### Phase 10: Future Work
- **Real-world validation** — W1AW recordings, contest recordings, A/B vs fldigi
- **Coherent detection** — PLL locked to CW tone for better SNR in noise+QSB
- **Narrowband adaptive filter** — runtime BPF tap update matched to locked WPM (needs glitch-free filter transition)
