# Profile-Guided Optimization — Phase 11

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-19 |
| Git revision | `496491e` + Phase 11 working tree |
| Machine | [windows-mingw](../machines/windows-mingw.md) |
| Host compiler | GCC 16.1.0 (MinGW-w64, UCRT) |
| Measurement | wall time, `bench/harness/pgo_speedup.py` |
| Corpus | 5 programs in `bench/programs/`, 0.15–0.5 s each |
| Repetitions | minimum of 15 interleaved runs after a discarded warm-up |

**Minimum, not median.** Interference from the rest of the machine only ever
*adds* time, so the fastest run of a set is the closest available estimate of
the program's own cost. Runs alternate between the two binaries so drift over
the measurement hits both equally.

`jitter` is how far the baseline's median sat above its own minimum: the amount
of interference present while measuring.

## 2. Metric PGO-14 — the North Star

> `optiforge bench.of -O2 -o a.out` and
> `optiforge bench.of -O2 --use-profile=bench.prof -o b.out` both produce correct
> executables, and `b.out` is measurably faster on the workload the profile was
> collected from.

| Program | -O2 (ms) | PGO (ms) | speedup | jitter | same output |
|---|---:|---:|---:|---:|---|
| nested_math.of | 151.6 | 143.6 | **5.5%** | 1.4% | yes |
| loop_sum.of | 380.2 | 375.7 | 1.2% | 2.2% | yes |
| biased_branch.of | 503.7 | 501.8 | 0.4% | 0.7% | yes |
| branchy.of | 482.0 | 480.8 | 0.2% | 0.4% | yes |
| recursive_fib.of | 256.4 | 256.6 | −0.1% | 1.0% | yes |

A second run an hour later reproduced every figure within 0.6 points
(`nested_math` 5.5%, `loop_sum` 1.1%, `biased_branch` 0.7%).

| Metric | Target | Measured | Met? |
|---|---|---|---|
| PGO-14 — PGO beats `-O2` on the profiled workload | "measurably faster" | **5.5%** on `nested_math.of`, 1.2% on `loop_sum.of`, both above their noise floor, output identical | ✅ |

**The sentence is true. It is also a modest result, and the modesty is the
finding**, not something to bury: three of five programs show nothing, and §4
says why for each.

## 3. Where the 5.5% comes from

Not asserted — counted. `nested_math.of`'s inner loop at `-O2`:

```asm
.L_kernel_while_cond_4:
    cmpq    %rdi, %rsi              # fused with the branch below
    jge     .L_kernel_while_end_6   # inverted: hot side falls through
.L_kernel_while_body_5:
    ... 12 instructions of arithmetic ...
    jmp     .L_kernel_while_cond_4
```

Fifteen instructions per iteration: twelve of work, two of loop control, one
back-edge jump.

The profile measures the inner loop's trip count at exactly 400 and unrolls by
8. Seven of every eight iterations then lose the back-edge jump:

```
before:  8 × 15                       = 120 instructions per 8 iterations
after:   8 × 12  +  8 × 2  +  1       = 113
                                        5.8% fewer
```

Measured: **5.5%**. The prediction and the measurement agree, which is the only
reason to believe either.

## 4. Where it comes to nothing, and why

| Program | What PGO did | Why it did not show |
|---|---|---|
| `loop_sum.of` | unrolled by 8 | Same mechanism as `nested_math`, but the loop body is longer, so the back-edge jump is a smaller share of it. 1.2% against a 2.2% jitter is at the edge of what this method resolves. |
| `biased_branch.of` | laid the hot path out as fall-through, sank the rare arm and the cold exit | The loop contains an `idiv`, which costs more than every branch around it put together. The layout is correct and visibly better; the benchmark cannot see it. |
| `branchy.of` | **refused** to unroll | Its loop is a call and two adds. See §5. |
| `recursive_fib.of` | nothing but layout | No loops at all. Its PGO win is bounded by what block layout buys, which on a two-block function is nothing. |

