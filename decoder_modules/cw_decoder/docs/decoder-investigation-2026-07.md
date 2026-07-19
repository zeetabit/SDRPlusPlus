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

The scorecard in §10 is the reason this matters: of eight predictions made from
code reading alone, four were wrong. **An unmarked plausible claim in this
codebase has roughly even odds.**

---

## 1. Summary

Three findings, in order of expected impact:

1. **The receive filter is 3–5× wider than optimal.** Measured **+2.8 to +4.8 dB**
   of weak-signal threshold gain available from narrowing the pre-detection BPF
   alone. This is larger than any decoding-statistics change on the table, and
   the improvement plan already identified it (quoting AG1LE) without ever
   measuring it. **Confirmed by experiment** — see §4.
2. **The pipeline makes hard decisions at every stage.** Schmitt trigger →
   duration → dit/dah → gap → beam search. Only the last stage is soft, and it
   sits downstream of three irreversible decisions. This is the structural
   reason the worst-case profile is stuck at CER ≈ 0.57 despite ten phases of
   statistical refinement.
3. **The benchmark suite measures a single coin flip.** Every noise/jitter/QSB
   result in `decoding-improvement-plan.md` is one realization on `seed = 42`.
   Some of the five "tried and reverted" verdicts may themselves be noise.

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
    │      ITiming             ── AdaptiveTimingStage (4 strategies)
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

`core_registry.h` is the single list; the config UI and the benchmark matrix both
enumerate it. Adding a core or a stage combination is one entry.

| name | configuration | status |
|---|---|---|
| `legacy` | Schmitt + Kalman V1 + beam search | **production default** |
| `legacy+kmeans` | K-means timing | worse on jitter, QRM, QRN |
| `legacy+median` | median-split timing | worst overall |
| `legacy+bimodal` | bimodal-histogram timing | best on handkeyed-25 |
| `legacy+kalman2` | corrected dah gain + gated learning (§8) | better on 10/13 |
| `legacy+log` | log-duration Kalman (§9) | best on 5, tied on 5 |
| `legacy+logrobust` | log + Huberised update (§9) | refuted |
| `legacy+bpf40` | 40/50 BPF, 64 Hz ENBW | breaks clean-25 WPM |
| `legacy+bpf30` | 30/40 BPF, 48 Hz ENBW | best on noise3.0 |
| `legacy+bpf20` | 20/30 BPF, 31 Hz ENBW | |

The three original timing variants already existed in `timing.h` and had **never
been benchmarked head-to-head across seeds**.

**Promotion rule.** A variant becomes the default only when it is no worse on
*every* profile. "Better on average" is not sufficient: a profile that decoded
better before and worse after is a regression, not a trade. Losing variants are
kept, not deleted — they are the comparison baseline for future cores.

### Running the benchmarks

```bash
cd decoder_modules/cw_decoder/tests/build && cmake .. && make -j8

./cw_decoder_tests                       # full suite incl. 24-seed gates (~17s)
./cw_decoder_tests "[characterize]" -s    # per-profile CER distribution
./cw_decoder_tests "[matrix-timing]" -s   # timing strategies head-to-head
./cw_decoder_tests "[matrix-bpf]"    -s   # front-end bandwidths
./cw_decoder_tests "[matrix-all]"    -s   # every registry core (~2 min)
```

Matrix tests are tagged `[.]` (Catch2 hidden) so the always-on suite keeps the
regression gates while the exploratory sweep stays opt-in. Run each suite **once**
and post-process the saved output — do not re-run to extract different fields.

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

The general shape: a local "fix" removes a compensation without addressing the
misspecification it was compensating for, and CER gets worse. Before correcting
a formula, check what currently absorbs its error.

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
2. **Hand-keyed is a timing problem, not a detector problem.** `+tim` alone
   takes handkeyed-15 from 0.1579 to 0.0153 and `+det` adds nothing on top.
   This *raises* the value of element-model work (`+kalman2`, `+log`), but aimed
   at weighting and gap boundaries rather than noise robustness.
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
* **Everything here is measured on the synthetic generator.** §13 lists the
  fidelity gaps. An oracle result on `worstcase` says nothing about real
  multipath, because the generator has none.

### Running it

