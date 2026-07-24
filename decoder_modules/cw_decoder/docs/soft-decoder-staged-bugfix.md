# CW Soft-Decoder — Staged Bug-Fix Plan

> **Goal:** bring the SOFT (Bayesian forward-HMM, `fb`) decoding path to bug-free
> parity-or-better with the HARD reference across a ladder of increasing signal
> complexity, **one level at a time**, fixing the true mechanism of every issue —
> no workarounds. When every level here is green, roll the outcome up into
> `decoding-improvement-plan.md` as a global stage.
>
> Started 2026-07-24 (see `decoding-improvement-plan.md` §54 for how we got here:
> word-correction disabled → raw decode debugging → the field says soft > hard).

## 1. Principles (non-negotiable)

1. **Evidence only. No assumptions.** Every claim — "bug", "fixed", "not a bug" —
   must be backed by a reproducible measurement. In a signal-processing context we
   trust statistics, not intuition.
2. **"Not a bug" is a JOINT decision.** If the evidence suggests an issue is
   fundamental (a real tradeoff, not a defect), STOP and review it together before
   concluding. Never close an issue as not-a-bug unilaterally. (Precedent: the fb
   hand-keyed gap was assumed a "tradeoff" and was actually a bug — §54.)
3. **No workarounds.** Fix the mechanism, not the symptom. A change that hides a
   symptom (dictionary repair, threshold tuned to a test, blanket substitution) is
   forbidden — it re-masks the bug one layer down.
4. **Level-gated.** Do not advance to level N+1 until level N is resolved (soft ≥
   reference, no open bugs at that level). A higher level cannot be trusted while a
   lower one is broken.
5. **Statistics best practice.** ≥24 deterministic seeds; report mean, stderr,
   worst, collapse-count. An issue is real when the soft-vs-reference gap exceeds
   **2·stderr**, or when soft ≠ 0 at a level where the reference is 0.
6. **Metric discipline.** Judge on **element-error rate excluding the first symbol**
   (content correctness), not raw CER. CER over-weights word-gap spacing and the
   parked initiation artifact (§54). Report CER too, but decide on element errors.
7. **Gates stay tight.** Never loosen a benchmark bound to admit a change
   ([[never-loosen-quality-gates]]).
8. **Never overfit to the noise generator.** The synthetic ladder is a *controlled
   probe*, NOT the target distribution. A fix or check must target the general
   signal-processing mechanism; anything that adapts to a quirk of `generateMessage`
   / the synthetic noise model — and could therefore *reduce real-world quality* — is
   a workaround and is forbidden. Prefer validation against theory and real recordings
   (`[replay]`). If a change only helps synthetic seeds, suspect overfitting.
9. **Theory-grounded logic changes only.** `fb` is an implementation of the Bayesian /
   HMM soft-decoding approach (CW Skimmer lineage). Any change to its logic MUST be
   justified by **proven public theory** — Bayesian inference, HMM forward/backward,
   matched filtering, ML sequence estimation, clock/speed recovery — with the principle
   cited and **why it matters explained in terms the reviewer can verify**. An empirical
   "it lowers CER on these seeds" with no theoretical basis is suspect (likely
   overfitting, Principle 8) and requires joint review before adoption.
10. **Decision-support format (human-in-the-loop efficiency).** Human review is the
    bottleneck, so every proposed logic change is prepared IN FULL before it is applied,
    as a **Decision Package** (§10): (a) Reason — the theory + why it matters; (b) Known
    risks / side-effects; (c) Evidence already gathered (matrix results); (d) Prepared
    test checks, written and ready to run, that will confirm/refute; (e) Recommendation;
    (f) a Decision Record row. The reviewer then approves/declines/adjusts in one pass
    with full context. Do not apply a logic change before its package is reviewed.

## 2. Approaches under comparison (the pair matrix)

The reference is the HARD decoder, treated as the (provisionally) bug-free oracle —
"if hard decoding works, it only indicates it is *potentially* free of bugs."

| id | approach | detector | timing | front-end | role |
|----|----------|----------|--------|-----------|------|
| **HARD** | `legacy+select` | Schmitt (hard) | SELECT (adaptive) | BPF 100/100 | reference oracle |
| **SOFT** | `legacy+fb+sel` | fb forward-HMM (soft) | SELECT (adaptive) | BPF 140/140 + smooth | path under debug |
| iso-T | `legacy+fb` | fb (soft) | **Kalman** | fb | timing-isolation control |
| iso-legacy | `legacy` | Schmitt (hard) | Kalman | 100/100 | detector-isolation control |

**Isolation pairs** (how we localize a root cause without guessing):
- **SOFT vs HARD** = product-level soft-vs-hard. NOTE the confound: they differ in
  BOTH detector AND front-end (BPF/smoothing). A gap here is "somewhere in the soft
  path", not yet localized.
- **SOFT vs iso-T** (`fb+sel` vs `fb`) = same detector, SELECT vs Kalman timing →
  isolates **timing**. (This pair found the half-speed collapse, ISSUE-1.)
- **iso-legacy vs HARD** (`legacy` vs `select`) = same Schmitt detector, Kalman vs
  SELECT timing → isolates **timing** on the hard side (control).
- For a pure **detector** isolation we currently lack a "Schmitt + fb-front-end" or
  "fb-detector + 100/100-front-end" variant. If a bug needs it, BUILD a controlled
  variant (registry) rather than reason around the confound.

## 3. The complexity ladder (levels)

Monotonic increase in difficulty. Machine-keyed (perfect timing) noise ladder first;
then introduce hand-keyed jitter; then real off-air. First symbol excluded throughout
(initiation artifact is a SEPARATE parked track — §5).

| level | name | profile(s) | reference expectation |
|-------|------|-----------|------------------------|
| **L0** | clean, noise-free | `profileClean` (15/20/25/30 wpm), noiseAmp 0 | CER = 0 both; ANY soft error = pure bug |
| **L1** | light AWGN | mild-noise, snr-noise1 | reference ~0 |
| **L2** | moderate AWGN | moderate-noise, snr-noise2 | reference low |
| **L3** | heavy/buried AWGN | snr-noise3, snr-noise4 | soft SHOULD win (its purpose) |
| **L4** | QSB (slow fade) | qsb | — |
| **L5** | QRM (interfering CW) | qrm | — |
| **L6** | QRN (impulse) | qrn | — |
| **L7** | hand-keyed jitter | handkeyed 15/20/25/30, then + noise | RSCW-tradeoff regime |
| **L8** | near-real-world | combined impairments + real baseband recordings (`[replay]`) | ground truth = operator copy |

Each level is tested across the WPM range where applicable (15/20/25/30) so a
speed-dependent bug (e.g. the half-speed octave trap) cannot hide.

## 4. Per-level procedure

For each level, in order:

1. **Measure** SOFT and HARD (≥24 seeds): element-error (excl 1st), CER mean±stderr,
   worst, collapse-count. Record in the level's results block.
