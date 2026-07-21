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

**Status (2026-07-20): the detector and timing lines are closed; real-recording
ground truth now exists (§18) and has already found two defects the synthetic
suite could not express.**

Detector line, Phases 15–19 — closed with no promotion:

1. **Peak tracking is closed pending a mechanism.** Five estimators tried
   (percentile, slow-attack, dual-window ×2, gated); none promoted. §12.6's
   measurement stands — `+peak` cuts the ON stretch 57% — but its mechanism was
   refuted by Phase 17 (§13.8), so there is no model left to design against.
   ⊘ EVIDENCE NEEDED: why the percentile reference moves the stretch.
2. **The dynamic-range guard is load-bearing, not a defect** (§13.9). Blindness
   tracks miss rates profile by profile; legacy's key-up instant attack holds
   `peakRef` above the guard.
3. **The guard is also honest** (§13.10). Disabling it (g=1.0) removes 55.4 pp of
   blindness on snr-noise4.0 and CER gets *worse* (0.9032 → 0.9173). 1.8 is
   Pareto-optimal. **No open defect remains in the detector.**

**Phase 21 closed Farnsworth (§16), but not by fixing it.** The cause is now
known exactly — one gap, the first char gap, classified in the cold window
against `dit*3`/`dit*7` defaults that *are* a hardcoded ratio-1.0 assumption.
The clustering that #29 proposed to add already existed and is exact. The fix
was **not shipped**: it fires by 0.5 ms of margin and breaks the `worstcase`
gate in combination with the retro fix. `farnsworth-2.0` stays at 0.0141.

**Shipped instead: retro gap seeding** — `retroDecode` restored the element model
and dropped the gap model, so every replay re-entered a cold gap window. 5
profiles better, 1 worse, 221/221 gates pass, `qrm` 0.0100 → **0.0035**. The
largest single-profile improvement in the last seven phases, and it came from a
bug found while investigating an unrelated one.

**Phase 22 closed the gap-under-noise ⊘ item (§17)** — measurement only, nothing
promoted. Error multiplication is confirmed: 12.2% of *structurally intact* gaps
are misclassified at noise 3.0 against 0.0% clean, and the mechanism is **dit
inflation (+38.9%)** rather than the gap fragmentation §16.4 assumed. It also
reframed #29: under realistic noise the cold-start bug is only 8 of 35 Farnsworth
errors, so a cold-start fix addresses under a quarter of the problem.

**Phase 23 built the real-recording gate (§18)** — #30 is closed as a blocker.
ARRL W1AW publishes paired audio and exact text; five sessions are pinned and
gated, two of them at CER 0.0000. It found and fixed two defects (a broken
audio→IQ conversion, unmapped period and comma) and produced the first evidence
bearing on §15.

**Phase 24 swept all 21 registry cores against the real recordings (§19).**
`legacy+mf` does fix the 35 WPM errors (0.0089 → 0.0000), so §12.3's refutation
was scope-limited — but `legacy+kmeans` and `legacy+median`, which are
timing-only, fix them too, so those errors are **marginal rather than the
signature of the matched-filter mechanism**. Nothing is promotable: every
candidate regresses on at least one synthetic profile, and the best core on real
audio (`legacy+edge+log`, mean 0.0013) more than doubles CER on heavy noise.

**Phase 25 repaired the comparison method itself (§20), and it was the binding
constraint — not measurement coverage.** Every better/worse tally in Phases
12–24 was read off mean CER across 24 seeds with a 1e-6 float epsilon. Measured:
n=24 gives the wrong verdict in both directions (a noise3.0 regression read
t=1.91 at 24 seeds and t=5.43 at 96; two hand-keyed improvements read t=−0.47
and −1.09 at 24 and −3.29 and −3.63 at 96), and the epsilon counts one character
across the whole seed set as a regression. The fix — paired per-seed testing,
n=96 for decisions, a non-inferiority bound, three new fast-hand-keyed profiles,
and an orthogonal speed×noise factorial — is now in the harness. Adding noise to
the clean ARRL audio (§20.5) showed the §19 tradeoff reproduces on real keying,
so noisy off-air recordings were demoted from first: they validate, they do not
resolve. No decoder changed; nothing became promotable.

**Candidates now, in dependency order:**

1. ✓ **The runaway-insertion mode — SOLVED, and the fix promoted (§20.7–20.8,
   §21).** `legacy+edge+log` reached CER 1.79 at 15 WPM / noise 3.0 (t=18) from
   `log` under-estimating dit by 44%. The tail-robust timing variant failed
   (§20.8): spike and interference are the same innovation magnitude, so no
   magnitude gate in the timing layer separates them. The fix was the
   non-magnitude discriminator §20.8 named — a sequential likelihood-ratio
   detector (`legacy+lr+log`, §21) that rejects spikes by evidence *duration*.
   It turns the +0.97 (t=18) noise3.0 catastrophe into −0.11 (a win), with 9
   significant improvements and zero significant regressions, and is now the
   production **DEFAULT_CORE** — the campaign's first promotion.
2. ✓ **Why is dit +38.9% at noise 3.0? — CLOSED (§23).** It was a Schmitt+Kalman
   property. The promoted LR+log default holds dit within 5% of truth there
   (vs +37% for legacy), because the LR detector removes the short spurious
   events that were the only bias log suffered. Gap classification improved with
   it (12.4% → 10.0%), which is why §21's noise3.0 win exists. A new, smaller
   residual remains (element-gap splitting at −18% dit), recorded not chased.
3. ✓ **The min-element filter's bias on hand-keyed — CLOSED (§24).** Confirmed
   under the new default (relaxing the filter helps handkeyed-40, t=−2.17), but
   the filter is not removable: it still catches short evidence-passing spikes,
   so disabling it regresses worstcase and noise3.0. A bounded, sub-character
   tradeoff — the same duration-ambiguity wall as §20.8, now at the filter.
   Confirmed-but-not-actionable.
4. **#29 in log-duration space** — §17.3.7 means it must be scored against
   `farns2.0-n1.5` rather than the clean profile. ⊘ If the §18.6.1 inference
   holds, the ARRL 5 and 10 WPM sessions are also real Farnsworth material with
   published text — but that inference is unverified.
5. **Noisy off-air recordings — demoted.** W1AW transmits these same texts on HF
   (3.5815 / 7.0475 / 14.0475 / 21.0675 / 28.0675 MHz), so an off-air capture
   would carry real propagation *and* published ground truth. 📎 `kiwirecorder.py`
   (github.com/jks-prv/kiwiclient) records audio and IQ WAV from public
   KiwiSDRs; not attempted. This would *validate* §15, not resolve any tradeoff
   (§20.5) — the cheaper intermediate it once proposed, adding synthetic noise
   to the clean ARRL audio, is now done and showed the tradeoff is real.

**A note on where this leaves the work.** Phases 15–19 closed the detector line
without promoting anything: every candidate defect in it is either measured and
load-bearing, or measured and absent. Phase 21 then closed the timing line's one
isolated defect as *understood but not fixable* at this formulation — at ratio
2.0 a stretched char gap (6·dit) and a standard word gap (7·dit) are
near-indistinguishable from a single observation, which is an information limit,
not a tuning problem. Both remaining routes are structural rather than
incremental:

1. **Log-duration coordinates** (§5.3b). Centres become offsets `0 : log3 : log7`
   and Farnsworth reduces to a single additive shift on the gap cluster — one
   parameter to estimate instead of three centres. This is where #29 reopens.
