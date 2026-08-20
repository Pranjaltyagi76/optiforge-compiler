# Profile Portability — what survives a change of workload?

> Metric **G-13**, descriptive (no target). The action Phase 11 §9 left open:
> *"Measure a profile applied to a different workload than it came from."*
> Numbers from [`../raw/2026-08-20-dd51bdf/published/pgo-portability.json`](../raw/2026-08-20-dd51bdf/published/pgo-portability.json),
> produced by `bench/harness/portability.py`.

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-20 |
| Git revision | `dd51bdf` + the Phase 12 working tree |
| Machine | [`windows-mingw`](../machines/windows-mingw.md) |
| Noise floor | **1.0%** |
| Reps / warm-ups discarded | 12 / 3, interleaved across the three builds of each program |
| Corpus | the three workload pairs in `bench/workloads/` |

## 2. The question, and how it is asked

Every speedup in [the benchmark run](2026-08-20-phase12-benchmarks.md) was
measured with the profile applied to *the very workload it was collected from*.
That is the best case for PGO, and it is not the case anyone deploys: real
builds profile something representative and then run on whatever arrives.

Each program has a **workload B** variant in `bench/workloads/` that changes only
`main`. Every function the profile describes is character-for-character the one
it was collected from, so the source hash differs — the compiler says so — and
matching falls back to names, which all still resolve at 100%. That isolates
*the workload changed* from *the code changed*, which would otherwise be two
variables at once.

Three builds of each B program are timed against each other:

| Build | Meaning |
|---|---|
| `baseline` | B at `-O2`, no profile |
| `matched` | B compiled against **B's own** profile — the upper bound |
| `crossed` | B compiled against **A's** profile — the realistic case |

| Program | Workload A | Workload B |
|---|---|---|
| `nested_math` | inner loop 400 trips × 600,000 entries | inner loop **3** trips × 80,000,000 entries (same 240M inner iterations) |
| `loop_kernel` | flat loop 350,000,000; nest 400 | **swapped** — nest 350,000,000 inner; flat loop 20 |
| `branch_machine` | each transition arm taken ~2% of the time | each transition arm taken **100%** of the time |

## 3. The table

| Workload B | matched (B's own profile) | crossed (A's profile) | retained |
|---|---:|---:|---:|
| `nested_math_b.of` | −0.0% | **+10.9%** | n/a — see §4 |
| `loop_kernel_b.of` | +1.9% | −1.8% | −90% |
| `branch_machine_b.of` | −4.7% | +0.6% | −12% |

**Not one of the three rows says what the metric was expecting to find.** The
expected shape was "the matched profile helps, the crossed profile helps less".
What actually happened is that on two of three programs the *correct* profile
was worse than no profile at all, and on the third the *wrong* profile beat the
right one by eleven points. Each has a specific cause, and none of them is that
stale profiles are good.

## 4. `nested_math_b`: the wrong profile gives the right answer, by luck

The decision, read straight off `--pgo-remarks`:

```
crossed (A's profile):  loop-unroll: @kernel:while.cond.4: trip count 400 measured; unrolling by 8
matched (B's profile):  loop-unroll: @kernel:while.cond.4: trip count 3 is too low to be worth unrolling
```

B's inner loop runs three times. A's profile does not know that, says 400, and
unrolls by 8. Every copy re-tests the exit condition, so this is correct — and
it is **+10.9% faster** than the build that measured the trip count accurately
and declined.

The reason is that a 3-iteration loop is nearly all loop control. Unrolling
converts it into straight-line code with early exits and removes the overhead
that dominates it. The unroller's minimum-trip-count guard is protecting against
a cost that, on a body this small, does not exist.

This is a **finding about the unroller, not about profiles**. The right profile
led to the wrong decision because the decision rule is wrong; the wrong profile
happened to route around it. It would be a serious misreading to conclude that
stale profiles are desirable — the next program in this table shows what
normally happens. The correct conclusion is that the guard costs 10.9% on short
inner loops, and it is filed as an action.

