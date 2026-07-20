# CW Decoder — Investigation Findings (July 2026)

Audit of the decode chain against the module's own docs, plus a controlled
filter-bandwidth experiment. Companion to `architecture.md` (what the code does)
and `decoding-improvement-plan.md` (what has been tried).

Scope: `src/cw/*.h` at branch `feature/modular-core-abstractions`.
Baseline suite at time of audit: **207 test cases, 1410 assertions, all passing, <1 s.**

### Evidence markers

Used across all three module docs. Anything **not** marked is measured in-tree
at 24 seeds and reproducible with the commands in §11.

| mark | meaning |
|---|---|
| ⊘ EVIDENCE NEEDED | reasoned from code reading or literature, never measured here |
| ⚠ CONTESTED | measured evidence exists on both sides, not yet decisive |
| ✗ REFUTED | measured and found wrong; text kept for history |
| 📎 SECOND-HAND | sourced from another project or paper, not verified first-hand |

The scorecard in §10 is the reason this matters: of **17** predictions made from
code reading alone, **8** were wrong. **An unmarked plausible claim in this
codebase has roughly even odds.**

---

## 1. Summary

### Where the error is (§11, oracle ablation, 24 seeds)

| profile family | attribution | measured |
|---|---|---|
| AWGN, QSB, QRN | **100% detector** | perfect detector → CER 0; perfect timing barely moves noise3.0 (0.8163 → 0.6614) |
| hand-keyed | detector and duration model both | a real detector fix reaches the *oracle detector* bound (§12.6) |
| Farnsworth | **100% gap classification** | 0.0141 → 0.0000 with ideal boundaries; detector irrelevant |
| worstcase | **both stages, super-additive** | 0.41 / 0.36 alone, **0.0792 together** |

### What is established

- **The old benchmark measured a single coin flip.** Every pre-2026-07 figure is
  one realization on `seed = 42`. Hand-keyed was ~4× worse than documented (§3).
- **Pre-detection bandwidth is worth +2.8 to +4.8 dB** — but narrowing it breaks
  clean 25 WPM decoding, which the AWGN-only view missed (§4).
- **"Noise+QSB is a physical limit" is refuted.** `worstcase` 0.6244 → 0.0792
  under oracle ablation; three independent lines agree (§11).
- **The detector stretches every ON by 9.8% of a dit**, so timing is fed a
  dah:dit ratio of 2.82 instead of 3.0. **The cause is unknown.** Two
  candidate mechanisms have been measured and refuted: the Schmitt threshold
  gap (§12.1, accounts for 11%) and instant-attack peak tracking (§13.8 —
  removing key-up attack entirely reproduces the stretch to 4 s.f.).
  `PEAK_PERCENTILE` does cut it 57%, but why is now ⊘ unexplained (§12.6).
- **Best single change measured:** `legacy+peak` takes handkeyed-15 from 0.1579
  to 0.0628 — equal to the oracle detector's own score on that profile.

### What is open

- **The threshold reference (§13).** Four experiments produced four distinct
  trade points and no dominating one: within-element stability and fade
  tracking are in direct opposition for windowed estimators.
- **Farnsworth gap centres.** The only measured win with *no* identified cost,
  still untouched.
- **Generator fidelity is unvalidated** and every number here is conditional on
  it (§15). It has no multipath, and its AWGN makes the envelope exactly Rician
  — the model a likelihood-ratio detector assumes, so that work would be tested
  against its own premise.
- **20 registry variants, zero promotions.** Every candidate so far trades, and
  the promotion rule has two known gaps, both unresolved:
  - *no notion of operating regime* — `+kalman2` is blocked by `noise4.0`
    0.9032 vs 0.9888, both total failure (§7);
  - *no notion of significance* — `+peakdual` is blocked partly by `qrn`
    0.0029 → 0.0035, about one character across 24 seeds (§13.6).

### Next action

**Phase 17 is done and refuted (§13.8).** The transition-gated peak reproduced
legacy's ON stretch exactly, proving the edge chase it was designed to remove
does not occur in steady-state keying, and cost 8 profiles to misses. Two
consequences for what comes next:

1. **The peak-tracking line is closed pending a mechanism.** Five estimators
   have now been tried (percentile, slow-attack, dual-window ×2, gated) and none
   promoted. §12.6's measurement stands — `+peak` does cut the stretch 57% — but
   its mechanism is refuted, so there is no model left to design against.
   Anything further here is guessing. ⊘ EVIDENCE NEEDED: why the percentile
   reference moves the stretch.

2. **Phase 18 confirmed the mechanism and found a question about `legacy`
   itself (§13.9).** The gate's misses are the `dynamicRange < 1.8` guard
   switching detection off inside elements — blindness tracks the miss rates
   profile by profile, negatives included. Instant attack during key-up is
   load-bearing: it holds `peakRef` above the guard.

3. **Phase 19 closed that question: the guard is honest (§13.10).** Disabling it
   outright (g=1.0) takes blindness to 0.00% everywhere and does *not* recover
   CER — snr-noise4.0 worsens 0.9032 → 0.9173 with 55.4 pp of blindness removed.
   Legacy's high-noise failure is upstream of the guard, and 1.8 is
   Pareto-optimal: no swept value dominates it. **The detector line has no
   open defect left in it.**

**Candidates, in the order the evidence supports:**

- **(a) Farnsworth gap centres** — still the only measured win with no identified
  cost (0.0141 → 0.0000), still untouched. Isolated, and the promotion rule's
  known gaps are unlikely to block it. The right choice if the goal is to finally
  promote something; the win is small (0.0141) against the 0.095 the detector
  line holds on hand-keyed.
- **(b) Real recordings** — the standing prerequisite for the LLR detector, and
  the only way to test generator fidelity (§15), which conditions every number in
  this document.

(a) before (b) only because (b) is a data-collection project rather than an
experiment. This ordering is a judgement about cost, not a measurement.

**A note on where this leaves the work.** Phases 15–19 closed the detector line
without promoting anything: every candidate defect in it is now either measured
and load-bearing, or measured and absent. That is a real result — the remaining
hand-keyed headroom is known not to be reachable by any of the five peak
estimators or by the guard — but it means the next genuine step is (b), which
is data collection rather than another experiment against a generator whose
fidelity is itself unvalidated.

---

## 2. Defects found by inspection

> **⊘ EVIDENCE NEEDED — applies to this entire section unless a subsection says
> otherwise.** Everything below is derived from reading the code. Where a claim
> has since been measured, the outcome is noted inline. Track record so far:
> (a) confirmed and was the whole Stage 1 win; (b) refuted — correcting it makes
> CER *worse*; (c) refuted as a root cause; (d) exact no-op. The remainder are
> still unmeasured.

### 2.1 Kalman timing (`timing.h`)

**(a) DAH measurement gain is wrong by 81×** — `timing.h:309-313`

```c
float impliedDit = durationMs / 3.0f;
float dahR = 9.0f * R;
float K = P / (P + dahR);
```

For `d = 3x + v`, the gain expressed against `impliedDit = d/3` is
`P/(P + R_dah/9)`. The code uses `P/(P + 9R)`. The classifier three lines above
sets `dahVar = 9.0f*P + R`, i.e. assumes `R_dah = R` — so the same function
contradicts itself. With `P ≈ R` the correct gain is ≈0.9; the code computes 0.1.
**Dah observations move the speed estimate ~9× less than they should.** Dah-heavy
text (`O`, `M`, `0`, `9`, `73`) tracks sluggishly.

**(b) R adapts on the posterior residual, not the innovation** — `timing.h:305-306`

```c
ditEst += K * (durationMs - ditEst);   // state updated first
float residual = durationMs - ditEst;  // ...then residual taken against the NEW state
R = R * 0.95f + 0.05f * residual * residual;
```

The residual must be computed *before* the update. As written the update has
already pulled `ditEst` toward `durationMs`, so the residual is systematically
shrunk → **R biased low → filter overconfident → P collapses → tracking stops.**
A plausible direct contributor to the QSB failures, where real edge variation is
rejected as outlier.

**(c) Dead branch conceals an unsolved bootstrap case** — `timing.h:266-270`
✗ **REFUTED as a root cause (§8).** The dead branch is real, but the bimodal
hand-keyed failure it was blamed for was caused by the dah-gain bug (a): with the
gain corrected, a bad seed self-corrects within a few elements. Deferring the
seed until unambiguous also defers timing lock and `retroDecode`, breaking clean
decoding (WPM sweep 0.01 → 0.364). The prediction below is wrong.

```c
if (maxD / minD > 1.8f) { ditEst = minD; } else { ditEst = minD; }
```

Both branches identical. Intent is legible: `ratio > 1.8` means the seed holds
both dits and dahs, so `minD` is a dit. The `else` case — three seed elements of
equal length — is genuinely ambiguous and is silently assumed all-dits. **A
message opening `O`, `MM` or `TT` seeds `ditEst` at a dah → 3× speed error at
lock**, and `retroDecode()` then replays the entire buffer with that wrong dit.
The disambiguating information is available and unused: gaps *within* a character
are element gaps ≈ 1 dit.

**(d) Dimensional error in the R clamp** — `timing.h:319`

```c
R = std::clamp(R, std::max(ditEst * 0.05f, 4.0f), ditEst * ditEst * 0.1f);
```

`R` is a variance (ms²); the upper bound is correctly quadratic but the lower
bound is linear in ms. `decoding-improvement-plan.md` describes this as "R floor
widened for faster WPM" — it does not do that; it evaluates to 4 ms² at both 15
and 30 WPM. If a 5 % σ floor was intended: `powf(0.05f * ditEst, 2.0f)`.

### 2.2 Decoder (`morse_tree.h`)

**(e) Gap decisions are hard and unrecoverable.** `classifyOff()` computes a
proper posterior over {element, char, word} — then discards it, using it only to
scale display confidence. A CHAR_GAP that was really an ELEMENT_GAP splits one
character in two, and `characterBreak()` resets every path to root, so **both
halves are wrong and nothing downstream can recover.** The beam explores only
element ambiguity — the *less* damaging error type.

**(f) Unmapped tree nodes silently delete characters.** ~40 of 127 nodes carry a
character. `characterBreak()` skips paths whose node has `character == '\0'`, and
callers guard `if (c)`. When garbled elements land the whole beam on unmapped
nodes the character **vanishes** — a deletion, costed identically to a
substitution in CER but with no visual cue to the operator. No fallback to the
best valid ancestor prefix.

**(g) The "letter frequency prior" is not a prior** — `morse_tree.h:162`

```c
score *= (1.0f + 0.1f * prior);   // prior ranges 0.07 (Z) .. 13.0 (E)
```

True prior odds E:Z ≈ 186:1; this compresses them to 2.3:1 — under-weighting by
~80×. A Bayesian combination is `logP += logf(prior)`. No bigram context exists.

### 2.3 Detector (`tone_detector.h`)

**(h) Schmitt asymmetry biases the dit/dah ratio.**
⊘ **EVIDENCE NEEDED — the magnitude has never been measured, and it is the
single most load-bearing unmeasured claim in this document.** The mechanism is
sound but the size is unknown: if the bias is comparable to the jitter σ it
matters, if it is 10× smaller it does not. §12's first step measures it directly
from ground truth. Note the correct statistic is *differential*
(ON stretch = onDelay − offDelay); the front end's constant group delay cancels
in the decode path but not in raw edge scoring.

`onThresh = noise + 0.55·range`,
`offThresh = noise + 0.35·range`. Key-down is declared on rising through 0.55 but
released only on falling through 0.35, so ON durations are systematically
*stretched* and the following gap *shortened*. This is a bias, not noise —
averaging does not remove it. It shifts both the dit/dah boundary and the gap
centers, and inflates what `estimateJitterSigma()` reports (that function also
measures deviation about `dit` rather than about the cluster mean, conflating
bias with jitter).

### 2.4 Corrector (`corrector.h`)

**(i) Confusions are measured in ASCII space, not Morse element space.** The
table at `corrector.h:106-114` mixes valid and implausible entries: `B(-...)`↔`6(-....)`
is one element insertion (correct), but `{'9','N'}` pairs `----.` with `-.`
(element distance 3) and `{'D','='}` pairs `-..` with `-...-` (distance 2). The
right metric is edit distance over dit/dah sequences, computable directly from
the existing tree.

**(j) `confidence > 0.8f → return word` averages over the word** (`corrector.h:42`).
One catastrophically bad character in an otherwise strong word is averaged away
and never examined.

**(k) No callsign database.** `vocabulary::isCallsign()` is a structural regex
(`[A-Z]{1,2}[0-9][A-Z]{1,4}`) accepting millions of strings, so it carries almost
no prior information. A master list (SCP / Master.DTA, ~50 k active calls) turns
callsign decoding into nearest-neighbour against a strong prior.
⊘ **EVIDENCE NEEDED for "cheapest large accuracy win."** That is a ranking claim
with nothing behind it — the benchmark messages contain two callsigns in 71
characters, so the achievable gain on *this* suite is bounded at roughly 0.1 CER
even with perfect callsign recovery. Its real value is on contest/DX traffic that
the suite does not represent. Needs a corpus before it can be ranked.

**(l) The corrector re-derives information the decoder threw away.** Beam search
holds 8 ranked hypotheses per character, then keeps 1. The corrector then
reconstructs alternatives by edit distance — strictly worse than the ranked
lattice that existed moments earlier. Keeping an N-best lattice per word and
rescoring against the vocabulary is nearly free.

### 2.5 Dead code

`Channel::processEnvelope()` (`channel.h:213-309`) duplicates ~95 lines of
`Channel::process()`. It has **zero references in the test suite** despite its
comment claiming "Used for fast tests," and the copies have already diverged:
`process()` has a third `timingFrozen` branch that skips decoding entirely, while
`processEnvelope()` still emits characters when frozen and never updates `wpm`.
Delete it.

### 2.6 Documentation drift

| Location | Says | Actually |
|---|---|---|
| `decoding-improvement-plan.md` | "197 tests, 527 assertions" | 207 tests, 1410 assertions |
| `architecture.md` (Stage 1) | "Settling time ~44 ms (< 40 ms dit at 30 WPM)" | 44 > 40; measured BPF group delay alone is 18.5 ms, chain total ~37 ms+ |
| `morse_tree.h:16` | "Depth 6 → 63 nodes" | `treeSize = 127` (depth 7) |
| `architecture.md` (Stage 3) | Noise floor "25th percentile" | correct, but window is ~2 s — worth stating |

---

## 3. Test methodology problem

`cw_test_signals.h:62` sets `unsigned seed = 42`. Across all 207 test cases
**exactly one** overrides it (`test_benchmark.cpp:66`). Every noise, jitter, QSB,
QRM and QRN benchmark is a **single realization**.

