# OptiForge — Metrics & Evaluation

> **Purpose:** the single home for every number this project produces and every judgement made from those numbers.
> **Doc version:** 1.0 · **Created:** 2026-08-18

---

## 1. Why This Folder Exists

OptiForge's central claim is that **profile-guided optimization measurably beats static optimization**. A claim like that is only worth as much as the evidence behind it. This folder holds that evidence, and the discipline for producing it.

Three failure modes it is designed to prevent:

1. **Unreproducible numbers.** A speedup figure with no machine spec, no repetition count, and no compiler revision is an anecdote. Every result here carries its provenance.
2. **Unfalsifiable claims.** "Optimization X helps" with no before/after is marketing. Every metric has a defined formula and a target.
3. **Silent regressions.** Without a tracked history, a pass that quietly costs 8% is invisible until it has compounded with four others.

---

## 2. Separation of Concerns: `bench/` vs `metrics/`

| | `bench/` | `metrics/` |
|---|---|---|
| Holds | **Code** — benchmark programs and the harness that runs them | **Data and judgement** — outputs, tables, evaluations |
| Changes when | You add a benchmark or change how measurement works | You take a measurement |
| Reviewed as | Source | Evidence |

`bench/harness/run.py` **writes into** `metrics/`. Nothing in `metrics/` is edited by hand except reports and machine specs.

> This supersedes earlier references to `bench/results/` in the `context/` documents — results live here.

---

## 3. Folder Layout

```
metrics/
├── README.md                # This file — index and workflow
├── metric-catalog.md        # ★ Every metric defined: ID, formula, target, requirement traced
├── methodology.md           # ★ How to measure so the numbers are trustworthy
├── machines/                # Machine specifications — results are meaningless without one
│   ├── TEMPLATE.md
│   └── <machine-id>.md      # e.g. wsl2-ryzen5600.md
├── raw/                     # Immutable raw harness output (JSON/CSV). Append-only.
│   └── <date>-<revision>/   # e.g. 2026-09-14-a3f21c9/
├── results/                 # Processed, committed result tables — the citable numbers
│   ├── TEMPLATE-benchmark-run.md
│   └── <date>-<label>.md    # e.g. 2026-09-14-phase11-pgo.md
├── reports/                 # Written evaluation — what the numbers mean
│   ├── TEMPLATE-phase-evaluation.md
│   └── <phase>-evaluation.md
└── templates/               # Copy these; do not edit in place
    └── regression-log.md
```

### What goes where — the one-line rule

| If it is… | It goes in… |
|---|---|
| A number a machine produced | `raw/` |
| A number a human will cite | `results/` |
| A sentence explaining why a number is what it is | `reports/` |
| A description of the hardware that produced a number | `machines/` |
| A definition of what a number *means* | `metric-catalog.md` |

---

## 4. Workflow

### Taking a measurement

```bash
python3 bench/harness/run.py --configs O0,O1,O2,PGO --reps 10 --machine wsl2-ryzen5600
```

The harness:
1. Writes raw output to `metrics/raw/<date>-<git-revision>/`.
2. Generates a processed table at `metrics/results/<date>-<label>.md` from the template.
3. Fails loudly if `--machine` names a spec file that does not exist in `machines/` — provenance is not optional.

### Then, by hand

4. Read the table. If anything moved by more than the noise floor (see `methodology.md` §4), write a paragraph in `reports/` explaining **why**.
5. If something regressed, add a row to `templates/regression-log.md` copied into `reports/`.

### The rule that makes this work

> **A result is not committed until its explanation is written.** A table with an unexplained 15% swing is a bug report, not a result.

---

## 5. Metric Categories

Defined in full in [`metric-catalog.md`](metric-catalog.md). Summary:

| Category | Answers | Key metrics | Traces to |
|---|---|---|---|
| **C — Correctness** | Does it still work? | Test pass rate, differential-test pass rate | QA-01…QA-06 |
| **P — Compiler performance** | Is the compiler usable? | Compile time, compiler peak RSS | NFR-01, NFR-02 |
| **Q — Code quality** | Is the generated code good? | Runtime, code size, IR instruction count | OPT-*, BE-* |
| **O — Optimization effectiveness** | Does each pass earn its place? | Per-pass instruction delta, fire count | OPT-10…OPT-20 |
| **I — Instrumentation** | Is profiling affordable? | Instrumentation overhead, profile size | NFR-09, PROF-13 |
| **G — PGO effectiveness** | ★ **The project thesis** | PGO speedup vs `-O2`, profile match rate, decision attribution | **NFR-10, PGO-14** |
| **R — Process** | Are we on track? | Requirement coverage, phase completion | roadmap.md |

**G is the headline.** Everything else is supporting evidence or a guard against regression.

---

## 6. The Headline Result

One table matters more than all the others. It is the centrepiece of the final report, and it is regenerated whenever the PGO passes change.

**Measured 2026-08-20** on [`windows-mingw`](machines/windows-mingw.md), noise floor **1.0%**, median of 15 interleaved repetitions. Full table with every configuration in [`results/2026-08-20-phase12-benchmarks.md`](results/2026-08-20-phase12-benchmarks.md); the Why column comes from the disable-one-decision-at-a-time sweep in [`results/2026-08-20-pgo-attribution.md`](results/2026-08-20-pgo-attribution.md).

