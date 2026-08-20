# Benchmarking

How to take a measurement from this project that someone else could defend.

The *rules* — why interleave, why median, why a noise floor — are in
[`../metrics/methodology.md`](../metrics/methodology.md), and every metric's
formula and target is in
[`../metrics/metric-catalog.md`](../metrics/metric-catalog.md). This file is the
operational half: which script to run, what it produces, and what it refuses to
do.

---

## 1. Before the first run

**A machine specification must exist.** Every harness here refuses to start
without `metrics/machines/<id>.md`, and this is not a formality: a runtime
number with no hardware behind it cannot be reproduced or compared, so it is not
evidence. Copy `metrics/machines/TEMPLATE.md` and fill it in.

**The noise floor must be measured before any result is believed.** Run the
harness with the same configuration twice:

```bash
python3 bench/harness/run.py --optiforge build-release/bin/optiforge.exe --machine <id> --configs O2,O2 --label noise-floor
```

Two identical builds should differ by nothing. Whatever difference you actually
see **is** the floor, and no result smaller than it may be reported. Take the
worst program's figure, not the average — the least trustworthy measurement is
the one that should set the bar. Record it in the machine spec.

On `windows-mingw` the floor is **1.0%**.

---

## 2. The main harness

```bash
python3 bench/harness/run.py \
    --optiforge build-release/bin/optiforge.exe \
    --machine windows-mingw \
    --label phase12-benchmarks \
    --configs O0,O1,O2,INST,PGO \
    --reps 15 --warmups 3 --noise-floor 1.0
```

For each program in `bench/programs/` it builds one binary per configuration,
**checks that they all print the same bytes**, times them interleaved, and
collects everything static that needs no timing at all.

| Configuration | What it is |
|---|---|
| `O0` `O1` `O2` | plain builds at that level |
| `INST` | the instrumented binary, timed so I-01 comes from the same session as everything else |
| `PGO` | the whole loop: instrument, run, rebuild with `--use-profile=` |

It writes two files, and the split matters:

| Path | Contents | Edited by hand? |
|---|---|---|
| `metrics/raw/<date>-<sha>/<label>.json` | every sample, immutable | never |
| `metrics/results/<date>-<label>.md` | the citable table | **sections 8–10 only** |

The generated result file leaves its analysis sections empty on purpose. A table
of numbers with no sentences explaining them is not a result yet
(`metrics/README.md` §4), and the harness prints a reminder saying so.

**Committing the raw data is a separate, deliberate act.** `.gitignore` excludes
`metrics/raw/**` — most harness runs are exploratory noise — and admits only
`metrics/raw/**/published/**`. So when a result file is ready to cite, move the
JSON that backs it:

```bash
mkdir -p metrics/raw/<date>-<sha>/published
mv metrics/raw/<date>-<sha>/*.json metrics/raw/<date>-<sha>/published/
```

and point the result file's "Raw data" link at `published/`. A published number
whose samples were thrown away is not reproducible.

### What it will not do

- Run without a machine spec.
- Accept an unknown configuration name.
- Stay quiet when two configurations of one program print different things. That
  is metric C-05 failing, it makes every timing on the page meaningless, and the
  harness exits non-zero.

---

## 3. Attribution — which decision earned the speedup?

A speedup nobody can name a cause for is as likely to be an accidental
code-layout shift as anything the profile bought.

```bash
python3 bench/harness/attribute.py --optiforge build-release/bin/optiforge.exe --machine windows-mingw --reps 12
```

This runs `methodology.md` §5 literally: measure with everything on, then once
per decision with that decision switched off, and subtract.

```
contribution(P) = full gain − gain with P disabled
```

The switch is `--disable-pgo=<decision>`, one of `inline`, `unroll`, `regalloc`,
`layout`, `cold-size`. A disabled decision takes its **no-profile path** — the
same code an ordinary `-O2` build runs — which is what makes the subtraction
mean anything. A misspelled name is a usage error, not a no-op, because an
attribution run that silently disabled nothing would report the full speedup as
one decision's contribution.

**Read the residual.** The contributions should roughly sum to the full gain. A
large residual means the decisions interact rather than add — which is a finding,
not a bug, and is why the harness prints it instead of distributing it away.

**The method has a resolution limit.** It combines seven medians, each carrying
about a noise floor's worth of error, so below roughly a 5% gain it cannot
attribute to better than a point or so. That is a property of subtraction, not of
the compiler.

---

## 4. Portability — what survives a different workload?

```bash
python3 bench/harness/portability.py --optiforge build-release/bin/optiforge.exe --machine windows-mingw --reps 12
```

Every headline speedup is measured with the profile applied to the run it was
collected from, which is PGO's best case and nobody's deployment. For each
`bench/workloads/<name>_b.of` — a variant that changes **only `main`**, so every
function the profile describes is character-for-character identical — this times
three builds:

| Build | Meaning |
|---|---|
| `baseline` | B at `-O2` |
| `matched` | B against B's own profile — the upper bound |
| `crossed` | B against A's profile — the realistic case |

Because only `main` changed, the source hash differs and the compiler says so,
then matches by name, which still resolves at 100%. That isolates *the workload
changed* from *the code changed*.

### Writing a workload-B variant

Change constants in `main` and nothing else. If you touch a function body, its
block names may shift and you will be measuring staleness rather than
portability — two variables at once.

---

## 5. The single-purpose scripts

Kept because each answers one question cheaply, without a full timing session:

| Script | Question | Metric |
|---|---|---|
| `count_ir.py` | How many IR instructions does each level leave? | O-03, O-04 |
| `count_memops.py` | How much frame traffic does graph colouring remove versus naive? | BE-04, Q-06 |
| `profile_overhead.py` | How much slower is `--profile`? | I-01 |
| `pgo_speedup.py` | Does PGO beat `-O2`? (minimum-of-N, the Phase 11 estimator) | PGO-14 |

`pgo_speedup.py` and `run.py` deliberately disagree about the estimator:
`pgo_speedup.py` reports the **minimum** of N runs, `run.py` the **median**, as
`methodology.md` §4 requires. The minimum is closer to the program's own cost
and the median is more stable; where both have been run they agree to about a
point, which is itself worth knowing.

---

## 6. Adding a benchmark

1. **200 ms to 2 s at `-O2`.** Below about 200 ms, process startup and paging
   dominate — and time it *inside* the harness, not with a shell `time` around
   it, which measures the shell.
2. **Print a checksum.** A binary that got faster by computing less is caught
   immediately, and the differential gate (C-05) needs something to compare.
3. **Deterministic.** No wall clock, no randomness, no I/O in the timed region.
4. **Say in the header which decision it is meant to exercise, and why nothing
   static can make that decision.** If a static heuristic could get it right,
   the benchmark is not measuring profile guidance.
5. **Write it before you look at what the pass does.** Sizing a benchmark to a
   pass's existing behaviour tests your assumptions, not the compiler.
6. Add it to `bench/programs/`; the harnesses glob the directory.
