# OptiForge — Measurement Methodology

> **Purpose:** how to take a measurement so the resulting number can be trusted, defended, and reproduced.
> **Doc version:** 1.0 · **Created:** 2026-08-18

The headline claim (NFR-10: ≥10% PGO speedup) sits close to the noise floor of a casually-measured benchmark. Sloppy measurement does not just add error — it can manufacture the entire result. This document exists to make that impossible.

---

## 1. The Cardinal Rules

1. **One variable at a time.** PGO vs `-O2` must differ *only* in the profile. Same compiler revision, same machine, same workload, same optimization level, same interleaved session.
2. **Median, never mean.** Runtime distributions are right-skewed: a scheduler hiccup adds time, nothing subtracts it. The mean chases outliers; the median does not.
3. **Report variance alongside every measurement.** A number without a spread is not a measurement.
4. **Interleave configurations, never batch them.** Run `O2, PGO, O2, PGO, …`, not all `O2` then all `PGO`. Machine thermal state drifts over minutes; batching converts that drift into a fake result.
5. **Never compare across machines.** Two numbers from different hardware are not comparable, no matter how similar the specs.
6. **State the noise floor before stating the result.** If run-to-run variance is 4%, a 3% speedup is nothing.

> Rule 4 deserves emphasis: it is the single most common way benchmark results become wrong while looking careful. A CPU that has been at full load for two minutes is slower than a cold one. Batching puts every `-O2` run in the cold window and every PGO run in the hot one, or the reverse — either way the answer is thermal, not compiler.

---

## 2. Benchmark Suite Design

Six programs, each chosen to exercise a *specific* optimization so gains are attributable (metric G-05).

| Benchmark | Shape | Primary PGO decision under test |
|---|---|---|
| `matmul` | Triple-nested loop, predictable trip counts | Unrolling from measured trip counts (G-07) |
| `nbody` | Float-heavy inner loop, invariant subexpressions | LICM + register pressure (G-08) |
| `sieve` | Memory-bound loop, biased branch | Layout + fall-through (G-09) |
| `fib-recursive` | Deep recursion, small hot function | Hot-call-site inlining (G-06) |
| `branch-machine` | State machine, strongly biased branches, cold error paths | Layout + cold-code handling (G-09, G-10) |
| `loop-kernel` | One hot loop, one cold loop with a *higher static* trip count | ★ The showcase: static heuristics guess wrong, profile gets it right |

> **`loop-kernel` is the most important benchmark.** It is constructed so a static optimizer's heuristics point the *wrong* way — a loop that looks hot syntactically but runs twice, next to one that looks minor but runs fifty million times. It isolates exactly what profile data buys and nothing else. Design this one first.

### Rules for benchmarks

- **Fixed before the pass is written**, wherever possible. Writing a benchmark after seeing what a pass does is how you accidentally test your own assumptions.
- **Deterministic.** No wall-clock dependence, no randomness without a fixed seed, no I/O in the timed region.
- **Long enough to dominate startup.** Target 200ms–2s per run. Below 50ms, process startup and the timer's own resolution dominate.
- **Verifiable output.** Each prints a checksum, so a "faster" run that computes garbage is caught immediately.
- **Committed with their expected output**, and included in the differential suite (C-05).

---

## 3. Controlling Measurement Noise

Do these before the first real measurement. Skipping them is the difference between a 3% noise floor and a 15% one.

### 3.1 Machine preparation (Linux / WSL2)

```bash
sudo cpupower frequency-set --governor performance
```

Then, in order of impact:

| Control | Why it matters | How |
|---|---|---|
| **CPU governor → performance** | `ondemand`/`schedutil` ramp frequency *during* your run — early reps are slower than late ones | command above |
| **Disable turbo boost** (optional) | Turbo is thermally dependent, so it drifts across a session | `echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo` |
| **Close everything else** | Browser tabs are the largest single noise source on a laptop | — |
| **Plug in the laptop** | Battery mode aggressively throttles | — |
| **Pin to a core** | Prevents migration between cores with different thermal states | `taskset -c 2 ./bench` |
| **Discard warmup reps** | First rep pays cold i-cache, cold branch predictors, page faults | Run 3 warmups, time the next 10 |
| **Let the machine settle** | Thermal steady state takes ~30s under load | Sleep 30s before the timed section |

WSL2 note: it runs in a lightweight VM, so it carries slightly more variance than bare metal, and the Windows host's activity leaks in. Acceptable for relative comparisons — which is all this project needs — but record it in the machine spec, and be extra strict about Rule 4.

### 3.2 Establishing the noise floor

**Before trusting any result, measure the same binary against itself:**

```bash
python3 bench/harness/run.py --configs O2,O2 --reps 10 --label noise-floor
```

Two identical configurations should report a ~0% difference. Whatever spread you actually see **is your noise floor**, and no result smaller than it is reportable. Record it in every machine spec, and re-measure it whenever the machine or environment changes.

If the noise floor exceeds 3% (metric Q-02), stop and fix the environment. Do not proceed to real measurements — you will not be able to defend anything you find.

