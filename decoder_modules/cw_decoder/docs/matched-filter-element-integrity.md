# CW Matched-Filter Element Integrity — design (weak-signal / fading axis)

> **Goal:** decode weak, fading signals the operator copies by ear but the fb
> detector fragments — *without regressing* clean/hand-keyed/contest. The lever is
> **element integrity via matched-filter energy**, not a per-sample state decision
> plus a duration debounce.
>
> Design-first (2026-07-25). No code until the go/no-go experiment (§5) validates
> the core assumption on real data. Ground truth + all prior evidence:
> `cw-baseband-6992455-ground-truth` memory; principles in
> `soft-decoder-staged-bugfix.md` §1 and the `cw-quality-gates` skill.

## 1. The problem, as measured (not assumed)

Real recording `baseband_6992455Hz` (~7.030 MHz, +703 Hz, copyable ~80% by ear:
`D COME OUT ONE TIME / TEML ADT 16C = SO EAL QQX`). The fb path fragments it:
`OUT`(`−−−/..−/−`) → `OITT` (U's dah detached), `ONE`→`ONEE` (false dit), etc.
The strong region `16C=` decodes perfectly; only the **faded** regions fragment.

`[replay-events]` (fb mark/space durations, 1 kHz internal = 1 ms/sample):
- Fragmentation = **sub-dit false splits**: gaps 12-20 ms + fragment-marks 12-32 ms,
  both just clearing fb's `MIN_RUN = 12` ms debounce. A real dit/element-gap ≈ 70 ms.

### Why every duration/state lever failed (all measured, all reverted)
| lever | real | synthetic | verdict |
|---|---|---|---|
| symmetric floor 18 ms | CER 0.74→0.50 ✓ | handkeyed 127→177, contest 86→112 ✗ | overlap |
| contrast-gated floor | no help | zero-reg | contrast overlaps (real 7-9 dB core ≈ handkeyed 6) |
| asymmetric space floor | 0.79 worse | — | leaves fragment-marks |
| transition-prior stickiness | collapses to "S" | — | weak emission ⇒ prior dominates |
| dip-depth (per-sample) | no help, inverted | — | core's narrowed envelope: dips reach floor |

**Root reason (proven at the individual-error level via `[rawerr-cores]`/`[fbbug]`):**
floor-18's new hand-keyed errors are the *same split/insertion class* it fixes on
the weak signal — `W1AW`→`W1AAT` (W split A+T), `599`→`59ON` (9 split O+N),
histogram `_>T` 5→11, `_>E` 0→8. Weak-signal **fade-artifacts** and hand-keyed
**legitimate jitter** occupy the **same sub-dit duration band**. A duration
threshold physically cannot separate them. → the discriminator must be a different
observable.

## 2. The observable that *does* separate them

Duration cannot; **shape/energy relative to the noise floor** can — in principle:
- A **real key-up** (element gap) returns the envelope energy to the **noise floor**.
- A **fade notch inside a mark** dips but the energy stays **above** the floor —
  it is signal-with-QSB, not an off-key.

Per-sample minimum failed on the core (narrowed BPF, noisy — a single sample dips
to the floor by chance). The fix is to integrate: the **matched-filter energy over
the whole gap window** is far more robust to per-sample noise than the min. That is
the untested-on-core assumption the go/no-go experiment (§5) must settle.

## 3. Approach — element-integrity re-gluing (minimal blast radius)

Chosen over the bigger options (§6) as the campaign's add-a-stage pattern.

Keep the fb detector. Add a **post-detector rescoring** pass over its (mark, gap,
mark, …) event stream that **re-glues** a mark a fade chopped, using matched-filter
energy — dit-relative, so it adapts to speed (unlike the fixed 18 ms floor):

**Statistic.** With the envelope `e[t]` and online noise floor `μ_lo`, mark level
`μ_hi` (fb already tracks both):
- Element energy: `E_elem = Σ (e[t] − μ_lo)` over the mark interval (matched to a
  rectangular keyed pulse — boxcar integral = the matched filter for a known-width
  pulse in noise).
- Gap fill-ratio: `R_gap = mean(e[t] − μ_lo) / (μ_hi − μ_lo)` over the gap interval.
  `R_gap ≈ 0` ⇒ returned to noise (real key-up); `R_gap ≫ 0` ⇒ fade notch.

**Re-glue rule.** Merge `mark—gap—mark` into one mark iff **both**:
1. `gap_duration < DIT_FRAC · dit_est` (only sub-dit gaps are candidates — a real
   element gap ≥ 1 dit is never touched), **and**
2. `R_gap > FILL_MIN` (the gap never returned to noise — a fade, not a key-up).

Condition 2 is the new physics. Clean/hand-keyed real gaps return to noise
(`R_gap ≈ 0`) ⇒ **never merged** ⇒ zero regression *by construction*. Only fade
notches (`R_gap` high) are re-glued. `dit_est` comes from the timing layer, making
the window **dit-relative** — the property the fixed floor lacked.

**Where it lives.** The `reDetect` (continuous re-decode) path already re-segments a
buffered envelope under matured θ and already has `μ_lo`/`μ_hi` + the envelope in
hand — the natural host. It runs off the RT hot path enough to afford an O(N) pass,
and it is where `dit_est` is available. Live single-pass stays untouched first;
promote only if the re-decode win holds.

## 4. Zero-regression argument (to be *verified*, not trusted)

- Rule condition 2 is a **no-op** when gaps return to noise. Clean, hand-keyed,
  contest, farnsworth all have gaps that reach the floor (high contrast) ⇒ `R_gap ≈
  0` ⇒ no merge ⇒ byte-identical. This is the structural reason to expect what the
  duration floor could not deliver.
- Dit-relative window (condition 1) means no fixed-ms band to catch jitter.
- Must still be proven on the full `[cont]` ladder + `[rawerr-cores]` + real CER,
  and the live-path/RT untouched until the re-decode win is real.

## 5. GO / NO-GO experiment (do this BEFORE any detector code)

The whole approach rests on one unverified claim on the **core** (not the mismatched
standalone): *do fade-split gaps have high `R_gap` while real gaps sit at `R_gap ≈
0`?* Per-sample min said no on the core; the **integrated** `R_gap` is the real test.

Requires the **core-accurate instrument (task #16):** tap the actual `StagedCore`
fb detector's envelope + `μ_lo`/`μ_hi` + emitted events on the real WAV (a debugLog
hook or a StagedCore diagnostic accessor — the standalone front-end must NOT be used,
it lacks the adaptive `fbBpf`).

Then, on the real signal, for every fb gap, plot `R_gap` bucketed by
`gap_duration/dit_est`:
- **GO:** short gaps (< 1 dit) split into two clusters — high `R_gap` (fade splits,
  the ones to re-glue) vs `R_gap ≈ 0` (real short gaps) — and long gaps sit at
  `R_gap ≈ 0`. Then a `FILL_MIN` threshold exists and Option A is built.
- **NO-GO:** fade-split gaps also sit at `R_gap ≈ 0` on the core (energy genuinely
  returns to noise even in a fade). Then re-gluing cannot be gated on fill-ratio;
  fall back to Option B/C (§6) — soft element scores into the beam, where sequence
  context, not a single gap statistic, resolves the fragment.

Cross-check the same statistic on synthetic hand-keyed to confirm its gaps are
`R_gap ≈ 0` (the zero-regression premise).

## 6. Options not chosen first (kept for the NO-GO branch)

- **B — soft element likelihood into the beam.** Emit per-element dit/dah/noise
  matched-filter likelihoods as `addElement` confidences (the plan doc's Phase 27,
  revived). The beam down-weights low-confidence fragments and the sequence prior
  glues them. More powerful, bigger; the right escalation if a single-gap statistic
  can't separate (needs sequence context).
- **C — matched-filter detector (filter bank + Viterbi over elements).** The proper
  weak-signal decoder; highest ceiling, highest cost/risk. Only if A and B underperform.

## 7. Staged plan (gated, one measurement per step)

1. **Instrument (#16).** Core-accurate fb tap on the real WAV. Verify it reproduces
   the known core decode (`OME OITT ONEE … 16C=`) — proves the tap is faithful.
2. **GO/NO-GO (§5).** `R_gap` vs `dur/dit` on real + hand-keyed. Decide A vs B/C
   *jointly* (per principle §1.2) — do not self-close.
3. **If GO:** build re-gluing in `reDetect` behind a default-off toggle. Gate:
   real CER improves; `[cont]` every regime same-or-better; `[rawerr-cores]`
   hand-keyed ELEM not worse; suite byte-identical; determinism; then consider live.
4. **If NO-GO:** scope Option B against the same gates.

## 7a. FINDING (2026-07-25) — Option A in the cont path is REDUNDANT

Built Option A (re-gluing) in the re-decode path behind `MF_REGLUE`/`MF_FILL`/
`MF_DITFRAC` (default off, byte-identical) + the shared foundation (`IDetector::
noiseLevel/markLevel`, fb `muLo/muHi`, `gapFillLocal`). Measured:

- **`legacy+fb+sel+cont` already decodes the real signal at CER 0.53** (`B COME OUT
  ONE TIGE … 16C= …`) — the cont re-decode's re-segmentation under matured θ ALREADY
  un-fragments (`OUT`, not the plain-`fb+sel` `OITT` at 0.74). The residual errors
  (`TIGE`, `ESSATM`, `HALEX6`) map to the regions the operator rated **<20-30%**
  confidence — genuinely faded, below the copy floor.
- At realistic `MF_FILL`, re-glue merges **nothing** — real signal AND synthetic
  `[cont]` byte-identical (snr3 241, snr4 1044 unchanged). The cont stream has no
  fade-notches left to glue; `reDetect`+`decodeStream` already handle element
  integrity. `MF_DBG` confirms `ev=214->214` (zero merges) and the ratchet picks
  `live` (live ≈ cont ≈ 0.99 conf).

**Conclusion:** element re-gluing in the CONT path is redundant with the existing
re-decode. Two live directions instead:
- **(i) Core selection** — the shippable win. The default `legacy+route` picks the
  fragmenting `select`/`fb+sel` (0.74), NOT `fb+sel+cont` (0.53). Making the weak/
  fading regime route to the cont path recovers the copyable content with no new DSP.
- **(ii) Element integrity in the LIVE path** — fragmentation only survives where the
  cont re-decode does NOT run (plain `fb+sel`, and the default's non-cont branch). If
  a matched filter is to add value it belongs there, not in the cont path.

Foundation + Option-A toggles retained (default-off) for the live-path port. Option
B (soft-into-beam) NOT built in the cont path — it would be equally redundant there.

## 7b. Fully-soft sequence decoding (2026-07-26, the remaining lever)

After 4 pure-DSP levers converged on "fb+sel+cont ≈ ceiling", the ONE thing the
field gold standard (CW Skimmer, "never hard-decide") does that we don't: we
collapse fb's soft mark/space posterior into HARD events at the `MIN_RUN`
debounce, before timing/beam. Fragmentation is a SEGMENTATION error (a dah split
into dit-gap-dit) frozen at that hard-decision point; the beam only softens
dit-vs-dah, never the segmentation.

**Design — soft-segmentation beam (`SoftSeqDecoder`, `soft_seq_decoder.h`):** a
trellis whose paths carry `{morse-tree node, prob, pendingMarkMs, text}`. Walk the
(mark,gap) stream:
- MARK m: every path accumulates `pendingMark += m`.
- GAP g (dit-relative): fork each path by `pReal = P(real boundary | g, dit)`:
  - **SPLIT** (weight `pReal`): close `pendingMark` as an element (dit if
    `<2·dit` else dah), advance the tree node; if g is a char/word gap emit
    `tree[node].character` into the path text and reset to root (+space if word);
    reset `pendingMark=0`.
  - **MERGE** (weight `1-pReal`): the gap was a spurious sub-dit notch — fold it
    into the pending mark (`pendingMark += g`), keep node/text.
  Prune to top-K by prob. The valid-Morse tree is the ONLY prior (structural, not
  linguistic): a split that yields an invalid element run dies; the merge that
  recovers the true dah survives. This is the soft version of the dip-depth idea
  that failed as a HARD gate — the sequence prior resolves what a per-gap
  threshold could not.
- `pReal(g,dit)`: ~0 for sub-dit notches (12-20 ms splits), rising through ~1·dit.
  Only sub-~1.5·dit gaps fork; clear char/word gaps (>2·dit) are hard boundaries.

**No vocabulary:** tie-break uses only the existing `letterPrior` (already shipped
in the beam), and the beam prior is Morse-code VALIDITY, not word/phrase models.

**Increments:** (1) `SoftSeqDecoder` self-contained (own tree copy → default
byte-identical), wired as the CONT candidate in a new core `legacy+fb+soft`;
measure real CER + `[cont]` (fragmenting snr-noise3/4 = headroom) + full gate.
(2) soft dit/dah fork (secondary). (3) soft P(realGap) from dip-depth (`_reEnv`),
not just duration. (4) if it wins, promote / feed the router. Gate every step:
default byte-identical, `[cont]` every regime same-or-better, real same-or-better.

## 8. Constraints (unchanged, non-negotiable)

Never loosen a gate. Worse on any profile is a regression (raw `[cont]` ladder is a
gate too). Test after each stage, one change at a time. No allocation added to
`process()`; validate default-core changes on the live SDR. Word-correction stays
OFF under test. Clamps are prod-only, never a test crutch. Measure — don't code-read,
don't trust the standalone instrument over the core.