`System_design.md` §16.5 predicted this for layout — "worth reporting honestly if
the effect is small on benchmarks of our size" — and it was right. A function
that fits in L1 instruction cache whatever order its blocks are in gains nothing
from being reordered.

## 5. Where PGO lost, and what was done about it

The first version of the unroller had no exception for calls. Measured on
`branchy.of`, whose loop is `acc = acc + classify(i)`:

| | -O2 | PGO | |
|---|---:|---:|---|
| before the guard | 607.0 ms | 609.5 ms | **−0.4%** |
| after the guard | 482.0 ms | 480.8 ms | +0.2% |

What unrolling removes is a back-edge jump. Next to the cost of a call that is
nothing, and the eight copies still grow the code by a factor. The unroller now
refuses any loop whose body contains a call, and the reasoning is in the source
next to the check rather than only here.

*(The absolute times fell between those two measurements because the compare-and-branch
fusion in §6 landed in between. Both rows are internally consistent.)*

## 6. What improved for everyone

Two changes made during this phase help the `-O2` baseline as much as the
profile-guided build, which raises the bar PGO has to clear. Doing them only for
profile-guided builds would have flattered every number above.

**Compare-and-branch fusion.** `if (a > 0)` produced an i1 nothing else read,
which meant `setcc`, `movzbq` and `testq` before the jump could look at it.
Branching on the comparison's own flags removes all three. `sum.of`'s loop went
from nine instructions per iteration to five.

**Fall-through elision.** A jump to the very next block costs nothing to reach,
so it is removed — inverting the condition when the *taken* side is what comes
next.

| | before Phase 11 | after |
|---|---:|---:|
| `sum.of` inner loop, instructions per iteration | 9 | 5 |
| BE-04 memory traffic, graph vs naive allocator | 61.0% | 58.7% |

The BE-04 ratio *fell* because the naive baseline improved: its frame accesses
dropped from 811 to 765 while the graph allocator's stayed at 316. The
allocator did not get worse; the thing it is measured against got better.

## 7. A latent allocator bug the fuzzer found

Adding a profile round-trip to `tests/fuzz_differential.py` — compile
instrumented, run, recompile against the profile — found a **Phase 8** bug on
its first substantial run, at `-O1`, with no profile involved:

```
internal compiler error: register allocation is unsound
  function @main, block 'if.end.17', before copy: %t193 and %t192 are both live in %rbx
```

The interference builder excluded the source of a copy from interfering with its
destination — the textbook move exception. That is sound only when the source
*dies* at the copy. When it lives on, the two really are simultaneous, and the
edge the textbook recovers at the source's next definition never appears here,
because SSA destruction's root copy reads its own location and is treated as
neither a definition nor a use.

The fix is a deletion: a source that dies at the copy is simply not in the live
set, so the exclusion was never doing anything except in the case where it was
wrong. Coalescing counts and spill counts across the corpus are unchanged.

Confirmed against the committed compiler that this predates Phase 11.

## 8. Threats to validity

- Five programs, all synthetic, all written by the person measuring them. The
  language has no arrays, which rules out the loop kernels real benchmark suites
  are built from.
- Wall time on a desktop. The jitter column is honest about that, and the two
  worst programs were measured again on a quiet machine before being reported.
- The profile is collected from **the same workload it is applied to**, which is
  the best case for PGO. How much survives a workload shift is not measured, and
  Phase 12 should.
- `--profile-time` overhead is still not measured separately.
- Cold-code size mode (PGO-10) is implemented and untested for effect. Its gain
  is instruction-cache pressure, which nothing in this corpus is short of.

## 9. Actions

| Action | Metric | Status |
|---|---|---|
| Measure a profile applied to a *different* workload than it came from | PGO-14 | ☐ Phase 12 |
| Benchmarks that are not synthetic, which needs arrays in the language | Q-01 | ☐ Phase 13 |
| Multi-block inlining, so PGO inlining has something to do | PGO-06 | ☐ open — the budget rule works, but the single-block restriction binds first |
| True unrolling with a remainder loop, to drop the per-copy test | PGO-07 | ☐ open |
| Measure whether cold-code size mode moves anything | PGO-10 | ☐ open |
