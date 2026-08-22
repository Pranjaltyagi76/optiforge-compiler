# Machine Spec — `windows-mingw`

| Field | Value |
|---|---|
| **Machine ID** | `windows-mingw` |
| Recorded on | 2026-08-19 |
| Hardware completed | 2026-08-20, before the Phase 12 timing run |

## Hardware

| Field | Value |
|---|---|
| Form factor | Laptop — ASUS TUF Gaming A15 (FA506NCQ) |
| CPU | AMD Ryzen 7 170 with Radeon Graphics |
| Cores / threads | 8 physical / 16 logical |
| Base clock | 3.20 GHz (boost is enabled and not pinned — see Noise Floor) |
| L2 / L3 cache | 4 MB / 16 MB |
| RAM | 15.3 GB |
| Power during measurement | On AC, battery present and charged |

## Software

| Field | Value |
|---|---|
| OS | Windows 11 Home Single Language, build 26200 |
| Host C++ compiler | GCC 16.1.0 (MinGW-w64, UCRT, POSIX threads, SEH) |
| Assembler / linker | GNU as / ld via the MinGW gcc driver |
| CMake / Ninja / Python | 4.3.3 / 1.13.2 / 3.14.6 |
| Target | x86-64 Windows, Microsoft x64 ABI (ADR-10) |
| Windows power plan | **Balanced** (`381b4222-…`) — see below |

## ★ Noise Floor

**1.0%**, measured 2026-08-20 per `methodology.md` §3.2: the same `-O2`
configuration built twice and timed against itself, interleaved, ten
repetitions after three discarded warm-ups, across all eight benchmark
programs.

| Measured on | Noise floor | Verdict |
|---|---|---|
| 2026-08-20 | **1.0%** | ✅ under the 3% Q-02 gate |

Evidence: [`../results/2026-08-20-noise-floor.md`](../results/2026-08-20-noise-floor.md).

Per-program same-configuration difference:

| Program | Difference | Program | Difference |
|---|---:|---|---:|
| `biased_branch.of` | 1.00% | `loop_sum.of` | 0.30% |
| `branch_machine.of` | 0.48% | `nbody.of` | 0.46% |
| `branchy.of` | 0.32% | `nested_math.of` | 0.72% |
| `loop_kernel.of` | 0.36% | `recursive_fib.of` | 0.49% |

**Nothing below 1.0% is reportable as a result on this machine.**

### Process startup offset

**61 ms minimum, 72 ms median**, measured 2026-08-21 over thirty runs of a
program whose whole body is one `print_int`. Every timing this project reports
includes it.

It is shared by both sides of any comparison, so it cannot create a speedup —
but it **dilutes every percentage**, always downward. See `methodology.md` §4.
Benchmarks are sized well above it for this reason.

### The floor is not uniform, and it moves

**1.0% is the floor measured on a quiet machine, and it does not hold for every
program or every session.** Two later observations, both worth carrying:

- **`loop_kernel.of` reached 12% IQR/median** during the Phase 13 session, from
  2.8% earlier the same day. At that spread the median of 25 samples carries
  roughly 3% of standard error, and two **byte-identical** binaries measured
  5.4 percentage points apart in one attribution run. Differences under about
  5% on that program are not resolvable when the machine is in that state.
- The cause is background activity this project does not control: `audiodg`
  and the Radeon service were both consuming CPU, and the machine idled at 20%.

**What follows.** A per-program floor has to be re-measured in the same session
as any result it gates, rather than assumed from this file. `--configs O2,O2`
does exactly that and costs one extra run.

### Two caveats that belong with the number

**The power plan is Balanced, not High Performance.** `methodology.md` §3.1
asks for a fixed-frequency governor, and Windows' Balanced plan lets the clock
move during a run. This was left alone rather than changed: the plan is a
machine-wide setting, and the measured floor came out under the gate anyway.
The cost is visible and is recorded rather than argued away — see the next
point.

**`branch_machine.of` has a spread the gate does not like.** Its IQR/median is
**5.2%** in both halves of the noise-floor run, against a 3% target (Q-02),
while its same-configuration *median* difference is a well-behaved 0.48%. The
samples are bimodal — roughly 330 ms or roughly 351 ms, with both halves of the
run showing the same pattern at the same time — which is frequency drift over
the session rather than anything the program does. Interleaving is what keeps
it from becoming a fake result: the drift lands on both configurations equally.
The consequence is stated plainly wherever the program is reported: **for
`branch_machine.of` alone, treat differences under about 5% as unresolved.**

## Notes

- `perf` is unavailable on this target (ADR-10), so the Level 2 profiler
  cross-check in `methodology.md` §6 cannot run. Level 1 hand verification of
  counter accuracy carries that weight, in Phase 9.
- Three `libstdc++-6.dll` copies sit on PATH; the build links the runtime
  statically to stay immune to load order. See `deployment.md` §3.4.