2. **Detect issues**: any level where soft ≠ 0 (L0) or soft − reference > 2·stderr.
3. **For each issue** — open a matrix row and:
   a. **Reproduce** on specific seeds (deterministic).
   b. **Root-cause with evidence**: dump decodes, trace internal state over time
      (WPM, SNR, posterior, EM stats), isolate via the pairs. Quote the numbers.
   c. **Classify**: bug (systematic/mechanistic → fixable) vs candidate-not-a-bug.
      A candidate-not-a-bug triggers **joint review** (Principle 2) — do not close it.
   d. **Fix the mechanism** (no workaround). Re-measure.
   e. **Regression-check** this level AND all lower levels stay green.
4. **Gate**: advance only when every issue at this level is Fixed/Confirmed and
   soft ≥ reference (element errors) at the level.

## 5. Parked track — decoder initiation (first symbol)

The first symbol is a known cold-start artifact (`+Q` on hard, `TRQ` on soft — §54),
worse on soft. It is NOT a level issue: excluded from all level metrics via
first-symbol-excluded scoring. It has its own future fix (low-confidence BACKWARD
correction once params are known — signal-level, not vocabulary, not BPF-narrowing).
Tracked here so it is not forgotten, but it does not gate the ladder.

## 6. Issue matrix (living)

Status: OPEN → ROOT-CAUSED → FIXED → CONFIRMED (fix + no regression) | REVIEW (candidate not-a-bug, awaiting joint review).

| ID | level | isolation pair | symptom | root cause | bug? | fix (mechanism) | status | evidence |
|----|-------|----------------|---------|-----------|------|-----------------|--------|----------|
| **1** | L7 | SOFT vs iso-T | fb collapses to garbage on 3/24 hand-keyed seeds | plain Kalman timing **half-speed-locks** (~8 vs 15 wpm); forward-only can't recover | **YES** | pair fb detector with adaptive SELECT timing (`legacy+fb+sel`) — proper timing, not a workaround | **CONFIRMED** | `[fbcollapse]` 3→0/24; `[fbtrace2]` wpm stuck 8; `[fbisolate]`; `[fbsel]` CER 0.139→0.092; `[fbsel-full]` strict-improvement all regimes |
| 2 | L0 | SOFT vs HARD | does SOFT decode clean noise-free at 0 errors? | n/a — SOFT is clean | **NO** | — | **CONFIRMED green** | `[L0-clean]` 2026-07-24: both cores 0 ELEM / 0 gap / CER 0.0000 at 15/20/25/30 wpm. Bonus: CER 0 incl. 1st symbol → the `+Q`/`TRQ` initiation artifact is **noise-triggered**, absent noise-free |
| 3 | L2 | SOFT vs HARD | moderate-noise element errors 23 vs 2 (excl 1st) | — | ? | — | OPEN | `[fbsel-elem]` |
| 4 | L4 | SOFT vs HARD | qsb element errors 71 vs 17 (excl 1st) | — | ? | — | OPEN | `[fbsel-elem]` |
| 5 | L7 | SOFT vs HARD | hand-keyed-15/20/25 element errors 127/158/134 vs 43/97/89 (excl 1st, collapse-free) | — | ? | — | OPEN | `[fbsel-elem]` |