Consequences:

- CER is quantized to `1/refChars`. On `MSG_CQ` (23 chars) one error = 0.043, so
  no threshold between 0 and 0.043 is meaningful. The reported `0.043` figures
  are "one error, on seed 42."
- Guarding assertions like `REQUIRE(s.cer < 0.07f)` carry ~1 character of margin
  on one draw.
- Errors are symmetric: a genuine improvement can flip a knife-edge character and
  *fail*; a genuine regression can get lucky and *pass*. The second is more
  dangerous because it is silent.
- The five changes listed as "tried and reverted" in the improvement plan were
  judged on this instrument. Some verdicts may be noise.

**Fix — tighten, never loosen.** Run each profile over N seeds (the suite is <1 s;
there is enormous headroom) and assert on the mean plus a worst-case bound:

```c
REQUIRE(s.cer < 0.07f);                          // 1 draw, ~1 char of slack
→ REQUIRE(meanCER(profile, seeds=24) < 0.045f);  // 24 draws, set at measured mean
  REQUIRE(p95CER (profile, seeds=24) < 0.09f);
```

Implemented in `tests/cw_bench_stats.h` + `tests/test_benchmark_multiseed.cpp`.
Design notes: all profiles use `MSG_FULL()` (69 chars, 3× finer CER quantization
than `MSG_CQ`), and an explicit SNR ladder (`noiseAmp` 1.0→4.0) was added because
the existing suite tops out at `noiseAmp = 1.5` where CER is 0.0 — i.e. **the
decoder is not stressed on the noise axis at all**, so the current tests cannot
detect a filter-bandwidth change even in principle.

### 3.1 Measured baseline (24 seeds, `src/cw/` unmodified)

| Profile | Plan claims | **mean** | ±stderr | median | p95 | worst |
|---|---|---|---|---|---|---|
| clean 15/20/25 WPM | 0.0 | **0.000** | 0.000 | 0.000 | 0.000 | 0.000 |
| mild noise (0.5) | 0.0 | **0.000** | 0.000 | 0.000 | 0.000 | 0.000 |
| moderate noise (1.5) | 0.0 | **0.000** | 0.000 | 0.000 | 0.000 | 0.000 |
| farnsworth 1.5 | 0.0 | **0.000** | 0.000 | 0.000 | 0.000 | 0.000 |
| **hand-keyed 15 WPM** | **0.0 "Perfect"** | **0.158** | 0.040 | 0.070 | 0.535 | 0.578 |
| **hand-keyed 20 WPM** | 0.043 | **0.200** | 0.055 | 0.056 | 0.761 | 0.859 |
| **hand-keyed 25 WPM** | 0.043 | **0.131** | 0.034 | 0.099 | 0.155 | 0.887 |
| QSB fading | 0.0 | 0.012 | 0.004 | 0.000 | 0.042 | 0.042 |
| QRM | 0.0 | 0.010 | 0.005 | 0.000 | 0.070 | 0.085 |
| QRN | 0.0 | 0.003 | 0.003 | 0.000 | 0.000 | 0.070 |
| contest 20 WPM | 0.0 | 0.003 | 0.002 | 0.000 | 0.037 | 0.037 |
| farnsworth 2.0 | 0.043 | 0.014 | 0.000 | 0.014 | 0.014 | 0.014 |
| worst case | 0.57 | 0.624 | 0.021 | 0.662 | 0.789 | 0.803 |
| snr noise=2.0 | — | 0.083 | 0.017 | 0.056 | 0.239 | 0.282 |
| snr noise=3.0 | — | 0.816 | 0.035 | 0.789 | 1.084 | **1.394** |
| snr noise=4.0 | — | 0.903 | 0.010 | 0.916 | 0.972 | 0.986 |

**The clean and AWGN profiles hold up exactly as documented — those 0.0 figures
are real.** The hand-keyed figures do not: `handkeyed-15wpm` is documented as
"0.0 Perfect" and measures **0.158 mean**, ~4× worse.

**The hand-keyed distribution is bimodal** (median 0.070 vs mean 0.158, p95 0.535).
That is not graceful degradation under jitter — it is an *intermittent hard
failure* on a minority of seeds. This is the signature §2.1(c) predicts: when the
opening elements happen to seed `ditEst` at a dah, `retroDecode()` replays the
entire buffer at 3× the wrong speed. **Testable prediction: fixing the Kalman
bootstrap should collapse the p95 toward the median without moving the median.**

CER > 1.0 at `noise=3.0` confirms the decoder emits more garbage than the
reference contains rather than squelching — consistent with §4.5.

Suite cost: 207 → 213 test cases, 1410 → 1430 assertions, runtime <1 s → ~17 s.

---

## 4. Experiment: filter bandwidth vs CER

**Hypothesis.** `dsp.h` uses `lowPass(100, 100, 1000)` for the pre-detection
complex BPF. AG1LE's fldigi data (quoted in the improvement plan) reports 35 Hz
beating 68 Hz by 20× in CER at −10 dB. If bandwidth dominates, this filter is
leaving several dB on the table.

**Method.** `EnvelopeDSP::init` parameterized with defaulted cutoff/transition
args (defaults preserve current behaviour); benchmark built as a separate target;
16–24 seeds per cell; `MSG_FULL()`; 15 and 20 WPM. Signal power 1, noise density
`2·noiseAmp²/8000` per Hz → `SNR(B) = 4000/(noiseAmp²·B)`.

### 4.1 Filter geometry (internal rate 1000 Hz ⇒ 1 sample = 1 ms)

| cut/trans | taps | ENBW (Hz) | group delay (ms) |
|---|---|---|---|
| **100/100 (current)** | 38 | **167.6** | 18.5 |
| 40/50 | 76 | 64.1 | 37.5 |
| 30/40 | 95 | 47.6 | 47.0 |
| 20/30 | 126 | 31.3 | 62.5 |
| 12/15 | 253 | 19.2 | 126.0 |

Baseline ENBW is 167.6 Hz, not the 200 Hz nominal — the Nuttall window rolls off
the corners.

### 4.2 CER vs bandwidth × noise, 15 WPM (16 seeds, capped at 1.0)

| filter | ENBW | delay/dit | n=2.0 | n=2.5 | n=3.0 | n=3.5 | n=4.0 |
|---|---|---|---|---|---|---|---|
| **100/100 base** | 167.6 | 0.46 | 0.10 | 0.34 | **0.76** | 0.85 | 0.92 |
| 40/50 | 64.1 | 0.70 | 0.01 | 0.07 | 0.32 | 0.75 | 0.95 |
| 30/40 | 47.6 | 0.82 | 0.00 | 0.03 | 0.22 | 0.54 | 0.84 |
| 20/30 | 31.3 | 1.01 | 0.00 | 0.01 | 0.08 | 0.23 | 0.52 |
| 12/15 | 19.2 | 1.81 | 0.00 | 0.01 | **0.02** | 0.10 | 0.23 |

At `noiseAmp = 3.0` (3.5 dB in 200 Hz), CER **0.76 → 0.02** — a 38× reduction.
Monotone in bandwidth across the entire grid.

### 4.3 Threshold gain (50 % copy point, CER < 0.10, 24 seeds/point)

| filter | ENBW | **measured gain** | predicted from ENBW |
|---|---|---|---|
| 100/100 base | 167.6 | 0.00 dB | 0.00 dB |
| 40/50 | 64.1 | +1.91 | +4.17 |
| 30/40 | 47.6 | +2.76 | +5.47 |
| 20/30 | 31.3 | +3.67 | +7.29 |
| 12/15 | 19.2 | **+4.83** | +9.40 |

20 WPM peaks at 12/15 = +3.99 dB. At 8/10 the decoder **breaks entirely** (never
reaches 50 % copy) — its apparent large "gain" is a no-crossing artifact.

### 4.4 Verdict

- **CONFIRMED for the pre-detection BPF.** 167.6 → 19.2 Hz ENBW buys +4.8 dB
  (15 WPM) / +4.0 dB (20 WPM).
- **REFUTED for the smoothing LPF.** Holding BPF at 100/100 and sweeping the
  smoothing filter 80/100 → 10/15 gave *zero* gain and then damage: at
  n=3.0/15 WPM, 0.688 → **0.880**; at n=1.5/20 WPM, 0.005 → **0.333**. Magnitude
  detection is nonlinear — **post-detection filtering cannot recover
  pre-detection SNR.** Only the complex BPF ahead of the magnitude operator
  matters.
- **Gain is ~50 % of theory.** 9.4 dB of noise-BW reduction yields 4.8 dB; edge
  smearing eats the rest. The AG1LE framing (bandwidth ⇒ near-proportional CER)
  **overstates** what is recoverable here. The earlier working estimate of
  "~7.6 dB thrown away," derived from noise bandwidth alone, was wrong.

### 4.5 Cost side — why the AWGN optimum is not the operating point

| profile | base | 40/50 | 20/30 | 15/20 | 12/15 |
|---|---|---|---|---|---|
| handKeyed 20 wpm | 0.320 | 0.327 | 0.340 | **0.511** | 0.475 |
| jitter20 15 wpm | 0.108 | 0.125 | 0.115 | 0.129 | **0.157** |
| jitter20 20 wpm | 0.101 | 0.119 | 0.116 | 0.129 | **0.150** |
| worstCase 15 wpm | 0.581 | **0.364** | 0.428 | 0.430 | 0.435 |
| QRM / clean / farnsworth | 0.000 | ~0.00 | ~0.00 | ~0.00 | 0.000 |

Below ~20/30 the hand-keying and jitter penalty bites. Note `worstCase` *improves*
0.581 → 0.364 at 40/50 — the profile the improvement plan calls a "physical
limit" moves substantially with a filter change alone.

Also: at very low SNR narrow filters **emit garbage rather than going silent**
(raw uncapped CER up to 2.95 for 40/50 at n=8, vs 0.96 for baseline). The squelch
in `channel.h` (`sqFactor = (snr-3)/7`) is calibrated against the wide filter and
must be retuned alongside any narrowing.

**Recommended operating point: BPF 30/40 to 20/30** (ENBW 48–31 Hz) — +2.8 to
+3.7 dB, no measurable regression on hand-keyed/jitter/QRM, delay at or just over
one dit. **Leave the smoothing LPF at 80/100.** The genuinely correct answer is
to make the BPF adaptive to locked WPM (targeting ENBW ≈ 2/T_dit), which the
improvement plan already lists as Phase 10 "needs glitch-free filter transition"
— solvable by running two filters and crossfading.

---

## 5. Prior art — what "Bayesian CW decoding" actually refers to

### 5.1 CW Skimmer *is* Bayesian by its author's own statement — but the method is undocumented

