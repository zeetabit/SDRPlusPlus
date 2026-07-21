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
> ### Work completed since (all in the registry; the one promotion was reverted — §25)
>
> **Current default: `legacy` (Schmitt + Kalman).** `legacy+lr+log` was promoted
> on 2026-07-21 (§21) and **reverted the same day (§25)** when the gate's noise
> axis was widened past 15 WPM: it regresses badly at fast CW under noise. §26–28
> then mapped that boundary from every angle and found it not closable by any
> bound rule. The campaign still has **zero standing promotions**; `legacy` is
> unchanged. See the LR rows below and the Approaches Matrix #24.
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
> | Edge-bias correction | `legacy+edge` — better on 6 profiles, over-corrects at low SNR |
> | Peak-reference fix | `legacy+peak` — hand-keyed 0.1579 → 0.0628, but `qsb` regresses 43× |
> | Threshold reference (Phase 16) | 4 experiments; `+peakdual` 7 better / 2 worse; no promotion |
> | Transition-gated peak (Phase 17) | `+peakgate` — 1 better / 8 worse; refuted its own premise |
> | Guard probe (Phase 18) | confirmed Phase 17's mechanism; key-up instant attack is load-bearing |
> | Guard sweep (Phase 19) | guard is honest, 1.8 Pareto-optimal; detector line closed |
> | Test hardening | every Phase 16–19 sweep had a non-failing assertion; all replaced (§13.11) |
> | Farnsworth gap centres (Phase 21) | cause found (one cold-start gap, not the clustering); fix **not shipped** — fragile by 0.5 ms and breaks a gate. Retro gap seeding promoted instead: `qrm` 0.0100 → 0.0035 (§16) |
> | Gap classification under noise (Phase 22) | ⊘ item closed, measurement only. Error multiplication **confirmed**: 12.2% of structurally intact gaps misclassified at noise 3.0 vs 0.0% clean — but the driver is a **+38.9% dit overestimate**, not gap fragmentation. Two new measured defects; #29 reframed (§17) |
> | Real recordings — ARRL W1AW (Phase 23) | **#30 closed as a blocker.** Paired audio + published text, five sessions pinned and gated; 15 and 20 WPM decode at **CER 0.0000**. Found and fixed two defects the synthetic suite cannot express: a broken audio→IQ conversion and unmapped period/comma (§18) |
> | LR detector — promoted then reverted (§21, §25) | `legacy+lr+log`: a sequential CUSUM detector that rejects spikes by evidence duration, not magnitude. Solved the §20.8 log-timing runaway (noise3.0 +0.97 t=18 → −0.11) and won on hand-keyed/worstcase — **promoted 2026-07-21, reverted the same day** when the gate's noise axis was widened past 15 WPM and exposed a large fast-CW × noise regression (25 WPM/noise2.0 +0.20 t=18, 30 WPM +0.33 t=33). Kept as a variant; `legacy` restored as default |
| LR fast-CW fix attempts — all failed (§26, §27, §28) | Three routes to close the fast+noise regression, each a measured dead end. **§26 speed-adaptive bound** (scale by self-measured element duration): helps fast, breaks slow+heavy noise — the estimate is corrupt where the safe bound matters. **§27 external ground-truth speed**: removes §26's breakage by construction but recovers only ~70% of the regression; a residual fast+heavy gap to legacy survives even with a perfect speed signal. **§28 SNR (noise-side) scaling**: refuted by the CER-vs-bound curve — fast+heavy wants a *small* bound, slow+heavy a *large* one, and SNR (low for both) cannot separate them. Bound-scaling is exhausted on every axis; the residual is intrinsic to the LR CUSUM |
| Measurement repair (Phase 25 = §20) | The comparison method, not the decoder. Paired per-seed testing added (`comparePaired`); n=24 shown to give wrong verdicts in both directions; the four-variable confound between the `noise*` and `handkeyed*` families identified and separated by a speed×noise factorial; hand-keyed coverage found to stop at 25 WPM. **Every better/worse tally in Phases 12–24 is unreliable as recorded.** No decoder change |
| Registry vs real audio (Phase 24 = §19) | `legacy+mf` fixes the 35 WPM errors (0.0089 → 0.0000), so §12.3's refutation was scope-limited — but timing-only cores fix them too, so they are **marginal**, not proof of the matched-filter mechanism. **Nothing promotable**: every candidate regresses on ≥1 synthetic profile; the real-audio winner doubles CER on heavy noise (§19) |
>
> **Next: repair the instrument before ranking anything else with it.** Phase 25
> found that the comparisons every ranking in these documents rests on were not
> capable of supporting them. Four defects, all measured:
>
> 1. **n=24 is too few.** Verdicts flipped at n=96 in both directions — a
>    `noise3.0` regression missed (t 1.91 → 5.43), two hand-keyed improvements
>    missed (t −0.47 → −3.29, −1.09 → −3.63). Baselines moved too: `legacy` on
>    `noise2.0` reads 0.0827 at n=24 and 0.1227 at n=96.
> 2. **Better/worse was counted on means with a 1e-6 float epsilon.** Under a
>    paired per-seed test `legacy+edge` goes from "7 better / 3 worse" to
>    3 better / **0 worse** / 9 not-significant; `legacy+mf` goes from
>    "5 better / 3 worse" to **nothing significant on any profile**. The `qrn`
>    regression that blocked `+edge` was 3 differing seeds in 96.
> 3. **The profile families confound four variables at once.** `noise*` is
>    15 WPM / no jitter / no weight bias; `handkeyed*` is `noiseAmp` 0.3 with
>    jitter 0.15 and bias 0.1. No claim about "noise" can be drawn by comparing
>    them. A speed×noise factorial separates them: jitter is *not* the driver,
>    noise is — but the failure magnitude is concentrated at slow speed.
> 4. **Speed coverage stopped at 25 WPM**, and 40 WPM is where candidates
>    diverge most (0.17 CER between `+log` and `+mf`).
>
> The real-recording matrix has the same defect in a worse form: it is **n=1**
> per cell with no variance estimate, and it ranks the top three cores by
> differences of about one character in total.
>
> Reprioritized leftovers follow the Approaches Matrix.
>
> **Standing context.** Phase 21 found the Farnsworth cause exactly but shipped a
> different fix: the cold-start correction is fragile by 0.5 ms and breaks a
> gate, so `farnsworth-2.0` stays at 0.0141 (§16.2). Retro gap seeding shipped
> instead (`qrm` 0.0100 → 0.0035). ✓ Gap classification under noise is now
> measured (§17), closing the §16.4 ⊘ item. The detector
> line is closed — see Phase 19. Oracle ablation confirms 100% of the
> AWGN/QSB/QRN error is at the detector, and Phase 15 measured that the detector
> *also* holds 67% of the hand-keyed headroom — an earlier reading of hand-keyed
> as a pure timing fault was too strong. `worstcase` needs the detector and the
> duration model fixed together.