> **CURRENT FRONTIER: L1.** L1 normal-speed is NEAR-GREEN on real ground truth:
> `[L1-real]` w1aw_15wpm fb+sel 0.0055 vs select 0.0000 (single punctuation slip
> `BRACKETS.`→`BRACKETSE=`). Real W1AW is the Principle-8 cross-check for machine-keyed
> levels L0–L6 (L7 hand-keyed has NO real ground truth — logged gap). The real deficit
> is NOT broad-clean but a **slow-speed bug → ISSUE-6**.
>
> **ISSUE-6 (slow-speed over-segmentation) — PARKED (real-audio-only, niche).**
> Investigated with evidence, NOT assumed: `[L0-slow]` shows synthetic-clean ≤8 WPM fails
> BOTH cores (select 0.45 CER!) while real w1aw_5wpm select is 0.0087 → **the synthetic
> generator is INVALID at ≤8 WPM** (true-slow 240 ms dits overflow detector windows;
> nothing real sends true-5-WPM — real slow code is Farnsworth). `[L-farns]` on valid
> Farnsworth shows fb+sel ≡ select (no fb-specific gap). So the fb-specific real-slow
> over-segmentation is **real-audio-only, not synthetically reproducible**, at a niche
> speed; normal speeds (10–30) are green. Parked like the initiation artifact — tracked,
> not blocking. Revisit only with a real Farnsworth recording + ground truth.
>
> **TEST-INFRA FINDING:** the synthetic ladder is INVALID at ≤8 WPM (both cores fail on
> unrealistic true-slow). Ladder rows ≤8 WPM must be ignored or the generator fixed to
> emit Farnsworth for slow speeds. `[L0-slow]`.
>
> **L1 & L2 near-green (modulo parked initiation).** Evidence (`[L2-moderate]`,
> `[init-trace]`): once the initiation artifact is excluded, the soft path is at
> near-parity with hard at normal speed — genuine residual is ~1 element/msg (Q
> over-segmentation + insertions). The DOMINANT soft-vs-hard gap at L0–L2 is the
> **initiation artifact**, which DR-2 proved is NOT fixable in-detector (settling
> corrupts the first char; needs backward correction). Re-parked as a cross-cutting
> backward-correction task, not a level blocker.
>
> **Initiation RE-PARKED (2026-07-24)** after two principled in-detector fixes were
> refuted: DR-2 mechanism A (burn-in) and DR-3 C1 (FFBS smoothing). Corrected theory:
> it is an online-EM PARAMETER-convergence transient (~1.5–2 s), not fixable by burn-in
> timing or bounded state-smoothing (DR-3 closed as WRONG AXIS — a state-smoother aimed
> at a parameter-convergence target). **Correct-axis fix now designed as DR-4 (§13):**
> acquisition-gated deferred decode — buffer until θ (dit/dah/space) is separable, then
> batch-decode the whole buffered window with converged θ (B′ done right = fixed-INTERVAL,
> not fixed-LAG), then confidence-ratcheted continuous re-decode for adaptive WPM.
> Awaiting joint review before build. First-char-only, noise-triggered; not a level blocker.
>
> **L3 CONFIRMED GREEN (2026-07-24).** `[L3-heavy]`: at snr-noise3 fb+sel stays LOCKED and
> copies the message skeleton (`DE W1AW QTH NEWINGTON CT UR RST 599 599 TNX FER`) with
> scattered char errors, while select collapses to noise-garbage after the first token —
> element errors 356 vs 1019, and the soft errors are on a READABLE decode (real content,
> not lucky garbage fragments). No collapse, no garbage-flood, no masking bug. **The soft
> path's value proposition (win the buried-noise regime) is validated.** Residual = noise-
> floor char flips + parked initiation; headroom to the §52.5 perfect-detector ceiling
> (CER→0) exists but is a DETECTOR-improvement effort (#42 matched-filter/bandwidth), not
> a bug — not chased here without evidence of a fixable pattern.
>
> **FRONTIER now L4** (QSB slow fade). L0–L3 resolved (L1/L2 near-green modulo parked
> initiation; L3 green). Ladder is now in the soft-path's favourable regimes.
>
> NB per Principle 4: ISSUE-3/4/5 are at higher levels than the current frontier.
> They are recorded from earlier exploration but MUST NOT be worked until the lower
> levels are confirmed green — a lower-level bug can produce or mask them.

## 7. Baseline snapshot (2026-07-24, element errors excl 1st symbol, 24 seeds)

| level | profile | SOFT `fb+sel` ELEM | HARD `select` ELEM | soft−hard |
|-------|---------|:--:|:--:|:--:|
| L1 | mild-noise | 10 | 0 | +10 |
| L2 | moderate | 23 | 2 | +21 |
| L2 | snr-noise2 | 44 | 60 | **−16 (soft wins)** |
| L3 | snr-noise3 | 356 | 1019 | **−663 (soft wins 3×)** |
| L7 | handkeyed-15 | 127 | 43 | +84 |
| L7 | handkeyed-20 | 158 | 97 | +61 |
| L7 | handkeyed-25 | 134 | 89 | +45 |
| L7 | handkeyed-30 | 87 | 121 | **−34 (soft wins)** |
| L4 | qsb | 71 | 17 | +54 |
| L5 | qrm | 0 | 0 | 0 |

Soft already **wins where it should** (buried noise L3, fast hand-keyed). Open gaps
concentrate at L1/L2 (light-moderate AWGN) and mid-speed L7 — worked in level order.
L0 not yet measured (ISSUE-2).

## 8. Test instruments (all `[.]`, test-only; run by exact tag, never `[.]`/`[cw]`)

- `[replay] [replay-cores] [replay-sweep] [route-why]` — real baseband IQ (test_replay.cpp)
- `[maskdiff]` — raw vs corrected per seed (which errors correction masked)
- `[rawerr] [rawerr-cores] [fbsel-elem]` — element/gap error histograms per core
- `[fbbug] [fbcollapse] [fbtrace2] [fbisolate] [fbsel] [fbsel-full] [fbmild]` — fb collapse investigation
- `[scanner-sens] [onsettrace]` — scanner keyed-penalty / cold-start

New per-level probes are added as we go (e.g. `[L0-clean]`, `[L1-awgn]`).

## 9. Stage rollup (to global plan)

When all ladder levels are CONFIRMED green, record the global outcome in
`decoding-improvement-plan.md` (a new §): "SOFT path debugged to hard-parity-or-better
L0→L8; `fb+sel` promoted from `fb`; N issues fixed." Until then this document is the
source of truth for the soft-debug effort.

## 10. Decision Package format & Decision Record

Every proposed **logic change** (not measurement, not a new probe) is written up as a
Decision Package and reviewed BEFORE it is applied (Principle 10). Template:

```
DR-N: <one-line change>
 (a) REASON — theory basis (cite the principle: Bayes/HMM/matched-filter/clock-recovery)
     + WHY IT MATTERS, verifiable by the reviewer.
 (b) RISKS — known side-effects, regimes it could hurt, overfitting exposure (Principle 8).
 (c) EVIDENCE — matrix results already gathered that motivate it (numbers, seeds).
 (d) PREPARED CHECKS — the exact probes/tests, written and ready, that confirm/refute
     (incl. real-recording [replay] where the mechanism is real-world-facing).
 (e) RECOMMENDATION — apply / hold / needs-joint-review, with confidence.
 (f) DECISION — (reviewer fills) approved / declined / adjusted, + rationale.
```

### Decision Record matrix (living)

| DR | date | change | theory basis | why it matters | risks | evidence | prepared checks | recommendation | DECISION |
|----|------|--------|--------------|----------------|-------|----------|-----------------|----------------|----------|
| **1** | 2026-07-24 | pair fb detector with adaptive SELECT timing (`legacy+fb+sel`), replacing plain Kalman | **Clock/speed recovery** (field §54: foundational; one dit-estimate drives element+gap). Kalman single-hypothesis timing falls into the Morse **half-speed octave** basin; an adaptive/robust estimator escapes it | fb collapses to garbage on ~12% of hand-keyed seeds; unusable as the soft path while it self-destructs | SELECT is a hard-decision timing *selector* bolted onto a soft detector — a **hybrid seam**, not the theory-pure Bayesian joint speed estimate. Could mask a deeper detector issue. Overfitting: LOW (strict improvement on ALL regimes incl. buried noise, not just collapse seeds) | `[fbsel-full]` strict-improvement all 12 regimes; `[fbsel]` collapses 3→0/24, CER 0.139→0.092; `[fbtrace2]` wpm-stuck-at-8 root cause | Synthetic (strong): `[fbsel]` `[fbsel-full]` `[fbisolate]`. **Real validation INCONCLUSIVE:** `[recording-matrix]` on ARRL W1AW (real audio, exact text) → fb+sel ≡ fb byte-identical (0.0163) because W1AW is MACHINE-keyed and does not exercise the hand-keyed collapse; `select` is 0.0044 (soft ~4× worse on real clean). The 7.030 "16C= capture" was NOT evidence (n=1, no ground truth, partial decode). **We have no real HAND-KEYED recording with trustworthy ground truth → collapse-fix real-world benefit UNPROVEN.** | **applied pre-governance — needs retroactive review.** Empirically strong + theory-consistent, but is the hybrid seam acceptable, or should the theory-pure fix be **joint speed marginalisation inside the soft detector** (§52.8 attempted, the CW-Skimmer-correct approach)? | **ACCEPTED (2026-07-24) as a never-worse replacement of `fb`** — strict synthetic improvement, ≡`fb` on real machine-keyed (no harm). NOT claimed as real-world-validated: real HAND-KEYED ground truth is a logged validation gap. Joint marginalisation remains the north star, revisit if L7 traces to the seam. |

| **2** | 2026-07-24 | **fb cold-start burn-in** — suppress fb event emission until its EM emission model is reliably calibrated | **Bayesian/EM convergence**: an EM estimator must not commit to (and act on) a calibration before it has sufficient evidence to separate the states. fb violates this — `refit()` adopts the first mark/space fit at `MIN_FIT=64` samples (~0.8 dit) with lax `FIT_LLR_MIN=0.02`, on warmup-noise + a partial first element, and `fwdDecide` emits from sample 0 with no calibration guard | it is the DOMINANT soft-vs-hard gap at L0–L2 and worse on fb than select (select has a `noiseWinCount≥10` warmup; fb has none on OUTPUT) | delays fb's first valid decode slightly (may lose the first char or two on very short transmissions); **overfitting risk if constants tuned to seeds** — must derive burn-in from CW physics (≥~1 char of data / stabilised getSNR), NOT from synthetic seeds, and validate on real W1AW | `[init-trace]`: getSNR 20→27 dB at t=0.25s (impossible for moderate noise), WPM spike to 70, garbage `ST T G T` until ~3.5s; code: `fwdDecide` no `_haveParams` guard, `MIN_FIT=64` < 1 dit | Candidate mechanisms (pick one): (A) **gate emission on `_haveParams` + a stronger first-fit** (require the first fit to span ≥~1 char of samples AND higher bimodality); (B) **suppress emission until getSNR stabilises** (don't act on unconverged posterior); (C) **reject implausible fits** (a >~15 dB contrast in <1 element is non-physical → don't adopt). Prepared checks: `[init-trace]` (garbage gone?), `[L1-real]`+`[recording-matrix]` W1AW (first char better, rest unharmed), full L0–L2 ladder + `[fbsel-full]` (no regression) | **DECLINED — mechanism A tried & REVERTED (2026-07-24).** A fixed the calibration catastrophe (no more 20 dB/70 WPM spike) but NOT the first-char garble (`SO Q`→`TRQ`, still wrong) AND slightly regressed mild/moderate/snr4/qsb (delayed calibration loses early signal). **Root reason: the first char is decoded during forward-pass SETTLING, not at fit-commit — an in-detector burn-in cannot recover it, only suppress/lose it.** Correctly recovering it requires re-decoding once locked = **low-confidence BACKWARD correction** (the parked track). Confirms the original parking decision. |

| **3** | 2026-07-24 | **Restore bounded fixed-lag forward–backward smoothing (FFBS) + continuous recalibration** — reverses §53's forward-only decision | **HMM smoothing theory.** (i) Smoother error ≤ filter error by the tower property: `Var(X_t∣Y_{1:t}) ≥ E[Var(X_t∣Y_{1:T})∣Y_{1:t}]` — only smoothing corrects an acquisition/derail estimate with future data. (ii) fb's transition prior `SWITCH_P=0.01` sets the exponential-forgetting rate `ρ≈0.98`, so a bad forward lock is forgotten slowly → the initiation transient AND the DR-1 half-speed collapse are the SAME slow-forgetting phenomenon. (iii) fixed-lag smoothing `P(X_t∣Y_{1:t+L})` approximates the full smoother with error geometric in L → a bounded backward window suffices, L derived from ρ. Refs: Cappé–Moulines online-EM-for-HMM (arxiv 0908.2359); exponential stability of filters/smoothers (IEEE 705429); large-lag smoothing (arxiv 1804.07117) | fixes initiation + DR-1 collapse from ONE change; the user's continuous-recalibration additions (track WPM drift, catch a mid-stream response / new near-signal) are the forward-adaptation half (online-EM + adaptive forgetting + re-acquisition) | **§53 disabled the backward pass for zero-lag AND because fixed-lag smoothing char-SPLIT high-SNR signals under narrow smoothing — C1 re-opens this exact risk.** +L latency. Adaptive forgetting can chase noise. Overfitting guard: derive L from ρ and forgetting from CW physics, NOT seeds; validate on real W1AW incl. the char-splitting (clean high-SNR) regime | theory (cited); dead scaffolding present (`finalize()`, `LAG=48`, `_fwdOnly=true`); targets `[init-trace]` + DR-1 collapse seeds | see §12 for the phased design + prepared checks | **C1 IMPLEMENTED, MEASURED, REFUTED, REVERTED (2026-07-24).** L=150/`_fwdOnly=false`: did NOT fix acquisition or collapse AND catastrophically regressed noisy regimes (mild 0.021→0.103, moderate 0.034→0.261, qsb 0.070→0.204). **Corrected theory:** the smoother≤filter theorem assumes FIXED KNOWN θ; fb estimates θ ONLINE, breaking it twice — (i) acquisition is a PARAMETER-convergence transient (`ONLINE_LAM`, ~1.5–2 s) not a state-lag (`ρ`, ~150 ms), so bounded fixed-lag can't span it; (ii) the backward pass over noisy time-local per-sample θ AMPLIFIES error under noise (the §53 char-splitting — **§53 forward-only VINDICATED**). C2/C3 moot as designed. **Reframed target: PARAMETER init/convergence, not state smoothing** → candidates: (A′) fast θ init from the front-end noise floor (muLo from envelope percentile immediately, skip transient); (B′) bounded one-time acquisition RE-DECODE with converged θ. **CLOSED (2026-07-24) — WRONG AXIS from the start (deepest reason).** An HMM has two unknowns on two axes: STATE `X` (mark/space) and PARAMETERS `θ` (dit/dah/space means+vars). FFBS/smoothing is a STATE-axis tool (improves `X` given θ). C1's two targets live on OTHER axes: initiation = θ-convergence (parameter axis); DR-1 collapse = WPM-octave lock (timing axis, already fixed by SELECT). Neither is a state-estimation error, so no lag length and no consistent-θ variant can hit them — you cannot smooth out a parameter error. The consistent-θ closure test is therefore also moot (still can't span the ~1.5–2 s acquisition with a ~150 ms lag). Even the "state part" of acquisition (early samples decoded under wrong θ) needs a FIXED-INTERVAL re-decode over the whole acquisition window AFTER θ converges (B′), not a bounded fixed-lag smoother. **Superseded by DR-4** (parameter-axis: acquisition-gated deferred decode + confidence-monotone re-decode). |