Retention is reported as `n/a` because the matched gain (−0.0%) is inside the
noise floor: a ratio to a denominator of zero is not a number.

## 5. `loop_kernel_b`: the wrong profile mis-targets exactly as predicted

```
crossed (A's profile):  loop-unroll: @kernel:while.cond.7: trip count 350000000 measured; unrolling by 8
                        loop-unroll: @kernel:while.cond.4: not hot; left alone
```

In workload B the two loops have swapped roles: `while.cond.7` — the flat loop
A's profile calls hot — now runs 20 times, and the nest A's profile dismisses
now carries 350,000,000 inner iterations. The crossed build unrolls the cold
loop by 8 and leaves the hot one alone.

Cost: +1.9% becomes −1.8%, a swing of 3.7 points. **This is the honest answer to
G-13** and the one the metric was written to obtain: a profile applied to a
workload it does not describe does not merely stop helping, it actively points
the optimizer at the wrong code.

## 6. `branch_machine_b`: a correct profile with nothing to exploit

The surprise in the table: B compiled against **its own** profile is **4.7%
slower** than B with no profile.

The profile is not wrong. Every branch it reports is 100% one-sided, and it is
right about all of them. The problem is which branches it can see. In workload B
the machine cycles `0 → 1 → 2 → 0` on every call, so the *state dispatch* chain
in `step` — `if (state == 0)`, `if (state == 1)`, `if (state == 2)` — is taken in
rotation, roughly a third each. There is no dominant path. Layout commits to one
anyway, and is wrong about two calls in three.

Compare workload A, where the machine sits in one state 98% of the time, one
path dominates, and the identical layout pass is worth **+16.0%**. Same code,
same pass, same profile format; the only difference is whether the program's
behaviour is *concentrated*.

That is the general lesson of this page, and it is worth more than the numbers:

> **Profile-guided layout pays in proportion to how concentrated the hot path
> is, not to how accurately the profile was measured.** A perfectly accurate
> profile of behaviour that is spread evenly across three paths gives the
> optimizer nothing to exploit, and committing to one of them costs real time.

The crossed build's +0.6% is inside the noise floor, so the fair reading is that
A's profile is *neutral* here rather than better — it happens to produce a layout
that does not commit as hard.

## 7. What G-13 is worth saying

Three programs is too few to put a number on portability, and the metric is
descriptive for that reason. What the three do establish:

1. **A mismatched profile can cost more than having no profile** — 3.7 points on
   `loop_kernel_b`. Profile-guided builds are not a free bet.
2. **Cross-workload behaviour is dominated by whichever decision the profile
   drives hardest.** For unrolling, a wrong trip count is directly harmful. For
   layout, a wrong bias is mostly neutral, because layout's downside is a
   mispredicted fall-through rather than eight copies of the wrong loop.
3. **The staleness machinery behaved exactly as designed.** All three crossed
   builds warned that the source hash did not match, fell back to name matching,
   reported 100% of functions matched, and produced correct output. PGO-11 and
   PGO-12 hold up under the case they were written for.

## 8. Threats to validity

- Three pairs, and each B variant was written by the person who knew what A's
  profile would say. These are constructed worst cases, not a sample of anything.
- The B variants differ from A only in `main`'s constants. A real workload shift
  usually also changes which *functions* run, which name matching would handle
  differently, and which is not tested here.
- `branch_machine_b`'s −4.7% is on the program with the corpus's worst measured
  spread (see the [machine spec](../machines/windows-mingw.md)); it is well
  outside the 1.0% floor but should be quoted as "about 5%".

## 9. Actions

| Action | Metric | Status |
|---|---|---|
| Reconsider the unroller's minimum trip count — declining a 3-trip loop costs 10.9% | G-07 | ☐ open |
| Layout could decline to commit when no path dominates, instead of always taking the most frequent | G-09 | ☐ open |
| Repeat G-13 with a variant that changes which functions run, not just their counts | G-13 | ☐ open |