---

## 4. Statistical Handling

### Per configuration

- **N ≥ 10 timed repetitions**, plus 3 discarded warmups.
- Report: **median**, **min**, **IQR** (75th − 25th percentile), and **N**.
- Minimum is worth reporting because it approximates the interference-free run.

### Comparing two configurations

```
speedup = (median_baseline / median_treatment) − 1
```

Report a difference as **real** only if:

1. It exceeds the noise floor from §3.2, **and**
2. The IQRs of the two configurations do not overlap, **and**
3. It reproduces across two independent sessions.

Condition 3 catches the case where something changed on the machine mid-session, which the first two cannot detect.

### What not to do

- Do not report a speedup from a single run.
- Do not cherry-pick the best run of each configuration — comparing two minimums exaggerates differences.
- Do not average speedup percentages across benchmarks; take the median of the per-benchmark speedups.
- Do not drop a benchmark because it shows a loss (see `README.md` §7).

---

## 5. Attribution Protocol (metric G-05)

A speedup is not a finding until it is attributed. To attribute a PGO gain:

```
1. Measure  -O2                                    -> baseline
2. Measure  -O2 --use-profile (all PGO on)         -> full gain
3. For each PGO pass P:
       measure -O2 --use-profile --disable-pgo=P   -> gain without P
       contribution(P) = full_gain − gain_without_P
4. Assert: sum of contributions ≈ full gain (within the noise floor)
```

A large unattributed residual means something other than your PGO decisions produced the difference — most often code layout shifting by accident. That is worth knowing and worth reporting.

This requires `--disable-pgo=<pass>` in the driver. Add it when Phase 11 starts; retrofitting it later is painful and you will be tempted to skip the attribution instead.

---

## 6. Validating the Profiler Against Ground Truth

Our profiler's numbers must be independently checked, or every PGO decision rests on an unverified foundation.

### Level 1 — Hand verification (Phase 9, mandatory)

Write a program with counts you can compute on paper:

```
fn main() -> int {
    int i = 0;
    while (i < 100) {     // header executes 101 times, body 100
        if (i < 90) {     // taken 90, not-taken 10
            work();       // called 90 times
        }
        i = i + 1;
    }
    return 0;
}
```

Every counter in the resulting `.prof` must match exactly (metric I-05 = 1.000). Off-by-one on loop headers is the classic failure and this catches it immediately.

### Level 2 — Cross-check against `perf` (Phase 12)

```bash
perf record -g ./bench_O2 && perf report --stdio
```

`perf` samples; we count exactly. Absolute numbers will differ. What must agree is the **ranking** — if `perf` says `compute()` dominates and our profile says `helper()` does, one of them is wrong, and it is almost certainly ours.

This is a free, independent oracle and it is the strongest evidence in the whole evaluation that the profiler works. It is also one of the concrete reasons the Linux/WSL2 target was recommended (`deployment.md` §2).

---

## 7. Reproducibility Record

Every result file records, without exception:

| Field | Why |
|---|---|
| Date and time | Correlates with machine state |
| Git revision (short SHA) | The exact compiler that produced it |
| Machine ID | Links to `machines/<id>.md` |
| Host compiler + version | GCC 13.2 vs Clang 17 changes compiler speed |
| Optimization level | The single most common omission |
| Reps, warmups discarded | Statistical weight |
| Noise floor at measurement time | Whether the result is distinguishable |
| Profile source (for PGO runs) | Which workload the profile came from |
| Anomalies observed | "Laptop was on battery for reps 4–6" saves hours later |

The harness fails if any of these is missing. A result without provenance is not admissible.

---

## 8. Common Traps

| Trap | Symptom | Avoidance |
|---|---|---|
| Batching configurations | Suspiciously clean, large speedup | Interleave (Rule 4) |
| Comparing against GCC as a headline | Demoralizing, and irrelevant | Compare OptiForge PGO vs OptiForge `-O2` |
| Benchmark too short | Variance exceeds the effect | Target 200ms+ per run |
| Dead code eliminated entirely | Impossible speedup, e.g. 400× | Print a checksum of the result |
| Profiling and measuring the same run | Circular — the profile trivially fits | Profile run and timed run are separate |
| Measuring the instrumented binary as the PGO result | PGO looks slower than `-O2` | The PGO binary links `libofrt` only, never `libofprof` |
| Optimization level mismatch between profile and PGO build | Match rate collapses, no gain | Driver enforces it; check `OPTLEVEL` in the header |
| Reading a stale `.prof` from a previous experiment | Inexplicable results | Delete `.prof` before each collection run |
| Thermal throttling mid-session | Later reps uniformly slower | Pin frequency, settle 30s, watch for monotonic drift |

> The "dead code eliminated entirely" trap is worth internalizing. A benchmark whose result is never used is legal to delete in full, and a sufficiently good optimizer will do exactly that — producing a spectacular, meaningless speedup. Printing a checksum makes the computation observably necessary. Expect to hit this once; the checksum turns it from a day of confusion into a moment of recognition.