| **4** | 2026-07-24 | **Acquisition-gated deferred decode + confidence-monotone continuous re-decode** (parameter-axis, supersedes DR-3) — buffer evidence without emitting; UNLOCK decoding only once the dit/dah/space parameters `θ` are separable to a confidence bound; then BATCH-decode the whole buffered window with converged `θ`; continue forward tracking `θ` adaptively (WPM drift); RE-DECODE a segment only when a fresh decode's confidence STRICTLY exceeds the committed one | **Sequential parameter estimation + fixed-interval MAP decoding.** (i) The initiation artifact is a θ-convergence transient (DR-3 corrected theory) → the correct-axis fix is: do NOT decode under unconverged θ; wait until θ is identified (a Wald/SPRT-style stopping problem — stop collecting when the dit/dah clusters are separated to a bound), THEN decode. (ii) With θ known, the buffered window is decoded by the SAME beam/forward pass with correct emissions = textbook batch (fixed-interval) MAP — the acquisition region no longer suffers wrong-θ garbage. This is B′ done right: fixed-INTERVAL re-decode (spans the whole ~1.5–2 s window), not fixed-LAG smoothing (~150 ms, DR-3's fatal mismatch). (iii) Adaptive WPM: online-EM already tracks slow drift; the confidence-monotone gate is a RATCHET/hysteresis (proper scoring — commit a re-decode only if path confidence strictly improves) that gives continuous adaptation + mid-stream catch WITHOUT FFBS's noise amplification (one converged θ over the window, never a backward pass over noisy per-sample θ) | correct-AXIS fix for the parked initiation artifact (dominant soft-vs-hard gap L0–L2) AND delivers the user's continuous adaptive-WPM + mid-stream/new-signal catch in one design; builds on existing machinery — fb's bootstrap `refit()`/`_haveParams` IS the unlock moment, online-EM IS the adaptive tracker; only three pieces are missing: buffer-before-unlock, batch-decode-at-unlock, confidence-monotone re-decode trigger | **Latency is VARIABLE, not a flat ~2 s** — θ splits into two independent always-on trackers: a persistent noise floor (`μ_lo`/`σ`, fed by silence, never reset) and per-signal params (`μ_hi`/WPM, fed by marks). The floor is continuously known, so the gate opens as soon as a mark clears it by `k·σ` — sub-second on clean signals, only worst-case when buried; the buffer covers only that unavoidable minimum (no audio lost). MUST bound the buffer (preallocated ring, RT no-alloc — [[rt-thread-no-alloc-validate-live]]) with a best-effort fallback at buffer-full. **Gate criterion** (dit/dah cluster separation ≥ k·pooled-std, both classes seen) MUST be derived from CW physics not seeds (Principle 8). **Confidence comparability**: "new > current" needs a normalised, calibrated confidence across decodes under different θ + a hysteresis margin — too small → thrash/flap, too large → miss a real correction. **Re-decode cost** bounded (last-K elements / current word), no alloc in `process()`. **Blast radius**: core-level (staged_core buffering + deferred emit) → new core, all existing cores byte-identical; deferred emit must not double-emit or lose committed text (`replaceLastN`) | DR-3 corrected theory (parameter axis); `[init-trace]` garbage until ~3.5 s under unconverged θ; **DR-2-A proved in-detector burn-in can SUPPRESS but not RECOVER the first char → re-decode is required** (the B′ distinction); existing `refit()`/`_haveParams` gate + online-EM already present | `[init-trace]` — garbage GONE **and early chars now CORRECT** (recovered, not suppressed — the DR-2-A distinction); NEW `[acq-gate]` — no emit before θ-separable, buffered window decoded correctly at unlock, latency bound respected; NEW `[redecode-mono]` — WPM step mid-stream re-decodes only on strict confidence gain, NO thrash on steady signal; NEW `[midstream]` — CQ@20 + long gap + response@25 caught; `[L0-clean]`+`[fbsel-full]`+full `[multiseed]` no regression (strong signal: θ separates fast → gate opens immediately → no latency penalty); `[recording-matrix]` W1AW unharmed; **live SDR weak-signal check before any default-core promotion** | **needs-joint-review, then phased**: P1 = buffer + acquisition gate + batch-decode-at-unlock (fixes initiation — headline); P2 = confidence-monotone re-decode + adaptive-WPM continuity; P3 = re-acquisition after silence (mid-stream catch). Each gated before the next. Confidence MODERATE-HIGH on P1 theory (correct axis, reuses the existing gate); the real risk is gate-criterion discipline + RT/latency engineering, not the concept | **P1 APPROVED to build (2026-07-24, user "looks good to try").** Params: 2.5 s circular ring, existing detection metric, new core `legacy+fb+sel+acq`, all existing cores byte-identical. Gate on `[init-trace]` early chars RECOVERED (not suppressed) + full `[multiseed]` no-regression before P2. Detection-metric (§13) + SparkGap WPM-bank (§11.1) parked post-P1. **→ P1 BUILT, MEASURED, REFUTED (2026-07-24).** `[acq-init]` full-ladder: net +138 element errors (snr-noise2 44→98 incl. new gap errors, snr-noise3 356→385, snr-noise4 1031→1058, worstcase 500→531, qsb 71→76; only moderate-noise −5 and handkeyed-30 −5), and the TARGET was NOT fixed (seed 0 `CQ CQ CQ→TRQ CQ CGT` byte-identical). **`[acq-axis]` trace found the WRONG AXIS (again):** the artifact is a TIMING-acquisition failure — early short/noise events (12 ms, 22 ms) yank SELECT timing to dit=12 ms / 100 WPM (8× true ~22 WPM), and `retroDecode` commits the first token at that bogus speed. Detector-θ (DR-4's assumed axis, and P1's target) is not the amplifier; the SPEED estimator locking off a single spurious element is. Also: frozen-θ replay is a noise liability (can't track non-stationary noise vs live online-EM). **All isolated in `legacy+fb+sel+acq` — zero ship impact, existing cores byte-identical, suite backlog still exactly 9.** → Re-aimed to the TIMING axis: **DR-4b / §14** robust speed gate (user). |

