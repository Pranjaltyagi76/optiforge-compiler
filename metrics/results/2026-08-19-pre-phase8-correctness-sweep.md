# Correctness sweep before Phase 8

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-19 |
| Git revision | `3564476` + correctness-sweep working tree |
| Machine | [windows-mingw](../machines/windows-mingw.md) |
| Method | code review of Phases 3–7, plus a differential fuzzer over `-O0`/`-O1`/`-O2` |
| Corpus | 15 programs from `tests/e2e/` and `examples/`, plus ~400 generated programs |

The fuzzer generates random well-typed programs — nested `if`/`while`, `int`,
`float` and `bool` locals, scope-correct variable references — compiles each at
every optimization level, runs all three binaries and compares stdout and exit
status. `-O0` is the reference. This is the same idea as the `tests/e2e/`
differential suite (QA-05), scaled up: it exercises shapes nobody thought to
write by hand.

## 2. What it found

| # | Where | Severity | Symptom |
|---|---|---|---|
| 1 | `SCCP` — block-based reachability | **miscompile** | a phi kept the value from the first executable edge and ignored the second |
| 2 | `SCCP` — values stuck at `Unknown` | **miscompile** | `bool == bool` made both arms of a branch look dead; live code deleted |
| 3 | `CodeGen::lowerCompare` | **wrong result** | every float comparison against NaN answered the opposite of IEEE-754 |
| 4 | `Liveness` — phi operands | over-approximation | a value was live across the block that defines it; bogus interferences for Phase 8 |
| 5 | `SimplifyCFG` — `condbr %c, X, X` | latent invariant break | folding it dropped both of the block's phi entries instead of one |
| 6 | `insertLoopPreheaders` | latent invariant break | redirected edges without rewriting the header's phis |
| 7 | `destroySSA` — `unordered_map` iteration | non-determinism (NFR-06) | copy emission order, and so temporary names, depended on heap addresses |
| 8 | `ir::Verifier` | verification gap | phi arity was checked only at the end of the pipeline, never by `--verify-each` |

Bugs 1 and 2 are the serious ones. Both are in SCCP, and both share a root
cause worth stating plainly: **an optimistic dataflow analysis is only sound if
"not yet known" can never become permanent.** Bug 1 made a block's phis
permanently un-revisited; bug 2 made a value permanently unfoldable *and*
permanently `Unknown`. In each case the pass then read "unknown" as "cannot
happen" and deleted code that runs.

Bugs 4–8 were found by review, not by the fuzzer, and none of them miscompiles
today. They are recorded because every one of them is load-bearing for Phase 8:
the register allocator consumes liveness (4), runs after the passes that break
phi arity (5, 6, 8), and its output has to be reproducible (7).

## 3. Metrics O-03 and O-04, re-measured

Same method as [the Phase 7 baseline](2026-08-19-phase7-ir-reduction.md):
static IR instruction count via `bench/harness/count_ir.py`. Three new
regression programs joined the corpus, so the totals are not comparable to that
table line for line; the per-program columns are.

| Program | -O0 | -O1 | -O2 | O-04 (-O2 vs -O0) | vs. Phase 7 |
|---|---:|---:|---:|---:|---|
| fib.of | 20 | 14 | 14 | 30.0% | — |
| sum.of | 27 | 15 | 15 | 44.4% | — |
| arithmetic.of | 11 | 8 | 8 | 27.3% | — |
| bool_compare.of | 69 | 16 | 16 | 76.8% | new |
| calling_convention.of | 64 | 26 | 9 | 85.9% | 20 → 9 at -O2 |
| comparisons.of | 17 | 9 | 9 | 47.1% | — |
| control_flow.of | 51 | 34 | 34 | 33.3% | — |
| floats.of | 16 | 13 | 13 | 18.8% | — |
| loop_phi_edges.of | 49 | 33 | 33 | 32.7% | new |
| nan_compare.of | 40 | 26 | 26 | 35.0% | new |
| recursion.of | 64 | 45 | 45 | 29.7% | — |
| scoping.of | 39 | 20 | 20 | 48.7% | — |
| short_circuit.of | 47 | 27 | 27 | 42.6% | — |
| ssa_swap.of | 80 | 46 | 46 | 42.5% | — |
| ssa_uninitialized.of | 18 | 5 | 5 | 72.2% | — |
| **TOTAL** | **612** | **337** | **320** | **47.7%** | |

| Metric | Target | Phase 7 | Now | Met? |
|---|---|---|---|---|
| O-03 — cumulative `-O1` reduction | > 25% | 42.3% | **44.9%** | ✅ |
| O-04 — cumulative `-O2` reduction | > 40% | 46.0% | **47.7%** | ✅ |

The gain is not from the bug fixes themselves. It comes from two things done
alongside them: `simplify-cfg` now merges a block into its only predecessor,
and constant folding evaluates comparisons between booleans. `calling_convention.of`
more than halves because the two together fold a chain of small helpers to
nothing.

## 4. Threats to validity

- The fuzzer only generates programs whose behaviour is defined: divisors are
  forced non-zero, and no expression relies on overflow. Whole classes of bug
  are therefore out of its reach by construction.
- It compares against `-O0`, so a bug shared by every optimization level — in
  the lexer, parser, sema, IR generation or the naive backend — is invisible to
  it. The frontend is covered by the golden suite instead.
- Program shapes are limited to what the generator emits: no recursion, no
  functions other than `main`, no deep call graphs. Inlining is barely exercised.

## 5. Actions

| Action | Metric | Status |
|---|---|---|
| Wire the differential fuzzer into CI as a nightly job | QA-05 | ☐ open |
| Generate multi-function programs so inlining is fuzzed | QA-05 | ☐ open |
| Constant folding does not handle floats | O-04 | ☐ open (carried from Phase 7) |
| Empty blocks with several predecessors are still not threaded | O-04 | ☐ open |