2. **Real recordings** (#30), to find out whether any of this generalises.

✓ **Gap classification under noise is now measured (§17).** The compounding this
paragraph previously flagged as unmeasured is confirmed — but the route is not
the one predicted. Fragmentation and misclassification-of-intact-gaps are two
separate effects of similar size (11.0% and 12.2% at noise 3.0), and the driver
of the second is a **39% dit overestimate**, i.e. a duration-model failure
surfacing as a gap failure.

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
> (These counts are this section's record, not the current suite size: Phases 22
> and 23 took it to **1622 assertions in 223 test cases**.)
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

**Outcome: one fix shipped, and it is not the one this phase set out to make.**
Retro gap seeding is promoted (5 profiles better, 1 worse, all 221 gates pass).
The Farnsworth cold-start fix is **not shipped** — it is fragile by 0.5 ms and
breaks a gate when combined with the retro fix. `farnsworth-2.0` stays at 0.0141.

### 16.1 The defect is fully understood

The plan (#29) framed this as *implement* 2-means clustering on OFF durations.
**That framing was stale — the clustering already existed** and works. A probe
(`[farnsworth-probe]`) feeding the generator's *true* gap durations straight into
`AdaptiveTiming`, bypassing the detector, found:

| ratio | gaps | errors | centres est/true | firstErr |
|---|---|---|---|---|
| 1.0 | 170 | 0 | 240/240, 560/560 | — |
| 1.5 | 170 | 0 | 360/360, 840/840 | — |
| 2.0 | 170 | **1** (C→W) | 480/480, 1120/1120 | **3** |
| 3.0 | 170 | **1** (C→W) | 720/720, 1680/1680 | **3** |

1. **The steady-state estimator is exact** — converged centres equal the
   generator's `ElementModel` to the digit; the sanity clamps never fire.
   ✗ REFUTED: that the largest-gap split degenerates.
   ⚠ **"The clamps never fire" holds only on noise-free signals, and this
   sentence originally omitted that condition.** §17.2 measures 199 clamp fires
   at noise 3.0 and 42 on worstcase. Both statements are about the same code;
   only the regime differs.
2. **`boundary = dit * 2.0f` is fine** — Farnsworth stretches char/word gaps
   only, element gaps stay at `1*dit`. ✗ REFUTED: that the boundary is not
   Farnsworth-aware.
3. **The entire deficit is one gap** — the first char gap, inside the 10-gap cold
   window, read as WORD. One inserted space in 71 characters is **0.0141**.
   Confirmed at the character level: the decoder emits `'C Q CQ CQ DE ...'`.

**Mechanism.** The cold window classifies against `dit*3` / `dit*7`, which are
not a neutral prior — **they are a hardcoded Farnsworth-ratio-1.0 assumption**.
At ratio 2.0 a true char gap (6*dit) sits nearer the default *word* centre:

| | centre | sigma | z | likelihood |
|---|---|---|---|---|
| char | 240 ms | 40 ms | 6.0 | ∝ e^-18 ≈ 1.5e-8 |
| word | 560 ms | 80 ms | −1.0 | ∝ 0.61 |

The 4× char prior loses against a seven-order-of-magnitude mismatch. **The prior
cannot fix this; only the centres can.**

> The adaptive estimator exists to remove the ratio-1.0 assumption, but engages
> only after 10 gaps — so the assumption survives in the one window where the
> decoder has no data to contradict it.

### 16.2 Why the fix was not shipped

Bootstrapping the char centre from the smallest long gap, gated to fire only
above `5.5*dit`, does take `farnsworth-2.0` to 0.0000 on all 24 seeds. It was
measured against a baseline arm that reproduced every documented figure exactly.

**It is fragile.** The debug trace shows the decisive gap at **468.0 ms** with
`dit = 85.0`, so the gate `5.5*dit` = **467.5 ms** fires by **0.5 ms**. A slightly
different `dit` estimate and the fix silently stops working. That is not a
threshold, it is a coincidence.

**And it is an information limit, not a tuning problem.** At ratio 2.0 a stretched
char gap is 6*dit while a *standard* word gap is 7*dit. From a single observation
these hypotheses are nearly indistinguishable, so any single-sample rule is
guessing. The working range for the gate — between the default decision boundary
(5*dit) and the target (5.5*dit in estimated units) — is inherently narrow.

**It breaks a gate in combination.** Configurations do not compose:

| config | farnsworth-2.0 | multiseed | 221 gates |
|---|---|---|---|
| A: live bootstrap only | 0.0000 | 1 better / 3 worse | pass |
| B: retro seeding + retro mode | 0.0141 | **5 better / 1 worse** | pass |
| A+B | 0.0000 | 5 better / 2 worse | ✗ **worstcase 0.6087 > 0.6** |

A+B has the best 24-seed table *and* fixes Farnsworth, yet is the only one that
fails. `worstcase`'s multiseed **mean improved** (0.6244 → 0.6232) while the
single seed behind the gate crossed it — averaging hides the tail. This is why
the single-seed gates exist alongside the multiseed table.

> ⚠ **A judgement call was overturned by evidence.** The cold-start fix was
> initially promoted on the argument that its regressions were
> "perturbation-level" and below profile stderr. A gate then refused it. The
> no-regression rule was load-bearing; relaxing it was wrong.

**✗ REFUTED — `retroDecode` as the clean fix.** Predicted to fix Farnsworth with
no cold-start change and therefore no perturbation. The trace disproves it:
`RETRO events=8` — retro sees only the four elements of the leading `C` — and the
offending gap is logged in the **post-lock** format. Timing locks at
`elementCount >= seedCount + 4` (~7–8 elements), so the gap that costs 0.0141
happens *after* retro has finished and can never be revisited. Prediction 21,
wrong 12.

**✗ REFUTED — gating on `isLocked()`.** Strictly worse than either alternative:
Farnsworth returns to 0.0141 *and* the regressions remain. The first char gap
arrives before the filter locks, so requiring lock disables the fix exactly where
it is needed. Prediction 20, wrong 11.

The structural fix — delaying lock so the gap falls inside retro, where real
clustered data replaces the guess — is **known harmful**: deferring lock also
defers retroDecode and breaks clean decoding (WPM sweep 0.01 → 0.364, see the
KalmanTiming seed row in `architecture.md`). That path is closed.

### 16.3 What shipped: retro gap seeding

`retroDecode` builds a fresh timing stage via `makeFresh()` and restores only the
**element** model (8 dit/dah pairs from `lockedDit`). The gap model is equally
learned state and was dropped, so the replay re-entered a cold gap window every
time. Seeding it from the events already in hand, plus relaxing the sample floor
in retro only (where the complete pre-lock set is known and no more will arrive):

| profile | baseline | shipped | Δ |
|---|---|---|---|
| **qrm** | 0.0100 | **0.0035** | **−0.0065** |
| snr-noise3.0 | 0.8163 | 0.8140 | −0.0023 |
| worstcase | 0.6244 | 0.6232 | −0.0012 |
| handkeyed-25wpm | 0.1309 | 0.1303 | −0.0006 |
| qrn | 0.0029 | 0.0023 | −0.0006 |
| snr-noise4.0 | 0.9032 | 0.9043 | +0.0011 |
| *14 others* | — | — | unchanged |

**5 better, 1 worse, 221/221 gates pass.** This has nothing to do with Farnsworth
— it is a general replay defect that happened to be found while looking for one.

> Any "replay with better parameters" design must enumerate **all** the learned
> state, not just the parameter that motivated the replay.

### 16.4 The probe is structurally noise-blind

`[farnsworth-probe]` walks the generator's truth segments, so the durations it
feeds in are identical at every noise level — noise reaches gap classification
only via the detector, which the probe bypasses on purpose. **Adding a noisy
profile produces a byte-identical table; it was tried and removed** rather than
shipped as a green row proving nothing.

This matters because gaps degrade differently from tones under noise: a gap is
defined by the *absence* of signal, so noise fragments one true gap into several
short ones, and an adaptive estimator then **learns from the corrupted
durations** — bad centres cause more misclassification, which feeds more bad data
back. That is a real error-multiplication mechanism, and it is exactly what the
ungated bootstrap did to every noisy profile.

- ✓ **CLOSED by §17.** This item was measured on 2026-07-20 with a
  detector-derived, group-delay-aligned probe (`[gap-noise]`). The
  error-multiplication mechanism described above is **confirmed**, and it is
  larger than assumed: 12.2% of *structurally intact* gaps are misclassified at
  noise 3.0, against 0.0% clean. See §17.
- 📎 Mills (§5.3b) points at the deeper formulation: in log-duration space the
  centres become offsets `0 : log3 : log7` and **Farnsworth is a pure additive
  shift on the gap cluster** — a single parameter to estimate rather than three
  centres, and the natural place to revisit #29.

### 16.5 Incidental findings

- **`Channel::debugLog` is a dead flag.** Declared on both `Channel` and
  `StagedCore`, never wired between them — `IDecodeCore` has no route to pass it
  through, so the entire debug trace is unreachable from the public API. It had
  to be enabled by editing the default to obtain the trace above. Not fixed.
- The probe samples `gapCenters()` *before* `classifyOff`, so its `source`
  histogram lags the actual classification by one gap.

<a id="s17"></a>
## 17. Gap classification under noise (Phase 22, 2026-07-20)

**Outcome: the §16.4 ⊘ item is closed. Error multiplication is real, and the
dominant mechanism is dit inflation, not gap fragmentation.** Nothing is
promoted — this phase is measurement only.

### 17.1 The instrument

`[gap-noise]` (`tests/test_gap_noise.cpp`) runs the **real chain** — front end,
matched filter, Schmitt detector — via `RecordingDetector` inside a `Channel`,
exactly as `measureGuard` does (§13.9). Detector events are shifted by the
measured group delay and matched against the generator's truth segments, then
the detector-derived durations are fed to `AdaptiveTiming`.

The critical design decision is that a noisy gap can be wrong in **two unrelated
ways**, and conflating them makes the result uninterpretable:

| failure | definition | whose fault |
|---|---|---|
| **structural damage** | the detector dropped or invented an edge, so the gap matches no single truth gap | the detector; the classifier never had a chance |
| **classification** | the gap matches exactly one truth gap and was still named wrong | the classifier |

A gap is *matched* only if it overlaps exactly one truth gap **and** its midpoint
lies inside that gap. The first condition rejects merges (a dropped element makes
one detector gap span gap-tone-gap); the second rejects gaps invented inside a
tone by a mid-element dropout. **Only matched gaps enter the confusion matrix.**
That split is what makes "does the estimator compound its own errors?"
falsifiable rather than narrative.

**Instrument self-check.** The probe asserts `damage(noise-3.0) > damage(clean)`
and `damage(clean) < 5%`. This is not decoration: the predecessor probe failed
precisely by being noise-blind (§16.4), so this one must *prove* noise reaches
it. It does — 0.0% → 11.0%.

### 17.2 Results (8 seeds, MSG_FULL, aggregated)

`clsErr%` is over matched gaps only. `filt` repeats the run with short elements
rejected as the decoder does (`staged_core.h:265`).

| profile | gaps | damaged | clsErr% raw | ditErr% raw | clamp raw | clsErr% filt | ditErr% filt | clamp filt |
|---|---|---|---|---|---|---|---|---|
| clean | 1360 | 0.0% | **0.0%** | +7.9 | 0 | 0.0% | +7.9 | 0 |
| mild-0.5 | 1362 | 0.1% | 0.0% | +3.7 | 0 | 0.0% | +3.7 | 0 |
| moderate-1.5 | 1365 | 0.3% | 0.1% | −4.3 | 0 | 0.1% | −4.3 | 0 |
| **noise-3.0** | 1323 | **11.0%** | **12.2%** | **+38.9** | **199** | 12.4% | +38.9 | 212 |
| qrn | 1363 | 0.2% | **0.0%** | +5.6 | 0 | 0.0% | +5.6 | 0 |
| handkeyed | 1372 | 0.9% | 1.2% | +5.9 | 20 | 1.5% | **+11.3** | **58** |
| worstcase | 1357 | 6.3% | 7.9% | +6.9 | 42 | 7.7% | +7.3 | 42 |
| farns2.0 | 1360 | 0.0% | 0.6% | +8.4 | 0 | 0.6% | +8.4 | 0 |
| farns2.0-n1.5 | 1379 | 0.4% | **2.5%** | +8.1 | 0 | 2.5% | +8.1 | 0 |

Confusion at noise-3.0 (raw): `E>C 65, C>E 34, W>C 37, C>W 3, E>W 0`.

### 17.3 Findings

1. **Error multiplication is confirmed, and it is not fragmentation.** 12.2% of
   gaps the detector delivered *intact* are misclassified at noise 3.0. The
   §16.4 hypothesis — noise fragments gaps, the estimator learns corrupted
   durations — is right in outcome but wrong in route: fragmentation shows up as
   the 11.0% *damaged* column, and classification error on intact gaps is a
   **separate, equally large** effect.

2. **The mechanism is dit inflation.** The learned dit is **+38.9%** too long at
   noise 3.0. Gap centres derive from `dit`, and `boundary = dit*2` decides which
   gaps feed the element cluster — inflate dit and true char gaps are pulled into
   the element cluster, contaminating `elemMean` upward. The confusion pattern
   confirms it: errors flow *toward* CHAR from both sides (`E>C 65`, `W>C 37`),
   i.e. the char centre has become a magnet. This is a duration-model failure
   expressing itself as a gap failure.

3. ⚠ **§16.1's "the sanity clamps never fire" is regime-specific and was stated
   unconditionally.** They fire **199 times** at noise-3.0 and 42 on worstcase,
   against zero in every noise-free case. On heavy noise the clamps are a primary
   code path, and no measurement in these docs describes their behaviour there.
   §16.1 is corrected in place.

4. **The decoder's own min-element filter biases the duration model on
   hand-keyed signals.** Enabling it moves handkeyed dit error +5.9% → **+11.3%**
   and triples clamp fires (20 → 58). Rejecting short elements raises the mean of
   what survives, so a filter whose purpose is rejecting noise spikes is also a
   systematic upward bias on a profile whose defining feature is short-element
   variance. ⊘ Not yet traced to CER. **New candidate defect.**

5. **QRN is harmless to gap classification** — 0.2% damage, **zero**
   classification errors, despite being a noise profile. Consistent with the
   impulse blanker doing its job, and it isolates QRN's residual CER (0.0023) as
   originating elsewhere.

6. **Farnsworth cross-validated through an independent path.** `farns2.0` clean
   shows exactly 8 `C>W` errors over 8 seeds — one per seed, matching §16.1's
   single cold-start gap, now confirmed via **detector-derived** durations rather
   than truth durations. The two probes agree.

7. **The Farnsworth defect is 4× larger under noise, and mostly a different
   defect.** `farns2.0-n1.5` rises to 2.5% (35 errors): the same 8 `C>W`
   cold-start errors plus **26 new `W>C`** — word gaps read as char, i.e. lost
   word boundaries. **This reframes #29:** fixing the cold start addresses 8 of
   35 errors under realistic noise. The log-duration reformulation must be
   evaluated against the noisy figure, not the clean one, or it will be scored
   against 23% of the problem.

### 17.4 Caveats on the instrument

- ⚠ **The probe uses a second `AdaptiveTiming` instance**, not the decoder's own,
  which is not readable per-gap without changing the pipeline. It sees the same
  detector event stream but **not** squelch gating or timing freeze, so it
  measures the classifier's response to detector output, not the decoder's
  end-to-end behaviour. The `filt` column bounds one of the two differences and
  finds it small (12.2% → 12.4%); freeze and squelch remain unbounded.
- 8 seeds, not the 24 used for CER baselines. Confusion counts aggregate across
  seeds, so per-seed variance is not visible.
- The damage/classification split depends on the matching rule in §17.1. A
  looser rule would move gaps from `damaged` into the confusion matrix and
  probably raise `clsErr%`; a stricter one would do the reverse.
- ⊘ **No causal link to CER is established.** That 12.2% of intact gaps are
  misread at noise 3.0 does not by itself say how much of that profile's 0.8140
  CER it explains. Establishing that needs an oracle ablation on the gap
  classifier specifically.

### 17.5 Incidental: #30 is not blocked on data, and its harness already exists

Found while wiring `[gap-noise]` into the build, not by looking for it.

- **`tests/test_recording.cpp` is not in `tests/CMakeLists.txt`.** It has never
  been compiled or run. It contains a complete real-recording harness: a mono
  16-bit WAV reader, audio→IQ conversion by beat-note mixing, and `Channel`
  wiring with a periodic WPM/SNR trace.
- **The recordings exist and have since April 2026.**
  `~/.config/sdrpp/recordings/` holds 5 audio WAVs — three on 40 m CW
  (7024.030 kHz ×2, 7028.020 kHz) — and 6 baseband IQ captures up to 956 MB.
  The path hardcoded in the test resolves to a real 2.9 MB file.
- **This contradicts the standing description of #30** as "a data-collection
  project rather than an experiment," blocked on recordings being supplied. It is
  neither blocked nor greenfield. What it needs is the file wired into the build,
  the hardcoded absolute path replaced, and the tone frequency derived rather
  than guessed (the test hardcodes 800 Hz with a comment saying "~789 Hz from
  analysis").
- Two defects inside it, both invisible while it is out of the build:
  it sets `ch.debugLog = true`, which §16.5 established is a **dead flag**; and a
  missing recording takes an early `return` before the sole `REQUIRE`, so the
  test would **pass silently** — the §13.11 non-failing-assertion pattern,
  missed because no audit that walks the build could see this file.

⚠ **Generator fidelity (§15) is therefore testable now.** Every number in this
document is conditional on the synthetic profiles resembling reality, and the
means to check that has been on disk, unbuilt, for three months.


<a id="s18"></a>
## 18. Real recordings: the ARRL W1AW gate (Phase 23, 2026-07-20)

**Outcome: the first ground truth in this project that the decoder's own
generator did not produce.** Two defects found and fixed, one measurement that
bears on §15, and a gate that decodes 6-7 minutes of real audio at CER 0.0000.

### 18.1 The source

ARRL publishes W1AW code-practice sessions as a **paired MP3 and exact text
file**, biweekly, at nine speeds. Verified by download: mono, **8000 Hz** —
the decoder's internal IQ rate, so the test resamples nothing. Tone measured by
Goertzel sweep at **750 Hz on all five** cached sessions.

📎 Two web searches found no public corpus of real off-air CW with
transcripts; the literature reports researchers generating their own. ✗ The
academic "Morse Code Datasets for Machine Learning" (arXiv 1807.04239) is
**synthetic one-dimensional sequences, not audio**, and cannot be used here.

`tests/fetch_recordings.sh` pins five sessions by date (5/10/15/20/35 WPM).
Neither audio nor text is committed. The gate is opt-in (`[.]`) and **fails at
a REQUIRE** when the cache is absent — see §18.5.

### 18.2 The audio→IQ conversion in the orphaned file was broken

§17.5 found `test_recording.cpp` was never in the build. Its conversion also
could not have worked. It computed `audio(t)·e^{jΩt}`; real audio has spectrum
at ±750 Hz, so the product sits at Ω±750, and the channel's translation by −Ω
returns it to ±750 — outside the ±100 Hz complex low-pass for **any** Ω.

Measured on the same file, same tone parameter:

| conversion | SNR | decoded |
|---|---|---|
| `audio(t)·e^{jΩt}` | 3.0 | `T ET  T E ET E EE E I T E I T EAI ITET...` |
| samples as `re`, `im = 0` | 71.6 | correct text |

The working conversion needs no Hilbert transform: translation by −750 places
the wanted component at DC and the image at −1500, which the complex low-pass
rejects.

### 18.3 Punctuation was unmapped — a defect class the suite cannot express

`morse_tree.h` mapped `/`, `=` and two prosigns but **no period (`.-.-.-`) and
no comma (`--..--`)**. Periods decoded as `*` (SK, the nearest mapped node);
commas vanished.

It survived 221 tests because **`MSG_CQ` and `MSG_FULL` are the only messages
the suite sends and neither contains punctuation.** Inventory of the 15/20/35
WPM reference texts: **23 periods, 21 commas**, 14 `=`, 3 angle-bracket
prosign markers.

Fix: `tree[84] = '.'`, `tree[114] = ','` — both inside the existing 127-node
depth-7 tree, no structural change. Measured before/after with one scorer held
fixed (whitespace-normalised Levenshtein, no reference cleaning):

| session | before | after |
|---|---|---|
| 15 WPM | 0.0201 | 0.0073 |
| 20 WPM | 0.0180 | 0.0045 |
| 35 WPM | 0.0287 | 0.0116 |

⚠ These are **not** the gate's numbers in §18.4, which use the repository's
`score()` plus reference cleaning and are therefore lower. Compare within a
column, never across.

**A test caught a wrong assumption of the author's.** A gate asserting that
comma's unmapped parent `--..-` returns `'\0'` failed, returning `'7'`.
`characterBreak()` scans every surviving beam path and returns the best
*mapped* one, and confidence is clamped to 0.95 so the alternate branch never
dies: **the decoder never returns `'\0'` while any live path is mapped.** The
assertion was wrong, not the tree. It was narrowed to the property actually
intended — comma must not capture the shallower pattern — rather than deleted.

### 18.4 The gate

Repository `score()`; the reference strips `\r`, `\x1a` and the bare `<`/`>`
prosign markers, whose ARRL meaning the files do not state — rather than guess
a mapping to `+`/`*`, they are dropped, so a decoder that emits a prosign there
scores an insertion.

| session | audio | chars | CER | WER | reported wpm | snr |
|---|---|---|---|---|---|---|
| 5 WPM | 954s | 556 | 0.0087 | 0.0632 | 12.0 | 19.2 |
| 10 WPM | 472s | 454 | 0.0044 | 0.0440 | 13.2 | 59.9 |
| 15 WPM | 375s | 544 | **0.0000** | 0.0000 | 14.1 | 71.6 |
| 20 WPM | 451s | 887 | **0.0000** | 0.0000 | 18.6 | 75.7 |
| 35 WPM | 438s | 1458 | 0.0089 | 0.0520 | 32.9 | 19.2 |

Thresholds are ratchets at these values; 15 and 20 WPM gate at exactly zero,
which is satisfiable because decoding a fixed WAV is deterministic. Total
runtime 0.83s for 45 minutes of audio.

### 18.5 The gate fails rather than skips

The orphaned file returned early on a missing recording, before its only
`REQUIRE` — it would have passed silently had it ever been built. The
replacement requires the cache and fails naming the directory and the fetch
script. Verified by pointing `CW_RECORDINGS_DIR` at an empty directory.

### 18.6 Findings

1. **The 5 and 10 WPM sessions report 12.0 and 13.2 WPM.** Measured. Their
   character speed is far above their label, and they carry the highest CER and
   WER of the five. The 5 WPM error diff is two spurious spaces plus one `W`→`O`
   — spurious spaces being char gaps read as word gaps, the §16.1 defect.
   ⊘ **That ARRL keys these in Farnsworth is an inference from the reported
   speed, not a measurement.** The gap ratios in these files have not been
   measured and no ARRL statement to that effect was located. If it holds, these
   are real Farnsworth material with published text, which #29 has never had.

2. **35 WPM residual errors are uniform: every one adds exactly one element.**
   Measured by diff against the reference:

   | truth | decoded | count |
   |---|---|---|
   | `A` `.-` | `U` `..-` | 5 |
   | `L` `.-..` | `5` `.....` | 3 |
   | `L` | dropped | 3 |
   | `R` `.-.` | `H` `....` | 1 |
   | `F` `..-.` | `+` `.-.-.` | 1 |

   ⊘ **The mechanism is not measured.** The pattern is consistent with one dah
   being split in two, which would match the mid-element dropout of §12.2, and
   `legacy+mf` exists as a candidate fix that §12.3 refuted **on synthetic
   profiles only** — none at 35 WPM against real keying. Whether it addresses
   these errors is untested.

3. **Reported WPM is low by 6.0-7.0% on the three sessions whose label matches
   their character speed** (15→14.1, 20→18.6, 35→32.9). §17.2 measured
   synthetic clean `ditErr = +7.9%`, and `WPM = 1200/dit` makes a +7.9% dit a
   −7.3% WPM. ⚠ **This is agreement between two different instruments on two
   different signals, not a controlled comparison** — §17's figure comes from
   the gap-noise probe on synthetic `MSG_FULL`, this one from the channel's own
   estimator on ARRL text. It is the first evidence bearing on §15 generator
   fidelity and it points toward the generator's keying being realistic, but it
   does not establish it.

### 18.7 Incidental

- **`fetch_recordings.sh` had two bugs visible only on a cold cache**, both
  from staging downloads through `.part` temporaries: ffmpeg could not infer the
  format of either the input or the output. With a warm cache the script
  reported success while being broken for any first-time user. Fixed with a
  `.tmp.mp3` input name and explicit `-f wav`, then verified by deleting the
  cache and re-fetching all five sessions.
- The five reference texts were committed in `74d35dfa` and subsequently
  untracked; `tests/.gitignore` now covers `recordings/`. The audio was never
  committed — the repository's pre-existing `*.wav` rule excluded it, which was
  luck rather than design.
- ⊘ Only the 15/20/35 WPM texts were inventoried for punctuation. The 5 and 10
  WPM texts were not.

<a id="s19"></a>
## 19. The registry against real audio (Phase 24, 2026-07-20)

**Outcome: §12.3's refutation of `legacy+mf` was scope-limited — but the
mechanism it was refuted for is still not established, and nothing is
promotable.** Measurement only.

### 19.1 Method

`[recording-matrix]` runs all 21 registry cores against the five ARRL sessions
(§18), scoring with the repository's `score()` against the cleaned reference.
Each WAV is loaded once and reused across cores. `Channel::init` falls back to
`DEFAULT_CORE` for an unknown name rather than failing, so the sweep asserts
`coreName()` matches — without that an unmatched name would silently measure
legacy.

### 19.2 Results (CER; one realization per session, see §19.5)

| core | 5 WPM | 10 WPM | 15 WPM | 20 WPM | 35 WPM | mean |
|---|---|---|---|---|---|---|
| legacy | 0.0087 | 0.0044 | 0.0000 | 0.0000 | 0.0089 | 0.0044 |
| legacy+mf | 0.0087 | 0.0044 | 0.0000 | 0.0000 | **0.0000** | 0.0026 |
| legacy+kmeans | 0.0087 | 0.0044 | 0.0000 | 0.0000 | **0.0000** | 0.0026 |
| legacy+median | 0.0087 | 0.0022 | 0.0000 | 0.0000 | **0.0000** | 0.0022 |
| legacy+edge | 0.0044 | 0.0044 | 0.0000 | 0.0000 | 0.0000 | 0.0018 |
| legacy+edge+mf | 0.0044 | 0.0022 | 0.0000 | 0.0000 | 0.0000 | **0.0013** |
| legacy+edge+log | 0.0044 | 0.0022 | 0.0000 | 0.0000 | 0.0000 | **0.0013** |
| legacy+peak | 0.1354 | 0.0088 | 0.0000 | 0.0000 | 0.0000 | 0.0288 |
| legacy+peakdual | 0.3603 | 0.0398 | 0.0018 | 0.0011 | 0.0000 | 0.0806 |
| legacy+peakdual16 | 0.5087 | 0.2876 | 0.0147 | 0.0090 | 0.0034 | 0.1647 |
| legacy+bpf20 | 0.0087 | 0.0022 | 0.0000 | 0.0000 | 0.0390 | 0.0100 |

### 19.3 Findings

1. **`legacy+mf` takes 35 WPM from 0.0089 to 0.0000.** The §18.6.2 prediction
   held: §12.3 refuted `+mf` on synthetic profiles, none of which run at 35 WPM
   against real keying, and the gate that could test it did not exist then.

2. ⚠ **But `+mf` is not uniquely responsible, so the §12.2 attribution is NOT
   confirmed.** `legacy+kmeans` and `legacy+median` also reach 0.0000 at 35 WPM,
   and those are **timing-only** changes that touch no filter. Two unrelated
   routes fixing the same errors means those errors sit near a decision boundary
   — they are *marginal*, not the signature of one mechanism. ⊘ Whether the
   matched-filter resize dropout actually produces them is still unmeasured;
   §18.6.2's error pattern remains consistent with it and unproven.

3. **No candidate is promotable.** Against the 24-seed synthetic matrix:

   > ⚠ **These better/worse counts are unreliable — §20 supersedes them.** They
   > were read off mean differences with a 1e-6 float epsilon, which counts one
   > character across 24 seeds as a regression, and n=24 gives the wrong verdict
   > in both directions. Under a paired n=96 test (§20.4) the same three cores
   > read differently; the conclusion "nothing is promotable" survives, the
   > tallies do not.

   | core | better | worse | notable regression |
   |---|---|---|---|
   | legacy+mf | 5 | 3 | worstcase 0.6244 → 0.6450 |
   | legacy+edge+mf | 7 | 3 | noise2.0 0.0827 → 0.1496 |
   | legacy+edge+log | 8 | 2 | noise3.0 0.8181 → **1.8421** |

   `legacy+edge+log` is the best core on real audio and **more than doubles CER
   on heavy noise**; its mean across synthetic profiles is 0.3324 against
   legacy's 0.2269. Worse on any profile is a regression, not a tradeoff.

4. **This does not contradict §12–§13; it adds a regime.** Those phases found
   edge correction "over-corrects at low SNR". Measured SNR on the five ARRL
   sessions is **19.2 to 75.7 dB** — all high. The two bodies of evidence agree
   and describe different conditions.

5. **The `+peak` family is far worse on slow real audio than synthetic testing
   suggested** — 5 WPM: `+peak` 0.1354, `+peakdual` 0.3603, `+peakdual16`
   0.5087, against legacy's 0.0087. Consistent with §13's decision not to
   promote them.

6. **Narrowing the front end costs fast CW**: at 35 WPM, bpf40 0.0151, bpf30
   0.0335, bpf20 0.0390 against legacy 0.0089, while all three are neutral or
   better at slow speeds. Keying bandwidth scales with speed.

### 19.4 The consequence for the project

Every real recording available is high-SNR, and every candidate that wins there
loses under noise. **The regime that decides promotion is currently arbitrated
entirely by synthetic profiles whose fidelity is unvalidated (§15).**

⚠ **This section originally called noisy real recordings "the highest-value
missing measurement — not a nice-to-have." That was wrong, and §20.5 measured
why.** More data would *validate* the synthetic profiles; it would not *resolve*
the tradeoff, because the candidates disagree with each other in the noise
regime — the one the recordings would confirm — not in the high-SNR regime they
would add. §20.5 took the cheaper route the candidate list already noted
(add synthetic noise to the clean ARRL audio) and found the tradeoff reproduces
on real keying: `legacy+edge` regresses at t=4.33, `legacy+edge+log` emits
CER > 1.0. The recordings were not the binding constraint. The comparison
method was (§20).

📎 W1AW transmits these same texts on HF (3.5815 / 7.0475 / 14.0475 / 21.0675 /
28.0675 MHz), so an off-air capture would carry real propagation *and* published
ground truth, with receiver distance acting as an SNR ladder over identical
text. `kiwirecorder.py` (github.com/jks-prv/kiwiclient) records audio and IQ WAV
from public KiwiSDRs. Not attempted.

### 19.5 Caveats

- ⚠ **One realization per session.** Real audio cannot be re-drawn with another
  seed, so these are single samples — the same weakness this document criticises
  in the pre-2026-07 single-seed tables. Total reference length across the five
  is ~3800 characters, so a CER difference of 0.0044 → 0.0013 is roughly **12
  errors against 5**. Small absolute counts; treat the ordering among the
  leaders as provisional.
- The five sessions are one text each from two QST issues, all machine-keyed by
  the same station at one tone frequency. Speed varies; nothing else does.
- ⊘ Reported SNR comes from the decoder's own estimator, not an independent
  measurement.

## 20. The comparison method was the defect (Phase 25, 2026-07-20)

Phases 12–24 produced twelve registry variants and a better/worse tally for each
against a synthetic profile matrix. Every one of those tallies was read off mean
CER over 24 seeds compared with a 1e-6 float epsilon. This phase measured that
method against itself and found it could not support the conclusions drawn from
it. No decoder code changed; the harness did.

### 20.1 n=24 gives the wrong verdict, in both directions

Re-running the §19 candidates at n=96 flips verdicts that n=24 reported with
confidence — not toward "more significant", but to the opposite sign of decision:

| profile | n=24 | n=96 | resolved |
|---|---|---|---|
| noise3.0, `+edge` | +0.105, t=1.91 → ns | +0.157, **t=5.43** | real regression |
| handkeyed-15, `+edge` | −0.038, t=−1.09 → ns | −0.061, **t=−3.63** | real improvement |
| handkeyed-25, `+edge` | −0.011, t=−0.47 → ns | −0.039, **t=−3.29** | real improvement |
| noise2.0, `+edge` | +0.043, t=1.62 | +0.009, t=0.66 | spurious |

The baselines are unstable at n=24 as well: `legacy` on noise2.0 reads 0.0827 at
24 seeds and 0.1227 at 96 — a 48% shift in the *reference* number. Any threshold
pinned to a synthetic n=24 mean is pinned to a seed artifact. (The real-recording
ratchets in §18 are unaffected: those decode fixed audio and are deterministic.)

### 20.2 Paired testing and non-inferiority

Both cores run the identical seed list, so per-seed difficulty is a shared term
that cancels in the difference. `comparePaired` (`cw_bench_stats.h`) reports the
mean per-seed delta, its standard error, `t`, and `nDiffer`. The `nDiffer`
column is the tell: the `qrn` result that blocked `legacy+edge` in §19 was **3
differing seeds in 96** — a profile that barely exercised the code path being
compared was supplying a promotion-blocking verdict.

Two corrections to the decision rule followed:

- **Zero-variance is decisive, not insignificant.** A deterministic profile
  (`noiseAmp` and `jitterPct` both 0) generates the identical signal every seed,
  so a real difference reproduces on all of them with zero variance. The first
  implementation returned `t=0` there and labelled it `ns`, silently discarding
  regressions on every noiseless profile — `legacy+peakdual16` made
  `farnsworth20` three times worse (0.0141 → 0.0423) on 96/96 seeds and read
  `ns`. Zero variance with a nonzero delta now returns `t=±∞`.

- **Significance is not equivalence.** `|t| < 2` is absence of evidence of harm,
  not evidence of absence: a small real regression on a high-variance profile
  reads `ns` and would pass. Promotion now requires the upper 95% bound on the
  regression (`meanDelta + 2·stderr`) to sit under a sub-character tolerance
  (0.005 CER; one character of MSG_FULL is 0.0141). The candidate must
  demonstrate it is safe, not merely fail to be caught.

This inverted one earlier reading. Under significance-only, `legacy+mf` was
"0 better, 1 worse, 15 ns" — read at the time as *no measurable effect*. Under
non-inferiority, **10 of its 16 profiles are HARM**: `handkeyed-40` has delta
+0.0496 with stderr 0.0253, an upper bound of +0.1002. The `ns` verdicts were
low statistical power, not equivalence. "We measured no effect" is not "we
measured that there is no effect."

### 20.3 Speed and noise are confounded, and separable

The named profiles vary several parameters at once: the `noise*` set is 15 WPM /
jitter 0 / bias 0, while the `handkeyed*` set is `noiseAmp` 0.3 / jitter 0.15 /
bias 0.1. Attributing a `noise*`-vs-`handkeyed*` difference to "noise" also
attributes it to speed, jitter and weight bias. Coverage compounded the problem:
every one of the five hardcoded profile lists stopped at 25 WPM.

A speed×noise factorial with jitter held (`profileFactorial`, `[factorial]`)
separates them. `legacy+edge+log` vs `legacy`, n=48, paired `t` (`*` = |t|≥2):

**jitter 0.00 / bias 0.00**

| speed | noise 0.0 | 1.0 | 2.0 | 3.0 |
|---|---|---|---|---|
| 15 WPM | +0.00 | −1.00 | −3.66* | **+12.48*** |
| 25 WPM | +0.00 | +1.66 | +8.69* | **+14.75*** |
| 35 WPM | +0.00 | −7.06* | −2.33* | +3.42* |
| 40 WPM | +0.00 | −7.02* | −1.87 | +2.82* |

**jitter 0.15 / bias 0.10**

| speed | noise 0.0 | 1.0 | 2.0 | 3.0 |
|---|---|---|---|---|
| 15 WPM | −6.58* | −4.86* | −2.59* | **+11.27*** |
| 25 WPM | −4.79* | −2.82* | +6.00* | **+18.38*** |
| 35 WPM | −8.39* | −5.75* | −3.64* | +5.63* |
| 40 WPM | −5.78* | −6.68* | −5.19* | +2.91* |

Three readings the confounded profiles could not support:

1. **At noise 0.0 / jitter 0, every cell is +0.00.** Legacy decodes perfectly at
   40 WPM; speed alone breaks nothing. An earlier probe's "legacy 0.0290 at 35
   WPM" was `noiseAmp` 0.3 doing the work, not speed.
2. **At noise 0.0 / jitter 0.15, `+log` wins at all four speeds** (t −6.58 to
   −8.39). With noise removed entirely the benefit remains and is large — so it
   is a timing/jitter benefit, independent of noise.
3. **At noise 3.0, `+log` loses at every speed and both jitter levels** — a pure
   noise effect, independent of jitter.

Together: **`legacy+edge+log` trades noise robustness for timing robustness.**
Two orthogonal axes, opposite signs, both large. That is a mechanism statement,
not a tally, and the old profile set was structurally incapable of producing it.

⊘ **Open:** the `25 WPM × noise 2.0` cell is anomalous — worse (t +8.69, +6.00)
while 15, 35 and 40 WPM at the same noise are better or ns, at both jitter
levels. Not a clean threshold in either variable, unexplained.

✗ **Refuted in passing:** the claim (asserted earlier this session as physics)
that fixed `noiseAmp` is harsher at high speed because per-element energy falls.
Reported SNR at noise 3.0 *rises* with speed — 1.3 at 15 WPM to 3.1 at 40 WPM.
A plausible mechanism stated without measurement, contradicted by measurement.

### 20.4 Re-adjudicating the §19 candidates

Paired, n=96, non-inferiority tolerance 0.005 CER, over the standard profile set
including handkeyed-30/35/40 (`[promotion]`). "harmful" = upper 95% bound exceeds
tolerance; promotion needs zero harmful and at least one significant improvement.

| candidate | better | worse | ns | harmful | worst regression |
|---|---|---|---|---|---|
| legacy+edge | 6 | 1 | 9 | 3 | noise3.0 +0.157 (t=5.43) |
| legacy+mf | 0 | 1 | 15 | 10 | qsb +0.002 (t=2.61); 10 underpowered |
| legacy+edge+mf | 6 | 1 | 9 | 3 | noise3.0 +0.065 (t=2.82) |
| legacy+edge+log | 9 | 2 | 5 | 2 | noise3.0 +0.971 (t=18.06) |
| legacy (self) | 0 | 0 | 16 | 0 | — |

**Nothing is promotable** — the same conclusion as §19, now on defensible
evidence rather than an epsilon count. `legacy+edge+log` is the closest: only 2
harmful profiles, both its known catastrophic noise failures, everything else
clears non-inferiority. `legacy` against itself reads 0 harmful, confirming the
rule is not vacuously strict.

The instrument self-checks matter here because three sweeps in this suite have
shipped non-failing assertions before (§13.11): `legacy` vs `legacy` must read
`ns` with `nDiffer=0` on every profile, and `legacy+edge+log` — measured, not
assumed, to fail — must register a significant regression. Both would fail if the
harness were inert.

### 20.5 The tradeoff reproduces on real keying

The §19.4 claim that noisy real recordings were the highest-value missing
measurement was wrong. The candidates disagree with each other under *noise* —
the regime the recordings would confirm — not at high SNR, the regime they would
add. The cheaper test the §19 candidate list already proposed settles it: add
the generator's own AWGN to the clean ARRL audio, peak-normalized so `noiseAmp`
means the same scale it does synthetically (`[recording-noise]`).

`w1aw_20wpm` (real keying, CER 0.0000 clean), paired vs legacy, n=24:

| noiseAmp | legacy | +edge | +edge+log | +edge+mf |
|---|---|---|---|---|
| 0.0 | 0.0000 | ns | ns | ns |
| 1.0 | 0.1209 | **+0.0245 WORSE** (t=4.33) | −0.0049 ns | **+0.0128 WORSE** (t=2.71) |
| 2.0 | 0.9847 | ns | **+0.2231 WORSE** (t=3.60), CER 1.21 | ns |
| 3.0 | 0.9958 | ns | ns | ns |

The real-audio matrix (§19.2) ranked `+edge` and `+edge+log` *above* legacy.
Adding noise inverts it: `+edge` becomes a significant regression on real keying,
and `+edge+log` reproduces its runaway insertion — **CER 1.21 > 1.0**, emitting
more than the reference contains. The synthetic factorial predicted this; it now
holds on a real transmission, not only against the generator's Rician noise. The
§19 tradeoff is real, not a synthetic artifact.

⚠ **Narrow informative band.** Peak-normalized keyed CW has a steep noise cliff —
legacy is 0.12 at noiseAmp 1.0 but 0.98 at 2.0. So noiseAmp 1.0 is the one point
carrying signal; 2.0 and 3.0 are past the cliff where every core reads ~1.0 and
deltas are meaningless (except `+edge+log`'s garbage spike, which is the
finding). The peak-to-average ratio of keyed CW is the cause.

### 20.6 The harness now runs in parallel

Seeds are independent decodes over independent `Channel` instances, and the
decode path holds no mutable global state — `Channel` owns no `ToneScanner`, so
no FFTW planner (not thread-safe) is reachable, and every `static` under
`src/cw/` is `static constexpr` or a function-local `static const` table
(`cw_parallel.h`). Parallelizing the seed loops in `decodeAndScoreMulti` and
`runCellWith` cut `[matrix-timing]` from 13.92 s to 2.03 s (6.9×) and the default
suite from 11.26 s to 1.85 s (6.1×) on 14 cores.

Determinism is the gate, not a nicety: results are written to a slot indexed by
seed, never appended, so `CW_TEST_THREADS=1` and the default run produce
byte-identical output — verified across all 53 matrix rows and against the
pre-change serial baseline. Every metric except `rt(x)` is thread-count
invariant by construction; `rt(x)` becomes contended throughput under
concurrency and is documented as not comparable across thread counts.

### 20.7 The runaway-insertion mechanism (diagnosed)

The CER > 1.0 mode of §20.3/§20.5 is `log`-driven noise-as-elements, and its
dit-error sign is **opposite** to what an earlier draft of this section guessed.

**It is insertion, not fragmentation.** At 15 WPM / noise 3.0, `legacy+edge+log`
emits 185 characters against a 71-character reference (2.6×), overwhelmingly the
five shortest letters (E T I A N). Legacy stays at ~76 (≈ reference) and fails by
substitution/deletion. Error mix confirms it: legacy ins 0.097 / del 0.269;
`+edge+log` ins **1.281** / del 0.009. `log` alone flips the sign (ins 0.521);
`edge` amplifies super-additively (1.281 ≫ the sum of parts).

**The driver is dit UNDER-estimation, not the §17.3.2 overestimate.** Sampling
the live dit estimate at 15 WPM (true dit 80 ms), 16 seeds:

| core | noise 0.0 | 1.0 | 2.0 | 3.0 | frac under at 3.0 |
|---|---|---|---|---|---|
| legacy | 86.7 | 82.6 | 83.9 | **109.8** | 0.33 |
| legacy+log | 84.7 | 81.8 | 78.8 | **61.1** | 0.85 |
| legacy+edge+log | 80.2 | 75.0 | 69.5 | **44.9** | 0.98 |

Legacy's 109.8 ms is **+37.3%**, independently reproducing §17.3.2's +38.9% dit
overestimate from a different path — but it is a *legacy* property. `+edge+log`
goes the other way, −44%, believing dit is too small for essentially the entire
decode (frac under 0.98).

**Mechanism — the estimator's coordinates set the sign of the same fault.** A
linear timing estimator (legacy) is pulled *up* by the long-duration tail (dahs,
noise-merged elements): dit inflates, elements look short, gaps merge, output
deletes and stays conservative. Log-duration timing compresses that tail, so the
many *short* noise excursions dominate and pull dit *down*: the element threshold
shrinks, every noise blip is admitted as an element, and output inserts. Same
noise, opposite bias, opposite catastrophe.

**Consequence for `log`.** Its clean-signal timing win (§20.3: helps jitter at
every speed) and its noise catastrophe are the *same* property — insensitivity to
the long-duration tail helps when jitter stretches elements and destroys it when
noise fragments them. It is not tunable into a free win at this formulation —
§20.8 measured the tail-robust variant and confirms it.

### 20.8 The tail-robust log experiment fails, for a precise reason

The §20.7 diagnosis names the driver as short spikes pulling `x` down until
`minElementMs` (`0.15·getDitDuration()`, staged_core.h) collapses and admits more
spikes. `TIMING_LOG_GUARDED` (`legacy+logguard`, `legacy+edge+logguard`) attacks
exactly that: freeze `x` for any element beyond `guardK = 3σ`, so a spike cannot
move the estimate. This is distinct from the two attempts already on record —
`logrobust` only *downweights* `x` (Huber, never zero), and the hard gate the
`LogTiming` comment rejected dropped the `R` update too.

Two configurations, both measured against the paired n=96 gate (`[promotion]`):

**Freeze `x`, let `R` learn** (shipped as `legacy+logguard`). The runaway halves
at moderate noise and is untouched at heavy noise:

| | legacy | +edge+log | +edge+logguard |
|---|---|---|---|
| noise3.0 | 0.8173 | +0.971 (t=18.1) | **+0.600 (t=10.7)** |
| noise4.0 | 0.9108 | +0.898 (t=12.4) | +0.970 (t=13.9) |

Interference is preserved (qrm/qrn unchanged), but noise4.0 does not move,
because `R` still learns the full spike innovation, saturates to its 0.25 cap,
`V` grows, and `z = |innov|/√V` falls back under `guardK` — the gate widens and
the guard stops firing. Freezing `x` cannot outrun `R` saturation.

**Also freeze `R` beyond `guardK`** (tried, reverted). It blunts the runaway
further but reintroduces the exact catastrophe the `LogTiming` comment warned of:

| | +log qrm | +log qrn |
|---|---|---|
| R learns (guard off) | 0.0018 | 0.0006 |
| R frozen on outliers | **0.1966** | **0.0980** |

**The finding is the coupling, and it is stronger than the author's note.** Noise
spikes and qrm/qrn interference are both large-innovation outliers,
indistinguishable by magnitude. `R`-learning-from-outliers is simultaneously the
runaway driver (saturates the gate under a flood) and the interference tolerance
(widens acceptance so the decoder rides through qrm/qrn). The same trigger serves
both, so no magnitude-based gate separates them: freezing `R` to stop the runaway
destroys the tolerance, and leaving `R` free lets the runaway proceed at heavy
noise. `legacy+logguard` is kept as the measured partial result; **nothing here is
promotable.** A real fix would need a spike discriminator that is not innovation
magnitude — envelope shape, or the detector rejecting the excursion before timing
sees it — which is the likelihood-ratio detector line (#24), still blocked on the
noise regime having no real-audio ground truth.

- ⊘ The `25 WPM × noise 2.0` anomaly (§20.3).
- **Debt, not done:** four of the five hardcoded profile lists still carry their
  own membership and naming. Only `test_matrix.cpp` and `test_promotion.cpp`
  adopted `standardProfiles()`; unifying the rest would renumber tables these
  docs cite and was deferred.

## 21. The likelihood-ratio detector — first promotion (Phase 26, 2026-07-21)

§20.8 proved the log runaway could not be fixed in the timing layer: a noise
spike and qrm/qrn interference are the same innovation magnitude, so no
magnitude-based gate separates them. §20.8 named the missing ingredient — a
discriminator that rejects a brief excursion by the **duration of sustained
evidence**, not amplitude. This phase built it, and it is the first change in
the campaign to be promoted to the production default.

### 21.1 The detector

The envelope out of `EnvelopeFrontEnd` is `|LPF(IQ)|`: Rayleigh under noise,
Rician under signal. The Schmitt detector keys on `v > threshold`, so a noise
excursion that momentarily clears the threshold becomes a spurious key event —
the *same* detector feeds legacy and log, which is why one flood of short events
sinks log (§20.7) while legacy merely deletes.

`LRDetector` (`tone_detector.h`) replaces the instantaneous threshold with a
clamped CUSUM. Per sample it forms a range-normalized evidence
`u = (v - noiseFloor) / (signalPeak - noiseFloor)` — 0 at the noise level, 1 at
the signal level — and accumulates `u - theta` into `S ∈ [-bound, +bound]`,
keying down when `S` reaches the upper bound and up at the lower. A brief spike
contributes only a few samples of evidence and never crosses; rejection is by
duration. Output is hard `KeyEvent`s, so timing, the beam, and every interface
are unchanged (registered via `makeLR`, cores `legacy+lr`, `legacy+lr+log`).

⚠ **The first statistic was wrong and clean signal exposed it.** The exact energy
form `v²/(2σ²)` explodes as the noise estimate → 0: on a noiseless signal the
25th-percentile noise floor collapses, the ringing tail during a gap reads as
`stat ≫ 1`, the CUSUM never falls, and elements merge (clean-15 decoded as 22
garbled characters). The exact Rician LLR also grows only *linearly* in `v`, not
quadratically. Range normalization — tracking the signal level the way the
Schmitt threshold does — keeps `u` bounded at every SNR, and `theta = 0.5` makes
both edges lag symmetrically so element and gap durations are preserved. This is
an SPRT in spirit with a robust affine proxy for the LLR increment, not the
textbook energy statistic. Clean and Farnsworth then decode identically to
legacy.

### 21.2 Operating point

A sweep over `theta × bound` (n=48 paired vs legacy):

- `theta = 0.6` is uniformly bad — it breaks the symmetric-lag property and
  every profile regresses.
- `bound = 2` leaves a heavy-noise regression (noise4.0 WORSE); `bound = 4+`
  starts costing qsb.
- **`theta = 0.5, bound = 3.0`** is the sweet spot: raising the required
  evidence from 2 to 3 rejects the residual spikes that drove the noise3.0 /
  noise4.0 regressions, turning noise3.0 into a *win* without losing the
  hand-keyed and worstcase gains.

### 21.3 Result: the runaway is solved

`legacy+lr+log` vs `legacy`, paired n=96 (`[promotion]`):

| profile | delta | t | verdict |
|---|---|---|---|
| handkeyed-15…35 | −0.09 to −0.16 | −6.3 to −9.9 | BETTER |
| worstcase | −0.191 | −13.7 | BETTER (0.61 → 0.42) |
| noise2.0 | −0.094 | −8.5 | BETTER |
| **noise3.0** | **−0.111** | **−5.51** | **BETTER** |
| qrm | −0.002 | −2.4 | BETTER |
| handkeyed-40 | −0.019 | −0.9 | ns (BETTER at n=192) |
| qsb | +0.003 | +1.3 | ns |
| noise4.0 | +0.017 | +0.7 | ns |
| clean-15/25, farnsworth20 | 0.0000 | — | identical |

**9 significant improvements, zero significant regressions.** The headline
comparison: `legacy+edge+log` had noise3.0 **+0.97 (t=18)** and noise4.0 **+0.90
(t=12)** — catastrophic; the LR detector turns those into **−0.11 (a win)** and
**+0.017 (ns)**. §20.8's prediction held exactly: the non-magnitude
discriminator broke the coupling the timing layer could not.

The strict non-inferiority gate flagged three profiles; n=192 resolved them —
handkeyed-40 was underpowered and is a win (−0.029, t=−2.3), leaving two
non-significant sub-character costs: qsb (+0.002, upper bound 0.0053, a hair over
the 0.005 tolerance) and noise4.0 (+0.011 at SNR ≈ 1.1, where legacy already
scores 0.90). Every previously blocked candidate had a *significant* regression;
this one has none.

### 21.4 Promotion and re-ratcheting

Promoted to `DEFAULT_CORE` on the user's decision, `legacy` retained in the
registry for comparison. Consequences, all handled:

- **Two single-seed contest benchmarks failed** (seed 42 unlucky). Multi-seed
  measurement showed the profile is fine — mean 0.0021, 47/48 seeds decode
  exactly, and the specific bug they guard (B → 6 in K1ABC) occurs **0/48**.
  Converted both to multi-seed gates: the fragile single-seed assertions were
  exactly what this document criticised, so this fixes a known-bad test rather
  than loosening.
- **The always-on gates re-ratcheted** to the new default's `mean + 2·stderr`.
  Most tightened as the promotion earned it: hand-keyed 0.238 → 0.045, worstcase
  0.667 → 0.46, noise3.0 0.886 → 0.77. Three rose — qsb, contest, noise4.0 — the
  approved sub-character costs.
- **The real-audio gate improved**: 35 WPM 0.0089 → 0.0014, ratchet tightened to
  0.005. Every session decodes as well or better; clean and Farnsworth exact.

⚠ **Validation caveat, unchanged.** The heavy-noise wins are measured against the
generator's Gaussian noise and the noise-augmented real audio (§20.5), both
partly the LR test's own Rician model. The real-recording gate (§18) is genuine
off-the-air keying and lr+log holds or improves there, but the specific
heavy-noise regime that decided the promotion has no real-audio ground truth.
The promotion rests on a broad, significant, cross-validated win; the noise4.0
cost is a bound, not a measured failure.

## 22. Soft evidence to the beam — refuted (Phase 27, 2026-07-21)

Phase 26 kept the LR detector's decision hard: it emits key events, the soft
CUSUM margin stays internal. Phase 27 tested whether propagating that margin
helps. The beam already has a soft channel — `addElement(Element, confidence)`
forks weighted by confidence and widens under low confidence (`morse_tree.h`) —
so the detector's per-element evidence was folded in without new interfaces:
`KeyEvent` gained a `confidence` field (1.0 for every other core), the LR
detector in *soft* mode set it from the mean normalized evidence over the
element, and `StagedCore` multiplied it into the beam confidence. Cores
`legacy+lr+soft`, `legacy+lr+soft+log`.

**It is significantly worse and helps nowhere.** Paired n=96 vs the promoted
default `legacy+lr+log`:

| profile | default | soft | delta | t |
|---|---|---|---|---|
| noise2.0 | 0.0289 | 0.1097 | +0.081 | **+50.4** |
| qsb | 0.0156 | 0.0399 | +0.024 | +25.2 |
| worstcase | 0.4183 | 0.4531 | +0.035 | +11.8 |
| handkeyed-20 | 0.0257 | 0.0317 | +0.006 | +6.9 |
| noise3.0 | 0.7060 | 0.7127 | +0.007 | +5.9 |
| qrm / noise4.0 | — | — | ~0 | ns |

**The confidence channel is the wrong home for detector plausibility, and the
reason is semantic.** `addElement`'s confidence means *how certain is the
dit-vs-dah classification* — a timing quantity. The soft evidence carries *how
strong was the detection* — orthogonal to it. A faded-but-real dit (weak signal,
but unambiguous by duration) has low evidence, so folding it in down-weights a
correctly-classified element and widens the beam onto wrong alternatives. The
damage lands exactly where real elements are weak — qsb (fading) and noise2.0.
The hard LR decision had already extracted the useful information; the soft
margin only injects noise into a channel that expects a different measurement.

This vindicates the Phase 26 scope choice (hard events out, LR internal). The
plan's fallback — genuine soft key-state to timing and gaps via new interfaces —
is not pursued: the beam demonstrably does not benefit from detection strength,
so a larger interface change is unjustified. Soft evidence is a dead end at this
formulation. The soft cores stay in the registry as the measured negative
result; the default is unchanged (the `KeyEvent.confidence` multiply is 1.0 for
every non-soft core, so `legacy+lr+log` is byte-identical).

## 23. The +38.9% dit overestimate is resolved by the promotion (2026-07-21)

§17.3.2 named a **+38.9% dit overestimate at noise 3.0** the largest unexplained
number in these documents and the root of the gap misclassification in §17: gap
centres are dit-derived (`boundary = dit·2`), so an inflated dit drags char and
word gaps into shorter classes. That measurement was on Schmitt + Kalman — the
old default. The shipping default is now LR detector + log timing (§21), and it
does not share the bias.

**Direct dit measurement, 15 WPM (true dit 80 ms), across the chain:**

| chain | noise 1.0 | 2.0 | 3.0 |
|---|---|---|---|
| Schmitt+Kalman (legacy) | 82.6 (+3%) | 83.9 (+5%) | **109.8 (+37%)** |
| LR+Kalman | 79.4 | 75.0 | 119.4 (+49%) |
| Schmitt+log | 81.8 | 78.8 | 61.1 (−24%) |
| **LR+log (default)** | 79.2 (−1%) | 74.8 (−6%) | **76.0 (−5%)** |

The default's dit is within 5% of truth where legacy is +37%. It is **not simple
cancellation**: the LR detector removes the short spurious events that dragged
log's estimate down, so log rises from 61 (−24%) to 76 (−5%) — a correction. For
Kalman, whose overestimate comes from long-tail/merged-element sensitivity rather
than short spikes, the cleaner events do not help (110 → 119, worse). The LR
detector cures exactly the one bias log suffers, and only that one.

**Gap classification (`[gap-noise-lr]`, noise 3.0, detector-derived):**

| chain | damaged | clsErr% | ditErr% | E>C | C>E | W>C |
|---|---|---|---|---|---|---|
| Schmitt+Kalman | 11.0% | 12.4% | +38.9% | 64 | 35 | 39 |
| LR+log (default) | 9.5% | 10.0% | −18.0% | 92 | 1 | 17 |

Better on every aggregate — and the confusion pattern **inverted with the dit
sign**. Legacy's inflated dit dragged char/word gaps *down* into shorter classes
(C>E 35, W>C 39, merging characters); the default's slight *under*-estimate
pushes element gaps *up* into the char class (C>E collapses to 1, E>C rises to
92, splitting characters). Same mechanism, opposite tilt, smaller magnitude. This
is why the LR+log promotion wins noise3.0 (§21): the gap centres are no longer
badly corrupted.

⊘ **New residual, smaller than the old.** The default's −18% dit (this probe;
−5% in the full decoder) leaves an element-gap-splitting tendency (E>C dominant)
rather than the old character-merging one. It is net-better and downstream of an
already-good decode, so it is recorded, not chased. The §17.3.2 item is closed:
the number is explained (a Schmitt+Kalman property) and the shipping decoder does
not carry it.

## 24. The min-element filter bias is real but the filter is not removable (2026-07-21)

§17.3.4 measured that enabling the decoder's short-element filter
(`staged_core.h`, `minElementMs`) moves the hand-keyed dit error +5.9% → +11.3%:
the filter drops short elements to reject noise spikes, but on hand-keyed signals
real short dits are exactly what it rejects, so it biases the surviving mean up.
That was on Schmitt + Kalman. The filter is in the shared pipeline, so it applies
to the LR+log default too — and the LR detector already rejects spikes by
evidence duration (§21), so the filter might now be redundant cost.

`minElementMs` was made configurable (`minElementScale`, default 1.0). Sweeping
it on the default's chain, paired n=96 vs the shipping default:

| profile | scale 0.0 (off) vs 1.0 | verdict |
|---|---|---|
| handkeyed-40 | −0.0104 (t=−2.17) | BETTER |
| worstcase | +0.0066 (t=+2.94) | WORSE |
| noise3.0 | +0.0098 (t=+2.34) | WORSE |
| everything else | ~0 | ns |

**Confirmed and bounded, but not removable.** §17.3.4's bias is real under the
new default — relaxing the filter improves fast hand-keyed. But the filter is
**not redundant**: it still helps worstcase and noise3.0, so disabling it is a
net regression and fails the gate.

The two spike defenses are complementary, not overlapping. The LR detector
rejects *brief* excursions (too little evidence duration to cross the CUSUM); the
min-element filter rejects *short-but-sustained* ones (enough evidence to cross,
still shorter than a real element). On hand-keyed the second over-rejects real
short dits; on noise it catches real spikes — the same duration signal with
opposite meaning, the identical spike/interference ambiguity §20.8 met, now at
the filter. Scale 0.0 and 0.5 give near-identical results (the effect is binary,
not graded), and no SNR or speed threshold separates worstcase from hand-keyed —
both span the SNR range, the same wall the earlier SNR-gating attempt hit.

All magnitudes are sub-character. The filter is a justified, understood tradeoff;
`legacy+lr+log+nofilt` preserves the measured negative result. §17.3.4 is closed
as confirmed-but-not-actionable.

## 25. The fast-CW × noise regression, and the promotion revert (2026-07-21)

Investigating the §20.3 "25 WPM × noise 2.0 anomaly" under the new default
dissolved the anomaly and exposed something larger. A fine speed sweep at noise
2.0 showed no narrow spike at 25 WPM — instead `legacy+lr+log` degrades
monotonically with speed and is **worse than legacy across the fast range**:

| noise 2.0 | legacy | lr+log |
|---|---|---|
| 15 WPM (the gate) | 0.083 | 0.029 (better) |
| 20 WPM | 0.129 | 0.148 |
| 25 WPM | 0.191 | 0.379 |
| 30 WPM | 0.267 | 0.618 |

**The promotion gate had a noise-axis coverage hole: every noise profile in it
was 15 WPM.** Re-adjudicated with `noise2.0-25wpm` and `noise2.0-30wpm` added,
`legacy+lr+log` is 9 better / **2 worse** — noise2.0-25wpm +0.198 (t=18.2),
noise2.0-30wpm +0.332 (t=32.6). Large, significant, not the sub-character costs
the original gate implied.

**Mechanism: the LR detector, not log timing.** `legacy+log` (Schmitt+log) has
ins 0.056 at 25 WPM / noise 2.0 — identical to legacy; `legacy+lr` (LR+Kalman)
jumps to ins 0.186. The regression is insertion, and it comes from the detector.
The CUSUM's evidence requirement is fixed in normalized-evidence units, so its
detection lag is a fixed ~12 ms — 15% of a 15 WPM dit but 30% of a 30 WPM dit.
At speed the smearing lets noise re-trigger spurious elements in the short fast
gaps. No CUSUM bound fixes it: raising it trades insertion for deletion (bound 8:
del 0.34), both bad. The detector's spike defense is calibrated in absolute time,
but fast CW lives at a different timescale — the same "fixed constant vs variable
element size" mismatch as the min-element filter (§24), now in the detector.

**Reverted (user decision).** With full coverage the promotion is a tradeoff, not
a clean win: comparable magnitudes on both sides. The wins (hand-keyed all
speeds, worstcase 0.61→0.42, 15 WPM under noise, the runaway fix) are in usable
regimes; the losses are fast CW at ~0 dB SNR where both decoders already fail
(30 WPM / noise 2.0: legacy 0.29, lr+log 0.62). `DEFAULT_CORE` is restored to
`legacy` and the §21.4 gate ratchets rolled back; `legacy+lr+log` stays a
registry variant. The fast+noise profiles are kept in `standardProfiles` (the
coverage that exposed this), so any re-promotion must clear them. A speed-adaptive
detector fix follows (§26).

## 26. Speed-adaptive LR bound — attempted, hits the same wall (2026-07-21)

§25 traced the fast-CW × noise regression to the LR detector's fixed evidence
lag (~12 ms), too large a fraction of a short fast element, smearing event
timing. Instrumentation confirmed it is timing distortion, not spurious
detections: at 25 WPM / noise 2.0 the LR detector emits *fewer* events than
Schmitt (161.8 vs 176) and fewer short elements (4.5 vs 7.3), yet the decoded
text carries extra short letters clustered in the number groups — the gap
classifier mis-segmenting the long dah-runs under distorted timing.

The fix: scale the CUSUM bound by the recent element duration so the lag is a
constant *fraction* of an element (`setAdaptive`, `legacy+lr+log+adapt`). Three
estimators, each defeated by the same obstacle:

| estimator | fast (handkeyed-40) | slow+heavy (noise3.0-15w) |
|---|---|---|
| mean ON duration | 0.076 (win) | 0.934 (broke; was 0.706) |
| windowed median | 0.055 (win) | 1.636 (broke worse) |
| median + sub-dit gate | 0.082 (win) | 1.541 (still broke) |

Every version helps fast CW — handkeyed-40 0.168 → ~0.08, and the target
fast+moderate-noise improves (noise2.0-25w 0.383 → 0.235, 30w 0.619 → 0.370).
And every version **breaks slow + heavy noise**, because the speed estimate is
built from the detector's own key-down durations, which at heavy noise are
dominated by spurious short/mid-length elements. The estimate reads "fast" when
the signal is slow, shrinks the bound, weakens spike rejection, and revives the
runaway — a positive feedback at exactly the regime that needs the safe bound.

The sub-dit gate (hold the full bound when a low percentile sits far below the
median, i.e. sub-dit spikes are present) does not save it: at noise 3.0 the
corruption is not only sub-dit spikes but mid-length spurious elements, so the
median is dragged down while the gate stays open. **The detector cannot know the
true 15 WPM median (237 ms) without external speed information**, and every
self-estimate is corrupted precisely where the safe bound matters most. This is
the §20.8 / §24 / §25 duration ambiguity a fifth time — a short element is a fast
dit or a slow spike — now corrupting the very speed estimate meant to resolve it.

**No re-promotion path via this route.** The adaptive variant is kept as the
measured negative result (`legacy+lr+log+adapt`); the base LR detector is
unchanged (the flag defaults off). A robust fix would need a speed signal from
outside the detector — the timing stage's dit estimate, or an operator WPM
setting — which is an architectural change (detector↔timing coupling) not
pursued here. The LR detector stands as a variant with a known fast+heavy-noise
weakness; `legacy` remains the default.

## 27. External-speed hypothesis check — necessary, not sufficient (2026-07-21)

§26 failed and could not tell *why*: is the in-detector speed *estimate* the
problem (corrupt at heavy noise), or is bound-scaling *itself* the problem
(broken even with a correct speed)? Before paying for the detector↔timing
coupling that would deliver a production speed signal, this isolates the
variable: feed the LR detector the generator's ground-truth dit
(`setExternalDitMs`, docs) instead of the self-estimate — same scaling law
(`scale = clamp(ditMs / 80 ms, 0.4, 1.2)`, 80 ms = the 15 WPM tuning point),
uncorrupted input. `test_lr_wpm.cpp`, `[lr-wpm]`, paired n=96, per-signal factory
so each profile's true speed reaches its own detector. This is a **hypothesis
check, not a shipping path**: no real decoder knows the true WPM.

The result splits the §26 question cleanly in two.

**Q1 — does a correct speed signal avoid §26's slow-noise breakage? Yes, by
construction.** At 15 WPM `scale ≡ 1.0`, so truth-WPM is byte-identical to base
`legacy+lr+log` on every 15 WPM profile (`nDiffer == 0`, asserted). noise3.0
stays 0.706, worstcase 0.418 — both better than legacy. §26's catastrophe was
*entirely* the corrupt estimate perturbing the slow cases; bound-scaling never
touches them. The estimate was the problem, not the scaling.

**Q2 — with a perfect speed signal, does fast CW return to legacy parity? No —
a residual fast + *heavy*-noise gap survives.**

| profile | legacy | base lr+log | truth-WPM | truth vs legacy |
|---|---|---|---|---|
| handkeyed-30 | 0.142 | 0.043 | 0.025 | −0.117 (better) |
| handkeyed-40 | 0.187 | 0.168 | 0.042 | −0.144 (better) |
| noise2.0-25wpm | 0.184 | 0.383 | **0.244** | **+0.059 (t=7.3, worse)** |
| noise2.0-30wpm | 0.287 | 0.619 | **0.391** | **+0.105 (t=14, worse)** |

The truth signal recovers ~70% of each fast+noise regression (base-LR 0.383 →
0.244, 0.619 → 0.391) and pushes clean/hand-keyed fast CW *past* legacy, but the
two fast + heavy-noise profiles stay significantly worse than legacy. The
mechanism is a genuine tradeoff a single scalar cannot win: shrinking the bound
for speed shortens the lag (the intended win) but also lowers the evidence margin
over noise, so at heavy noise the shortened bound admits more spikes. Fast+light
noise (handkeyed, noiseAmp 0.3) has margin to spare and wins outright; fast+heavy
noise (noiseAmp 2.0) does not.

**Conclusion: the external-speed thesis is necessary but not sufficient.** A
correct speed signal removes the §26 failure mode and would make the LR detector
beat legacy on every realistic hand-keyed profile — but it does **not** unblock
promotion-to-default, because noise2.0-25/30wpm remain harmful even in the
best case a production speed source could ever provide. Building the
detector↔timing coupling *for promotion's sake* is therefore not justified: the
gate would still block. The value of an external speed signal is real but narrower
than promotion — it is a hand-keyed-accuracy lever, not a fast+heavy-noise fix.
`legacy` remains the default; the `setExternalDitMs` knob is kept as the
instrument that measured this, defaulting off (0 → no override, base LR
unchanged).

## 28. Noise-side (SNR) bound scaling — wrong axis, refuted by the curve (2026-07-21)

§27 left a fast + heavy-noise residual and proposed closing it from the noise
side: grow the CUSUM bound when SNR is low (more noise → more evidence required
→ fewer admitted spikes). Before writing any SNR law, the decisive question is
whether the CER-vs-bound curve at the target profiles even has a minimum away
from where speed-scaling already sits. `test_lr_snr.cpp`, `[lr-snr]`:
`setExternalDitMs` sets the bound scale to `clamp(ditMs/80, 0.4, 1.2)`, so a fake
dit sweeps the bound at a fixed real signal — a controlled bound-vs-CER curve.

| profile (real speed) | scale 0.4 | 0.6 | 0.8 | 1.0 | 1.2 | legacy | curve |
|---|---|---|---|---|---|---|---|
| noise2.0-25wpm (fast+heavy) | 0.221 | 0.238 | 0.297 | 0.387 | 0.505 | 0.187 | ↑ small best |
| noise2.0-30wpm (fast+heavy) | 0.355 | 0.434 | 0.500 | 0.615 | 0.740 | 0.279 | ↑ small best |
| noise3.0 (slow+heavy) | 1.757 | 1.002 | 0.772 | 0.710 | 0.697 | 0.820 | ↓ large best |
| handkeyed-40 (fast+light) | 0.043 | 0.049 | 0.069 | 0.151 | 0.363 | 0.177 | ↑ small best |

**The two heavy-noise regimes want opposite bounds.** Fast+heavy is monotonic
*increasing* — a small bound is always better, because at fast speed the CUSUM
lag dominates and a larger bound only adds timing distortion. Slow+heavy is
monotonic *decreasing* — a large bound is better, because at slow speed there is
timing headroom and spike rejection dominates. (Asserted in the test as
invariants, both directions.)

**SNR cannot separate them.** Both regimes are low-SNR, so any rule keyed on SNR
moves their bounds the *same* way — it would grow the bound for fast+heavy
(making it worse, 0.238 → 0.387) while helping slow+heavy. The variable that
actually separates "wants small" from "wants large" is **speed**, and speed-
scaling (§27) already picks the correct direction in every quadrant:
fast → small, slow → large. There is no quadrant where speed chooses the wrong
bound and SNR would rescue it; SNR is redundant with speed here, and strictly
worse because it is blind to the axis that matters.

**And the residual is intrinsic.** Even at the floor scale 0.4 — the best any
bound rule can do — fast+heavy stays above legacy (0.221 vs 0.187, 0.355 vs
0.279; asserted). Schmitt simply handles fast + heavy noise better than the LR
CUSUM at *any* bound. The fast+heavy gap is not a bound-tuning miss; it is a
property of sequential evidence accumulation under a short element and a low
margin, which no bound scalar removes.

**Conclusion: bound-scaling is exhausted, on every axis.** Speed is the right
axis and §27 already rides it to its limit; SNR is the wrong axis; and the
remaining gap to legacy is intrinsic to the detector. The only bound-side lever
left is lowering the speed clamp floor below 0.4 (fast+heavy is still improving
at the floor), which is a marginal speed-side tweak, not a noise-side fix, and
does not reach legacy. `legacy` remains the default. The LR detector stands as a
variant that wins on hand-keyed and the runaway but loses on fast + heavy noise
— a boundary now mapped from both the speed and the noise side.

The residual is intrinsic to *fixing it at the detector*, not to the problem —
see §29, which closes it from the front end.

## 29. WPM-matched BPF — the fast+heavy residual was a front-end problem (2026-07-21)

§21–28 attacked the fast + heavy-noise regression at the *detector* and hit a
wall: the CUSUM bound cannot be short (for lag) and long (for spike rejection) at
once. But §4 already measured that the largest heavy-noise lever is upstream of
the detector entirely — the pre-detection BPF bandwidth (noise3.0/15WPM
0.76 → 0.02, 38×, monotone in bandwidth). §4 could not promote a narrow BPF
because a *fixed* narrow filter smears fast keying edges and costs fast CW
(35 WPM 0.039). #31's proposal: match bandwidth to the locked WPM
(ENBW ≈ 2/T_dit), so slow gets a narrow filter and fast a wide one — dissolving
the tradeoff §4 was stuck on.

Ceiling experiment before any runtime-retuning machinery (`test_bpf_wpm.cpp`,
`[bpf-wpm]`, paired n=96): a `legacy` core (Schmitt + Kalman, unchanged) built
per-signal with its BPF cutoff derived from the generator's ground-truth dit —
the best a perfect WPM lock could do. Cutoff from the §4.1 fit
`cut = (2000/ditMs + 2)/1.65`, clamped to the characterised 20–40 Hz range.

| profile | cut (Hz) | legacy | matched | delta | t |
|---|---|---|---|---|---|
| noise3.0 (15wpm) | 20 | 0.817 | **0.075** | −0.743 | −39 |
| noise4.0 | 20 | 0.911 | 0.521 | −0.390 | −22 |
| noise2.0 | 20 | 0.123 | 0.005 | −0.118 | −11 |
| worstcase | 20 | 0.610 | 0.462 | −0.148 | −7 |
| **noise2.0-25wpm** | 26 | 0.184 | **0.023** | −0.161 | −25 |
| **noise2.0-30wpm** | 31 | 0.287 | 0.125 | −0.162 | −18 |
| qsb | 20 | 0.013 | 0.008 | −0.005 | −2.2 |
| handkeyed-15..35 | 20–37 | ~0.15 | ~0.17 | +0.008..+0.033 | <1.7 (ns) |

**7 significant wins, 0 significant regressions.** The two profiles that reverted
the LR promotion (§25) and defeated every bound rule (§26–28) —
`noise2.0-25/30wpm` — improve 8× and 2.3×. The residual §28 called "intrinsic to
the CUSUM" was intrinsic to fixing it *at the detector*: raising pre-detection
SNR sidesteps the lag/margin conflict, and WPM-matching keeps fast CW from paying
the smear (handkeyed-40 gets a 40 Hz filter, −0.009 ns). This is a front-end
mechanism, orthogonal to the detector, and a *minimal* change to the shipped
default — only the BPF bandwidth, Schmitt and timing unchanged.

**One caveat blocked a clean promotion, and it is now fixed.** In the
speed-only rule six hand-keyed profiles flagged the non-inferiority bound: small
positive deltas (+0.008…+0.033), all *non-significant*. Hand-keyed is noiseAmp
0.3 + 15% jitter — jitter-limited, not noise-limited — so a bandwidth matched to
*white noise* over-narrows it and the narrow filter's longer impulse response
smears the jittered edges. The `[bpf-sweep]` curve confirmed the mechanism: at a
*fixed* 15 WPM, the heavy-noise profile argmins narrow (noise3.0 20 Hz 0.075 vs
100 Hz 0.827) while hand-keyed argmins wide (0.158 at 100 Hz) — same speed,
opposite bandwidth. Bandwidth must key on noise, not just speed.

**Noise-aware bandwidth (the fix).** The WPM-matched cutoff becomes a *floor*
reached only at low SNR; at high SNR the geometry widens back to legacy's exact
(100, 100) — a byte-exact no-op, so a jitter-limited signal cannot regress.
Blend on noise level: legacy at noiseAmp ≤ 0.5, matched at ≥ 1.5, linear between.
Result (`[bpf-noise]`, paired n=96):

| | count |
|---|---|
| significant better | 6 (worstcase, noise2.0/3.0/4.0, noise2.0-25/30wpm) |
| significant worse | 0 |
| harmful (non-inferiority) | **0** |

Every light-noise profile is now delta exactly 0.0000 (byte-identical to legacy),
every heavy-noise win is retained. **The ground-truth ceiling is clean and
promotable** — including the noise2.0-25/30wpm regime the entire LR arc (§21–28)
could not touch. Notably `worstcase` carries 20% jitter *and* heavy noise and
still wins narrow (0.610 → 0.462), evidence the narrowing is safe under jitter
once noise is heavy.

**What this is not, yet: a runtime rule.** The ceiling keys on the generator's
*true* dit and *true* noiseAmp. A shipping decoder must approximate both — WPM
from the timing lock (`getDitDuration`), noise from the detector's SNR
(`getSNR`) — and retune the BPF once at lock without a filter-transition glitch
(cf. the §12 MF-resize transient). The ceiling says the target is worth building;
the runtime rule is future work. But before promoting on these numbers at all,
the noise axis itself had to be put in physical units — §31.

## 31. Calibrated SNR — the benchmark's noise axis in dB (2026-07-21)

Prompted by a methodology question before promoting the BPF: is the noise axis
compliant with signal-processing benchmark practice? It was not. Every profile
was parametrized by raw `noiseAmp`, which is not a physical quantity — it depends
on signal amplitude, sample rate, and measurement bandwidth, so "noise3.0" could
not be reproduced by another lab or placed on any external CER-vs-SNR curve
(AG1LE, PA3FWM). The statistics were sound (paired, n=96, non-inferiority); the
*units* were not.

**Calibration** (`cw_snr.h`, `[snr]` always-on). The generator's model:
signal power `S = A²` at key-down; complex AWGN added to I and Q each
`~noiseAmp·N(0,1)`, so noise power `2·noiseAmp²` white over the full rate ⇒
`N0 = 2·noiseAmp²/f_s` and `SNR(B) = A²·f_s/(2·noiseAmp²·B)`. This is the
**input** (pre-detection) SNR — a property of the signal, invertible, and
reported in a stated reference bandwidth (2500 Hz SSB, 500 Hz CW). The standard
profiles translate to:

| noiseAmp | SNR/2500 Hz | SNR/500 Hz | detector getSNR (post-BPF) |
|---|---|---|---|
| 0.3 (hand-keyed) | +12.5 | +19.5 | +11.4 |
| 1.0 | +2.0 | +9.0 | +6.9 |
| 2.0 (noise2.0) | −4.0 | +3.0 | +4.5 |
| 3.0 (noise3.0) | −7.5 | −0.5 | +3.2 |
| 4.0 (noise4.0) | −10.0 | −3.0 | +2.3 |

Two things this makes visible. First, **input SNR and the detector's `getSNR`
diverge and even cross** — at noiseAmp 3.0 the input is −7.5 dB but getSNR reads
+3.2 dB, because the wide BPF already rejects most out-of-band noise. So neither
`noiseAmp` nor `getSNR` is a valid benchmark unit; only the analytical input SNR
is. Second, **every profile sits above PA3FWM's ≈−18 dB by-ear copy floor**, so
legacy's 0.82 CER at noise3.0 (−7.5 dB) is nowhere near a physical limit — the
headroom the campaign kept finding is real, and now quantified.

**The §29 result in physical units** (`[bpf-snr-sweep]`, 15 WPM, paired n=96):

| SNR/2500 Hz | legacy | noise-aware BPF | delta | t |
|---|---|---|---|---|
| +12 … +3 | ~0.001 | ~0.002 | ns | — |
| −3 | 0.117 | 0.003 | −0.114 | −8.3 |
| −6 | 0.397 | 0.017 | −0.381 | −24 |
| −9 | 0.878 | 0.266 | −0.612 | −37 |

Exactly the signature of a front-end SNR gain: no effect where SNR is ample,
growing benefit as it falls, **0 regressions across the calibrated axis**. This
sweep also exercises the blend zone (partial narrowing at 0…+3 dB, noiseAmp
0.9–1.3) that the discrete profiles skipped — safe there too. The BPF win is now
stated where the literature states it: at −6 dB SNR/2500 Hz, CER 0.40 → 0.017.

**Scope, stated honestly.** This fixes the *unit* problem — the largest
benchmark-rigor gap and a prerequisite for a credible promotion. Two lower-order
gaps remain, both lower urgency because the paired design absorbs most of their
risk: the noise model is complex-Gaussian, so the envelope is exactly Rician —
the LR detector's own assumption (a circular validation for §21–28, less so for a
front-end filter); and most profiles use one fixed message, so multi-seed
averages over noise realizations, not over text content. Independent per-impairment
RNG streams and a second message would close those.

## 30. WPM-locked BPF — the runtime rule (2026-07-21)

The §29 ceiling keys on the generator's true dit and true noise. The shippable
core `legacy+bpfauto` (`makeStaged(..., adaptiveBpf=true)`) approximates both from
runtime signals: at timing lock it reads the locked WPM (`getDitDuration`) and the
detector's SNR (`getSNR`), computes the noise-aware geometry (`StagedCore::bpfGeom`,
SNR thresholds from the §31 calibration: wide above 9.5 dB getSNR, matched below
5.4), and retunes the BPF once. `legacy` is byte-identical (the flag defaults off).

**Two defects surfaced; one was a bug, one is fundamental.**

*The retune transient (fixed).* Rebuilding the BPF clears its history, a one-time
transient. On the first run clean-25 went 0.0000 → 0.0141 (a spurious character,
t=∞) because at high SNR the target geometry is the baseline (100, 100) yet the
retune fired anyway and injected the transient onto a clean decode; the same
mechanism inflated the hand-keyed non-inferiority flags. Fix: skip the retune when
no narrowing is wanted (target cut ≥ 99). Because clean and hand-keyed both sit at
narrowFrac 0, the guard makes them byte-identical to legacy — clearing clean *and*
hand-keyed at once.

Result (`[bpf-runtime]`, paired n=96):

| profile | legacy | bpfauto | ceiling (§29) |
|---|---|---|---|
| clean, hand-keyed, qrm, qrn, farnsworth | — | **= legacy** | = legacy |
| qsb | 0.0126 | 0.0082 | 0.0076 |
| worstcase | 0.610 | 0.488 | 0.462 |
| noise2.0 | 0.123 | 0.029 | 0.005 |
| noise3.0 | 0.817 | **0.443** | **0.075** |
| noise2.0-25wpm | 0.184 | 0.081 | 0.023 |
| noise2.0-30wpm | 0.287 | 0.157 | 0.125 |

**6 significant wins, 0 significant regressions** — the first core in the campaign
to beat legacy with no significant regression anywhere. (One non-inferiority flag,
noise4.0, but its delta is *negative* — −0.007 — a high-variance non-significant
improvement on a −10 dB, 0.90-CER profile, not a regression.)

*The acquisition gap (fundamental).* The runtime rule reaches only ~50–70% of the
ceiling on heavy noise (noise3.0 0.443 vs 0.075) because it can only narrow *after*
lock, and acquiring WPM through the wide filter at −7.5 dB is exactly what is slow
— a chicken-and-egg the ground-truth ceiling does not have: the pre-lock portion is
decoded and retro-replayed through the wide filter. Closing it needs the signal
re-filtered from the start once WPM is known (buffer the raw IQ and re-run the front
end at lock) — a larger change, deferred. The runtime rule as it stands is a clean,
large, regression-free win; the ceiling marks the headroom still on the table.

Kept as the variant `legacy+bpfauto`; promotion to default is a separate decision.

## 32. Real audio refutes the runtime rule — but validates the thesis (2026-07-21)

Before promoting `legacy+bpfauto`, the deferred real-audio validation (§31): add
calibrated AWGN to the peak-normalized W1AW 20 WPM recording and adjudicate
against legacy, plus fixed narrow filters as a diagnostic (`[recording-noise]`).
The result split cleanly, and it blocks the promotion.

| noiseAmp (~dB/2500, synth-eq) | legacy | bpfauto | bpf20 (fixed) | bpf30 |
|---|---|---|---|---|
| 1.0 (+2.0) | 0.121 | **0.121 (ns)** | **0.021 (t=−19)** | 0.024 |
| 2.0 (−4.0) | 0.985 | 1.001 (ns) | 0.522 (t=−74) | 0.981 |
| 3.0 (−7.5) | 0.996 | 0.996 (ns) | 1.264 (worse) | 1.711 |

**The thesis transfers to real audio.** A fixed narrow BPF is a large, real win on
real keying at moderate noise — `bpf20` cuts noiseAmp-1.0 CER 6× (0.121 → 0.021,
t=−19). Narrowing the pre-detection bandwidth helps real signals, not only the
synthetic generator. The §29 physics is sound.

**But `legacy+bpfauto` fails on real audio — 0 wins, and it does not engage.** At
noiseAmp 1.0, where `bpf20` wins 6×, `bpfauto` is byte-identical to legacy
(delta 0.0000 on all 24 seeds): its narrowing never fired. The cause is the
`getSNR` trigger. §31 already showed input SNR and the post-BPF `getSNR` diverge;
here it is decisive — on real 20 WPM audio the post-wide-BPF `getSNR` reads above
the 9.5 dB "stay wide" threshold (calibrated on synthetic 15 WPM), so the rule
concludes no narrowing is needed exactly when a 6× win is available. The runtime
trigger does not transfer across signal type, and `getSNR` was the wrong signal to
gate on — the same post-filter-vs-input confusion the calibration section warned
about.

**A fixed narrow filter is not the fix either.** `bpf20` *worsens* the heaviest
noise (noiseAmp 3.0: 0.996 → 1.264, emitting garbage) — at −7.5 dB it over-narrows
the 20 WPM keying, the §4 "narrow filters emit garbage rather than going silent"
floor. So the bandwidth genuinely must adapt to both speed and noise; the §29
noise-aware *shape* is right, only its runtime *trigger* is broken.

**Promotion blocked, and correctly so** — the synthetic gate ([bpf-runtime], 6
wins / 0 regressions) passed a rule that does nothing on real audio. This is the
methodology working: real audio caught what synthetic could not, and it also
proved the underlying win is real and large (6× at moderate noise). The next step
is a narrowing trigger that transfers — §33.

## 33. The input-referred trigger — bpfauto works on real audio (2026-07-21)

The §32 diagnosis: the win is real, the trigger is broken. `getSNR` is measured
*after* the narrow BPF, so it reads high on real audio (the filter already removed
the noise) and never fires. The fix is a signal that reflects the *input*.

**Input-referred SNR** (`EnvelopeDSP::getInputSnrDb`, dsp.h). A second noise-floor
+ peak estimator on the *decimated* signal, before the narrow BPF, where the full
decimated-band noise is still present. It transfers across signal type — the
property `getSNR` lacked:

| noiseAmp | synthetic inputSnr | real inputSnr | (getSNR real) |
|---|---|---|---|
| 1.0 | 6.1 | 6.0 | 4.6 |
| clean | — | 23.4 | 25.3 |

It reads ~6 dB at noiseAmp ≥ 1 on both synthetic and real, ~9+ light, ~23 clean.
(It *saturates* at ~6 once the signal is buried — the Rayleigh peak/p25 ratio is
noise-independent — so it is a binary "noisy or not", which is what the trigger
needs.)

Getting it to work took three more fixes, each found by measurement:

1. **Convergence gate.** Read at the instant of timing lock (~1.7 s) `inputSnr` is
   a 60 dB transient — its 2 s noise window has not filled. The retune must wait
   for the estimate to converge (`inputSnrReady`), not just for lock. This is what
   made it fire at all.
2. **Out-of-band interferer guard.** QRM's interferer sits in the wide decimated
   band, so `inputSnr` reads it as noise (4.7) and mis-narrowed a near-perfect
   profile (qrm 0.0018 → 0.014). But the 100 Hz BPF already rejects it, so `getSNR`
   stays high (11.4). Rule: **narrow only when `getSNR` is also low** — a high
   getSNR with low inputSnr means out-of-band interference the filter handles.
   This also keeps light-noise hand-keyed (getSNR ~11) wide, resolving §29 for free.
3. **Garbage floor.** Below ~−7 dB the signal is unrecoverable and a narrow filter
   makes noise look like signal (§4/§32), so narrowing *worsens* an already-failed
   decode. A low `getSNR` floor (< 3.5) keeps it wide there. But the separation is
   tight: at the decision point noise3.0 (narrow helps) reads getSNR 3.7 and
   noise4.0 (narrow hurts) reads 3.2 — 0.5 dB apart. So the floor is a genuine
   tradeoff, not a clean cut: 3.5 protects against garbage at the cost of clipping
   part of the noise3.0 win (0.20 → 0.55, still a large gain). Safety wins for a
   shipped default.

**Result — bpfauto now works on real audio** (the §32 blocker is cleared):

| | real noiseAmp 1.0 | synthetic noise3.0 | synthetic grid |
|---|---|---|---|
| legacy | 0.121 | 0.817 | — |
| bpfauto | **0.026 (t=−16)** | 0.551 (t=−8) | **6 wins, 0 significant regressions** |

Real audio at moderate noise — the realistic, important case — improves ~5×, and
bpfauto no longer emits garbage at the deepest noise (byte-identical to legacy at
real noiseAmp 3.0). The trigger transfers. One residual: synthetic noise4.0
(−10 dB) trips the non-inferiority bound (+0.017, *ns*, t=0.8) — the same
high-variance total-failure regime where the 0.5 dB getSNR separation cannot be
resolved. It is not a significant regression.

**State:** `legacy+bpfauto` is now a strong, real-audio-validated candidate — 6
significant wins, 0 significant regressions, works on real keying at moderate
noise, degrades safely at extreme noise. The lone non-inferiority flag is *ns* at
a −10 dB total-failure SNR.

## 34. The acquisition gap is blocked by the garbage floor (2026-07-21)

The one remaining avenue for more: bpfauto narrows only *after* timing lock, so at
heavy noise (lock late, 10 s+) the acquisition window is decoded wide — the fixed
narrow filter `bpf20` reaches 0.52 at real noiseAmp 2.0 where bpfauto reaches only
0.97. Tried firing the retune as soon as `inputSnr` converges (~2 s), before lock,
using a mid-range default WPM.

Refuted. It changed nothing at noiseAmp 2.0 and slightly perturbed lighter-noise
synthetic (6 → 5 wins). The reason is decisive: at the heavy-noise SNRs where the
acquisition gap matters, the `getSNR` garbage-floor guard (< 3.5) *already blocks
narrowing* — noiseAmp 2.0 reads getSNR 2.3. Narrowing earlier cannot help where
narrowing is forbidden. The binding constraint is the garbage floor, not the
acquisition timing, and the floor cannot be relaxed because getSNR does not
separate "narrow helps" at noiseAmp 2.0 (2.3) from "narrow hurts" at 3.0 (2.4) —
the same 0.5 dB wall as §33. Reverted.

Closing the acquisition gap would need the raw IQ re-filtered from the start once
WPM is known — architecturally hard (the front end is not re-runnable on buffered
input: the xlator phase and filter state have advanced). Not pursued. bpfauto is
at the limit of what bandwidth/trigger tuning reaches.

## 35. Promotion attempt — a real single-seed variance blocker (2026-07-21)

Setting `DEFAULT_CORE = legacy+bpfauto` fails 7 always-on single-seed gates, the
worst being `worstcase` on MSG_CQ / seed 42 at CER 0.957 where legacy passes under
0.6. This is not a threshold that needs re-ratcheting — it is a real failure mode
the profile-averaged gates hid, exactly the class that reverted the LR detector
(§25). Characterized (`[bpf-tail]`, worstcase n=96):

| | mean | p95 | max | per-seed vs legacy |
|---|---|---|---|---|
| legacy | 0.610 | 0.761 | 0.887 | — |
| bpfauto | 0.460 | 0.676 | 0.831 | **better on 75, worse on 16** |

So bpfauto is better on the whole — mean, p95 *and* max all improve on the long
message — yet it is worse on 16 of 96 seeds. The narrowing adds variance: during a
QSB dip the getSNR/inputSnr readings and the one-shot retune can land wrong, and on
a short message (MSG_CQ, 23 chars) there is not enough data to recover from it. For
a shipped default, decoding *worse than the current decoder* on ~1 in 6 realizations
of a realistic hard signal — occasionally much worse on short overs — is a genuine
regression, even with a better mean.

Converting the single-seed gates to multiseed would make the promotion pass, but
that is loosening a check that is catching something real, not an artifact. The
disciplined outcome: **not promoted.** `legacy+bpfauto` stays a strong,
real-audio-validated variant (§29–33) with a documented variance tradeoff; whether
its better-mean-worse-tail profile is worth shipping as the default is a judgment
call for the maintainer, not a gate the campaign can pass on its own terms.
`legacy` remains the default.