| **5** | 2026-07-25 | **Continuous windowed re-decode with confidence ratchet** — the DR-4 concept realized, but on the ENVELOPE (not the parked θ-gate): buffer the detection-input envelope + live events; every 0.5 s re-detect the whole window under the detector's matured θ; decode BOTH the re-detected and live streams comparably; emit the ratchet winner. Sliding window on 60 s overflow. Core `legacy+fb+sel+cont`, `StagedCore._contRedecode` (off by default) | **Continuous re-decode + confidence ratchet.** Re-detection under matured θ un-merges the cold-start θ-blob (fixes seed-0 `SO Q`→`CQ CQ CQ`); the ratchet (override live only when strictly more confident + margin 0.04) makes it regression-safe — live wins on QSB/heavy-noise where a single θ can't track, so it only helps; sliding-window commit handles messages > window without dropping the tail | delivers the user's "continuous, always re-decode" vision; recovers acquisition AND holds every regime; **the ratchet is what turns a −338-with-holes into a clean win** (unconditional replace regressed noise +463 on qsb; the margin fixed the snr-floor coin-flip +103→+21) | re-decode ALLOCS every 0.5 s in `process()` (makeFresh timing/symbol, vectors, strings) → **RT-unsafe, blocks default promotion** ([[rt-thread-no-alloc-validate-live]]); ratchet metric fooled by high-conf noise (mitigated by margin, not solved — task C); scratch-sink real-fb+sel fallback FAILED (streaming flush-latency drops tail) — REVERTED | `[cont]` full ladder n=24: net **−746 elem (~28%)** vs fb+sel (moderate 23→0, handkeyed-20 158→45, qsb 71→27, worstcase 500→384, snr3 356→241, contest 86→3); only snr-noise4 +21 (noise-floor garbage). Suite byte-identical (9-backlog) | `[cont]`/`[cont-dbg]` (`[.]`); before any default swap: RT-safety pass + live SDR weak-signal check | **BUILT, MEASURED, KEPT as an experimental non-default core (2026-07-25).** Validated net win, all ladder regressions debugged to root & resolved to the noise floor. Remaining before promotion: **D (RT-safety, mandatory)**, C (ratchet metric). **B (first-char) is NOT closed by this** — its residual (seed-5 `CQ→TRQ`) is a FRONT-END bug: the `_fbBpf` fast-gate narrows to a fixed 32 Hz before WPM is known (user: "not adapted to what to filter"), attenuating the first dit below threshold. The envelope re-decode inherits post-BPF errors and CANNOT fix them → **B's true fix = re-decode from RAW/pre-BPF IQ with matched re-filtering per window** (scoped, connects to §13). |

> **Open governance question for DR-1:** `fb+sel` fixes the collapse and is a strict
> improvement, but per Principle 9 the *theory-pure* soft solution is joint Bayesian
> speed estimation (marginalise over WPM hypotheses in the forward pass), not an external
> hard timing selector. Two paths: (A) accept `fb+sel` as the pragmatic soft baseline and
> keep climbing the ladder; (B) treat the collapse as evidence the soft detector must own
> speed estimation, and build joint marginalisation. Recommend **A now, revisit B** if L7
> hand-keyed bugs trace to the detector↔timing seam. Reviewer to confirm.

## 11. North star (context, not the immediate task)

### 11.1 SparkGap — the north-star realized (reference; PARKED for post-P1 eval)