**The author does state the method.** [dxatlas.com/CwSkimmer](https://dxatlas.com/CwSkimmer/),
verified verbatim 2026-07-19:

> "a high sensitivity CW decoding algorithm based on the methods of **Bayesian
> statistics**"

Other specifications on the same page:

> "up to 700 signals can be decoded in parallel on a 3-GHz P4 if a wideband
> receiver is used"
>
> "a DSP processor with a noise blanker, AGC, and a **sharp, variable-bandwidth
> CW filter**"
>
> "a fast waterfall display, with a resolution sufficient for reading Morse Code
> dots and dashes visually"

Note **"variable-bandwidth"** — independent support for making the BPF adaptive
to locked WPM rather than fixed (§4.4, Stage 2).

**What is missing is any reproducible detail.** No state space, no likelihood
model, no priors, no pruning rule. The user manual contains no algorithm
description. A sweep of the Skimmertalk reflector archive (125 of 168 monthly
files, ~136 000 lines, 2008–2024) found **4 posts total by VE3NEA**, all one-line
support replies, and **0 occurrences of `"bayes"`** in 136 000 lines — i.e. the
author never elaborated in the venue where he answered users for 16 years.

**Specifically not attributable to CW Skimmer by any source: HMM, Viterbi,
"blind synchronization", ITU-R noise modelling.** These are community conjecture.
The "50 Hz channels" figure traces to N4ZR describing *Skimmer Server's spotting
logic*, not the decoder front end.

The practical consequence: "make it work like CW Skimmer" is not an implementable
specification. Bell 1977 (§5.2) is the documented instance of the same idea and
is the reproducible target.

The only substantive statement by VE3NEA is generic methodology, relayed by AG1LE:
> "Express all your prior knowledge in the form of probabilities, and use
> observed data to update those probabilities."

**Callsign database usage is post-decode only.** Master.dta gates display colour
and whether a spot is emitted (1–5 required repetitions by validation level).
No evidence it enters a trellis. This *supports* §2.4(k) — but as post-processing.

### 5.2 Bell 1977 — the actual documented algorithm

E. L. Bell, *Optimal Bayesian estimation of the state of a probabilistically
mapped memory-conditional Markov process with application to manual Morse
decoding*, PhD, Naval Postgraduate School, 1977 (NSA-sponsored). Abstract:

> "the resulting optimal decoder is shown to consist of a **denumerable but
> exponentially expanding set of linear Kalman filters operating on a
> dynamically evolving trellis**."

- **Two keystates only** (`x_k ∈ {0,1}`). Morse symbols live in a *memory
  function* `a_k = f_a(s_k, a_{k-1})`, not as states.
- **Speed is discrete**: `R_k ∈ {10..60}` WPM integers, changing only at keystate
  transitions, by symbol-conditional increments (±0/2/4 after dot/dash/element-
  space, ±5 after word-space, ±10 after pause).
- **The Kalman filter tracks fading amplitude, not speed.** Its role is
  closed-form evaluation of the likelihood integral.
- **Pruning is M-path/stack, not fixed-width beam**: keep enough paths that
  cumulative posterior ≥ `P_opt` (=0.9), plus ≥1 path per element state.
  Typical saved paths: **8–16** — comparable to the current beam width of 8/12.
- **Front end**: optimal BPF for 50 WPM computed as **25 Hz**; Bell widened to
  ~100 Hz to tolerate carrier chirp of ~50 Hz. Consistent with §4's finding that
  the AWGN optimum is not the operational optimum.
- Bell modelled letters as **equiprobable and independent** and identified better
  text models as the "largest payoff" remaining.

Performance (n ≈ 200 letters — 90 % CI for a measured 10 % is 7–14 %, so treat
as indicative): Bell beat Gold's MAUDE but **lost to Howe's quasi-Bayes** at high
SNR; his advantage is at low SNR.

**Actionable artifact: [github.com/ag1le/morse-wip](https://github.com/ag1le/morse-wip)**
— GPL-3.0 C++/C port of Bell's original Fortran, subroutines mapping 1:1
(`kalfil.c`, `probp.c`, `noise.c`), ~3335 lines, 8–80 WPM, reported ~3 % base
error rate.

### 5.3 Mills 1977 — two findings that contradict this codebase's design

D. L. Mills, *Real-Time Recognition of Manual Morse Telegraphy Using Nonlinear
Estimation and Viterbi Decoding*, TR-554, Univ. Maryland
([DTIC ADA051001](https://apps.dtic.mil/sti/tr/pdf/ADA051001.pdf)).

**(a) Amplitude information is harmful under fading.** Mills, p. 49–50:
> "The algorithm described here does not use signal amplitudes when calculating
> likelihoods. Use of this information under severe multipath conditions has
> proven **worthless and even a source of error**. Bell's algorithm includes the
> use of signal amplitudes."

`tone_detector.h` is entirely amplitude-threshold driven (`signalPeak`,
`noiseFloor`, fractional Schmitt thresholds), and the profile it fails worst on
is **noise + QSB** — i.e. fading. This is the condition Mills singles out.

**(b) Timing jitter is multiplicative ⇒ log-duration is the right coordinate.**
Mills models observation noise variance as proportional to distance from origin,
`σ² = k²(x₁² + x₂²)`, over mark/space *pairs*.

This is the deepest finding for `timing.h`. `KalmanTiming` works in **linear
milliseconds with additive R**, and consequently needs `dahVar = 9*P + R`, a
separate dah update path, and a dimensionally-inconsistent clamp — all of which
are patches over a coordinate mismatch. In **log-duration space**:

- dit and dah differ by a constant offset `log 3` — one state, no separate means
- jitter is homoscedastic — one `R`, no `9R` / `R/9` confusion
- the §2.1(a) 81× gain bug **cannot be expressed**, it disappears structurally
- gap centers 1:3:7 become offsets `0 : log3 : log7`, and Farnsworth stretching
  becomes an additive shift on the gap cluster only

Mills' a-priori mark/space table (Table 3), directly usable as gap priors —
note these are *joint* over (element, following-gap), unlike the current
independent `pElem *= 5.0f` style priors:

| gap following | dot | dash |
|---|---|---|
| element space | .327 | .229 |
| character space | .218 | .153 |
| word space | .044 | .031 |

Mills tracks speed **outside** the trellis ("Ratio-Weighted Estimation"), gating
the adaptation weight on how close the observed max/min ratio is to the ideal 3:1
— a directly portable improvement over the current unconditional Kalman update.

### 5.4 fldigi's "SOM" is not a SOM — correction to `decoding-improvement-plan.md`

📎 **SECOND-HAND.** Based on a source read of `fldigi/src/cw_rtty/cw.cxx` that has
not been re-verified against a checked-out fldigi tree in this session. The
`decoding-improvement-plan.md` entry it corrects is marked ⚠ CONTESTED for the
same reason. Both should be settled by reading the file directly.

That document states: *"SOM (Self-Organizing Map) decoder gives ~5 % better CER
than legacy at -13 dB."*

Reading `fldigi/src/cw_rtty/cw.cxx` directly: `som_table[]` is a **hardcoded,
hand-written codebook** (weights literally `0.33` for dit, `1.0` for dah, never
trained), and `find_winner()` is plain **nearest-neighbour Euclidean distance**
over a 7-element vector. It is a fixed template matcher. AG1LE's trained-SOM
experiments (7×7 / 10×10 / 20×20 on ~40 000 characters) were real but **never
shipped in fldigi**. The claim should be struck or re-sourced.

Genuinely worth copying from fldigi: `src/cw_rtty/view_cw.cxx` — the Signal
Browser is already **30-channel** (`VCW_MAXCH 30`), each with its own `fftfilt`,
AGC and `two_dots` tracking.

### 5.5 Closest open-source analogue

[hotairfred/SparkGap](https://github.com/hotairfred/SparkGap) (GPL-3.0, alpha):
two-state HMM + forward-backward + EM over noise mean/amplitude/variance/WPM,
**speed marginalized over 16 WPM bins (8–60)**, late quantization at posterior
0.5, 64-wide beam search, callsign extraction via SCP + edit distance.
Architecturally the right shape. Its "~2× CW Skimmer recall" claim rests on a
single author-run 15-minute recording — statistically thin.

### 5.6 Negative result on continuous latent speed

**No published work treats dit duration as a genuine continuous latent variable
estimated jointly inside a trellis.** Bell discretizes speed into the trellis;
Mills decouples it into an external tracker; Howe 1976 (quasi-Bayes, beat Bell,
no free copy located) is closest. The Rao-Blackwellized particle filter
construction — Bell's discrete structure, continuous log-dit-duration as a
conditionally-Gaussian sub-state, Mills' `σ² ∝ |x|²` observation model — appears
unoccupied. **Caveat:** Bell's Table XVII shows his speed-adaptive trellis buys
only 0–2 % extra error resistance at 9 dB, so the marginal value over a good
decoupled tracker is unestablished. Do not start here.

---

## 6. Proposal

> **STATUS: partly superseded by §11.** Ordering below was by *expected* return.
> Oracle ablation has since measured per-stage headroom, which changes the
> ranking: Stage 5 (gap ambiguity) is worth less than assumed on the AWGN
> profiles and Farnsworth gap classification — not listed here at all — is a
> measured, isolated win. Stages still marked ⊘ are unmeasured estimates.

Ordered by measured or expected return per unit risk. Each stage gated on the
full suite plus no regression in any multi-seed mean (§3.1).

### Stage 1 — Kalman defects (§2.1 a–d)
Pure bug fixes, no new concepts. **Testable prediction: the bimodal hand-keyed
distribution collapses — p95 falls toward the median while the median barely
moves.** If that prediction fails, the bimodality has another cause and the
diagnosis in §3.1 is wrong. Low risk, high diagnostic value; do it first
precisely because it is falsifiable.

### Stage 2 — Narrow the pre-detection BPF to 30/40 (§4)
⚠ **SUPERSEDED — do not act on this as written.** The "no regression" below came
from a single-axis AWGN sweep. The core × profile matrix later showed `+bpf40`
breaks clean 25 WPM decoding (0.0000 → 0.1549) and `+bpf30` is worse on
hand-keyed (§10). Narrowing the BPF is still worth real gain, but not at these
settings and not without retuning `sqFactor` (§4.5).

**Measured +2.8 dB** threshold gain, no regression on hand-keyed/jitter/QRM,
group delay 47 ms (0.59 dit at 15 WPM). Leave the smoothing LPF at 80/100 — §4.4
shows narrowing it *hurts*. Requires retuning `sqFactor` in `channel.h`, which is
calibrated against the wide filter (§4.5). Then re-baseline the SNR ladder.

### Stage 3 — Delete dead code, fix docs (§2.5, §2.6, §5.4)
Zero risk. Remove `processEnvelope()`; correct the tap/assertion/tree-depth
figures and the fldigi SOM citation.

### Stage 4 — Move timing to log-duration (§5.3b)
The highest-value structural change that is still local to one file. Converts
three separate patches into one coherent model and makes §2.1(a) unexpressible.
Recommended over any trellis work.

### Stage 5 — Carry gap ambiguity into the beam (§2.2e) ⊘
Currently the gap posterior is computed and discarded, and `characterBreak()`
resets all paths. Extending each beam path with a pending-gap hypothesis makes
element *and* gap errors jointly recoverable. Use Bell's M-path/stack pruning
(cumulative posterior ≥ 0.9, ≥1 path per element state) rather than fixed width —
his typical 8–16 paths matches the current beam, so cost is comparable.

⊘ **"This is where the remaining worst-case CER lives" was an assumption and
§11 contradicts it.** With a perfect detector *and* a perfect duration model,
`worstcase` reaches 0.0792 — so at most 0.0792 of the 0.6244 is available to
anything downstream of timing, including this stage. The other 0.545 is in
detection and duration modelling.

### Stage 6 — Proper priors (§2.2g, §5.3) ⊘
`logP += logf(prior)` instead of `score *= (1 + 0.1f*prior)`; Mills' joint
mark/space table instead of independent gap priors; a character bigram model.
Bell identified text modelling as the "largest payoff" remaining, and he was
working with equiprobable letters.

### Stage 7 — Callsign database (§2.4k, §5.1) ⊘
SCP/Master.dta as **post-decode validation**, matching what CW Skimmer documents.
Independent of all DSP work; can be done any time.

### Not recommended
- **Trellis over raw envelope samples with amplitude likelihoods** — Mills
  reports amplitude information is "worthless and even a source of error" under
  the fading conditions this decoder fails on (§5.3a).
- **Continuous latent speed inside a trellis** (§5.6) — unoccupied ground, but
  Bell measured only 0–2 % gain for the adaptive-speed trellis.
- **Rewriting toward "what CW Skimmer does"** — no such published description
  exists (§5.1).

## 7. Pluggable decoding architecture (Stage A — done)

Two levels, because two kinds of decoder must coexist:

```
IDecodeCore                    ── IQ in, characters out. The outer contract.
    ├── StagedCore             ── composes independently swappable stages
    │      IFrontEnd           ── EnvelopeFrontEnd (bandwidth parameterized)
    │      IDetector           ── SchmittDetector
    │      ITiming             ── AdaptiveTimingStage (7 strategies)
    │      ISymbolDecoder      ── BeamSymbolDecoder
    │      + inline sequencing (pre-lock capture, retroDecode, flush, freeze)
    │
    ├── BellCore   (future)    ── implements IDecodeCore DIRECTLY
    └── MillsCore  (future)
```

**Why Bell cannot be a stage.** Bell 1977 is "a set of linear Kalman filters
operating on a dynamically evolving trellis" — detection, timing and symbol
decoding are one inseparable computation. Forcing it through
`IDetector`→`ITiming`→`ISymbolDecoder` would destroy the property that makes it
work. It sits beside `StagedCore` as a peer; both appear in the same registry.

**Sequencing is inline in `StagedCore`, not a fifth stage.** A jointly-estimating
core replaces that logic wholesale, so there is no second implementation to
validate an `ISequencer` interface against. Extract it when one exists.

**Post-processing is outside every core.** `Channel` implements `CharSink`; a
core reaches decoded text only through `emitChar` / `flushWord` / `emitWordGap` /
`clearEmitted`. Corrector, conversation tracking and text buffering are therefore
identical across cores, so the matrix measures **decoding**, not post-processing.

**Front-end bandwidth is a declared core parameter**, not a hidden one — §4
showed bandwidth alone moves CER by 38×, so a core must not be able to win the
benchmark by quietly narrowing its filter.

### Registry

`core_registry.h` is the single list of benchmarkable configurations; the config
UI and the benchmark matrix both enumerate it. Adding a core or a stage
combination is one entry.

**The table of variants and their measured status lives in `architecture.md`.**
It is not duplicated here — this copy had already drifted to 10 rows against 18
actual entries at the time it was removed; there are now 20.

**Promotion rule.** A variant becomes the default only when it is no worse on
*every* profile. "Better on average" is not sufficient: a profile that decoded
better before and worse after is a regression, not a trade. Losing variants are
kept — they are the comparison baseline for future cores.

⚠ That rule has a known gap: it has no notion of operating regime, so a profile
where the decoder is already useless carries the same veto as one where it
works. `+kalman2` is blocked by `noise4.0` at 0.9032 vs 0.9888 — 90% against 99%
character error, a regime nobody copies in. Unresolved.

**Commands: see `architecture.md` → Running the benchmarks.** Run each suite
**once** and post-process the saved output — do not re-run to extract different
fields.

### Refactor gate

A pure extraction must change nothing. Verified by capturing the 22-row
characterization table before and after: **byte-identical**. Suite 214 → 215
cases (registry test added), 1431 → 1440 assertions, all green. `channel.h`
533 → 159 lines, which also removed the dead `processEnvelope()` of §2.5.

## 8. Stage 1 — Kalman corrections (done, NOT promoted)

Shipped as the registry variant **`legacy+kalman2`**, not as a change to
`legacy`. `legacy` remains byte-identical to its pre-refactor behaviour and every
gate still passes at its original threshold. **No threshold was raised.**

### Why a variant rather than a promotion

`kalman2` is better on 10 of 13 profiles and worse on 1 — but "better on
average" is not a promotion criterion. A profile that decoded better before and
decodes worse now is a regression, not a trade. Keeping both in the registry
costs one line, preserves the comparison, and lets a future core (log-domain
timing, Bell) be measured against *both* the historical baseline and the
corrected one.

| profile | `legacy` | `legacy+kalman2` | `legacy+bimodal` |
|---|---|---|---|
| clean-15 / clean-25 | 0.0000 | 0.0000 | 0.0000 |
| handkeyed-15 | 0.1579 (p95 0.535) | **0.0340** (p95 0.070) | 0.0399 |
| handkeyed-20 | 0.2001 (p95 0.761) | **0.0446** (p95 0.085) | 0.0469 |
| handkeyed-25 | 0.1309 | 0.1320 | **0.0563** |
| qsb | 0.0117 | **0.0070** | 0.0364 |
| qrm | 0.0100 | **0.0018** | 0.0053 |
| qrn | 0.0029 | 0.0023 | **0.0006** |
| farnsworth 2.0 | 0.0141 | 0.0141 | 0.0282 |
| worstcase | 0.6244 | **0.3498** | 0.6408 |
| noise2.0 | 0.0827 | **0.0399** | 0.0915 |
| noise3.0 | 0.8163 | **0.7060** | 1.0681 |
| **noise4.0** | **0.9032** | 0.9888 | 1.3762 |

WPM tracking RMS, handkeyed-15: 3.25 → **1.10**.

`worstcase` improved **44%** (0.624 → 0.350) from timing corrections alone —
more than any filter-bandwidth change achieved, and further evidence against the
"physical limit" claim in `decoding-improvement-plan.md`.

### What was applied, and what was tried and reverted

| # | change | outcome |
|---|---|---|
| 1b | **dah gain**: `K = P/(P + 9R)` → `P/(P + R/9)` | ✅ the entire win |
| — | **confidence-gated learning**: skip the state update when classification confidence < 0.60 | ✅ recovered the noise loss 1b caused |
| 1c | **R floor** made quadratic (§2.1d) | ⚪ exact no-op — R rarely reaches the floor |
| 1a | **R innovation** (§2.1b) | ❌ reverted |
| 1d | **seed disambiguation** (§2.1c) | ❌ reverted |

**1a is the most instructive failure.** Computing the innovation before the state
update is textbook-correct, and it *degrades*: hand-keyed 0.158 → 0.223 alone,
and worstcase 0.383 → 0.397 combined with 1b. The buggy low R keeps the Kalman
gain high, **compensating for the model being in the wrong coordinate** — linear
milliseconds with additive noise, when jitter is multiplicative (§5.3b, Mills
1977). Fixing one equation inside a misspecified model removes the compensation
without addressing the cause. This is empirical support for Stage 4 (log-domain
timing) and is recorded in a code comment so it is not "fixed" again.

**1d refuted §2.1(c).** I predicted the seed ambiguity caused the bimodal
hand-keyed distribution. It did not — 1b alone collapsed p95 from 0.535 to 0.070.
The seed defect is real, but its impact was entirely *mediated* by the gain bug:
with K≈0.1 a bad seed could never be corrected; with K≈0.9 it self-corrects
within a few elements. Deferring the seed until unambiguous also defers timing
lock and `retroDecode`, breaking clean decoding (WPM sweep CER 0.01 → 0.364).
The correct fix uses gap durations, which `classifyOn()` cannot see.

### Promotion criterion for `kalman2`

Promote to default only when `noise4.0` no longer regresses. Note that profile is
CER ≈ 0.90 vs 0.99 — total failure either way — so the useful work is not
"win noise4.0" but "find why fast tracking hurts at very low SNR". The ins/del
split says `legacy` fails *silently* there (ins 0.0006, del 0.840) while
`kalman2` emits more garbage — and for a skimmer, insertions are worse than
deletions.

## 9. Stage 4 — log-duration timing (done, NOT promoted)

Registry variant **`legacy+log`**. `legacy` unchanged; no threshold touched.

Motivated directly by Stage 1: fix 1a (textbook-correct Kalman) *degraded*
because the linear-millisecond model is misspecified. `LogTiming` tracks
`x = ln(dit_ms)` instead, per Mills 1977 (§5.3b).

**What the coordinate change buys structurally** — not tuning, but defects that
become unwriteable:

* dit and dah differ by a fixed offset `ln 3` → one state, one offset, rather
  than two independently-drifting means.
* jitter is homoscedastic → measurement noise is `R` for both, so the gain is
  `P/(P+R)` in **both** branches. **The 81× dah-gain error of §2.1(a) cannot be
  expressed in this model.**
* gap centres 1:3:7 become additive offsets `0 : ln3 : ln7`, so Farnsworth
  stretching is a shift rather than a scale change.

| profile | `legacy` | `+kalman2` | **`+log`** | `+bimodal` |
|---|---|---|---|---|
| clean-15 / clean-25 | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| handkeyed-15 | 0.1579 | 0.0340 | **0.0288** | 0.0399 |
| handkeyed-20 | 0.2001 | 0.0446 | **0.0329** | 0.0469 |
| handkeyed-25 | 0.1309 | 0.1320 | 0.0857 | **0.0563** |
| qsb | 0.0117 | **0.0070** | 0.0100 | 0.0364 |
| qrm | 0.0100 | **0.0018** | **0.0018** | 0.0053 |
| qrn | 0.0029 | 0.0023 | **0.0006** | **0.0006** |
| farnsworth 2.0 | 0.0141 | 0.0141 | 0.0141 | 0.0282 |
| worstcase | 0.6244 | **0.3498** | 0.3615 | 0.6408 |
| noise2.0 | 0.0827 | 0.0399 | **0.0340** | 0.0915 |
| noise3.0 | 0.8163 | **0.7060** | 1.0839 | 1.0681 |
| noise4.0 | **0.9032** | 0.9888 | 1.5487 | 1.3762 |

Best-in-class on 5 profiles, tied on 5. **Blocked on heavy noise**: noise3.0 and
noise4.0 are worse than both `legacy` and `kalman2`, driven by insertions
(noise3.0 ins = 0.514 vs legacy 0.092) — it floods garbage where `legacy` goes
silent.

### The outlier gate — tried, rejected, and instructive

The garbage flood has an obvious cause: the confidence gate measures *dit-vs-dah
discrimination*, not plausibility. A 5 ms spike against an 80 ms dit is 14σ from
any real element, yet scores confidence ≈ 1.0 because it is unambiguously
"more dit than dah". So the filter learns from spikes at full confidence.

Adding a plausibility gate (distance to the *nearest* hypothesis) fixed exactly
that — and broke interference profiles at every threshold tried:

| gate | noise3.0 | qrm | qrn |
|---|---|---|---|
| none | 1.0839 | **0.0018** | **0.0006** |
| 3σ, relative to `P+R` | 0.9830 | 0.1966 | 0.0980 |
| ln(2), absolute | **0.7025** | 0.3932 | 0.0980 |

Two distinct failures. The σ-relative gate **tightens as the filter gains
confidence** — P and R shrink, the window narrows, ordinary jitter is rejected
and learning stops. Making the tolerance absolute removes that lockup but still
costs qrm/qrn, which reveals the deeper point: **learning from outliers inflates
R, which widens the acceptance window and keeps the filter tolerant.** Rejecting
them keeps R small and the filter brittle.

This is the same compensation pattern as §2.1(b) — a mechanism that looks wrong
in isolation is load-bearing. Recorded in a code comment so it is not
re-attempted blindly.

### Huberised update — tried, refuted, and it localises the fault

The gate experiment suggested the two effects should be separated: an outlier
must not move `x`, but should still teach `R`. `legacy+logrobust` implements
exactly that — Huber weight `w = min(1, k/z)` on the state update, `R` always
learning from the full innovation.

| profile | `+log` | `+logrobust` |
|---|---|---|
| handkeyed-15 | 0.0288 | **0.0276** |
| handkeyed-25 | 0.0857 | **0.0810** |
| qrm | **0.0018** | 0.0065 |
| qrn | **0.0006** | 0.0029 |
| noise3.0 | **1.0839** | 1.1009 |
| noise4.0 | **1.5487** | 1.8046 |

Marginal gains on jitter, marginal losses elsewhere, **no effect on the flood**.

**This is the useful result.** Three structurally different interventions in the
timing model have now failed on the same blocker:

| intervention | noise3.0 | qrm |
|---|---|---|
| none | 1.0839 | 0.0018 |
| hard gate, σ-relative | 0.9830 | 0.1966 |
| hard gate, absolute | 0.7025 | 0.3932 |
| soft Huber weight | 1.1009 | 0.0065 |

When three independent attacks on a subsystem all miss, the fault is not in that
subsystem. **The timing model cannot repair what the detector hands it**: once
the Schmitt trigger emits a spurious key transition, it becomes an "element"
no matter how its duration is subsequently modelled. Insertions at noise3.0
(ins = 0.51) are *detector* events, not timing errors.

### Promotion criterion — now redirected

Promotion of `legacy+log` is blocked on `tone_detector.h` (§2.3), not on timing.
The detector applies a fixed-fraction Schmitt threshold to a magnitude envelope
and emits a hard binary decision; the optimal test in Rayleigh/Rice noise is a
likelihood ratio, and the soft information is discarded before timing ever sees
it. That is the next piece of real work, and it unblocks `kalman2` and `log`
simultaneously — both are blocked on the same profile family.

## 10. Methodology — what this investigation taught

Recorded because the pattern was consistent and is likely to repeat.

### Code reading predicted outcomes about half the time

| prediction (from reading the code / literature) | outcome |
|---|---|
| Filter bandwidth worth ~7.6 dB (from noise BW) | ❌ measured +4.8 dB max, +2.8 dB usable |
| Smoothing LPF bandwidth matters | ❌ narrowing it *hurts*; only the pre-detection BPF matters |
| Seed ambiguity causes the bimodal hand-keyed failure | ❌ it was the dah gain |
| Correcting the R innovation helps | ❌ regressed hand-keyed 0.158 → 0.223 |
| Dah gain is 81× wrong and matters | ✅ the entire Stage 1 win |
| Log-duration is the right coordinate | ✅ best-in-class on 5 profiles |
| An outlier gate fixes the heavy-noise flood | ❌ broke QRM/QRN at every threshold |
| Huber separates "move x" from "teach R" | ❌ no effect on the blocker |
| The detector holds the heavy-noise error (§9, by elimination) | ✅ confirmed by oracle ablation, and understated |
| Detector and timing contribute about equally on hand-keyed | ❌ hand-keyed is almost entirely timing |
| Schmitt asymmetry biases the dit/dah ratio (§2.3h) | ✅ the bias is real: +9.8% of a dit |
| …and the threshold ratio gap is what causes it | ❌ accounts for 11% |
| …then instant-attack peak tracking causes it (57% correlated) | ❌ refuted by `+peakgate` (§13.8); cause still unknown |
| `clean-25` false detections are an alignment artifact | ❌ real, deterministic, and a distinct defect |
| Removing spurious detector events improves CER | ❌ 17× fewer, CER unchanged, 3 profiles regressed |
| Correcting the edge bias helps hand-keyed | ✅ 0.1579 → 0.0628, 67% of the headroom |
| A slow-attack peak tracker sits between edge and fade timescales | ❌ stretch rises monotonically; a slow EMA tracks the *mean*, not the peak |
| Shortening the percentile window fixes QSB | ✅ 0.5070 → 0.0194, monotone |
| …and costs nothing elsewhere | ❌ estimator variance breaks stationary profiles |
| Dual windows resolve the trade | ⚠ partly — QSB fixed (0.0088), 7 better / 2 worse, still not promotable |

Every correction came from measurement, not from re-reading the code. **Build the
instrument before using it** — the multi-seed harness and the matrix each paid
for themselves on first use, the latter by revealing that the recommended BPF
narrowing regresses clean 25 WPM decoding (0.0000 → 0.1549), which the
single-axis AWGN experiment had entirely missed.

### Measure attribution, not just comparison

The matrix ranks cores against each other. Nothing in it expresses how close any
of them is to what is achievable, and that is the quantity that decides where to
work: a core at CER 0.70 is near-optimal if the ceiling is 0.65 and is wasting
0.68 if the ceiling is 0.02 — same number, opposite decisions.

Ablation-by-degradation (change a stage, watch CER move) also confounds two
hypotheses: "this stage is fine" and "my change to it was bad." That ambiguity
is why three negative results were needed to localise the fault in §9, and why
the conclusion was still an inference. **Oracle ablation runs the other
direction** — replace a stage with a perfect one — and a null result there is
unambiguous. §11 reproduced §9's conclusion in one 16 s run, and corrected a
second conclusion that three prior experiments had not surfaced.

### An instrument's own defects look exactly like findings

The first oracle run reported a floor of ~0.05 on hand-keyed and 0.1608 on
`worstcase` that neither oracle cleared, which reads as an irreducible limit.
It was not: `ClairvoyantTiming` classified against the *nominal* dit while the
generator applies `weightBias`, putting the decision boundary at 160 ms when the
true elements were 92 ms and 222 ms — misclassifying ~8% of `worstcase` dahs.
Recording the element model in the generator and reading it from there halved
`worstcase` (0.1608 → 0.0792) and cut hand-keyed by 3×.

The general shape: an oracle that re-derives ground truth instead of reading it
will silently encode the deriver's assumptions, and its errors are indistinguish-
able from real floors. Take truth from the source, and self-test the instrument
(`[oracle-self]`) in the always-on suite.

### Apparent bugs can be load-bearing

Three separate mechanisms in `timing.h` look wrong in isolation and are
compensating for something real:

1. **R adapting on the posterior residual** keeps R low, keeping the Kalman gain
   high, compensating for the linear-millisecond coordinate being misspecified.
2. **Learning from timing outliers** inflates R, widening the acceptance window;
   rejecting them keeps the filter brittle and wrecks QRM/QRN.
3. **The historical dah gain being 9× too small** made a mis-seeded `ditEst`
   unrecoverable — which is why the seed defect appeared to be the root cause.
4. **The matched-filter resize transient** (§12.2) is unambiguously a bug and
   produces spurious events on a *noiseless* signal — but the artifacts fall
   below `minElementMs()` and are discarded, so removing them costs more than it
   saves (§12.3).

The general shape: a local "fix" removes a compensation without addressing the
misspecification it was compensating for, and CER gets worse. Before correcting
a formula, check what currently absorbs its error.

**Corollary — a stage metric is not the objective.** §12 built a detector-level
instrument and the metric it was designed to optimise (false-event rate) turned
out to be nearly uncorrelated with CER: −93% on `clean-25` bought zero, −24% on
`noise3.0` bought 1.4%. Deletions are what cost. Always close the loop back to
CER before treating a stage metric as a target.

### Negative results localise faults

Three structurally different interventions in the timing model all failed on the
same blocker. That is stronger evidence than any single success: it says the
fault is not in that subsystem. Cheaper than continuing to tune it.

### Aggregate CER hides failure *mode*

At `noise4.0` three cores score 0.90 / 1.38 / 2.12 — comparable-looking numbers
describing completely different behaviour: `legacy` goes silent (ins 0.0006,
del 0.840), `+bimodal` emits garbage, `+kmeans` floods it (ins 1.435). For a
skimmer, insertions are worse than deletions — a false spot costs more than a
missed one. Always read the ins/del/sub split before judging a core.

## 11. Oracle ablation — where the error actually is

§9 concluded "the fault is upstream in the detector" by elimination: three
structurally different timing interventions all failed on the same profiles.
That is an inference, and it cost three implementations to reach. This section
measures it directly.

### Method

The generator knows the exact key state at every sample, so a stage can be
replaced by a perfect one and the CER difference read off. `generateMessage()`
now records ground truth alongside the samples:

* `TruthSegment[]` — sample-exact tone/silence boundaries with their semantic
  class (DIT/DAH, ELEMENT/CHAR/WORD gap).
* `ElementModel` — the durations actually keyed, **taken from the generator
  itself** rather than re-derived, so the oracle cannot drift from it.
  `weightBias` and `farnsworthRatio` move these off the nominal 1:3 and 1:3:7
  ratios and an oracle that assumes the nominal ratios measures its own error.

Two oracle stages (`tests/cw_oracle.h`):

| stage | knows | isolates |
|---|---|---|
| `OracleDetector` | true key transitions | headroom from perfect detection |
| `ClairvoyantTiming` | true element and gap durations | headroom from a perfect duration model |

Run as a **2×2 rather than a cumulative chain**, because the interaction turns
out to carry the result. The refactor is inert: full suite green and the 22-row
characterization table byte-identical.

**These live in `tests/`, deliberately not in `coreRegistry()`.** A core that
requires ground truth cannot run on a real signal and must never appear in the
module's config UI. `Channel::initWithCore()` exists for exactly this: hosting a
core the registry cannot construct.

### Results (24 seeds, MSG_FULL, mean CER)

| profile | baseline | +det | +tim | +det+tim |
|---|---|---|---|---|
| clean-15wpm | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| handkeyed-15wpm | 0.1579 | 0.0651 | **0.0153** | 0.0153 |
| handkeyed-20wpm | 0.2001 | 0.1103 | 0.0158 | **0.0129** |
| handkeyed-25wpm | 0.1309 | 0.0716 | 0.0182 | **0.0100** |
| qsb | 0.0117 | **0.0000** | 0.0082 | 0.0000 |
| qrn | 0.0029 | **0.0000** | 0.0035 | 0.0000 |
| farnsworth-2.0 | 0.0141 | 0.0141 | **0.0000** | 0.0000 |
| worstcase | 0.6244 | 0.4149 | 0.3644 | **0.0792** |
| snr-noise2.0 | 0.0827 | **0.0000** | 0.0423 | 0.0000 |
| snr-noise3.0 | 0.8163 | **0.0000** | 0.6614 | 0.0000 |
| snr-noise4.0 | 0.9032 | **0.0000** | 0.8292 | 0.0000 |

Whole sweep: 15.8 s.

### How to read the zeros — they are a trivial bound

In this architecture the envelope is consumed **only** by the detector;
everything downstream sees event *times* and nothing else. An oracle detector
therefore makes the pipeline noise-blind by construction, and 0.0000 on the AWGN
ladder is guaranteed rather than earned.

The supportable claim is **"100% of AWGN-profile error is attributable to the
detector"**, not "a better detector reaches zero." Do not quote these as
achievable targets.

The `+tim` column is the load-bearing one: it is an oracle that leaves noise in
the signal path, so its *failure* to move noise3.0 (0.8163 → 0.6614) and
noise4.0 (0.9032 → 0.8292) is a real measurement, not an artifact.

### The two failure families are structurally different

| profile family | interaction | reading |
|---|---|---|
| hand-keyed | **sub-additive** — `+tim` alone = `+det+tim` | detector errors are small perturbations of *correct* events; a good duration model absorbs them |
| worstcase | **super-additive** — 0.41 / 0.36 alone, 0.079 together | the detector emits *spurious* events; no duration model can absorb an element that never existed |

That is the same distinction §9 reached by elimination, now measured, and it
explains why the timing interventions failed only on the noise profiles.

### Findings

1. **§9's attribution is confirmed and was understated.** On the entire AWGN
   ladder plus QSB and QRN, timing contributes nothing. The three failed timing
   interventions were not badly designed; they were aimed at a stage holding
   none of the error.
2. **Hand-keyed is a timing problem, not a detector problem.**
   ⚠ **AMENDED by §12.6 — this wording was too strong.** `+tim` alone does take
   handkeyed-15 from 0.1579 to 0.0153 with `+det` adding nothing on top, and
   that measurement stands. But sub-additivity describes the *combination*: a
   perfect duration model masks detector error rather than proving the detector
   holds none. A real detector fix (`legacy+peak`) reaches 0.0628 — **67% of the
   total hand-keyed headroom, and equal to the oracle detector's own 0.0651** —
   without touching timing.
3. **`worstcase` requires both stages.** Either alone buys ~40%; both buy 87%.
   Phase 13 alone will not fix this profile.
4. **Farnsworth is purely gap classification.** `+det` changes nothing
   (0.0141 → 0.0141); `+tim` takes it to exactly zero. The adaptive gap-centre
   algorithm is the entire fault — isolated, cheap, and unrelated to Phase 13.
5. **The "Noise+QSB is a Physical Limit" claim is refuted**, not merely
   contested: `worstcase` 0.6244 → 0.0792 with no change to the physics of the
   signal. See `decoding-improvement-plan.md`.

### Caveats on the instrument

* **`OracleDetector` also disables the squelch.** `StagedCore` derives
  `sqFactor` from `getSNR()`; reporting a high SNR removes detection error and
  squelch suppression together, so `+det` bounds the pair. Splitting them means
  constructing it with the measured SNR instead.
* **`ClairvoyantTiming` knows `weightBias` exactly and with zero adaptation
  delay.** Reachable in principle — `KMeansTiming` already tracks dit and dah
  centres independently — but never instantly, so `+tim` on hand-keyed is a
  genuine but optimistic bound.
* Gap boundaries are geometric means, correct for the generator's multiplicative
  jitter. Detected gaps also carry the Schmitt trigger's *additive* edge bias, so
  the practical optimum is not purely geometric. Switching arithmetic → geometric
  moved `qsb` `+tim` 0.0006 → 0.0082, within noise at 24 seeds; recorded, not
  acted on.
* **Everything here is measured on the synthetic generator.** §15 lists the
  fidelity gaps. An oracle result on `worstcase` says nothing about real
  multipath, because the generator has none.

### Instrument self-checks

`[oracle-self]` runs in the always-on suite, not hidden behind `[.]`. It asserts
that the oracle detector reproduces the generator's transitions exactly, that the
recorded element model matches what the generator keys under weight bias and
Farnsworth, and that a clean signal with both oracles decodes at CER 0. If any
break, every headroom number above is measuring the harness.

## 12. Detector ground-truth metrics

§11 settled *attribution* — the detector holds all the AWGN-family error. This
settles *kind*: hallucinating events, dropping them, or misplacing their edges
need different fixes, and CER cannot tell them apart.

### Method

A `RecordingDetector` decorator captures what the real detector emits, in
absolute time. A decorator rather than a reimplementation of `StagedCore`'s
chain: the events must come through the real front end and matched filter, or
the group delay and edge shaping being measured are not the ones the decoder
sees.

Alignment is the hard part. The front end's latency (57–85 ms measured) exceeds
half a dit at 25 WPM, so naive windowed matching rejects correct pairs. So:
cross-correlate the two key-state square waves to estimate the constant lag
(robust to heavy false detection in a way that matching the first few edges is
not), then DP-align the transition lists allowing insertions and deletions.

**Bias is reported differentially** — `ON stretch = d_up − d_down`. The constant
group delay cancels in the decode path, because timing consumes only differences
between event times. Only the *asymmetry* between rising and falling edges
biases the dit/dah ratio.

### Results (24 seeds, MSG_FULL)

| profile | false /elem | miss /elem | ON stretch %dit | edge sd %dit | delay ms |
|---|---|---|---|---|---|
| clean-15wpm | 0.000 | 0.000 | **+9.80** | 4.36 | 65.0 |
| clean-25wpm | 0.088 | 0.006 | +7.94 | 3.62 | 57.0 |
| handkeyed-15wpm | 0.027 | 0.006 | +6.96 | 6.38 | 70.1 |
| handkeyed-25wpm | 0.149 | 0.008 | +4.27 | 5.16 | 57.8 |
| qsb | 0.016 | 0.010 | +2.19 | 6.73 | 65.5 |
| qrm | 0.012 | 0.004 | +7.56 | 3.94 | 64.9 |
| qrn | 0.009 | 0.004 | +7.50 | 3.90 | 64.9 |
| worstcase | 0.101 | 0.116 | −2.42 | 12.50 | 77.1 |
| snr-noise2.0 | 0.037 | 0.019 | −2.00 | 9.66 | 66.4 |
| snr-noise3.0 | 0.411 | 0.323 | −8.79 | 16.43 | 77.7 |
| snr-noise4.0 | 0.429 | **1.384** | −15.78 | 17.84 | 84.8 |

Sweep cost: 3.7 s.

### 12.1 §2.3(h) confirmed — but the mechanism attributed here was wrong

On a **noiseless** signal with zero false detections and zero misses, every ON is
stretched by **9.80% of a dit** (7.84 ms). Pure bias, no noise involved. The
existence of the bias is confirmed and is not in doubt.

> **✗ REFUTED — the mechanism originally recorded here.** This section
> attributed the bias to the Schmitt threshold gap and reported the arithmetic
> agreement below as confirmation:
>
> | term | value |
> |---|---|
> | post-lock matched filter `0.4 × dit` turns a step into a ramp | 32 ms |
> | threshold gap `0.55 − 0.35` at high SNR | 0.20 |
> | crossing separation on that ramp: `0.20 × 32` | 6.40 ms |
> | keying envelope, one-pole τ = 5 ms: `τ·ln(0.35/0.45)` | 1.25 ms |
> | predicted | 7.65 ms |
> | measured | 7.84 ms |
>
> §12.6 falsified it directly. Removing the threshold gap entirely
> (`EDGE_SYMMETRIC`, both ratios 0.45) should have eliminated the 6.40 ms term
> and left ~1.75% of a dit. **Measured: 9.80% → 8.69%, i.e. 11% of the
> predicted effect.** The agreement was a coincidence of magnitude.
>
> The replacement attribution — instant-attack peak tracking, §12.6 — was
> *also* refuted later (§13.8). Both are kept here because the arithmetic is a
> good example of how convincing a wrong mechanism can look when it lands on
> the right number, and because it happened twice in the same line of work.

**The +9.8% figure is the clean-signal case.** It varies by profile — +6.96 on
handkeyed-15, +2.19 on qsb, and it goes *negative* under heavy noise (−15.78 at
noise4.0), where misses dominate instead. See the §12 results table.

**The bias is a constant time offset, not a proportional one** — so a dit becomes
`dit + 7.8 ms` (×1.098) while a dah becomes `dah + 7.8 ms` (×1.033): the
observed dah:dit ratio is **2.82, not 3.0**, and the element gap shrinks to
0.90 dit. Every model in `timing.h` assumes exact 1:3 and 1:3:7 ratios and is
being fed distorted ones. That part stands — it depends only on the offset being
constant, not on what causes it.

### 12.2 Matched-filter resize injects a dropout mid-element

`clean-25wpm` showed 0.088 false detections per element on a signal with **zero
noise and zero jitter**. Because `profileClean` has neither, the signal is
bit-identical across seeds — and the result reproduced exactly on every seed,
ruling out an alignment artifact:

| profile | truth transitions | detected |
|---|---|---|
| clean-15wpm | 342 | 342 |
| clean-25wpm | 342 | **356** |
| handkeyed-25wpm | 342 | 358–368 |

**Mechanism.** `StagedCore::applyMatchedFilter` resizes on every block from
`int(ditDuration × 0.4)` and zeroes the ring buffer on change, so the output
collapses to ~0 and takes W samples to refill.

**Proof of attribution.** Re-running the identical signal with `ClairvoyantTiming`
— which reports a constant dit, so the window never changes — gives exactly 342
transitions on both profiles. The detector's only input is `mfBuf`, and the only
timing-dependent quantity feeding it is `computeFilterWindow()`; classification
runs *downstream* of the detector on the same buffer and cannot reach it. So
swapping the timing stage changed detector output ⟹ it did so through the window.
There is no other path in the data flow.

**Corroboration.** Every spurious event is an UP/DOWN pair 3 ms and 13 ms into an
element onset — `ditEst` updates in `classifyOn()` at key-*up*, so a new window
arrives between elements and the next onset hits a freshly-zeroed filter.

**Why it is quiet at 15 WPM and loud at 25 WPM:** `0.4 × dit` is 32.0 at 15 WPM
but 19.2 at 25 WPM, and the §12.1 bias inflates measured dits, pushing `ditEst`
toward the boundary where `int()` flips. ⊘ The second half of that (that the bias
is what pushes it across) is inference, not measured.

### 12.3 Fixing it does not reach CER — `legacy+mf`

`MF_PRESERVE` refills the ring buffer with the running mean instead of zero, so
the output is continuous across a resize.

| profile | false/elem | CER | ins / del |
|---|---|---|---|
| clean-25wpm | 0.088 → **0.006** | 0.0000 → 0.0000 | 0.000/0.000 → 0.000/0.000 |
| handkeyed-25wpm | 0.149 → **0.009** | 0.1309 → 0.1256 | 0.001/0.057 → 0.000/**0.076** |
| handkeyed-15wpm | 0.027 → 0.009 | 0.1579 → 0.1567 | 0.000/0.076 → 0.000/0.084 |
| qsb | 0.016 → 0.016 | 0.0117 → **0.0147** | — |
| worstcase | 0.101 → 0.071 | 0.6244 → **0.6467** | 0.035/0.213 → **0.021**/0.217 |
| snr-noise2.0 | 0.037 → 0.028 | 0.0827 → **0.1045** | 0.001/0.046 → 0.001/0.058 |
| snr-noise3.0 | 0.411 → 0.310 | 0.8163 → 0.8052 | 0.092/0.272 → **0.050**/0.281 |
| snr-noise4.0 | 0.429 → 0.467 | 0.9032 → 0.8967 | 0.001/0.840 → 0.002/0.809 |

**17× fewer spurious events, and CER does not follow.** Three profiles regress.
**Not promoted.** Kept as `legacy+mf` and `legacy+mf+log`.

The spurious events were already absorbed: `clean-25wpm` decoded at CER 0.0000
*with* 14 spurious transitions. The dropout is ~10 ms and `minElementMs()`
rejects anything below `0.3 × dit` = 14.4 ms at 25 WPM, so the split does not
create an extra element — it truncates a real one. ⊘ That mechanism is inference
from the geometry, but it is consistent with every column: insertions fall
(`worstcase` 0.035 → 0.021, `noise3.0` 0.092 → 0.050) while deletions rise
(`handkeyed-25` 0.057 → 0.076, `noise2.0` 0.046 → 0.058).

**Third instance of the load-bearing-bug pattern** (§10). The pipeline has three
absorbers downstream of the detector — min-element filter, beam search,
corrector. A defect producing only sub-minimum-element artifacts is invisible to
CER by construction, and removing it takes away a perturbation the timing model
had adapted to.

### 12.4 The planning consequence: false-event rate is a poor CER proxy

This instrument was built to identify an optimisation target. It found that the
obvious one is the wrong one:

| change | false rate | CER |
|---|---|---|
| `noise3.0`, legacy → +mf | −24% | −1.4% |
| `clean-25`, legacy → +mf | −93% | 0 (already 0) |

**Deletions are what cost.** At `noise4.0` the miss rate is 1.384 per element —
~70% of transitions never fire — and `del = 0.840` dominates. Phase 16 should
target **misses and edge bias, not false events**.

One reservation: CER weights insertions and deletions equally, but §10 records
the skimmer argument that a false spot costs more than a missed one. By *that*
criterion `legacy+mf` is an improvement — insertions fall on every profile. The
promotion rule is CER-based, so it stays a variant; if the product goal is
spotting rather than transcription, this trade may be the right one. ⊘ No
spotting-level metric exists to decide it.

### 12.5 Removing the bias — `legacy+sym`, `legacy+edge`

Two routes with different costs. `EDGE_SYMMETRIC` collapses both thresholds to
0.45, removing the ratio gap and the hysteresis with it. `EDGE_COMPENSATE` keeps
the hysteresis and pulls the release edge earlier by the modelled stretch
`(onRatio − offRatio) × 0.4 × ditEst`.

| profile | ON stretch %dit | | | CER mean | | |
|---|---|---|---|---|---|---|
| | legacy | +sym | +edge | legacy | +sym | +edge |
| clean-15wpm | +9.80 | +8.69 | +0.90 | 0.0000 | 0.0000 | 0.0000 |
| clean-25wpm | +7.94 | +7.64 | −0.55 | 0.0000 | 0.0000 | 0.0000 |
| handkeyed-15wpm | +6.96 | +6.16 | −2.23 | 0.1579 | 0.1667 | **0.1197** |
| handkeyed-25wpm | +4.27 | +4.14 | −3.35 | 0.1309 | 0.1626 | **0.1197** |
| qsb | +2.19 | +1.41 | −10.02 | 0.0117 | 0.0205 | **0.0088** |
| qrm | +7.56 | +6.59 | −1.38 | 0.0100 | 0.0147 | **0.0018** |
| qrn | +7.50 | +6.50 | −1.51 | 0.0029 | 0.0147 | 0.0029 |
| worstcase | −2.42 | −7.42 | −17.42 | 0.6244 | **1.2283** | **0.5205** |
| snr-noise1.0 | +3.97 | +2.66 | −9.08 | 0.0000 | 0.0106 | 0.0000 |
| snr-noise2.0 | −2.00 | −4.08 | −17.48 | 0.0827 | 0.4137 | **0.1332** |
| snr-noise3.0 | −8.79 | −15.67 | −18.87 | 0.8163 | 2.0335 | **0.9237** |
| snr-noise4.0 | −15.78 | −16.82 | −22.46 | 0.9032 | 1.1338 | **0.8709** |

**`+sym` is refuted twice over.** It barely moved the bias (§12.1) *and*
removing the hysteresis is catastrophic at low SNR — `worstcase` 0.6244 →
1.2283, `noise3.0` 0.8163 → 2.0335. Hysteresis is load-bearing.

**`+edge` improves 6 profiles and regresses 2.** Not promoted. The regressions
are diagnosable from the stretch column: at low SNR the thresholds widen to
0.65/0.25, so the modelled correction grows to `0.40 × 0.4 × dit` = 16% of a dit
while the real bias does not, and it **over-corrects to −17%**. The correction is
scaled by a quantity §12.1 shows is not the cause.

### 12.6 The real mechanism — `legacy+peak`

`PEAK_INSTANT_ATTACK` sets `signalPeak = v` the moment `v` exceeds it. On a
rising edge the threshold reference therefore tracks the signal upward and the
threshold `noiseFloor + ratio × range` chases it; on the falling edge the
reference holds, because decay is a 0.5 s exponential. That is an on/off
asymmetry **independent of the threshold ratios** — which is exactly why `+sym`
failed to remove the bias.

`PEAK_PERCENTILE` references the threshold to the 90th percentile of the same
subsampled window the noise floor already uses: lagged and stable by
construction, so it cannot follow the sample being thresholded. Only the
threshold reference changes — `signalPeak` still drives the impulse blanker and
`getSNR` — so the variant isolates one mechanism.

**Contribution to the ON stretch, clean-15wpm:**

| removed | stretch | share of the 9.80% |
|---|---|---|
| nothing (`legacy`) | +9.80 | — |
| threshold ratio gap (`+sym`) | +8.69 | **11%** |
| instant attack (`+peak`) | +4.24 | **57%** — correlation only; the mechanism was refuted by `+peakgate` (§13.8) |

The remaining ~32% is unattributed — keying ramp, debounce and matched-filter
shaping are the candidates. ⊘ Not separated.

**CER:**

| profile | legacy | +edge | **+peak** | oracle `+det` (§11) |
|---|---|---|---|---|
| clean-15 / clean-25 | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| handkeyed-15wpm | 0.1579 | 0.1197 | **0.0628** | 0.0651 |
| handkeyed-25wpm | 0.1309 | 0.1197 | **0.0898** | 0.0716 |
| qsb | 0.0117 | 0.0088 | **0.5065** | 0.0000 |
| qrm | 0.0100 | **0.0018** | 0.0070 | 0.0000 |
| qrn | 0.0029 | 0.0029 | 0.0029 | 0.0000 |
| worstcase | 0.6244 | **0.5205** | 0.6749 | 0.4149 |
| snr-noise2.0 | 0.0827 | 0.1332 | **0.0276** | 0.0000 |
| snr-noise3.0 | 0.8163 | 0.9237 | **0.7682** | 0.0000 |
| snr-noise4.0 | 0.9032 | **0.8709** | 0.9278 | 0.0000 |

**On handkeyed-15 the real detector scores 0.0628 against the oracle detector's
0.0651** — a percentile reference reaches the perfect-detector bound on that
profile. Largest single-change improvement measured in this investigation.

**This corrects §11 finding 2.** "Hand-keyed is a timing problem, not a detector
problem" was too strong. The oracle's sub-additivity was real, but it described
the *combination*: a perfect duration model masks detector error, which is not
the same as the detector holding none. `+peak` captures
`(0.1579 − 0.0628) / (0.1579 − 0.0153)` = **67% of the total hand-keyed
headroom** by changing the detector alone.

**Not promoted — `qsb` regresses 43×** (0.0117 → 0.5065), plus `worstcase` and
`noise4.0`. The cause is a timescale collision: the percentile window is ~2 s and
`profileQSB` fades at 0.3 Hz (3.3 s period), so the reference remembers the loud
half of the cycle and the threshold sits far too high through the null.

**This suggested a fix — which §13.1 then refuted.** The reasoning was that the
reference should be slow relative to a keying edge (~32 ms) and fast relative to
fading (~3300 ms), so an asymmetric tracker with an attack constant of
~200–500 ms should sit in the gap. ✗ **Do not try this.** An EMA with a slow
attack is not a peak tracker with an adjustable response time — it converges to
the *mean* envelope, and the ON stretch rises monotonically with the constant.
The property that matters is element-independence, not timescale. See §13.1.

### Instrument self-checks

`[detector-self]` injects a known 37 ms constant latency and a known 8 ms edge
asymmetry into synthetic transition lists and asserts the scorer recovers both
without inventing false detections.

Needed because an earlier version of the diagnostic located spurious events at
*raw* detected time without removing the group delay. That placed them "inside
element gaps" and would have supported a wrong mechanism — the same class of
error as the `ClairvoyantTiming` weight-bias bug in §10.

## 13. Phase 16 — the threshold reference

§12.6 localised the dominant cause of the +9.8% ON stretch to instant-attack
peak tracking. This phase tried to remove it. Four experiments, two refuted
hypotheses, one confirmed mechanism, and **no promotable variant** — but the
trade is now fully characterised rather than guessed.

### 13.1 Attack constant — ✗ REFUTED

Hypothesis: the reference should be slow relative to a ~32 ms keying edge and
fast relative to a ~3300 ms fade. Two orders of magnitude apart, so an
asymmetric EMA with an attack constant in between should work.

ON stretch, clean-15wpm, noiseless:

| reference | instant | 50 | 100 | 200 | 300 | 500 | 800 | 1200 ms | percentile |
|---|---|---|---|---|---|---|---|---|---|
| stretch %dit | +9.80 | +10.90 | +12.59 | +16.41 | +19.68 | +25.28 | +30.51 | +36.53 | **+4.24** |

**Monotonically worse, with no interior optimum.** Hand-keyed degrades 0.1579 →
0.6209 and clean 15 WPM — perfect at every other setting in this investigation —
breaks entirely at 800 ms.

**Why the framing was wrong.** An asymmetric EMA with a slow attack is not a
peak tracker with an adjustable response time; it converges to the **mean**
envelope, ~0.5A for 50%-duty CW. The threshold becomes
`noiseFloor + 0.55 × 0.5A` ≈ 0.275A, crossed very early on the rise and released
very late on the fall. At 1200 ms there is no peak behaviour left at all.

**What actually matters is element-independence, not timescale.** A percentile
window spans many key cycles, so its estimate does not depend on where within an
element the current sample sits. An EMA of *any* constant varies within the
element: fast ones chase the edge, slow ones sag toward the duty-cycle mean.
That is why the sweep has no interior optimum.

### 13.2 Percentile window length — mechanism confirmed, second effect found

The window was ~2000 ms against a 3300 ms QSB cycle, which §12.6 identified as
why `+peak` broke on that profile. Shortening it:

| profile | legacy | 250 | 500 | 750 | 1000 | 1500 | 2000 | 3000 |
|---|---|---|---|---|---|---|---|---|
| handkeyed-15wpm | 0.1579 | 0.1092 | 0.0951 | 0.0933 | 0.0792 | 0.0810 | **0.0628** | 0.0657 |
| **qsb** | 0.0117 | **0.0194** | 0.1250 | 0.3809 | 0.4613 | 0.4971 | 0.5065 | 0.5070 |
| worstcase | 0.6244 | **0.4630** | 0.5957 | 0.6761 | 0.6901 | 0.6825 | 0.6749 | 0.6989 |
| snr-noise1.0 | 0.0000 | 0.0123 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 | 0.0000 |
| snr-noise3.0 | 0.8163 | 0.9748 | 0.7958 | 0.8022 | 0.8075 | 0.8275 | **0.7682** | 0.8257 |

**QSB improves monotonically as the window shortens — 0.5070 → 0.0194, a 26×
recovery.** The prediction held. `worstcase` at 250 ms reaches 0.4630, the best
any single change has produced on that profile.

**But a second effect appears.** `snr-noise1.0` (0.0000 → 0.0123) and
`snr-noise3.0` (0.7682 → 0.9748) have a *stationary* level with nothing to
track, and a 250 ms window holds only 31 subsampled entries. Window length
trades two things that had been treated as one: short windows follow amplitude
change, long windows estimate it precisely (variance ~ 1/N). No single length
serves both a stationary and a fading signal.

### 13.3 Dual window — the trade made adaptive

Run 250 ms and 2000 ms concurrently; switch to the short estimate when they
disagree by more than a relative threshold. **The disagreement is itself the
fade detector** — no new signal model.

| profile | legacy | t=0.05 | t=0.10 | t=0.25 | t=0.40 |
|---|---|---|---|---|---|
| handkeyed-15wpm | 0.1579 | 0.1062 | 0.1068 | 0.1056 | 0.1050 |
| **qsb** | 0.0117 | **0.0088** | 0.0094 | 0.0252 | 0.1426 |
| worstcase | 0.6244 | **0.4730** | 0.5070 | 0.5381 | 0.6831 |
| snr-noise1.0 | 0.0000 | 0.0123 | 0.0123 | 0.0123 | 0.0123 |
| snr-noise3.0 | 0.8163 | 0.9325 | 0.9536 | 0.9266 | 0.8392 |

**QSB reaches 0.0088 — better than legacy**, and better on 7 profiles overall.
First variant in this line to improve QSB at all.

Remaining regressions are identical to the `250 only` column: the short window's
own numbers leaking through on stationary profiles. **The switch was firing on
estimator noise, not on fading.** A 250 ms window has relative standard error
≈ 1/√31 ≈ 18% against a 5% threshold, so on a stationary signal the two windows
disagree *by construction*. Raising the threshold does not fix it: at t=0.40 the
switch stops firing on noise (noise3.0 → 0.8392) but also stops firing on real
fades (qsb → 0.1426).

### 13.4 Short-window length × persistence

Two independent ways to make estimator noise smaller than the fade it must
detect: lengthen the short window (lower variance), or require N consecutive
disagreements (estimator noise is uncorrelated between subsamples, a fade is
not).

| profile | legacy | 250/1 | 250/16 | **500/1** | 500/16 |
|---|---|---|---|---|---|
| handkeyed-15wpm | 0.1579 | 0.1068 | **0.0640** | 0.0951 | 0.0945 |
| handkeyed-25wpm | 0.1309 | 0.0869 | 0.0998 | 0.0974 | 0.1004 |
| **qsb** | **0.0117** | **0.0088** | 0.0223 | 0.1215 | 0.1309 |
| qrm | 0.0100 | 0.0070 | 0.0070 | 0.0070 | 0.0070 |
| qrn | 0.0029 | 0.0059 | 0.0029 | 0.0035 | 0.0029 |
| worstcase | 0.6244 | 0.4754 | 0.4953 | 0.6039 | 0.6414 |
| snr-noise1.0 | 0.0000 | 0.0123 | **0.0000** | **0.0000** | 0.0000 |
| snr-noise2.0 | 0.0827 | 0.0370 | 0.0411 | **0.0241** | 0.0252 |
| snr-noise3.0 | 0.8163 | 0.9225 | 0.8656 | **0.7529** | 0.8087 |
| snr-noise4.0 | 0.9032 | 0.9478 | 0.9102 | **0.8867** | 0.8779 |
| **better / worse** | — | 6 / 4 | 5 / 3 | **7 / 2** | 6 / 2 |

**Both refinements worked, on the same regression, by different routes.**
Lengthening to 500 ms (62 entries, halved standard error) returns `noise1.0` to
0.0000 and takes `noise3.0` to 0.7529, *better than legacy*. Persistence at
250 ms achieves the same recovery and gets hand-keyed to 0.0640 — against
0.0628, the best figure measured anywhere in this investigation.

**But both work by making the switch less eager, and tracking a fade needs it
eager.** That is the whole trade in one sentence.

### 13.5 Why there is no dominating point

| config | qsb | noise1.0 | noise3.0 | handkeyed-15 |
|---|---|---|---|---|
| legacy (instant attack) | **0.0117** | **0.0000** | 0.8163 | 0.1579 |
| 250/1 (eager) | **0.0088** | 0.0123 | 0.9225 | 0.1068 |
| 250/16 (persistent) | 0.0223 | **0.0000** | 0.8656 | **0.0640** |
| 500/1 (quiet) | 0.1215 | **0.0000** | **0.7529** | 0.0951 |

Legacy's instant attack is not merely adequate on QSB — at 0.0117 it is close to
the best figure in the entire sweep, achieved by having **zero tracking lag**.
Every percentile variant buys within-element stability with a window, and a
window is exactly what costs fade tracking. For this estimator family the two
requirements are in direct opposition, which is why four experiments produced
four trade points and no dominating one.

**Kept in the registry:** `legacy+peakdual` (500/1, best overall spread) and
`legacy+peakdual16` (250/16, best on hand-keyed).

### 13.6 A measurement caveat on the promotion rule

`legacy+peakdual`'s two regressions are not equal. `qsb` 0.0117 → 0.1215 is real
and 10×. `qrn` 0.0029 → 0.0035 is **≈1 character across 24 seeds of a
71-character message** — below what this instrument resolves.

The promotion rule compares point estimates with no reference to their standard
error, so it treats those identically. That is the significance-blindness
counterpart to the operating-regime gap noted in §7. Honestly stated,
`legacy+peakdual` is **7 better, 1 real regression, 1 indistinguishable,
3 ties**. Unresolved.

### 13.7 Next in this line

The diagnosis points somewhere untried. Instant attack's *only* defect is that
it chases the rising edge; its fade tracking is the best in the table. Rather
than replacing it with a windowed estimator, freeze the peak update while the
key state is unstable (during debounce/transition) and let it track instantly
otherwise — keeping zero-lag fade following while removing edge-chasing, instead
of trading one for the other.

**✗ REFUTED — see §13.8.** Built as `legacy+peakgate`, and it refuted the
premise of its own paragraph.

### 13.8 Phase 17: transition-gated peak — refuted twice over

`PEAK_GATED` enables the instant-attack update only while the key is confirmed
down; decay runs unconditionally. Before the first confirmed key-down it
degenerates to instant attack, the same bootstrap the impulse blanker uses.
Run with `./cw_decoder_tests "[peak-gate]" -s` (24 seeds).

| profile | ON stretch legacy / +peak / +gate | CER legacy / +peak / +gate | false/miss legacy → +gate |
|---|---|---|---|
| clean-15wpm | 9.80 / 4.24 / **9.80** | 0.0000 / 0.0000 / 0.0000 | 0.000/0.000 → 0.000/0.000 |
| clean-25wpm | 7.94 / 3.94 / **7.94** | 0.0000 / 0.0000 / 0.0000 | 0.088/0.006 → 0.088/0.006 |
| handkeyed-15wpm | 6.96 / 1.21 / **6.96** | 0.1579 / 0.0628 / 0.1631 | 0.027/0.006 → 0.027/0.011 |
| handkeyed-25wpm | 4.27 / 0.73 / 4.25 | 0.1309 / 0.0898 / 0.1268 | 0.149/0.008 → 0.146/0.015 |
| qsb | 2.19 / −0.45 / 2.16 | 0.0117 / 0.5065 / 0.0252 | 0.016/0.010 → 0.017/0.025 |
| qrm | 7.56 / 1.89 / 7.53 | 0.0100 / 0.0070 / 0.0223 | 0.012/0.004 → 0.012/0.016 |
| qrn | 7.50 / 1.85 / 7.50 | 0.0029 / 0.0029 / 0.0029 | 0.009/0.004 → 0.009/0.004 |
| worstcase | −2.42 / −7.65 / −5.73 | 0.6244 / 0.6749 / **0.8991** | 0.101/0.116 → 0.086/**1.023** |
| snr-noise1.0 | 3.97 / −1.31 / 3.95 | 0.0000 / 0.0000 / 0.0047 | 0.008/0.004 → 0.008/0.008 |
| snr-noise2.0 | −2.00 / −7.07 / −3.75 | 0.0827 / 0.0276 / **0.8967** | 0.037/0.019 → 0.050/**0.856** |
| snr-noise3.0 | −8.79 / −9.27 / −12.04 | 0.8163 / 0.7682 / 0.9642 | 0.411/0.323 → 0.334/**1.401** |
| snr-noise4.0 | −15.78 / −8.96 / −15.44 | 0.9032 / 0.9278 / 0.9742 | 0.429/1.384 → 0.359/1.681 |

**1 better, 8 worse.** No promotion. Kept in the registry as `legacy+peakgate`.

**Refutation 1 — the edge chase does not exist in steady-state keying.**
`+gate` reproduces legacy's ON stretch *exactly* on the three cleanest profiles
(9.80, 7.94, 6.96) and to within 0.03 pp on the rest. An intervention that
removes attack during key-up entirely moves the stretch by nothing, so key-up
attack is not what produces it. The arithmetic agrees: `decayAlpha` is
0.002/sample, so across an 80 ms element gap at 1 kHz internal rate the peak
retains 0.998^80 ≈ 85% of the previous element's amplitude, and the ON threshold
at ≈0.47·A is crossed long before `v` exceeds that held level. Instant attack
and a held reference are the same estimator once keying is underway; the chase
only exists on the very first element.

This does not overturn §12.6's *measurement* (`+peak` does cut the stretch 57%)
— it overturns the mechanism §12.6 assigned to it. Why the percentile reference
moves the stretch is now **unexplained**. ⊘ EVIDENCE NEEDED.

**Refutation 2 — the damage is misses, and the predicted sign was wrong.**
Before measuring, the write-up predicted the gate would *raise* false events
under noise by removing a threshold-raising effect. False events went **down**
on every degraded profile (noise3.0 0.411 → 0.334, noise4.0 0.429 → 0.359,
worstcase 0.101 → 0.086) while CER collapsed. The whole effect is misses:
snr-noise2.0 0.019 → 0.856, a 45× increase.

⊘ **EVIDENCE NEEDED — mechanism.** Consistent with the miss signature and with
`tone_detector.h`'s `dynamicRange < 1.8` guard, which skips the sample and
disables detection outright: under noise, legacy's key-up instant attack is
ratcheted upward by noise spikes, holding `peakRef` above that cutoff and
keeping the detector armed. Gating removes the ratchet, `gatedPeak` decays
toward the noise floor during key-up, and elements stop being detected. Not
measured — the direct test is to log the fraction of samples rejected by the
1.8 guard per variant.

If that holds it is a **fifth load-bearing mechanism** (§10), and the one with
the largest blast radius: legacy's noise robustness would partly depend on noise
spikes inflating its own signal-peak estimate. That is worth knowing about
`legacy` independently of anything to do with peak tracking.

**Scorecard.** Predictions from code reading: 19 made, 10 wrong. Both of this
phase's were wrong, and the second was wrong in sign after the first had already
failed.

### 13.9 Phase 18: the dynamic-range guard probe — §13.8 confirmed

`tone_detector.h` counts every evaluation of `dynamicRange < 1.8` and, under an
opt-in trace, records rejection runs. The probe intersects those runs with the
true key-down intervals, shifted by the measured group delay so chain latency is
not counted as blindness. Run with `./cw_decoder_tests "[guard-probe]" -s`
(8 seeds).

| profile | guard reject (% samples) legacy / +gate | blind during key-ON (%) legacy / +gate |
|---|---|---|
| clean-15wpm | 0.00 / 0.00 | 0.00 / 0.00 |
| clean-25wpm | 0.00 / 0.00 | 0.00 / 0.00 |
| handkeyed-15wpm | 0.03 / 0.03 | 0.00 / 0.00 |
| handkeyed-25wpm | 0.05 / 0.05 | 0.00 / 0.00 |
| qsb | 0.04 / 0.93 | 0.00 / 0.86 |
| qrm | 0.05 / 0.05 | 0.00 / 0.00 |
| qrn | 0.03 / 0.03 | 0.00 / 0.00 |
| worstcase | 5.46 / 46.01 | 0.84 / **45.26** |
| snr-noise1.0 | 0.03 / 0.03 | 0.00 / 0.00 |
| snr-noise2.0 | 1.64 / 40.17 | 0.00 / **40.65** |
| snr-noise3.0 | 22.29 / 66.44 | 7.56 / **64.76** |
| snr-noise4.0 | 68.45 / 80.77 | 55.36 / 77.79 |

**✓ CONFIRMED — §13.8's mechanism.** Blindness tracks §13.8's miss rates
profile by profile, including the negatives: the four profiles where the gate
cost nothing (clean ×2, qrm, qrn) are exactly the four where blindness stays at
0.00%. snr-noise2.0 moves 0.00% → 40.65% blind, 0.019 → 0.856 misses,
0.0827 → 0.8967 CER.

The discriminating detail is that *rejecting* and *rejecting where it matters*
separate. On snr-noise2.0 legacy rejects 1.64% of samples yet is blind during
key-ON 0.00% of the time — every legacy rejection falls in key-up, where the
guard costs nothing. Gating attack to key-down moves those rejections inside
elements. Instant attack during key-up is therefore **load-bearing** (§10, fifth
instance): it holds `peakRef` above the guard so the detector stays armed.

**⊘ EVIDENCE NEEDED — a separate finding about `legacy`.** Legacy is itself
blind for **55.36%** of key-down time on snr-noise4.0 and **7.56%** on
snr-noise3.0, the two profiles where it fails outright (CER 0.9032, 0.8163).
Whether the guard *causes* that failure or merely reports it is untested, and
the two are genuinely hard to tell apart: at 4.0 noise amplitude the dynamic
range may honestly be below 1.8, in which case the guard is doing its job and
the failure is upstream.

The direct test is a one-parameter sweep of the 1.8 constant. If CER at
noise3.0/4.0 improves as the constant falls, the guard is over-triggering and
this is a defect in the shipping default; if CER is flat or worsens while
blindness drops, the guard is honest and the failure is a real SNR limit. Either
outcome is worth having — it is the first candidate in this investigation that
concerns `legacy` rather than a variant. Recorded as Phase 19.

### 13.10 Phase 19: the guard constant swept — the guard is honest

`setGuardThreshold()` makes the constant settable; `legacy` keeps 1.8.
Run with `./cw_decoder_tests "[guard-sweep]" -s` (CER 24 seeds, blindness 8).

Two properties the sweep depends on are **asserted, not assumed** (180 checks):

- **g=1.0 disables the guard.** Measured: 0.00% of samples rejected on all 12
  profiles. The reasoning was `peakRef >= noiseFloor` by construction, but that
  is an inference about the code, and inferences of that kind have a 10-of-19
  failure rate in this investigation, so the test asserts it.
- **g=1.8 is a no-op.** The default column reproduces the registry `legacy` CER
  exactly on all 12 profiles. Without this the sweep could be measuring the
  `setGuardThreshold` plumbing rather than the constant.

| profile | g=1.0 | g=1.3 | g=1.6 | **g=1.8** | g=2.2 | g=3.0 |
|---|---|---|---|---|---|---|
| clean-15wpm | 0.0000 | 0.0000 | 0.0000 | **0.0000** | 0.0000 | 0.0000 |
| clean-25wpm | 0.0000 | 0.0000 | 0.0000 | **0.0000** | 0.0000 | 0.0000 |
| handkeyed-15wpm | 0.1573 | 0.1573 | 0.1573 | **0.1579** | 0.1579 | 0.1714 |
| handkeyed-25wpm | 0.1315 | 0.1315 | 0.1320 | **0.1309** | 0.1238 | 0.1221 |
| qsb | 0.0117 | 0.0117 | 0.0117 | **0.0117** | 0.0217 | 0.2388 |
| qrm | 0.0094 | 0.0094 | 0.0100 | **0.0100** | 0.0106 | 0.0012 |
| qrn | 0.0029 | 0.0029 | 0.0029 | **0.0029** | 0.0000 | 0.0000 |
| worstcase | 0.6526 | 0.6526 | 0.6391 | **0.6244** | 0.6455 | 0.7653 |
| snr-noise1.0 | 0.0000 | 0.0000 | 0.0000 | **0.0000** | 0.0000 | 0.0000 |
| snr-noise2.0 | 0.1966 | 0.1966 | 0.1954 | **0.0827** | 0.0604 | 0.8568 |
| snr-noise3.0 | 0.7887 | 0.7893 | 0.7870 | **0.8163** | 0.9061 | 0.9818 |
| snr-noise4.0 | 0.9173 | 0.9167 | 0.9067 | **0.9032** | 0.9249 | 0.9953 |
| **vs g=1.8 (better/worse)** | 3/4 | 3/4 | 2/4 | — | 3/5 | 3/6 |

Blind during key-ON (%), same sweep:

| profile | g=1.0 | g=1.3 | g=1.6 | **g=1.8** | g=2.2 | g=3.0 |
|---|---|---|---|---|---|---|
| worstcase | 0.00 | 0.00 | 0.09 | **0.84** | 9.24 | 57.56 |
| snr-noise2.0 | 0.00 | 0.00 | 0.00 | **0.00** | 0.35 | 69.79 |
| snr-noise3.0 | 0.00 | 0.00 | 1.19 | **7.56** | 60.93 | 97.10 |
| snr-noise4.0 | 0.00 | 0.16 | 19.19 | **55.36** | 86.41 | 99.63 |

(All other profiles are 0.00 at every value except `qsb`/`handkeyed-15` at
g=3.0.)

**✓ The guard is honest — §13.9's open question is closed.** The discriminator
was: does CER improve as blindness falls? It does not. At g=1.0 blindness is
**0.00% everywhere** and snr-noise4.0 gets *worse* — 0.9032 → 0.9173 with 55.4
percentage points of blindness removed — while snr-noise2.0 more than doubles,
0.0827 → 0.1966. Legacy's high-noise failure is therefore **upstream** of the
guard. The guard reports the SNR limit; it does not create it, and it is not a
defect in the shipping default.

**The 1.8 default is Pareto-optimal in this sweep.** No value dominates it:
every alternative buys 2–3 profiles and loses 4–6. It is the first constant in
this codebase to be swept and found already correct — worth stating explicitly,
because the Constants table's standing caveat is that almost none of them have
been.

**Third instance of stage metric ≠ objective (§10), and the strongest.** 55 pp
of a stage metric eliminated; the objective moved 0.014 the wrong way. The two
earlier instances were `+mf` (17× fewer spurious events, CER unchanged) and ON
stretch (moved the right way twice without CER following).

⊘ **EVIDENCE NEEDED — why suppression helps.** The plausible reading is that in
a regime where detection yields garbage, emitting nothing beats emitting wrong
events, i.e. the guard trades insertions for deletions at a favourable rate.
Untested: this sweep records CER only. The direct test is to re-run it printing
`insRate`/`delRate` per cell, as `[mf-fix]` already does.

<a id="s13-11"></a>
### 13.11 Test-quality hardening (2026-07)

Not a phase — a correction to the instrument, recorded because it invalidates
nothing above but explains why the diagnostic tests changed shape.

Every `[.]`-hidden sweep from Phases 16–19 had been written as a reporting
harness with an assertion that could not fail: `CHECK(true)`, `CHECK(NG > 0)` on
a compile-time constant, `CHECK(bestIdx >= 0)` immediately after the loop that
guarantees it, `CHECK(better + worse >= 0)` on a sum of two counts. They printed
their tables and reported "passed" whatever the numbers were. The published
results are unaffected — the tables were read by eye — but nothing would have
caught a silently dead parameter.

All of them now assert three things:

1. **Value validity** — `std::isfinite`, non-negative.
2. **The swept parameter reaches the code** — some cell must differ from the
   baseline column. A dead knob otherwise prints one value N times and every
   conclusion drawn from the grid is vacuous.
3. **The default is a no-op** where a sweep overrides a shipping constant —
   `[guard-sweep]` asserts `g=1.8` reproduces registry `legacy` exactly on all
   12 profiles, so the sweep cannot be measuring its own plumbing.

`[guard-sweep]` additionally asserts what §13.10 previously only argued: that
`g=1.0` rejects 0.00% of samples, i.e. that it really is the guard-disabled
control.

**One assertion was itself wrong, and it is the useful part of this entry.**
`CHECK(cer <= 1.0f)` looks like a sanity bound and is false: CER is edits ÷
reference length and insertions are unbounded, so a decoder emitting garbage
scores above 1. It failed immediately on `snr-noise4.0` at 1.014 — and the
§12.6 table already contained a 1.1338 that nobody had noticed contradicted the
bound. Replaced with `std::isfinite`. Assert what is invariant, not what looks
tidy.

> ✓ **VERIFIED (2026-07-20).** Always-on suite green at **1617 assertions in 221
> test cases**; `[dual-window]` green at **145 assertions in 1 test case**. The
> hidden sweeps passed individually with the tightened assertions
> (`attack-sweep` 193, `window-sweep` 170, `dual-refine` 217, `peak-gate` 41,
> `guard-probe` 76, `guard-sweep` 180). Assertion count is unchanged at 1617
> because the correction replaced six bounds one-for-one rather than adding any.
>
> The `[dual-window]` table still prints **CER = 1.0141** for `snr-noise4.0` at
> `thresh=0.15` — the cell that failed the bad `c <= 1.0f` bound. It is retained
> deliberately: it is the standing counter-example to treating CER as a fraction,
> and any future assertion that would reject it is wrong for the same reason.

## 14. Next: the detector (planning)

### Status

| item | state |
|---|---|
| Attribution — which stage holds the error | done (§11) |
| Detector characterisation — which *kind* of error | done (§12) |
| Matched-filter resize | measured, `legacy+mf`, not promoted (§12.3) |
| Edge bias — ratio-gap route | refuted, `legacy+sym` (§12.5) |
| Edge bias — event-time route | measured, `legacy+edge`, over-corrects at low SNR (§12.5) |
| Edge bias — threshold-reference route | measured, `legacy+peak`, best result so far, breaks `qsb` (§12.6) |
| Threshold reference — attack constant | refuted (§13.1) |
| Threshold reference — window length | mechanism confirmed, no promotion (§13.2–13.4) |
| **Freeze peak update during transitions** | **refuted — 1 better / 8 worse (§13.8)** |
| Likelihood-ratio detector | blocked on §15 (generator fidelity) |

### Threshold reference — done, no promotion (§13)

Four experiments. The attack-constant framing was refuted; window length is the
right knob and its mechanism is confirmed; dual windows fix QSB but cannot
separate a fade from estimator noise cleanly. Within-element stability and fade
tracking are in direct opposition for windowed estimators, so there is no
dominating configuration. Two trade points kept in the registry.

Remaining idea in this line (§13.7): freeze the instant-attack peak update while
the key state is unstable, keeping zero-lag fade tracking while removing
edge-chasing.

### After that — the likelihood-ratio detector

§12.4 retargeted this. False events are cheap (cutting them 17× moved CER by
nothing); **misses are what cost** — 1.384 per element at `noise4.0` with
`del = 0.840`, a detector going silent rather than hallucinating. That is the
genuine likelihood-ratio case: key-up `|x| ~ Rayleigh(σ)`, key-down
`|x| ~ Rice(A, σ)`, a per-sample log-likelihood ratio instead of a boolean.

Open questions:

1. Does `IDetector` need to emit soft events, or does hysteresis on the LLR keep
   the existing `KeyEvent` interface? The latter is far smaller — measure first.
2. Where do `A` and `σ` come from — reuse `noiseFloor`/`signalPeak`, or estimate
   jointly (EM, as SparkGap does)?
3. Does it subsume the impulse blanker and the minimum-element filter, both
   patches on hard-decision output?

**Prerequisite, not a follow-up.** The generator adds complex Gaussian noise, so
its envelope is *exactly Rician* — the model an LLR detector assumes. On this
suite alone such a detector is tested against its own premise and is guaranteed
to look good. One consequence is already visible: the claim that an LLR
"subsumes the impulse blanker" holds under Gaussian noise and fails under real
impulsive HF noise, where a Rayleigh tail *over*-reacts to spikes. Real
recordings and model-mismatch profiles (§15) come first.

Measurement plan: new `IDetector` → registry entries `legacy+llr` and
`+llr+log`. `legacy` untouched, gates green. Success criterion:
`noise3.0`/`noise4.0` no worse than `legacy` while `+log`'s jitter advantage
survives.

### Detector defects, with verdicts

| defect | status |
|---|---|
| Hard binary decision destroys soft information before timing | open — the LLR case |
| Thresholds are fixed fractions of `signalPeak − noiseFloor`; the optimal point in Rayleigh/Rice noise is not | open — the LLR case |
| On/off ratio asymmetry stretches ON | measured: real but only 11% of the bias (§12.5) |
| `signalPeak` instant attack | `+peak` removes **57% of the bias** (§12.6), but instant attack is not the cause — `+peakgate` refuted that (§13.8). Separately **load-bearing**: it holds `peakRef` above the 1.8 guard (§13.9). |
| Matched-filter resize dropout | measured; fixing it does not reach CER (§12.3) |
| Mills 1977: amplitude likelihoods "worthless and even a source of error" under multipath | 📎 second-hand, and **untestable here** — the generator has no multipath (§15) |

⊘ §12.3 is the standing warning for all of the above: a detector metric improving
does not imply CER improving. Judge on CER across every profile, never on the
number the change was designed to move.

## 15. Open items

- Whether `worstCase` 0.581 → 0.364 under a narrower BPF survives 24-seed
  measurement (that figure is 16-seed).
- `ag1le/morse-wip` builds and runs — worth benchmarking against this decoder on
  the same profiles before committing to Stage 5.
- **Generator fidelity is unvalidated, and every number in this document is
  conditional on it.** Known gaps, in rough order of consequence:

  | generator | reality |
  |---|---|
  | **no multipath at all** | the exact condition Mills' amplitude finding is about |
  | AWGN | HF noise is impulsive, heavy-tailed, non-stationary |
  | QSB = single sinusoid, one rate | Watterson: Doppler spread, frequency-selective, phase disturbance |
  | exponential keying envelope, no chirp | real keying has click and ~50 Hz chirp — Bell widened his filter to 100 Hz specifically for it |
  | jitter iid Gaussian per element | hand-keying is correlated: systematic weighting, speed drift, operator-characteristic errors |
  | tone fixed at 700 Hz forever | real signals drift; this front end has no AFC |
  | QRM = one pure carrier | real QRM is other Morse, splatter, digital modes |

  Validation route: run real recordings and synthetic profiles through the same
  core and compare error *signatures* (ins/del/sub split, edge jitter
  distribution). If they disagree, the profiles need fixing before more
  conclusions are drawn from them — including the ones already in this document.

<a id="s16"></a>
## 16. Gap classification and the Farnsworth cold start (Phase 21, 2026-07-20)

### 16.1 What was actually wrong

The plan (#29) framed this as *implement* 2-means clustering on OFF durations to
replace hardcoded 1:3:7 ratios. **That framing was stale — the clustering already
existed**, in `AdaptiveTiming::estimateGapCenters`, and `classifyOff` already used
its output. The open question was why an adaptive estimator still left
`farnsworth-2.0` at CER 0.0141.

A probe (`[farnsworth-probe]`) answered it by feeding the generator's *true* gap
durations straight into `AdaptiveTiming`, bypassing the detector entirely.
`profileFarnsworth` is noiseless, so every misclassification it reports belongs
to the gap classifier and nothing upstream.

| ratio | gaps | errors | confusion | centres est/true | source | firstErr |
|---|---|---|---|---|---|---|
| 1.0 | 170 | 0 | — | 240/240, 560/560 | cold=10 adapted=160 | — |
| 1.5 | 170 | 0 | — | 360/360, 840/840 | cold=10 adapted=160 | — |
| 2.0 | 170 | **1** | C→W ×1 | 480/480, 1120/1120 | cold=10 adapted=160 | **3** |
| 3.0 | 170 | **1** | C→W ×1 | 720/720, 1680/1680 | cold=10 adapted=160 | **3** |

Three findings, two of which killed the leading hypotheses:

1. **The steady-state estimator is exact.** Converged centres equal the
   generator's `ElementModel` to the digit at every ratio. The sanity clamps at
   the end of `estimateGapCenters` **never fire** (`adapted=160`, zero `clamped`,
   zero `nosplit`). ✗ REFUTED: that the largest-gap split degenerates.
2. **`boundary = dit * 2.0f` is fine.** Farnsworth stretches char and word gaps
   only — element gaps stay at `1*dit` (generator, `TRUTH_ELEMENT_GAP`), so a
   2*dit element/long boundary separates them at every ratio. ✗ REFUTED: that
   the boundary is not Farnsworth-aware.
3. **The entire deficit is one gap.** Exactly one error, at gap index 3 — the
   first char gap in `CQ` — inside the 10-gap cold window, classified as WORD.
   One inserted space in a 71-character message is 1/71 = **0.0141**, the
   documented figure to four decimals.

**Mechanism.** `estimateGapCenters` returns early while fewer than 10 gaps have
been seen, so the cold window classifies against `dit*3` and `dit*7`. Those
defaults are not a neutral prior — **they are a hardcoded Farnsworth-ratio-1.0
assumption**. At ratio 2.0 a true char gap is 6*dit, which sits nearer the
default *word* centre (7*dit) than the default *char* centre (3*dit):

| | centre | sigma | z | likelihood |
|---|---|---|---|---|
| char | 240 ms | 40 ms | 6.0 | ∝ e^-18 ≈ 1.5e-8 |
| word | 560 ms | 80 ms | −1.0 | ∝ 0.61 |

The 4× char prior (`pElem*=5, pChar*=2, pWord*=0.5`) is fighting a
seven-order-of-magnitude likelihood mismatch and loses. **The prior cannot fix
this; only the centres can.**

> The adaptive estimator was built precisely to remove the ratio-1.0 assumption,
> but it engages only after 10 gaps — so the assumption survives intact in the
> one window where the decoder has no data to contradict it. An adaptive
> system's behaviour is often set by whatever it does *before* adaptation starts.

### 16.2 The fix, and what it cost

`classifyOff` pushes the current gap into `gapDurations` *before* estimating
centres, so even the first long gap can be measured against itself. Char gaps
outnumber word gaps ~2.5:1 and are the shorter class, so the smallest long gap
seen is the best single-sample estimate of the char centre.

Three variants were measured, each a paired 24-seed run on identical seeds
against a baseline arm with the bootstrap disabled. The baseline arm reproduced
every documented figure exactly (`farnsworth-2.0` 0.0141, `worstcase` 0.6244,
`snr-noise4.0` 0.9032), so the comparison is sound.

| variant | farnsworth-2.0 | better | worse | unchanged |
|---|---|---|---|---|
| ungated bootstrap | 0.0000 | 5 | **5** | 9 |
| gated `minLong >= 4.5*dit` | 0.0000 | 3 | **4** | 12 |
| **gated `minLong >= 5.5*dit`** (promoted) | **0.0000** | 1 | **3** | **15** |
| `+ isLocked()` gate | 0.0141 ✗ | 0 | 3 | 15 |

**Why the ungated version regressed.** Under noise the detector emits spurious
gaps, so the smallest long gap early in a message can be a noise artifact rather
than a real char gap — and bootstrapping from it corrupts the centres for the
whole cold window. The gate fixes this by overriding the default *only when the
observation contradicts it*: below the ~5*dit midpoint of the default 3:7
centres the default already classifies correctly, so overriding can only add
error. 5.5 rather than 5.0 keeps standard-timing signals untouched.

**✗ REFUTED — gating on `isLocked()`.** Predicted to remove the residual noise
perturbation, since the threshold is scaled by an unreliable `dit`. It is
**strictly worse than either alternative**: `farnsworth-2.0` returns to 0.0141
*and* all three regressions remain. The first char gap arrives after four
elements, before the Kalman filter locks — **the cold-start error happens
precisely because we are in the unlocked regime**, so requiring lock disables the
fix exactly where it is needed, while lock arrives partway through the cold
window so the bootstrap still fires on later gaps and still perturbs the noisy
profiles. Prediction 20, wrong 11.

### 16.3 Promoted with a documented regression

The promoted variant is **not** a clean win and does not satisfy the standing
"worse on any profile is a regression, not a tradeoff" rule:

| profile | baseline | promoted | Δ | profile stderr |
|---|---|---|---|---|
| **farnsworth-2.0** | 0.0141 | **0.0000** | **−0.0141** | 0.0000 |
| handkeyed-25wpm | 0.1309 | 0.1315 | +0.0006 | 0.0338 |
| snr-noise2.0 | 0.0827 | 0.0833 | +0.0006 | 0.0166 |
| snr-noise4.0 | 0.9032 | 0.9055 | +0.0023 | 0.0096 |
| *15 others* | — | — | unchanged | — |

In edits rather than ratios, over the 24-seed run of a 71-character message: the
win removes **~24 edits** (one spurious space per seed, every seed, worst case
included); the losses total **~4 edits**, concentrated on `snr-noise4.0`, a
profile at CER 0.90 where the decoder already emits garbage.

The asymmetry that justified promotion is not the size but the **kind**: the win
has stderr 0.0000 and is structural (one misclassified gap, deterministically
removed), while all three losses sit *below the seed-to-seed spread of their own
profiles* and could flip sign on a different seed set.

> ⚠ **This was a judgement call that relaxed a standing rule, not a measurement
> that satisfied it.** It is revertible in one line: `setGapBootstrap(false)`
> restores baseline behaviour exactly, and the losing arm stays reproducible.

> 📎 **Threshold hunting was stopped deliberately.** The residual deltas (0.0006)
> are an order of magnitude below the stderr of the profiles carrying them
> (0.0166–0.0338). Further tuning of the 5.5 constant against this table would be
> fitting 24 seeds of noise, not improving the decoder — the same failure the
> Phase 19 guard sweep was designed to catch.

### 16.4 Open

- ⊘ **EVIDENCE NEEDED: why `retroDecode` does not already cover this.** The
  decoder replays key events saved during timing convergence. A character
  emitted at gap index 3 and re-decoded once centres adapt would fix the
  Farnsworth error with *no* cold-start change and therefore no perturbation of
  the noisy profiles — strictly better than the promoted fix. Whether the replay
  path re-runs gap classification, or only element classification, is unread.
  **This is the principled fix if it works.**
- The probe samples `gapCenters()` *before* `classifyOff`, so its `source`
  histogram lags the actual classification by one gap. Harmless for the error
  counts, but the histogram must not be read as the state a given gap was
  classified under.
