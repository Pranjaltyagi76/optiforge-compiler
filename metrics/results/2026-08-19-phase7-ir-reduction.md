# Optimization Effectiveness — Phase 7 baseline

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-19 |
| Git revision | `fbea12f` + Phase 7 working tree |
| Machine | [windows-mingw](../machines/windows-mingw.md) |
| Host compiler | GCC 16.1.0 (MinGW-w64, UCRT) |
| Measurement | static IR instruction count via `bench/harness/count_ir.py` |
| Corpus | 12 programs from `tests/e2e/` and `examples/` |

This is a **static count, not a timing**. It needs no noise floor and no
repetitions, so the constraints in `methodology.md` §3 do not apply. Runtime
measurement (metric Q-01) waits for the benchmark suite in Phase 12.

## 2. Metrics O-03 and O-04

| Program | -O0 | -O1 | -O2 | O-04 (-O2 vs -O0) |
|---|---:|---:|---:|---:|
| fib.of | 20 | 14 | 14 | 30.0% |
| sum.of | 27 | 15 | 15 | 44.4% |
| arithmetic.of | 11 | 8 | 8 | 27.3% |
| calling_convention.of | 64 | 26 | 20 | 68.8% |
| comparisons.of | 17 | 9 | 9 | 47.1% |
| control_flow.of | 51 | 34 | 34 | 33.3% |
| floats.of | 16 | 13 | 13 | 18.8% |
| recursion.of | 64 | 45 | 45 | 29.7% |
| scoping.of | 39 | 20 | 20 | 48.7% |
| short_circuit.of | 47 | 27 | 27 | 42.6% |
| ssa_swap.of | 80 | 46 | 46 | 42.5% |
| ssa_uninitialized.of | 18 | 5 | 5 | 72.2% |
| **TOTAL** | **454** | **262** | **245** | **46.0%** |

| Metric | Target | Measured | Met? |
|---|---|---|---|
| O-03 — cumulative `-O1` reduction | > 25% | **42.3%** | ✅ |
| O-04 — cumulative `-O2` reduction | > 40% | **46.0%** | ✅ |

## 3. Analysis

Most of the reduction is **mem2reg**, not the scalar passes. At `-O0` every
local is an alloca with a load and a store around each use; promoting them
removes roughly two instructions for each one that survives. That is why `-O1`
already reaches 42% — the passes that follow are working on IR a third smaller
than what they would otherwise see. Crediting the reduction to constant folding
or DCE would misread this table.

`ssa_uninitialized.of` at 72% is the extreme and an unrepresentative one: it is
almost entirely declarations, so nearly all of it is memory traffic that
promotion deletes outright.

`floats.of` at 18.8% is the weakest, and for a real reason rather than a
property of the program: **constant folding is implemented for integers only**,
so nothing collapses the literal float expressions. Recorded as an action below.

### `-O2` was worse than `-O1` until dead-function elimination

The first measurement had `-O2` at **273** against `-O1`'s **262** — the higher
optimization level producing *more* code. Inlining copies a callee's body into
its caller, and nothing removed the original: no function-scoped pass can see
that a function has stopped being called. A module-level `removeUnusedFunctions`
took `-O2` to 245, and `calling_convention.of` from 37 to 20.

Worth carrying into Phase 12: **inlining trades size for speed, so static
instruction count is the wrong metric to judge it by.** Runtime is the one that
matters, and this table cannot show it.

## 4. Threats to validity

- Twelve small programs, none longer than about eighty lines. These numbers say
  what these passes do to *this* code, not to code in general.
- A static count says nothing about speed. An instruction removed from a loop
  body is worth far more than one removed from an entry block, and this table
  weights them identically.
- No comparison against another compiler, deliberately. The meaningful
  comparison is OptiForge against itself (`metrics/README.md` §7).

## 5. Actions

| Action | Metric | Status |
|---|---|---|
| Constant folding does not handle floats | O-04 | ☐ open |
| Runtime measurement needs the benchmark suite | Q-01 | ☐ Phase 12 |
| Per-pass attribution (which pass removed what) | O-01, O-02 | ☐ open — PassManager already tallies runs and changes |