> **PARKED (2026-07-24, user).** Post-P1 architectural candidate, NOT a P1 dependency.
> Would SUBSUME `TIMING_SELECT` + the DR-1 octave-collapse patch. Evaluate only after the
> DR-4 decoding fix is green. (Details below are from a fast-model summary of the SparkGap
> README — https://github.com/hotairfred/SparkGap — READ THE ACTUAL SOURCE before building.)

SparkGap is joint soft speed+symbol inference, working. Its design, mapped to ours:
- **16 WPM bins (8–60 WPM) on ONE fixed 200 Hz noncoherent envelope** — same bandwidth as
  our `dsp.h` front-end. Bins differ only in the HMM **dit-length/timing model**; it
  marginalises the forward-backward **log-evidence** across bins → **dissolves the
  chicken-and-egg** (bandwidth is fixed; WPM is integrated out, never pre-required). The
  per-WPM narrowband matched-filter idea (§13 deferred note) is NOT how it gets sensitivity.
- **Per-bin Bayesian forward-BACKWARD** with the bin's FIXED dit-length θ → the
  smoother ≤ filter theorem HOLDS. **This vindicates the DR-3 corrected theory**: our
  backward pass failed because it smoothed over online-EM per-sample (time-varying) θ;
  SparkGap smooths over fixed per-bin θ. Same conclusion DR-4 reached (batch-decode with a
  fixed converged θ), now confirmed by a shipping decoder.
- **Continuous posteriors, LATE quantisation** (`γ_marg > 0.5` only at the end), then a
  **64-wide beam** over run-lengths → top-K text. Our `morse_tree` beam already has the
  soft `addElement(confidence)` hook to consume this.
- **Theory-pure DR-1 fix:** a marginalised bank cannot octave-collapse (correct-WPM bin
  keeps dominant log-evidence) → replaces the hard `TIMING_SELECT` selector with
  marginalisation. This is exactly path (B) in the DR-1 governance question.
- **RT cost:** the envelope/BPF is SHARED across bins (fixed bandwidth), so only the small
  per-bin HMM replicates — affordable for a many-channel skimmer, unlike a per-WPM
  matched-filter bank. Still MUST be measured against the RT no-alloc budget
  ([[rt-thread-no-alloc-validate-live]]) before adoption; bin count could reduce to major
  WPM (~5–6) if 16 is too costly.
- **`M8` multi-WPM spawn** (a slow runner + fast caller on one channel decoded in parallel)
  is relevant to our multi-station channels + DR-4 P3 re-acquisition — secondary.

Relationship to DR-4: compatible, not competing. DR-4 P1 (acquisition gate + fixed-θ batch
decode) is the bounded near-term fix; the SparkGap bank generalises DR-4's single converged
θ to a marginalised θ-bank. Build P1 first; if the ladder shows the single-hypothesis timing
still leaks (L7 hand-keyed, mid-QSO speed changes), the bank is the principled next step.

### 11.2 The hybrid-seam question

The field (§54) converges on **joint soft speed+symbol inference** (CW Skimmer
Bayesian). `fb+sel` is a hybrid: soft detector + separate adaptive timing. Fixing it
level-by-level is the pragmatic path; whether the timing itself should become soft/
joint is a deeper question to revisit once the ladder exposes where the hybrid seam
leaks (e.g. if remaining L7 bugs trace to the detector↔timing handoff).

## 12. DR-3 design detail (CLOSED — wrong axis; superseded by §13/DR-4)

> **SUPERSEDED (2026-07-24).** DR-3's FFBS/C1 is a STATE-axis tool aimed at
> parameter/timing-axis targets (see the DR-3 DECISION cell). The correct-axis
> replacement is **DR-4 (§13)**. This section is kept for the audit trail only.

Reverses §53's `_fwdOnly=true`. Three components; the first is the core theory fix,
the other two are the user-requested continuous-recalibration half.

**C1 — bounded fixed-lag FFBS (the smoothing fix).** Re-enable `finalize()`
(`_fwdOnly=false`). The MAP state at time `t` is taken from `P(X_t∣Y_{1:t+L})`, so the
backward window carries the CONVERGED emission model back over the acquisition / a
derailed segment. **Lag `L` from theory, not seeds:** L ≈ the forgetting horizon of the
chain, `L ≈ ln(ε)/ln(ρ)` with `ρ≈1−2·SWITCH_P≈0.98` → `L≈150` samples (~150 ms, ~1–2
dits) for `ε=0.05`. (Current `LAG=48`≈one-third of that — too short to carry the
estimate across an element; part of why the disabled path underperformed.) Adds `L`
samples of constant latency (cancels in timing differences, as `finalize()` already
notes). **Fixes: initiation artifact + DR-1 collapse (recovery).**

**C2 — continuous adaptation (track drift + mid-stream changes).** Keep online-EM
(`updateEmission`) but make the forgetting `λ` ADAPTIVE: hold the steady-state
`ONLINE_LAM` while the emission likelihood is high, and TRANSIENTLY raise `λ` (forget
faster) when the per-sample log-likelihood under the current `θ̂` drops below a threshold
for a sustained run — a change was detected (new WPM, new level, a station taking over).
Theory: adaptive-forgetting stochastic approximation / online change-point; a single
fixed timescale (Cappé–Moulines) tracks slow QSB but not abrupt change. **Enables: WPM
drift tracking + catching a response that starts mid-stream.**

**C3 — re-acquisition after long silence.** After a word-gap/silence longer than K·dit,
mark the emission model STALE; on the next sustained mark, re-run the bootstrap `refit()`
so a NEW station (a mid-stream answer, or a new near-signal that has taken the channel)
gets a fresh calibration, and C1's backward pass smooths ITS onset the same way it
smooths the first acquisition. **Enables: catch a new-near / responding signal cleanly.**

**Risks (must be checked, not assumed).**
- §53 disabled backward because fixed-lag smoothing **char-split high-SNR signals under
  narrow smoothing**. C1 re-opens this — the L=150 ms lag + emission variance interact.
  MUST verify high-SNR clean copy is byte-clean (L0 + W1AW 15 wpm).
- Latency +L (~150 ms) — acceptable for a skimmer; confirm no timing breakage.
- C2 adaptive λ can chase noise → gate the λ-boost on a SUSTAINED likelihood drop (a run),
  not a single sample; derive the threshold from the emission variance, not seeds.
- C3 re-acquisition mid-QSO could drop a character at the boundary — measure.

**Prepared checks (written/ready or to add).**
- `[init-trace]` — acquisition garbage gone (the C1 target).
- `[fbsel]` / `[fbisolate]` / `[fbtrace2]` — DR-1 collapse seeds now recover (C1).
- NEW `[midstream]` — generate CQ@20wpm + long gap + response@25wpm at a different level;
  assert the response is caught (C2/C3).
- `[L0-clean]` + `[fbsel-full]` + `[recording-matrix]` W1AW — **no regression, especially
  the §53 char-splitting regime** (high-SNR clean 15 wpm byte-identical).
- Full `[multiseed]` gate suite green.

**Recommended order (phased, each validated before the next):**
1. **C1 alone** — the core theory fix; biggest impact (initiation + collapse); tests the
   §53 char-splitting risk in isolation. Gate on: init fixed, collapse fixed, NO
   char-splitting regression.
2. **C2** — adaptive forgetting; gate on `[midstream]` catch + no steady-state regression.
3. **C3** — re-acquisition; gate on `[midstream]` new-station + no mid-QSO char drop.

Do NOT bundle — if C1 re-introduces char-splitting, that must be seen alone, not masked
by C2/C3.

## 13. DR-4 design detail (PROPOSED — awaiting review)

The parameter-axis replacement for DR-3. Reframes initiation as **"learn the
dit/dah/space parameters before committing to a decode"** — a sequential
parameter-estimation stopping problem followed by fixed-interval MAP decoding, then
continuous confidence-ratcheted re-decode for adaptive WPM.

**Mental model.** The chain has three lifecycle states per station:
`ACQUIRING` (buffering, not emitting) → `LOCKED` (θ separable, decode the buffer +
stream forward) → `TRACKING` (θ drifts; re-decode only on strict confidence gain).
fb's existing `refit()`/`_haveParams` already draws the ACQUIRING→LOCKED line at the
*parameter* level; DR-4 adds the buffer, the batch decode at the transition, and the
re-decode ratchet. No backward pass, no per-sample-θ smoothing (the DR-3 traps).

**θ splits into TWO INDEPENDENT, ALWAYS-ON trackers with different timescales and reset
semantics — this is the latency key.** Both adapt continuously and forever; they differ
only in which samples feed them and whether a new station resets them.

- **Noise tracker — `μ_lo` / `σ` (channel, persistent).** Fed by SPACE/silent samples
  (state-posterior soft-weighted, as `updateEmission` already does). Seeded IMMEDIATELY
  from the front-end percentile noise scale, then adapts continuously so slow band-noise /
  QRN-level drift is followed. Runs even before the first lock and through long silence.
  **NEVER reset** by silence or re-acquisition — the channel noise outlives any one station.
- **Signal tracker — `μ_hi` (mark level) + WPM (per-signal).** Fed by MARK samples. Adapts
  continuously too, so QSB mark-fade and WPM drift are followed while a station is up.
  Marked STALE / reset on a new-station re-acquisition (P3) — this half is signal-specific.
- Both are the SAME online-EM machinery (`updateEmission`, soft-weighted by the state
  posterior) — the change is DECOUPLING them: the noise half runs independently of any
  locked signal and is never reset; the signal half is what the acquisition gate waits on.
- Consequence: acquisition no longer waits for a JOINT bimodal fit (the ~2 s transient in
  today's `refit()`, which relearns the floor from scratch). It only has to detect a mark
  standing `≥ k·σ` above the CONTINUOUSLY-TRACKED floor for the duration of a real element →
  **latency is VARIABLE and minimal**: one clean dit (sub-second) when SNR permits, only
  collapsing to the worst-case bound when the signal is genuinely buried. This is candidate
  **A′** (immediate/continuous floor) fused with **B′** (buffer + batch decode).

**Detection metric — modern SP, IQ-domain, WPM-independent (DEFERRED, check LAST).**
> **PARKED (2026-07-24, user).** This is a SENSITIVITY win, separable from and SUBORDINATE
> to the core decoding fix. **P1 is built on the EXISTING detection metric** (fb's `refit`
> separation stat on the current envelope) — do NOT make the narrowband carrier detector a
> P1 dependency. Revisit this ONLY after the harder current problem (the acquisition-gate +
> batch-decode DECODING fix, P1/P2) is solved and green. Captured here so the analysis
> isn't lost; it is a later, standalone task, not part of the DR-4 build order below.

The gate's separation statistic must NOT be the wide acquisition-time envelope. Evidence
from the current chain: the speed-matched BPF narrowing (`staged_core.h:70`,
`ENBW ≈ 2/T_dit` → 20–40 Hz) is GATED on a timing lock (`:95`, `cut = 100·(1−nf) +
matched·nf`), and the boxcar matched filter is DISABLED on the fb path (`:591`). So today
acquisition runs at the WIDEST BW (~200 Hz BPF, no MF) — the least-sensitive point — a
chicken-and-egg: narrowing needs the WPM, the WPM needs acquisition.
- **Break it:** DETECTION (carrier present / key down) does NOT need the WPM. The tone
  frequency is fixed (the channel's assigned center → DC after freq-xlate). A narrowband
  **carrier-presence SNR** (energy in the tone bin vs the noise floor measured in the SAME
  band) runs at the narrowest justifiable BW from sample zero, WPM-independent. "If the ear
  can hear it, the IQ bin sees it easier": a carrier standing `k·σ` above noise in a narrow
  band is detectable regardless of keying speed — the processing gain the wide envelope throws away.
- **Two bandwidths for two jobs (the honest caveat):** detection wants NARROW (carrier
  SNR); decoding/timing CANNOT go narrower than the keying BW (~2/T_dit) or it smears
  element edges (`dsp.h:50` tradeoff). So the narrow carrier detector feeds only the
  ACQUISITION GATE and the noise/signal SNR; the wider keying-BW envelope still feeds
  element timing. Do not collapse the two.
- **Noise floor in the detection band:** today the floor is tracked on the WIDE decimated
  band (`dsp.h:124`). For the gate SNR to be meaningful, the dual noise/signal trackers must
  measure in the SAME narrow detection band as the metric.
- **Noncoherent energy** (matches the Rician envelope model + CW-Skimmer per-bin approach);
  coherent integration is OUT OF SCOPE unless measured to help (carrier drift/QSB make phase
  tracking fragile).
- **MEASURE, don't assume:** a prepared check must quantify the acquisition-SNR gain of the
  narrow carrier metric vs the current wide-BW gate on buried signals — the predicted
  4–6 dB (200 Hz → keying BW) is a hypothesis to confirm, not a given (Principle 8).

**P1 — acquisition gate + batch decode (the initiation fix, headline).**
- The buffer is a **CIRCULAR (ring) buffer holding the trailing 2.5 s**, always rolling —
  NOT a fill-once-then-stop. Preallocated (RT no-alloc — [[rt-thread-no-alloc-validate-live]]),
  continuously overwritten so it always carries the most recent onset for batch decode AND
  stays available for the P2 confidence-ratchet re-decode. Both trackers keep adapting off
  the live stream the whole time; the ring is the decode-history window, not a gate on adaptation.
- While not yet LOCKED on the per-signal params: emit NO text (the ring fills/rolls; the
  persistent floor keeps updating the whole time).
- Gate criterion ("minimal required knowledge"), derived from CW physics not seeds: LOCK
  when a mark cluster separates from the persistent floor by `≥ k·σ` AND has persisted for
  ≥ a plausible element (a Wald/SPRT stopping rule against the known floor), plus one gap
  observed to seed WPM. Because the floor is known, this fires as fast as the SNR allows.
  Reuse fb's existing `refit()` separation statistic — but run it MARK-vs-known-floor, not
  a cold joint fit.
- At the →LOCKED transition: **batch-decode the entire buffered window** with the converged
  θ (fixed-interval MAP over the same beam/forward pass), emit that text, then stream
  forward normally. The early chars are now decoded under GOOD θ → recovered, not lost
  (the DR-2-A distinction). Even in the buried worst case where the mark needs the full
  bound to separate, the buffer still holds that onset → the missed time is decoded
  retroactively, no audio lost.
- Fallback: if the gate has not opened by buffer-full (bound = max acquisition seconds),
  decode best-effort with the current θ and log the truncation (Principle: no silent cap).

**P2 — confidence-monotone re-decode + adaptive-WPM continuity.**
- Keep online-EM tracking θ forward (already present). Maintain a normalised per-element
  path confidence for the committed decode of the current word / last-K elements.
- When θ has moved materially, RE-DECODE that bounded segment with the new θ; **replace
  the committed text ONLY if the new normalised confidence strictly exceeds the current
  by a hysteresis margin `δ`.** This is a ratchet: it can only improve, never thrash.
  `δ` derived from the confidence noise floor (measured on a steady signal), not seeds.
- Re-decode scope is bounded (current word / last-K elements) → O(K), no alloc in
  `process()`.

**P3 — re-acquisition after long silence (mid-stream / new-station catch).**
- After silence > K·dit, mark only the PER-SIGNAL params (`μ_hi`, WPM) STALE; **KEEP the
  persistent noise floor** (`μ_lo`/`σ`) — the channel noise did not change, only the
  station did. On the next sustained mark, re-run the gate (mark-vs-known-floor) so a NEW
  station (a response starting mid-QSO, or a near-signal that took the channel) locks fast
  against the floor already learned during the silence — often sub-second, exactly the
  weak-signal-catch the user wants.

**Risks (must be checked, not assumed).**
- Latency +acquisition (~1–2 s) on WEAK signals; on STRONG signals θ separates almost
  immediately so the gate opens fast → confirm no latency penalty on clean copy.
- Gate criterion `k` and margin `δ` are the overfitting-exposed knobs → derive from
  physics / measured noise floors, sweep opt-in, register one operating point.
- Deferred emit changes WHEN text appears → the text/`replaceLastN` path must not
  double-emit or drop committed characters across a re-decode.
- Buffer bound + fallback are mandatory (RT memory + latency ceiling).

**Prepared checks (to write before build).**
- `[init-trace]` — acquisition garbage GONE and early chars CORRECT (recovered).
- NEW `[acq-gate]` — no emit before θ-separable; buffered window decoded right at unlock;
  latency bound respected; strong-signal gate opens immediately (no penalty).
- NEW `[redecode-mono]` — WPM step re-decodes only on strict confidence gain; steady
  signal shows ZERO re-decode thrash (committed text stable across ticks).
- NEW `[midstream]` — CQ@20 + long gap + response@25 at a different level → response caught.
- `[L0-clean]` + `[fbsel-full]` + full `[multiseed]` — no regression; `[recording-matrix]`
  W1AW unharmed; **live SDR weak-signal check before any default-core promotion.**

**Phased order (each validated before the next):** P1 → P2 → P3. P1 alone must fix
initiation with no regression before P2/P3 are touched — same "don't bundle" discipline
as DR-3, but now the phases are on the SAME axis so they compose rather than mask.

**New core:** `legacy+fb+sel+acq` (P1), extended in place through P2/P3. `legacy`,
`fb`, `fb+sel`, and every existing core stay byte-identical.

## 14. DR-4b — timing-axis robust speed gate (PROPOSED, awaiting review)

**Why this section exists.** DR-4 P1 (detector-θ acquisition gate) was built and REFUTED
(§ Decision Record DR-4). The `[acq-axis]` trace on seed 0 relocated the artifact to the
TIMING axis:

```
true signal ~22 WPM (dit ~50 ms). During acquisition:
  t=524 elem=12ms  -> dit=80  wpm=15   (noise spike)
  t=593 elem=22ms  -> dit=12  wpm=100  (SELECT jumps to 100 WPM off ONE 22 ms element)
  t=664 RETRO events=7 dit=12           (first token committed at 8x true speed) -> "SO"/"TRQ"
```

The initiation garble is the SPEED estimator locking to a wildly wrong octave from a
single spurious short element, then `retroDecode` committing at it — NOT a detector-θ
convergence problem. Same wrong-axis trap as DR-3; now evidence-confirmed.

**The fix (user, evidence-aligned): a robust speed-calibration gate.** Do not lock or
adapt the dit/dah speed from insufficient or inconsistent evidence.
- **Cold acquisition — need both classes, consistently.** Require **≥2 dits AND ≥2 dahs**
  with **duration variance <10%** (per class) before committing a speed. Rationale: one
  element of a single class cannot separate dit from dah (the estimator has no scale); a
  lone short noise spike must never move the speed. One dit + one dah is the bare minimum
  to define the ratio; >1 of each with tight variance is a confident lock. This is the
  concrete, physics-derived form of DR-4's "minimal required knowledge" gate — on the
  TIMING axis (dit/dah durations), not the detector-θ axis.
- **Continuous mode — confirm, don't re-adapt.** Once locked, a single element only
  CONFIRMS the current speed (keeps the estimate), it does not re-fit it. Reject any
  update whose duration is >10% off the running estimate (an outlier — noise, QRM, a
  merged/split element) so a single bad element cannot derail a good lock. Sustained
  consistent drift (real QSB/speed change) still adapts because it clears the ≥2-of-each
  consistency bar over time.

**Unreal-WPM filter — always tracked, always an error (user, anti-masking).** Reject any
speed estimate outside the physical CW range (~5–60 WPM ⇒ dit ~240–20 ms; `dit_ms =
1200/WPM`, PARIS): never commit it, hold the last good lock. This is a production backstop
AND an error signal — every rejection increments `CoreStats.unrealWpmRejections`, tracked
**always** (not test-only), because a raw estimator producing a non-physical speed is an
implementation smell, not a normal event. Tests and the multiseed gates assert the counter
is **ZERO**; a decode that only looks clean because the filter clamped an unreal estimate
is hiding a bug. The filter protects the field; the counter guarantees it never launders a
raw-decode error past the suite. Same principle as word-correction OFF under test. A correct
implementation drives the counter to zero via the robust speed gate above, not via the clamp.

**Pile-up / multi-signal (user, forward-looking — parked with §11.1).** The speed+element
calibration should be keyed to a **per-signal signature — tone (pitch) and/or phase** —
so that when multiple signals overlap on a channel (a pile-up), each is tracked by its OWN
estimator instead of one signal's elements corrupting another's speed lock. Same
per-hypothesis principle as SparkGap's parallel decode (§11.1) and the narrowband
per-tone detection metric (§13 deferred). Not for the single-signal fix below; captured so
the robust-gate design leaves room for a signature key rather than assuming one estimator
per channel.

**Where it lives.** The timing stage (`AdaptiveTimingStage` / the SELECT + Kalman
estimators in `timing.h`) and the `retroDecode` speed it commits at (`staged_core.h:492`,
which currently trusts `timing->getDitDuration()` at lock — the exact value the trace shows
going to 12 ms). The gate belongs in the timing lock criterion, not the detector.

**Prepared checks (to write before build).**
- `[acq-axis]` — the 22 ms element no longer moves the speed; lock waits for ≥2 dits+dahs;
  seed 0 first token `CQ CQ CQ` recovered.
- `[acq-init]` full ladder — net element errors DOWN vs `fb+sel`, ZERO regression on the
  snr-noise ladder / worstcase / qsb (the regimes P1 broke).
- Full `[multiseed]` gate suite green; `[recording-matrix]` W1AW unharmed.

**Status:** proposed, awaiting joint review. P1's detector-θ core (`legacy+fb+sel+acq`)
stays isolated behind its flag pending the decision to revert it or repurpose the ring.
