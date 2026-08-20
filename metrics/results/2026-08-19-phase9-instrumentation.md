# Instrumentation Overhead — Phase 9

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-19 |
| Git revision | `4af43d5` + Phase 9 working tree |
| Machine | [windows-mingw](../machines/windows-mingw.md) |
| Host compiler | GCC 16.1.0 (MinGW-w64, UCRT) |
| Measurement | wall time, `bench/harness/profile_overhead.py` |
| Corpus | 3 programs in `bench/programs/`, each ~0.3–0.7 s of work |
| Repetitions | 9 timed runs after 1 discarded warm-up, median reported |

Unlike the Phase 7 and Phase 8 tables this is a **timing**, so the constraints
in [`methodology.md`](../methodology.md) §3 do apply: warm-up, repetitions,
median rather than mean, and a stated noise floor.

## 2. Metric PROF-13 — instrumentation overhead at `-O2`

`spread` is `(max − min) / median` over the *uninstrumented* runs. It is the
noise floor: an overhead smaller than it is not a measurement.

| Program | plain (ms) | instrumented (ms) | overhead | spread |
|---|---:|---:|---:|---:|
| branchy.of | 608.7 | 604.2 | −0.7% | 2.9% |
| loop_sum.of | 691.9 | 781.2 | **12.9%** | 4.4% |
| recursive_fib.of | 324.8 | 342.2 | 5.4% | 6.5% |

| Metric | Target | Measured | Met? |
|---|---|---|---|
| PROF-13 — instrumentation overhead | under 40% | **12.9%** worst case | ✅ |

Only `loop_sum.of` produces a result clearly above its own noise. `branchy.of`
at −0.7% against a 2.9% spread means *no measurable overhead*, and reporting it
as a speedup would be reading noise as signal. `recursive_fib.of` at 5.4%
against 6.5% is borderline and should be read as "small, probably real".

### Why it is this cheap

One `incq __ofprof_counters+8n(%rip)` per basic block. No call, no register
saved, no spill: the counter array is static, so the increment needs no operand
and touches nothing the register allocator cares about. `loop_sum.of` is the
worst case for exactly the reason it should be — its loop body is three
instructions, so one added increment is a third more work in the hot path.

The first measurement of this table was useless and worth recording as a
lesson. The benchmark programs ran in 75 ms, of which essentially all was
Windows process creation, and the spread came out at **840%** — individual runs
varying by eight times their own median. The numbers it produced (1.4%, −3.5%,
−4.8%) looked plausible and meant nothing. Scaling the programs to ~0.5 s and
discarding a warm-up run brought the spread to 3–6%, and only then was there a
signal to report.

## 3. Exit criterion — counts are hand-verifiable

The roadmap asks that the counts on a small loop program be checkable by hand.
`tests/pgo/loop_counts.of` is that program, and its expected counts are written
into the source as assertions before the compiler is run:

```
FUNCTION sum 1
BLOCK sum entry 1
BLOCK sum while.cond.1 5      <- 4 iterations + the check that fails
BLOCK sum while.body.2 4
BLOCK sum while.end.3 1
BRANCH sum while.cond.1 taken=4 not_taken=1
LOOP sum while.cond.1 entries=1 iterations=4
```

`tests/pgo/timing.of` is the same idea on recursion: `fib(6)` makes 25 calls by
`C(n) = 1 + C(n−1) + C(n−2)`, of which 13 hit the base case — which is `fib(7)`
— and 12 recurse. The profile says 25, 13 and 12.

## 4. What is counted, and what that costs

One counter per basic block, and nothing else. `FUNCTION`, `BRANCH` and `LOOP`
records are all derived from block counters by the compiler, which writes the
derivation into a static table the runtime walks.

| Alternative | Cost |
|---|---|
| Four counter kinds, as the roadmap sketched | 4× the hot-path increments, and four numbers that can disagree with each other |
| One counter per block, derived records | 1× increments, and the four numbers are arithmetically consistent by construction |

`BRANCH` is derivable only because instrumentation splits critical edges first:
afterwards each successor of a conditional branch has exactly one predecessor,
so its block counter *is* that edge's count. The instrumented build therefore
contains `crit.edge.N` blocks the ordinary build does not — that is where the
edge counts live, and Phase 10's name-based matching will not find them in the
optimized CFG. Documented rather than papered over.

## 5. Threats to validity

- Three programs, all synthetic, all chosen to be *bad* cases for
  instrumentation — tight loops and heavy call traffic. Real programs with more
  work per block will show less overhead, not more, so this is the right
  direction to be wrong in.
- Wall time on a desktop, with a 3–6% noise floor. A 5% result is at the edge of
  what this method can resolve.
- `--profile-time` is **not** in these numbers. It puts a real call at every
  function entry and exit, which is why it is opt-in and separately flagged; its
  overhead has not been measured.
- No independent oracle. ADR-10 costs us `perf`, so these are OptiForge measured
  against itself, which is the comparison `metrics/README.md` §7 says is the
  meaningful one anyway.

## 6. Actions

| Action | Metric | Status |
|---|---|---|
| Measure `--profile-time` overhead separately | PROF-06, PROF-13 | ☐ open |
| Merge several runs into one profile | PROF-14 | ☐ open, priority C |
| Ball–Larus edge profiling, which needs far fewer counters | PROF-13 | ☐ open, Phase 13 |
| A benchmark suite that is not synthetic | Q-01 | ☐ Phase 12 |
