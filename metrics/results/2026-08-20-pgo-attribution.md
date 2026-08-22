# PGO Attribution — which decision produced the speedup?

> Metric **G-05**. `methodology.md` §5, run literally.
> Numbers from [`../raw/2026-08-20-dd51bdf/published/pgo-attribution.json`](../raw/2026-08-20-dd51bdf/published/pgo-attribution.json),
> produced by `bench/harness/attribute.py`.

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-20 |
| Git revision | `dd51bdf` + the Phase 12 working tree |
| Machine | [`windows-mingw`](../machines/windows-mingw.md) |
| Noise floor | **1.0%** |
| Reps / warm-ups discarded | 12 / 3, interleaved across all seven variants of each program |
| Corpus | the 8 programs in `bench/programs` |

## 2. Why this table exists

A speedup nobody can name a cause for is as likely to be an accidental
code-layout shift as anything the profile bought. G-05 exists so that "PGO is
16% faster" can become "PGO is 16% faster **and 16 of those points are block
layout**", which is a claim that can be checked, argued with, and built on.

The method is subtraction. Build with every profile-guided decision on; build
again with exactly one switched off; the difference is that decision's
contribution. `--disable-pgo=<decision>` was added for this, and a disabled
decision takes its **no-profile path** — the same code an ordinary `-O2` build
runs — so the two builds differ in one decision and nothing else.

## 3. The table

Each cell is that decision's contribution in percentage points of the full gain.
Anything under ±1.0% is inside the noise floor and means nothing on its own.

| Program | full PGO | inline | unroll | regalloc | layout | cold-size | residual |
|---|---:|---:|---:|---:|---:|---:|---:|
| `branch_machine.of` | **+16.0%** | +0.1% | −0.1% | +0.9% | **+16.0%** | −0.1% | −0.8% |
| `nested_math.of` | +4.1% | +0.2% | **+5.0%** | −0.3% | −0.2% | +1.1% | −1.5% |
| `loop_sum.of` | +2.3% | −0.2% | **+2.6%** | +0.5% | −0.0% | +0.4% | −1.0% |
| `biased_branch.of` | +0.2% | +0.2% | +0.5% | −0.1% | +0.5% | −0.2% | −0.6% |
| `recursive_fib.of` | −0.1% | −0.1% | +0.2% | +0.1% | −0.1% | +0.1% | −0.3% |
| `branchy.of` | −0.5% | +0.2% | −0.4% | −0.0% | −0.6% | +0.5% | −0.2% |
| `nbody.of` | −0.6% | +0.7% | −0.4% | **−1.5%** | +0.8% | +0.1% | −0.3% |
| `loop_kernel.of` | **−7.1%** | −0.4% | **−6.8%** | −0.6% | −0.7% | −0.1% | +1.5% |

Bold entries are the ones outside the noise floor.

## 4. What it says

**Two decisions do all the work, and each on exactly one program shape.**

- **Block layout** is worth +16.0 points on `branch_machine.of` and is inside
  the noise everywhere else. It pays when a function has one dominant path and
  several rare ones, and `branch_machine.of` is the only program in the corpus
  built that way.
- **Unrolling** is worth +5.0 on `nested_math.of` and +2.6 on `loop_sum.of`, and
  **−6.8 on `loop_kernel.of`**. It is simultaneously the largest positive and
  the largest negative contribution in the table.

**Three decisions never register.** `inline`, `cold-size` and — except on
`nbody.of` — `regalloc` stay inside ±1.0% on every program:

- **Inlining** is bounded by the single-block-callee restriction inherited from
  Phase 7, not by the profile-driven budget. The budget rule works; there is
  almost nothing it is allowed to apply to. `branch_machine.of` shows the
  decision firing correctly — `cold call site to @report; not inlined` — and
  worth +0.1%, because a call taken zero times costs nothing whether it is
  inlined or not.
- **Cold-code size mode** has the same problem from the other end. `report` in
  `branch_machine.of` is the corpus's only genuinely cold function, and shrinking
  code that never executes buys instruction-cache pressure this corpus does not
  suffer from. Phase 11 recorded it as "implemented and untested for effect";
  it is now tested, and the effect is −0.1%.
- **Profile-weighted spill costs** register once, negatively (see below).

**The one place register allocation shows up, it hurts.** `nbody.of` loses 1.5
points to `regalloc`. Its `simulate` is a single loop holding 35 values live
against 10 allocatable XMM registers, so the profile's honest answer — every block in this
function is equally hot — is true and useless. It replaces a loop-depth ranking
that at least separated the entry block from the body with a flat one that does
not, and the allocator's tie-breaking gets worse. The showcase benchmark
`loop_kernel.of` was written specifically to make this decision pay, and its
regalloc contribution is −0.6%, inside the noise: the unroller's damage
(−6.8) swamps whatever the allocator was doing.

## 5. Where the method runs out

**The residual is only meaningful when the gain is.** G-05's target is ">80%
attributed", and it is met on the one program where the question is worth
asking: `branch_machine.of` attributes 16.7 of a 16.0-point gain, a residual of
−0.8% or 5% of the total. On `nested_math.of` (residual −1.5 of +4.1) and
`loop_sum.of` (−1.0 of +2.3) the residual is a *larger share* — but the
subtraction combines seven medians each carrying about 1% of noise, so a residual
of 1–1.5 points is what this method produces at these effect sizes even when the
attribution is perfect. **Below roughly a 5% gain, this technique cannot
attribute to better than a point or so, and that is a property of the method,
not a finding about the compiler.**

`loop_kernel.of`'s residual is **+1.5%**, and it is the one residual that is
probably real rather than noise: the contributions sum to −8.6% against a
measured −7.1%. Unrolling and register allocation interact — the eight copies
raise the live set from 40 coloured units to 123 — so disabling either one alone
removes part of a cost the two share, and their separately-measured
contributions double-count it. That is exactly the interaction the residual is
there to reveal, and it is why the harness prints the residual rather than
distributing it across the decisions.

## 6. Actions

| Action | Metric | Status |
|---|---|---|
| Unroller cost model — the −6.8 on `loop_kernel.of` is the single largest correctable loss in the project | G-07 | ☐ open |
| Multi-block inlining, so the PGO inline budget has something to apply to | G-06 | ☑ **Done in Phase 13.** The inliner now clones callees with branches, phis, loops and several returns, merging the returns with a phi at the call site. `branchy.of`'s `classify` and `opt_pipeline`'s `work` -- a callee containing a whole loop -- now inline. Still refused: a callee containing a call (so a call-graph cycle cannot expand forever) or an alloca. |
| Profile-weighted spill costs need a tie-break for functions that are one flat loop | G-08 | ☐ open |
| Cold-code size mode is measured at −0.1%; keep or drop it on evidence rather than leaving it unexamined | G-10 | ☐ open |
