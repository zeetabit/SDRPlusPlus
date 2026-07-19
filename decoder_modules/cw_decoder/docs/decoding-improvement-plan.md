# CW Decoder — Decoding Improvement Plan

> **STATUS (2026-07): this document is now partly historical.**
>
> It records Phases 1–10 and the research behind them, and remains the reference
> for *why* the current design looks the way it does. But several of its
> measurements and two of its research claims did not survive re-testing.
> Superseding document: **`decoder-investigation-2026-07.md`** — audit,
> controlled experiments, 24-seed baselines, and the current proposal.
>
> Corrections are marked inline below with ⚠ and an evidence-strength label.
> Where evidence is not yet conclusive the claim is marked CONTESTED rather than
> rewritten — it should be amended only once the evidence is decisive.
>
> **Evidence markers** (shared across the module docs; legend in
> `decoder-investigation-2026-07.md` §1): ⊘ EVIDENCE NEEDED, ⚠ CONTESTED,
> ✗ REFUTED, 📎 SECOND-HAND.
>
> ⊘ **This document predates the measurement discipline.** Its "Key Insights",
> "What Was Tried and Didn't Work" and "Research" sections record reasoning and
> single-seed observations, not multi-seed measurement. Treat every ranking or
> causal claim in them as unverified unless the investigation document confirms
> it.
>
> **The single most important caveat:** every benchmark number in this document
> is a **single noise realization on `seed = 42`**. See the Benchmark Results
> section.
>
> ### Work completed since (all in the registry, none promoted)
>
> | stage | outcome |
> |---|---|
> | Multi-seed harness | 24-seed gates; hand-keyed found ~4× worse than documented |
> | Pluggable cores | `IDecodeCore` + stages + registry; `legacy` byte-identical |
> | Benchmark matrix | core × profile × {CER/WER, ins/del/sub, latency/CPU, WPM RMS} |
> | Kalman corrections | `legacy+kalman2` — worstcase 0.624 → 0.350 |
> | Log-duration timing | `legacy+log` — best on 5 profiles, blocked on heavy noise |
> | Huberised update | `legacy+logrobust` — refuted; localises blocker to the detector |
> | Oracle ablation | per-stage headroom measured; "physical limit" claim refuted |
> | Detector metrics | edge bias +9.8% dit confirmed; matched-filter resize defect found |
> | Matched-filter fix | `legacy+mf` — 17× fewer spurious events, CER unchanged, not promoted |
>
> **Next:** the detector (`tone_detector.h`), for the AWGN/QSB/QRN family only.
> Oracle ablation confirms 100% of the error on those profiles is at the
> detector. It also corrects the scope: hand-keyed profiles are a *timing*
> fault, Farnsworth is a *gap-classification* fault, and `worstcase` needs both
> the detector and the duration model fixed together.

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