| Benchmark | `-O0` | `-O1` | `-O2` | `-O2 +PGO` | **PGO vs `-O2`** | Why |
|---|---:|---:|---:|---:|---:|---|
| `branch_machine` | 384.4 | 340.3 | 340.3 | 294.0 | **+15.8%** | block layout, +16.0 attributed — one dominant path among four states |
| `nested_math` | 295.2 | 173.7 | 175.2 | 166.6 | **+5.2%** | unrolling, +5.0 — measured trip count 400, factor 8 |
| `loop_sum` | 618.6 | 407.3 | 406.5 | 398.1 | **+2.1%** | unrolling, +2.6 — same mechanism, longer body |
| `biased_branch` | 536.2 | 531.7 | 531.9 | 529.6 | +0.4% | inside the noise floor — the loop's `idiv` costs more than every branch around it |
| `branchy` | 512.8 | 510.1 | 509.0 | 509.2 | −0.0% | inside the floor — the unroller refuses a loop whose body is a call, deliberately |
| `nbody` | 488.3 | 402.3 | 397.1 | 398.5 | −0.4% | profile-weighted spill costs, −1.5 — one flat loop, so the profile says only that everything is equally hot |
| `recursive_fib` | 283.7 | 280.1 | 279.4 | 281.3 | −0.7% | inside the floor — no loops at all |
| `loop_kernel` | 762.6 | 305.4 | 274.3 | 295.2 | **−7.1%** | **unrolling, −6.8** — right decision, no cost model; see the results file §8 |
| **Median** | | | | | **+0.1%** | target ≥10% on ≥3 benchmarks (NFR-10): **not met**, 1 benchmark over 10% |

Times in ms. **Three benchmarks beat `-O2` above the noise floor, which is the Phase 12 exit criterion, and it is met. NFR-10's ≥10% on three is a different bar and is missed.** Both statements belong here; neither substitutes for the other.

> **Superseded by the ten-program run.** Phase 13 added arrays and with them `matmul.of` and `sieve.of`. The table above is the eight-program Phase 12 run, left exactly as measured. The current corpus is measured in [`results/2026-08-22-phase13-baseline.md`](results/2026-08-22-phase13-baseline.md), across **two independent sessions**, where **five** benchmarks beat `-O2` above the noise floor rather than three — `branch_machine` +16%, `matmul` +9 to +13%, `sieve` +8%, `nested_math` +4 to +7%, `loop_sum` +3 to +4% — and `loop_kernel` still loses about 6%.

The **Why** column is mandatory. A speedup you cannot attribute to a specific PGO decision is a speedup you cannot defend, and may well be measurement noise.

---

## 7. Honesty Requirements

These are not optional politeness; they are what makes the evaluation credible.

- **Report losses.** If PGO makes a benchmark slower, it goes in the table with an explanation. A suite where PGO wins 6/6 invites the question of how the benchmarks were chosen.
- **Never compare against GCC or Clang as a headline.** OptiForge's `-O2` is a student compiler; theirs is thirty years of work. The meaningful comparison is **OptiForge PGO vs OptiForge `-O2`** — same backend, same everything, one variable. A GCC column may appear as context, clearly labelled as such.
- **State the noise floor.** Any difference smaller than the measured run-to-run variance is not a result.
- **Record failed hypotheses.** "We expected cold-code size mode to help and it did not, because our benchmarks fit in L1i" is a genuine finding and belongs in `reports/`.
- **Never tune a benchmark to make a pass look good.** Benchmarks are fixed before the pass is written where possible.

---

## 8. Git Policy

| Path | Committed? | Reason |
|---|---|---|
| `metrics/README.md`, `metric-catalog.md`, `methodology.md` | ✅ | Definitions |
| `metrics/machines/*.md` | ✅ | Provenance — small and essential |
| `metrics/results/*.md` | ✅ | The citable numbers |
| `metrics/reports/*.md` | ✅ | The evaluation |
| `metrics/raw/**` | ⚠️ Selectively | Commit the raw data behind any *published* result; the rest is regenerable noise |
| `*.prof` files | ❌ | Machine- and workload-specific; regenerable (see `deployment.md` §9) |

Add to `.gitignore`:

```
metrics/raw/**
!metrics/raw/.gitkeep
!metrics/raw/**/published/
```

---

## 9. Cross-References

| Document | Relationship |
|---|---|
| [`../context/requirement.md`](../context/requirement.md) | Source of the targets every metric is measured against (NFR-01, NFR-02, NFR-09, NFR-10, PGO-14) |
| [`../context/roadmap.md`](../context/roadmap.md) | Phase 12 is the dedicated benchmarking phase; exit criteria are checked with these numbers |
| [`../context/System_design.md`](../context/System_design.md) §18.6 | Benchmark harness design |
| [`../context/deployment.md`](../context/deployment.md) §7, §8 | CI regression alerting and reproducibility guarantees |