## Current State (after Phase 1-10 implementation)

### Implemented

**Signal Processing:**
- **Kalman filter timing** — state = dit duration + uncertainty. Gaussian likelihood ratio for dit/dah classification. Measurement noise adapts from residuals. R floor stated as "widened for faster WPM (ditEst * 0.05)" — ✗ **it does not do that**: it evaluates to 4 ms² at both 15 and 30 WPM (investigation §2.1d), and correcting it measured as an exact no-op (§8). Clamped to 34-150ms (8-35 WPM).
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
> ⚠ **These do not match `decoder-investigation-2026-07.md` §3.1 (0.158 / 0.200 /
> 0.131), and that is not a contradiction.** This ablation deliberately holds
> the message fixed at `MSG_CQ()` so that *seed count alone* varies; §3.1 uses
> `MSG_FULL()` throughout. On hand-keyed 15 WPM the two effects separate as
> 0.0000 (1 seed) → 0.1341 (24 seeds, same message) → 0.1579 (24 seeds,
> MSG_FULL) — i.e. seed count contributes 0.134 and message length a further
> 0.024. Quote §3.1 for baselines; quote this table only for the seed-count
> ablation.
>
> **Status: the jitter rows are superseded** — see
> `decoder-investigation-2026-07.md` §3.1 for the 24-seed baseline. The clean,
> mild-noise, moderate-noise and Farnsworth-1.5 rows were re-measured at 24
> seeds and **hold at 0.000**.
>
> **Superseded by 24-seed measurement (Phase 21, §16).** The remaining rows now
> have multiseed values; the single-seed figures below understate several of
> them and are kept only for history:
>
> | profile | this table (1 seed) | 24-seed mean |
> |---|---|---|
> | QSB | 0.0 | 0.0117 |
> | QRM | 0.0 | **0.0035** (was 0.0100 before retro gap seeding) |
> | QRN | 0.0 | 0.0023 |
> | Contest | 0.0 | 0.0031 |
> | Farnsworth 2.0 | 0.043 | **0.0141** |
> | Worst case | 0.57 | 0.6232 |
>
> Note the direction: single-seed reported **0.0** for four profiles that are
> non-zero over 24 seeds, and *overstated* Farnsworth 2.0 by 3×.

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
| Farnsworth ratio 2.0 | 0.043 | ⚠ misattributed — adaptive gap centres are **not** the fault; the error is one cold-start gap (§16.1). 24-seed value is 0.0141 |
| Worst case (all combined) | 0.57 | Improved from 0.61 by corrector (+Q→CQ) — ⚠ see note |
| Worst case long message | 0.45 | More data → better timing |