```bash
./cw_decoder_tests "[oracle]" -s        # the ablation sweep (~16 s)
./cw_decoder_tests "[oracle-self]"      # instrument self-checks (always on)
```

The self-checks are deliberately **not** hidden: they assert the oracle detector
reproduces the generator's transitions exactly, that the recorded element model
matches what the generator keys under weight bias and Farnsworth, and that a
clean signal with both oracles decodes at CER 0. If any of those break, every
headroom number above is measuring the harness.

## 12. Next: the detector (planning)

**Why here.** `+kalman2` and `+log` are both blocked on `noise3.0`/`noise4.0`,
and §11 measured that the entire error on those profiles sits in the detector —
a perfect duration model moves noise3.0 only 0.8163 → 0.6614, while a perfect
detector takes it to zero. One piece of work unblocks both candidates.

**Scope correction from §11.** This phase addresses the AWGN/QSB/QRN family
only. Hand-keyed profiles are a *timing* fault (`+tim` alone captures all of the
available gain), Farnsworth is a *gap-classification* fault, and `worstcase`
needs the detector and the duration model fixed together. Phase 13 is not a
general fix.

**What is wrong with `tone_detector.h` today** (§2.3):

* A fixed-fraction Schmitt threshold on a magnitude envelope, emitting a **hard
  binary decision**. All soft information is destroyed before timing sees it.
* Thresholds are fractions of `signalPeak − noiseFloor`, but the optimal decision
  point in Rayleigh/Rice noise is not a fixed fraction — hence the ad-hoc
  SNR-dependent table of 0.55/0.35, 0.60/0.30, 0.65/0.25.
* On/off asymmetry (0.55 rising vs 0.35 falling) systematically stretches ON
  durations and shortens the following gap. A bias, not noise — averaging does
  not remove it, and it shifts both the dit/dah boundary and the gap centres.
* `signalPeak` uses instant attack, so a single impulse during key-down raises
  the thresholds for ~0.5 s (decay tau).
* Mills 1977 (§5.3a): amplitude-based likelihoods are "worthless and even a
  source of error" under multipath — and fading profiles are exactly where this
  decoder fails.

**Candidate direction.** Replace the threshold with a likelihood ratio:
key-up `|x| ~ Rayleigh(σ)`, key-down `|x| ~ Rice(A, σ)`, giving a per-sample
log-likelihood ratio instead of a boolean. That is the statistically correct test
and it preserves soft information. Open questions to settle during planning:

1. Does `IDetector` need to emit soft events, or does a hysteresis on the LLR
   keep the existing `KeyEvent` interface? (The latter is a much smaller change
   and should be measured first.)
2. Where do `A` and `σ` come from — reuse `noiseFloor`/`signalPeak`, or estimate
   them jointly (EM, as SparkGap does)?
3. Does this subsume the impulse blanker and the minimum-element filter, both of
   which are patches on the hard-decision output?

**Measurement plan.** New `IDetector` implementation → new registry entries
(`legacy+llr`, and `+llr+log` to test the combination). Existing gates must stay
green with `legacy` untouched. Success criterion: `noise3.0`/`noise4.0` no worse
than `legacy` while `+log`'s jitter advantage survives.

**Before committing to the LLR rewrite — a circularity to avoid.** The generator
adds complex Gaussian noise to a tone, so the envelope is *exactly Rician* — the
model an LLR detector assumes. Benchmarked on this suite alone, such a detector
is being tested against its own generative assumptions and is guaranteed to look
good. One consequence is already visible: the claim in §11's planning notes that
an LLR "subsumes the impulse blanker" holds under Gaussian noise and fails under
real impulsive HF noise, where a Rayleigh-tailed likelihood *over*-reacts to
spikes. Real recordings and model-mismatch profiles (§13) are a prerequisite,
not a follow-up.

**Immediate next step: detector ground-truth metrics.** Attribution is settled;
the open question is *which kind* of detector error dominates. Using the same
ground truth as §11, per profile: false key-down rate, missed elements, edge
timing bias, edge jitter σ. Bias must be reported **differentially**
(ON stretch = onDelay − offDelay) — the front end's constant group delay cancels
in the decode path but not in edge scoring, and the differential is the quantity
§2.3(h) predicts.

## 13. Open items

- Whether fixing the Kalman defects changes the QSB verdict.
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
