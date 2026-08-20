# Profile-Guided Optimization

> **Status:** complete for Phase 11. Written against `src/transforms/LoopUnroll.cpp`,
> `src/transforms/Inline.cpp`, `src/backend/Layout.cpp`, `src/backend/RegAlloc.cpp`
> and the pipeline in `src/passes/PassManager.cpp`.

---

## 1. The loop

```
optiforge prog.of -O2 -o baseline                        # what we have to beat
optiforge prog.of -O2 --profile -o instrumented
./instrumented                                           # writes instrumented.prof
optiforge prog.of -O2 --use-profile=instrumented.prof -o guided
```

`--pgo-remarks` explains every decision either build made:

```
loop-unroll: @compute:while.cond.1: trip count 200000 measured; unrolling by 8
inline: @main:while.end.3: cold call site to @triangle; not inlined
layout: 8 block(s) reordered, 11 jump(s) became fall-through
```

**The `-O` levels must match.** ADR-05 puts instrumentation after the
optimization pipeline, so block names describe the optimized CFG. A profile
collected at `-O1` names a different set of blocks; the driver warns.

---

## 2. Each pass, with and without a profile

The "without" column is not a degradation path bolted on afterwards. It is the
pass's normal behaviour, and it is the one every non-PGO build and most of the
test suite exercises.

| Pass | Without a profile | With a profile |
|---|---|---|
| **Inlining** (PGO-06) | callee under 12 instructions, single block | hot call site: budget 250. **Cold call site: not inlined at any size** |
| **Loop unrolling** (PGO-07) | **does nothing at all** | hot loops only, factor from the measured trip count |
| **Register allocation** (PGO-08) | spill cost weighted by 10^loopDepth | weighted by measured block executions |
| **Block layout** (PGO-09) | IR order; fall-through jumps still elided | greedy hot chains, cold blocks sunk to the end |
| **Cold code** (PGO-10) | every function gets the full pipeline | cold functions get the `-O1` pipeline |

### Why unrolling does nothing without a profile

Phase 7 deferred static unrolling deliberately. A static unroller guesses at two
things — is this loop hot, and how many iterations does it run — and pays real
code size for both guesses. With a profile it knows both. Leaving the pass inert
without one also makes the comparison clean: the `-O2` baseline is exactly what
it was before Phase 11 existed.

---

## 3. How unrolling works here

The measured trip count is an **average**, not a bound. A loop entered twice for
one iteration and once for a thousand averages 334, and no single factor is
right for both. So the transformation never assumes a count:

```
    header:  p = phi ...;  c = test(p);   condbr c, body, X
    body:    ...;          c1 = test(p1); condbr c1, body.1, X
    body.1:  ...;          c2 = test(p2); condbr c2, body.2, X
    body.2:  ...;                         br header
    X:       px = phi [p, header], [p1, body], [p2, body.1];  br exit
```

Every copy re-tests the condition, so the loop still runs exactly the iterations
it would have. What disappears is one back-edge jump per copy. Getting the factor
wrong costs code size, never correctness.

`X` is new, and introducing it is what keeps the SSA repair in one place: the
original exit block keeps its single predecessor, and the values escaping the
loop are merged exactly once.

### What it refuses to touch

Each of these is a place the rewrite would otherwise need another case, and a
loop that does not fit is skipped rather than half-transformed:

- more than one body block, or more than one latch;
- more than one exit, or an exit reached from anywhere but the header;
- a value defined inside the loop and read after it, other than a header phi;
- an exit block that already has phis;
- **a body containing a call.** Measured: unrolling `bench/programs/branchy.of`,
  whose loop is a call and two adds, made it 0.9% *slower*. What unrolling saves
  is a jump; next to a call that is nothing, and the copies still grow the code.

### Choosing the factor

```
trip < 4          ->  do not unroll
4 <= trip < 8     ->  2
8 <= trip < 32    ->  4
trip >= 32        ->  8
```
bounded by `bodySize * factor <= 400` instructions.

---

## 4. Block layout

Greedy chains from the entry, appending the hottest unplaced successor, then
starting again at the hottest block left. Zero-count blocks end up last.

Then jumps to the next block are removed, inverting the condition when the
*taken* side is what comes next. `bench/programs/biased_branch.of`, whose loop
takes one arm 999 times in 1000:

```asm
; -O2                              ; with a profile
.L_cond:  jge  .L_end              .L_cond:  jge  .L_end
.L_body:  je   .L_then             .L_body:  je   .L_then
          jmp  .L_else             .L_else:
.L_end:   ...                      .L_end_of_if:
.L_then:  jmp  .L_end_of_if                  jmp  .L_cond
.L_else:                           .L_then:  jmp  .L_end_of_if
.L_end_of_if:  jmp .L_cond         .L_end:   ...
```

The hot path becomes straight-line fall-through and the cold exit block stops
sitting in the middle of the loop.

**This produced no measurable speedup**, and that is worth saying plainly. The
loop in question contains an `idiv`, which costs more than every branch around
it put together, and the whole function fits in L1 instruction cache whatever
order it is in. Layout is correct, visibly better, and invisible at this scale —
System_design.md §16.5 predicted exactly that for benchmarks of this size.

The fall-through elision runs **without** a profile too. It is a straight win on
every build, and doing it only for profile-guided ones would flatter the
comparison.

---

## 5. The fallback is the point

Requirement PGO-11: *a missing, empty, corrupt, or stale profile never produces
an incorrect binary.* Every pass above reaches its profile through
`getCached<ProfileAnalysis>`, and the null it returns when there is none is the
ordinary path.

`tests/run_profile.py` recompiles every profile program four ways — no profile,
missing file, corrupt file, a profile of a different program — and requires
byte-identical output from all of them. `tests/fuzz_differential.py` adds a full
profile round-trip on randomly generated programs, which is the only thing that
seriously exercises the unroller's SSA rewrite on loop shapes nobody wrote by
hand.

### One subtlety worth recording

A supplied profile must **not** be invalidated when the IR changes.
`AnalysisManager` drops cached results whenever a pass reports a change, which
is right for anything derived from the IR and wrong for a profile: nothing a
pass does can change what the program did when it ran. Keeping profiles in a
separate map was not an optimization — before it, the first pass that changed
anything threw the profile away, and every profile-guided decision after that
silently took the no-profile path while looking like it worked.