**Test suite: an always-on battery plus opt-in `[.]` sweeps.**

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
| Full worst case | 0.65 | Dominated by noise+QSB — ⚠ see note |

> ⚠ **`worstCase` appears in this document as 0.57, 0.61 and 0.65.** All three
> are single-seed (`seed = 42`) figures captured at different points in Phases
> 1–10, against a decoder that changed between them; they were never one
> measurement. They are left as recorded rather than harmonised, because
> retro-fitting a single number would invent a consistency that never existed.
>
> The current figure is **0.6244** (24 seeds, MSG_FULL,
> `decoder-investigation-2026-07.md` §3.1), with an oracle bound of **0.0792**
> (§11) and a best measured variant of **0.4630** (§13.2).

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
> implementation*, not measurements. Rows #14–16, #25 and #27 in particular carry
> no data (#24 is now heavily measured — §21–28). Rows marked **Done** have
> measured CER in the results table above or in `decoder-investigation-2026-07.md`.
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
| 24 | Likelihood-ratio detector | Medium | High | High | **DONE — promoted then REVERTED (§21→§25); boundary fully mapped (§26–28).** Sequential CUSUM (`legacy+lr+log`) rejects spikes by evidence duration, solving the §20.8 runaway (noise3.0 +0.97 t=18 → −0.11) and winning on hand-keyed/worstcase. Reverted when fast CW × noise was found to regress hard (25 WPM/noise2.0 +0.20 t=18, 30 WPM +0.33 t=33) — the CUSUM's fixed ~12 ms lag is too large a fraction of a short fast element. §26 (speed-adaptive) / §27 (external speed) / §28 (SNR) all failed to close it: bound-scaling is exhausted, the residual is intrinsic, Schmitt handles fast+heavy better at any bound. **Kept as a variant; `legacy` is the default.** ⚠ heavy-noise wins only ever validated against Gaussian noise |
| 25 | Gap ambiguity inside the beam | High | Medium | Medium | Candidate |
| 26 | Callsign database (SCP/Master.dta) | N/A | N/A | Low | Candidate |
| 27 | Bell 1977 trellis core | High | Very High | Very High | Future |
| 28 | Oracle ablation harness | N/A | N/A | Low | **Done (2026-07)** |
| 28b | Detector ground-truth metrics | N/A | N/A | Low | **Done (2026-07)** |
| 28c | Matched-filter resize fix | Low | Low | Low | **Measured — variant `+mf`, not promoted** |
| 28d | Schmitt edge-bias correction | High | Medium | Low | **Measured — variant `+edge`, not promoted** |
| 28e | Symmetric Schmitt thresholds | N/A | N/A | Low | **Refuted (2026-07)** |
| 28f | Percentile threshold reference | High | High | Low | **Measured — variant `+peak`, not promoted** |
| 28g | Attack-constant sweep for the peak reference | N/A | N/A | Low | **Refuted (2026-07)** |
| 28h | Percentile window length sweep | High | Medium | Low | **Measured (2026-07)** |
| 28i | Dual-window peak reference | High | Medium | Low | **Measured — `+peakdual`, not promoted** |
| 28j | Transition-gated instant-attack peak | N/A | N/A | Low | **Refuted (2026-07)** |
| 28k | Probe the `dynamicRange < 1.8` guard rejection rate | N/A | N/A | Low | **Done (2026-07) — confirmed §13.8** |
| 28l | Sweep the 1.8 guard constant | N/A | N/A | Low | **Done (2026-07) — guard is honest, 1.8 optimal** |
| 29 | Adaptive gap centres rewrite (Farnsworth) | N/A | Low | Low | **Candidate — measured 0.0141 → 0** |
| 30 | Real-recording benchmark + model-mismatch profiles | N/A | N/A | Medium | **Done (Phase 23)** — closed as data collection, open as validation for #24 |
| 31 | WPM-locked adaptive BPF (runtime tap update) | N/A | High | Medium | **Thesis validated on real audio, runtime rule blocked (§29–32).** Ground-truth ceiling (`[bpf-wpm]`/`[bpf-noise]`): 6–7 wins, 0 regressions, noise3.0 0.817 → 0.075. Noise-aware fix cleared the hand-keyed flags (§29). Calibrated to dB (§31). **Real audio (§32) is the verdict:** a fixed narrow BPF cuts real W1AW CER 6× at moderate noise (0.121 → 0.021, t=−19) — the thesis transfers — **but the runtime core `legacy+bpfauto` does nothing on real audio**, because its `getSNR` trigger (synthetic-calibrated) never fires post-BPF on real 20 WPM. Promotion blocked. Next: a narrowing trigger that transfers (input-referred noise, or WPM-driven + over-narrowing guard) |
| 32 | Paired per-seed comparison + factorial profiles | N/A | N/A | Low | **Done (Phase 25)** — `comparePaired` in `cw_bench_stats.h` |
| 33 | Guarded log timing (x-freeze for spikes) | Low | High | Low | **Measured, not promoted (§20.8)** — `+logguard`. Halves the noise3.0 runaway, useless at noise4.0; R-saturation is inseparable from qrm/qrn tolerance |
| 34 | Speed-adaptive LR bound (self-estimate) | Medium | High | Medium | **Refuted (§26)** — `+lr+log+adapt`. Scales the CUSUM bound by self-measured element duration to cut the fixed lag on fast CW. Helps fast, breaks slow+heavy noise: the estimate is built from key-down durations the spurious short elements dominate, so it reads "fast" when slow and revives the runaway |
| 35 | External ground-truth speed to the LR bound | Medium | High | Low | **Hypothesis check, necessary but not sufficient (§27)** — `setExternalDitMs`, `[lr-wpm]`. Feeds the detector the true dit instead of the self-estimate. Removes §26's slow-noise breakage by construction (15 WPM scale ≡ 1.0) and recovers ~70% of the fast regression, but a residual fast+heavy gap to legacy survives even with a perfect speed signal. No shipping path (no decoder knows true WPM); does not unblock promotion |
| 36 | SNR (noise-side) LR bound scaling | Medium | High | Low | **Refuted by the bound curve (§28)** — `[lr-snr]`. Fast+heavy noise wants a *small* bound (lag dominates), slow+heavy a *large* one (rejection dominates); SNR is low for both and cannot separate them, so a noise-side rule hurts the target. Even the floor bound leaves fast+heavy above legacy: the residual is intrinsic, not a tuning miss |

### Reprioritized leftovers (2026-07-21, after §28)

Ordered by strength of the evidence that the work is needed, not by expected
gain. The list changed twice: Phase 25 (§20) repaired the ranking instrument, and
§21–28 exhausted the detector line — the LR detector was promoted, reverted, and
its fast+heavy-noise boundary mapped from every bound-scaling angle. Two former
top items are now closed, which promotes the post-decode and front-end ideas.

| rank | item | why here |
|---|---|---|
| 1 | **#31 WPM-locked adaptive BPF** | **Ceiling now measured and it is the campaign's largest win (§29):** 7 significant wins / 0 significant regressions vs legacy at n=96, noise3.0 11×, and it closes the fast+heavy residual §21–28 could not. Front-end mechanism, so §28's "bound-scaling exhausted" does not apply. **Two steps left:** (a) a noise-aware bandwidth floor to clear the 6 ns hand-keyed non-inferiority flags; (b) the runtime WPM-lock with a glitch-free filter transition (cf. §12 MF-resize) |
| 2 | **#25 gap ambiguity inside the beam** | §17 measured 12.2% of structurally intact gaps misclassified at `noise 3.0`, errors flowing toward CHAR. A measured defect, and — unlike the detector bound — untouched by §21–28. Addresses the segmentation that the fast+heavy residual partly comes from |
| 3 | **#26 callsign database** | Low complexity, low risk, post-decode only — cannot regress the detector. The safest remaining lever now that detector-side gains are exhausted |
| 4 | **LR detector as a non-default variant** | ⊘ Open question, not a task: the LR detector *wins* on hand-keyed and worstcase and *loses* only on fast+heavy noise. Worth deciding whether it ships as a user-selectable option (not the default) for operators who know they are on clean hand-keyed signals. Needs a UI/config decision, not more measurement |
| 5 | **#14 ATC, #15 coherent PLL** | No data and no oracle headroom estimate. The §10 scorecard puts pre-implementation predictions near 50% |
| 6 | **#16 LSTM, #27 Bell trellis** | Future — a different detector class, the only lever §28 leaves open in principle (a new decision rule, not a new bound) |

**Closed since the 2026-07-20 list:**
- ~~Finish Phase 25~~ — **done** (§20): paired test in `[promotion]`, `handkeyed-30/35/40` added, `noise*`/`handkeyed*` decoupled by the factorial, real-audio variance noted.
- ~~Runaway insertion at slow speed × heavy noise~~ — **resolved** (§21). The LR detector cut the log-timing runaway (noise3.0 CER 1.77 → 0.71); and the shipped default (`legacy`, Kalman) never had it — the runaway was a log-timing property. The linked +38.9% dit overestimate is also resolved (§23).
- ~~#24 likelihood-ratio detector~~ — **done and boundary-mapped** (§21–28). Not promotable as the default; see the Approaches Matrix #24 and the variant row above.
| 7 | **#14 ATC, #15 coherent PLL** | No data and no oracle headroom estimate. The §10 scorecard puts pre-implementation predictions near 50% |
| 8 | **#16 LSTM, #27 Bell trellis** | Future |
| — | **Noisy real recordings** (was rank 1) | **Demoted.** It would validate §15's synthetic fidelity; it would not resolve a tradeoff, because the candidates disagree with each other in the regime the recordings would add, not in the one they would confirm |

### Results of 2026-07 work (24 seeds, MSG_FULL, mean CER)

> ⚠ **CONTESTED by Phase 25 — this table is n=24.** Kept because Phases 12–19
> were decided against these numbers, so it is the historical record. At n=96,
> `legacy` on `noise 2.0` is 0.1227 (not 0.0827) and on hand-keyed 25 WPM is
> 0.1536 (not 0.1309). Do not use any row here to rank a variant; re-measure
> with `comparePaired`.

`legacy` is unchanged and **is the default.** `legacy+lr+log` was promoted to
`DEFAULT_CORE` on 2026-07-21 (§21) and reverted the same day (§25) — the
campaign has no standing promotion. Full tables and method:
`decoder-investigation-2026-07.md` §8–10, §21 (promotion), §25 (revert).

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

### Phase 15: Detector Correction (DONE, 2026-07 — no promotion)
Full results `decoder-investigation-2026-07.md` §12.5–12.6.

- **The ON-stretch mechanism was misattributed and is now measured.** The
  threshold ratio gap accounts for 11% of it; **instant-attack peak tracking
  accounts for 57%**. `legacy+sym` removed the ratio gap entirely and moved the
  bias by only a tenth of what the model predicted.
- **Correcting it is worth a lot.** `legacy+peak` takes handkeyed-15 from 0.1579
  to **0.0628** — 67% of the available headroom, and equal to the *oracle*
  detector's own 0.0651 on that profile. Largest single-change improvement so far.
- **This amends the §11 reading.** "Hand-keyed is a timing problem" described the
  oracle *combination*, not the attribution: a perfect duration model masks
  detector error rather than proving there is none.
- **Nothing promoted.** `+peak` regresses `qsb` 43× (2 s window vs a 3.3 s fade);
  `+edge` over-corrects at low SNR because it is scaled by the ratio gap, which
  is not the cause; `+sym` is catastrophic — hysteresis is load-bearing.

### Phase 16: Threshold Reference (DONE, 2026-07 — no promotion)
Four experiments. Full results `decoder-investigation-2026-07.md` §13.

- **The timescale framing was refuted.** An asymmetric EMA with a slow attack is
  not a peak tracker — it converges to the *mean* envelope. ON stretch rises
  monotonically with the attack constant (+9.80 → +36.53% of a dit); clean
  15 WPM, perfect everywhere else, breaks entirely at 800 ms.
- **The property that matters is element-independence, not timescale.** A
  percentile window spans many key cycles; an EMA of any constant varies within
  the element.
- **Window length is the right knob and its mechanism holds** — QSB improves
  monotonically as the window shortens, 0.5070 → 0.0194.
- **But window length trades tracking against variance.** Short windows follow
  fading, long windows estimate precisely (~1/N). No single length serves both.
- **Dual windows make it adaptive**, but no single configuration gets both:
  - `250/1` reaches **qsb 0.0088** (better than legacy) — 6 better / 4 worse
  - `500/1` reaches **7 better / 2 worse**, the best spread — but its qsb is
    **0.1215**, 10× worse than legacy
  The switch cannot cleanly separate a fade from estimator noise.
- **No promotion.** Two trade points kept: `legacy+peakdual`, `legacy+peakdual16`.

### Phase 17: Transition-Gated Peak (REFUTED)
Premise: instant attack's only defect is chasing the rising edge, so enable the
attack update only while the key is confirmed down — keeping the fade tracking
every Phase 16 estimator gave up. Built as `legacy+peakgate`.

- **1 better / 8 worse.** No promotion.
- **The premise was false.** `+gate` reproduces legacy's ON stretch *exactly*
  (9.80 / 7.94 / 6.96 on the clean and hand-keyed profiles). Removing key-up
  attack entirely moves the stretch by nothing, so key-up attack never caused
  it: with a 0.5 s decay the reference still holds ~85% of the previous
  element's amplitude across an 80 ms gap, well above the ≈0.47·A trigger.
- **Damage is misses, not false events.** snr-noise2.0 misses 0.019 → 0.856
  (45×) while false events *fell* on every degraded profile.
- ⊘ Suspected but unmeasured: legacy's key-up instant attack is load-bearing,
  holding `peakRef` above the `dynamicRange < 1.8` guard that otherwise disables
  detection outright. That would be a defect in `legacy`, not in the variant.

Full tables: `decoder-investigation-2026-07.md` §13.8.

### Phase 18: Probe the 1.8 Dynamic-Range Guard (CONFIRMED)
Counted guard rejections and intersected them with true key-down time, shifted
by the measured group delay.

- **Phase 17's mechanism confirmed.** Blindness during key-ON tracks the miss
  rates profile by profile, negatives included: snr-noise2.0 0.00% → 40.65%,
  worstcase 0.84% → 45.26%, and 0.00% on all four profiles where the gate cost
  nothing.
- **Rejecting ≠ rejecting where it matters.** On snr-noise2.0 legacy rejects
  1.64% of samples but is blind during key-ON 0.00% of the time — every legacy
  rejection lands in key-up. Instant attack during key-up is load-bearing: it
  holds `peakRef` above the guard so the detector stays armed.
- ⊘ **New, about `legacy`:** it is blind for 55.36% of key-down time on
  snr-noise4.0 and 7.56% on snr-noise3.0 — the two profiles where it fails
  outright. Cause or symptom is untested.

Full tables: `decoder-investigation-2026-07.md` §13.9.

### Phase 19: Sweep the 1.8 Guard Constant (GUARD IS HONEST)
Swept 1.0–3.0. That g=1.0 disables the guard, and that g=1.8 reproduces
registry `legacy` exactly, are both asserted in the test rather than assumed.

- **Not a defect.** At g=1.0 blindness is 0.00% on every profile and CER does
  not recover: snr-noise4.0 *worsens* 0.9032 → 0.9173 with 55.4 pp of blindness
  removed; snr-noise2.0 more than doubles, 0.0827 → 0.1966. Legacy's high-noise
  failure is upstream of the guard.
- **1.8 is Pareto-optimal.** No swept value dominates it — each buys 2–3
  profiles and loses 4–6 (1.0 → 3/4, 2.2 → 3/5, 3.0 → 3/6). First constant in
  this codebase swept and found already correct.
- **Third instance of stage metric ≠ objective**, and the strongest: 55 pp of a
  stage metric eliminated, objective moved 0.014 the wrong way.
- ⊘ Why suppression helps (silence beating garbage) is plausible but untested —
  this sweep records CER only.

Full tables: `decoder-investigation-2026-07.md` §13.10.

### Phase 20: Real Recordings (SUPERSEDED — delivered by Phase 23)
Kept for the reasoning that motivated it. The prerequisite argument below is
still sound but is **only half satisfied**: Phase 23 delivered paired audio and
published text, but all five sessions are 19–76 dB SNR. A likelihood-ratio
detector is a noise technique, so the recordings do not exercise the regime
#24 targets. #30 is closed as a data-collection blocker and open as a
validation one.

Phases 15–19 closed the detector line without promoting anything: every
candidate defect in it is now either measured and load-bearing, or measured and
absent. The remaining hand-keyed headroom is known not to be reachable by any of
the five peak estimators or by the guard. What is left is #30 — real recordings
and model-mismatch profiles — which is data collection rather than another
experiment against a generator whose fidelity is itself unvalidated. Farnsworth
gap centres (#29) remains the alternative if the goal is to promote something.
Scoped by Phase 13 to the AWGN/QSB/QRN family. `tone_detector.h` applies a
fixed-fraction Schmitt threshold to a magnitude envelope and emits a hard binary
decision, discarding the soft information before timing ever sees it.

**Prerequisite (#30):** the generator adds complex Gaussian noise, so its
envelope is exactly Rician — the model a likelihood-ratio detector assumes.
Benchmarked on this suite alone such a detector is being tested against its own
generative assumptions. Real recordings and model-mismatch profiles are required
before committing to the rewrite, not after.

Planning: `decoder-investigation-2026-07.md` §13.7 and §14.

### Phase 21: Farnsworth gap centres (#29) — CAUSE FOUND, FIX NOT SHIPPED
The plan framing was stale: 2-means gap clustering already existed and converges
to the generator's centres exactly at every ratio. The entire `farnsworth-2.0`
deficit is **one gap** — the first char gap, classified inside the cold window
against `dit*3`/`dit*7`, a hardcoded ratio-1.0 assumption. Decoded output is
`'C Q CQ CQ ...'`: one spurious space, 1/71 = 0.0141.

The cold-start bootstrap that fixes it fires by **0.5 ms** of margin and breaks
the `worstcase` gate (0.6087 > 0.6) when combined with the retro fix — while that
same profile's 24-seed *mean* improved, which is why the single-seed gates exist.
At ratio 2.0 a stretched char gap (6·dit) and a standard word gap (7·dit) are
near-indistinguishable from one observation: an information limit. `retroDecode`
cannot help (the gap is post-lock, `RETRO events=8`), and delaying lock to reach
it is known harmful. **#29 is closed as understood-but-not-fixable** without the
log-duration reformulation, where Farnsworth becomes an additive shift (§5.3b).

**Shipped instead:** retro gap seeding — `makeFresh()` restored the element model
but dropped the gap model, so every replay re-entered a cold gap window. 5
profiles better, 1 worse, all gates pass, `qrm` 0.0100 → **0.0035**. Details: §16.

### Phase 25: Measurement repair (2026-07-20 — method, no decoder change)

Full write-up, tables, and the re-adjudication: **`decoder-investigation-2026-07.md`
§20.** In brief:

- **Paired per-seed testing** (`comparePaired`, `cw_bench_stats.h`) replaces the
  1e-6 mean-difference epsilon at every promotion-counting site. Zero-variance
  deterministic profiles are decisive (`t=±∞`), not `ns`.
- **n=96 for decisions** (`[promotion]`), n=24 kept for the survey matrix. Three
  §19 verdicts flipped at n=96, in both directions.
- **Non-inferiority gate:** promotion requires the upper 95% bound on any
  regression under 0.005 CER — significance alone is absence of evidence, not
  evidence of absence. This overturned "`legacy+mf` has no measurable effect":
  10 of 16 profiles are underpowered, not equivalent.
- **Orthogonal speed×noise factorial** (`profileFactorial`, `[factorial]`) plus
  `handkeyed-30/35/40`: `legacy+edge+log` trades noise robustness for timing
  robustness — two orthogonal axes, opposite signs.
- **Noise-augmented real audio** (`[recording-noise]`): the §19 tradeoff
  reproduces on real keying; `+edge` regresses at t=4.33, `+edge+log` emits
  CER > 1.0.
- **Parallel harness** (`cw_parallel.h`): 6× faster, byte-identical output.

Still nothing promotable — the §19 conclusion, now on defensible evidence.

**Both open items are since closed.** The runaway-insertion mode (CER > 1.0) was
cut by the LR detector (§21, noise3.0 1.77 → 0.71) and is absent from the shipped
`legacy` path entirely (it was a log-timing property). The `25 WPM × noise 2.0`
anomaly dissolved under a fine speed sweep (§25): there is no narrow spike at
25 WPM — `legacy+lr+log` degrades monotonically across the fast range under noise,
which is the regression that reverted the promotion. §26–28 then confirmed that
regression is not closable by any bound rule. The detector line is closed.