> **⚠ MEASUREMENT CAVEAT (added 2026-07).** Every figure in this table is a
> **single noise realization on `seed = 42`** (`cw_test_signals.h:62`; only one
> test in the suite ever overrides it). The numbers below reproduce exactly and
> are not arithmetic errors — but a single draw carries no variance estimate,
> and CER is quantized to 1/23 = 0.043 on `MSG_CQ`, so no value between 0 and
> 0.043 is distinguishable from zero.
>
> Verified by ablation (`test_benchmark_multiseed.cpp`, "single-seed vs
> multi-seed"), varying seed count alone on the identical message:
>
> | Profile | this table (seed 42) | **24 seeds, same message** |
> |---|---|---|
> | Hand-keyed 15 WPM | 0.0 "Perfect" | **0.134** |
> | Hand-keyed 20 WPM | 0.043 | **0.156** |
> | Hand-keyed 25 WPM | 0.043 | **0.150** |
>
> **Status: the jitter rows are superseded** — see
> `decoder-investigation-2026-07.md` §3.1 for the 24-seed baseline. The clean,
> mild-noise, moderate-noise and Farnsworth-1.5 rows were re-measured at 24
> seeds and **hold at 0.000**.
>
> **Not yet re-measured (treat as single-seed until confirmed):** QSB, QRM, QRN,
> contest, Farnsworth 2.0, worst case.

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

#### RetroDecoding is the Single Biggest Improvement ⊘

> ⊘ **EVIDENCE NEEDED for the ranking.** The mechanism below is real and the
> code does what it says, but "single biggest" was never measured against the
> alternatives — no ablation removing retroDecode was ever run. The oracle
> harness (`decoder-investigation-2026-07.md` §11) can now settle this directly.

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

> **✗ REFUTED (2026-07). This heading is retained for history; the conclusion
> below is wrong.**
>
> Oracle ablation takes `worstCase` from **0.6244 to 0.0792** — an 87%
> reduction with no change whatsoever to the signal, only to the decoder. A
> profile that improves 8× under a better decoder is not at a physical limit.
>
> Evidence: 24 seeds, in-tree, `decoder-investigation-2026-07.md` §11. Three
> independent lines now agree — timing corrections alone gave 0.624 → 0.350
> (§8), a narrower BPF gave 0.581 → 0.364 at 16 seeds (§4.5), and the oracle
> bound is 0.0792.
>
> **What is true instead:** `worstCase` is a *two-stage* failure. Perfect
> detection alone reaches 0.4149; a perfect duration model alone reaches
> 0.3644; together they reach 0.0792. The stages are strongly super-additive,
> so fixing either one in isolation recovers only ~40% and looks like hitting a
> wall. That is the observation the "physical limit" reading came from.

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

> 📎 **SECOND-HAND — applies to this entire section.** Every figure below is
> quoted from another project, blog post or paper and none has been reproduced
> in-tree. The AG1LE CER/SNR table in particular is the basis for this module's
> filter-bandwidth reasoning, and a controlled in-tree experiment
> (`decoder-investigation-2026-07.md` §4) found the achievable gain to be about
> **half** what a noise-bandwidth reading of that table predicts.

### fldigi (source: cw.cxx)
- `agc_peak`: fast attack (factor 20) / slow decay (factor 800) envelope follower
- Dual thresholds: `CWupper` / `CWlower` with state machine (RS_IDLE → RS_IN_TONE → RS_AFTER_TONE)
- `cw_adaptive_receive_threshold`: tracks 2×dit duration via tracking filter
- FFT sin(x)/x filter — very steep lowpass, optimum for CW in white noise
- SOM (Self-Organizing Map) decoder gives ~5% better CER than legacy at -13 dB
  > **⚠ CONTESTED (2026-07) — verify against fldigi source before relying on
  > this.** A source read of `fldigi/src/cw_rtty/cw.cxx` reports `som_table[]`
  > is a *hardcoded, hand-written* codebook (weights `0.33` dit / `1.0` dah,
  > never trained) and `find_winner()` is plain nearest-neighbour Euclidean
  > distance over a 7-element vector — i.e. a fixed template matcher, not a
  > self-organizing map. AG1LE's trained-SOM experiments were real but may
  > never have shipped.
  >
  > Evidence strength: **second-hand source read, not verified first-hand.**
  > The "~5% better CER at -13 dB" figure also has no located primary source.
  > Recheck by reading `cw.cxx` directly before amending or striking.
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

> ⊘ **EVIDENCE NEEDED for every row not marked Done.** The Jitter / Noise /
> Complexity columns on candidate and future rows are *estimates made before
> implementation*, not measurements. Rows #14–16 and #24–27 in particular carry
> no data. Rows marked **Done** have measured CER in the results table above or
> in `decoder-investigation-2026-07.md`.
>
> The scorecard in that document (§10) puts pre-implementation predictions at
> roughly 50% accuracy, so these ratings should not be used to order work
> without an oracle-ablation estimate of the headroom first.

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
| 17 | Multi-seed benchmark harness | N/A | N/A | Low | **Done (2026-07)** |
| 18 | Pluggable decode cores + registry | N/A | N/A | Medium | **Done (2026-07)** |
| 19 | Core × profile benchmark matrix | N/A | N/A | Medium | **Done (2026-07)** |
| 20 | Kalman dah-gain correction | High | Medium | Low | **Done — variant `+kalman2`** |
| 21 | Log-duration timing (Mills) | Very High | Medium | Medium | **Done — variant `+log`** |
| 22 | Narrow pre-detection BPF | Low (−) | Very High | Low | **Measured — variants `+bpf*`** |
| 23 | Huberised robust timing update | Low | None | Low | **Refuted (2026-07)** |
| 24 | Likelihood-ratio detector | Medium | High | High | **Next** |
| 25 | Gap ambiguity inside the beam | High | Medium | Medium | Candidate |
| 26 | Callsign database (SCP/Master.dta) | N/A | N/A | Low | Candidate |
| 27 | Bell 1977 trellis core | High | Very High | Very High | Future |
| 28 | Oracle ablation harness | N/A | N/A | Low | **Done (2026-07)** |
| 28b | Detector ground-truth metrics | N/A | N/A | Low | **Done (2026-07)** |
| 28c | Matched-filter resize fix | Low | Low | Low | **Measured — variant `+mf`, not promoted** |
| 28d | Schmitt edge-bias correction | High | Medium | Low | **Next** |
| 29 | Adaptive gap centres rewrite (Farnsworth) | N/A | Low | Low | **Candidate — measured 0.0141 → 0** |
| 30 | Real-recording benchmark + model-mismatch profiles | N/A | N/A | Medium | **Prerequisite for #24** |

### Results of 2026-07 work (24 seeds, MSG_FULL, mean CER)

`legacy` is unchanged and remains the default; nothing below was promoted.
Full tables and method: `decoder-investigation-2026-07.md` §8–10.

| profile | `legacy` | `+kalman2` | `+log` | `+bimodal` |
|---|---|---|---|---|
| clean 15/25 WPM | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| hand-keyed 15 WPM | 0.1579 | 0.0340 | **0.0288** | 0.0399 |
| hand-keyed 20 WPM | 0.2001 | 0.0446 | **0.0329** | 0.0469 |
| hand-keyed 25 WPM | 0.1309 | 0.1320 | 0.0857 | **0.0563** |
| QSB | 0.0117 | **0.0070** | 0.0100 | 0.0364 |
| QRM | 0.0100 | **0.0018** | **0.0018** | 0.0053 |
| QRN | 0.0029 | 0.0023 | **0.0006** | **0.0006** |
| worst case | 0.6244 | **0.3498** | 0.3615 | 0.6408 |
| noise 2.0 | 0.0827 | 0.0399 | **0.0340** | 0.0915 |
| noise 3.0 | 0.8163 | **0.7060** | 1.0839 | 1.0681 |
| noise 4.0 | **0.9032** | 0.9888 | 1.5487 | 1.3762 |

**worst case improved 44% from timing corrections alone** (0.624 → 0.350) — more
than any filter change achieved, and further evidence against the "physical
limit" framing marked CONTESTED above.

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

### Phase 11: Measurement + Modularity (DONE, 2026-07)
- Multi-seed benchmark harness (`cw_bench_stats.h`, 24 seeds, mean + p95 + stderr)
- Error-type breakdown (ins/del/sub) — distinguishes a decoder that goes silent
  from one that emits garbage; aggregate CER cannot
- Pluggable `IDecodeCore` + swappable stages + name-keyed registry
- Core × profile benchmark matrix, on-demand (`[matrix]`)
- Verified by byte-identical characterization output before/after the refactor

### Phase 12: Timing Corrections (DONE, 2026-07 — variants, not promoted)
- `+kalman2`: dah measurement gain corrected (was 81× too small), confidence-gated
  learning, R floor made dimensionally correct
- `+log`: state is `ln(dit)` — dit/dah differ by a constant `ln 3`, jitter is
  homoscedastic, so the dah-gain class of bug becomes inexpressible
- `+logrobust`: Huberised state update — refuted, no blocker relief

### Phase 13: Oracle Ablation (DONE, 2026-07)
Ground truth recorded in the signal generator (`TruthSegment[]`, `ElementModel`)
plus two test-only oracle stages, run as a 2×2. Measures per-stage headroom
instead of only ranking cores against each other.

Results and caveats: `decoder-investigation-2026-07.md` §11. Headlines:
- 100% of AWGN/QSB/QRN error is at the detector; timing contributes nothing there
- hand-keyed is almost entirely *timing* (0.1579 → 0.0153 with `+tim` alone)
- Farnsworth is purely *gap classification* (0.0141 → 0.0000)
- `worstcase` is super-additive: 0.6244 → 0.0792 only with both stages
- "Noise+QSB is a physical limit" refuted

It also corrected a conclusion three prior experiments had not surfaced, and its
own first version reported an instrument defect as an irreducible floor — see
§10 "An instrument's own defects look exactly like findings".

### Phase 14: Detector Characterisation (DONE, 2026-07)
Ground-truth scoring of the real detector: false/miss rates, edge bias, edge
jitter, group delay. Full results `decoder-investigation-2026-07.md` §12.

- **§2.3(h) confirmed quantitatively** — every ON is stretched +9.8% of a dit on
  a *noiseless* signal, predicted 7.65 ms vs measured 7.84 ms. A constant offset,
  so the observed dah:dit ratio is 2.82 rather than 3.0 and the element gap is
  0.90 dit. Every model in `timing.h` assumes exact ratios.
- **New defect found:** the matched filter zeroes its ring buffer on window
  resize, injecting a dropout mid-element — 14 spurious transitions on a
  noiseless 25 WPM signal, reproducible on every seed.
- **Fixing it does not help.** `legacy+mf` cuts spurious events 17× and CER does
  not follow; `qsb`, `worstcase` and `noise2.0` regress. Third instance of the
  load-bearing-bug pattern.
- **False-event rate is a poor CER proxy.** Deletions carry the cost — at
  `noise4.0` the miss rate is 1.384 per element and `del = 0.840`.

### Phase 15: Detector Correction (NEXT)
Scoped by Phase 13 to the AWGN/QSB/QRN family. `tone_detector.h` applies a
fixed-fraction Schmitt threshold to a magnitude envelope and emits a hard binary
decision, discarding the soft information before timing ever sees it.

Immediate step is **detector ground-truth metrics** — attribution is settled, so
the open question is which kind of detector error dominates (false key-downs vs
edge bias vs jitter).

**Prerequisite (#30):** the generator adds complex Gaussian noise, so its
envelope is exactly Rician — the model a likelihood-ratio detector assumes.
Benchmarked on this suite alone such a detector is being tested against its own
generative assumptions. Real recordings and model-mismatch profiles are required
before committing to the rewrite, not after.

Planning: `decoder-investigation-2026-07.md` §12.
