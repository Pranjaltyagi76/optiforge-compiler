# OptiForge — Metric Catalog

> **Purpose:** the authoritative definition of every metric. If a number appears anywhere in this project, it is defined here.
> **Doc version:** 1.0 · **Created:** 2026-08-18

Each metric has an **ID**, a **precise formula** (so two people compute it identically), a **unit**, a **target** where one exists, the **requirement it traces to**, and the **phase** from which it becomes measurable.

A metric with no defined formula is an opinion. A metric with no target is a curiosity. Everything below has both, or is explicitly marked as descriptive-only.

---

## C — Correctness Metrics

Correctness is a gate, not a trend. These are all-or-nothing: anything below target blocks a phase from being called complete.

| ID | Metric | Formula | Unit | Target | Traces to | From |
|---|---|---|---|---|---|---|
| **C-01** | Unit test pass rate | `passed / total` | % | **100%** | QA-01 | P0 |
| **C-02** | Golden-file test pass rate | `passed / total` | % | **100%** | QA-02 | P1 |
| **C-03** | Negative test pass rate | diagnostics match exactly, no missing or extra | % | **100%** | QA-03 | P2 |
| **C-04** | End-to-end pass rate | `programs with correct stdout and exit code / total` | % | **100%** | QA-04 | P4 |
| **C-05** | ★ **Differential pass rate** | `programs with byte-identical output across -O0/-O1/-O2/instrumented/PGO / total` | % | **100%** | **QA-05** | P7 |
| **C-06** | PGO cycle integration pass | full instrument→run→recompile→verify cycle succeeds | bool | **pass** | QA-06 | P11 |
| **C-07** | Sanitizer cleanliness | ASan + UBSan findings across the suite | count | **0** | QA-07 | P0 |
| **C-08** | Diagnostic coverage | `diagnostics with ≥1 triggering test / diagnostics defined` | % | **100%** | QA-03 | P2 |
| **C-09** | IR verifier failures | verifier assertions triggered during the suite | count | **0** | IR-08 | P3 |
| **C-10** | Determinism | `.s` output byte-identical across two compilations of the same input | bool | **identical** | NFR-06 | P4 |

> **C-05 is the most important metric in this document.** It is the only one that scales automatically: every new pass is instantly covered by every existing test program. A single failure here means an optimization changed program semantics, which invalidates every performance number until fixed.

---

## P — Compiler Performance Metrics

How usable the compiler is as a tool. Descriptive until they breach target.

| ID | Metric | Formula | Unit | Target | Traces to | From |
|---|---|---|---|---|---|---|
| **P-01** | Compile time (`-O2`) | wall time, 1,000-line input, median of 5 | s | **< 2.0** | NFR-01 | P7 |
| **P-02** | Compile time (`-O0`) | wall time, same input | s | < 0.5 | NFR-01 | P4 |
| **P-03** | Compiler peak memory | peak RSS during compilation | MB | **< 500** | NFR-02 | P7 |
| **P-04** | Per-pass time share | `pass time / total pipeline time` | % | descriptive | — | P7 |
| **P-05** | Compile-time PGO overhead | `compile time with --use-profile / compile time without` | ratio | < 1.3 | — | P11 |
| **P-06** | Test suite duration | wall time for the full suite | s | **< 300** | QA-10 | P1 |

> **P-04 has no target deliberately.** Its value is diagnostic: if one pass consumes 60% of pipeline time, that is where optimization effort goes. Expect the dominator tree and GVN to be the top entries.

---

## Q — Generated Code Quality Metrics

The output of the compiler, measured per benchmark per configuration.

| ID | Metric | Formula | Unit | Target | Traces to | From |
|---|---|---|---|---|---|---|
| **Q-01** | ★ Runtime | median wall time over N≥10 reps | ms | lower is better | — | P4 |
| **Q-02** | Runtime variance | interquartile range / median | % | **< 3%** or results are untrustworthy | — | P4 |
| **Q-03** | Code size | `.text` section size via `size` | bytes | descriptive | — | P4 |
| **Q-04** | IR instruction count | instructions after the pipeline | count | lower is better | OPT-* | P3 |
| **Q-05** | Machine instruction count | instructions in emitted `.s` | count | lower is better | BE-* | P4 |
| **Q-06** | Static memory traffic | count of `load`/`store` in emitted code | count | lower is better | BE-04 | P8 |
| **Q-07** | Spill count | reload/spill instructions inserted by regalloc | count | lower is better | BE-04 | P8 |
| **Q-08** | Register pressure | max simultaneously live values per function | count | descriptive | BE-04 | P8 |

> **Q-02 gates everything else.** If run-to-run variance exceeds 3%, no speedup below ~6% is distinguishable from noise, and the headline PGO number becomes unreportable. Fix variance (see `methodology.md` §3) before trusting any Q or G metric.

> **Q-06 is the cleanest demonstration of Phase 8.** The naive allocator stores every value to the stack; graph coloring should cut this dramatically. Measure it with `--regalloc=naive` vs `--regalloc=graph` on identical input — a controlled single-variable comparison.

---

## O — Optimization Effectiveness Metrics

Per-pass accountability. Every pass must justify its existence.

| ID | Metric | Formula | Unit | Target | Traces to | From |
|---|---|---|---|---|---|---|
| **O-01** | Pass instruction delta | `(IR count before − after) / before` | % | > 0 or the pass is dead weight | OPT-10…20 | P7 |
| **O-02** | Pass fire count | times the pass changed anything, across the suite | count | > 0 | OPT-07 | P7 |
| **O-03** | Cumulative `-O1` reduction | `1 − (IR count at -O1 / at -O0)` | % | > 25% | OPT-04 | P7 |
| **O-04** | Cumulative `-O2` reduction | `1 − (IR count at -O2 / at -O0)` | % | **> 40%** | OPT-04 | P7 |
| **O-05** | Constants folded | count folded by SCCP + constant folding | count | descriptive | OPT-10/11 | P7 |
| **O-06** | Dead instructions removed | count removed by DCE | count | descriptive | OPT-12 | P7 |
| **O-07** | CSE eliminations | redundant expressions removed | count | descriptive | OPT-15 | P7 |
| **O-08** | LICM hoists | instructions moved to a preheader | count | descriptive | OPT-17 | P7 |
| **O-09** | Inline decisions | call sites inlined / considered | ratio | descriptive | OPT-19 | P7 |
| **O-10** | Pipeline convergence | iterations before no pass reports a change | count | **< 5** | OPT-07 | P7 |

> **O-01 and O-02 together catch the silent no-op pass** — one that runs, costs compile time, and never fires because a precondition is never met. Without these, such a pass can sit in the pipeline for months looking productive.

> **O-10 guards against an infinite pipeline loop**, where two passes undo each other's work forever. If convergence exceeds 5, two passes are fighting.

---

## I — Instrumentation Metrics

The cost of collecting a profile. Phase 9–10.

| ID | Metric | Formula | Unit | Target | Traces to | From |
|---|---|---|---|---|---|---|
| **I-01** | ★ Instrumentation overhead | `(instrumented runtime / uninstrumented runtime) − 1` | % | **< 40%** | NFR-09, PROF-13 | P9 |
| **I-02** | Counter count | counters inserted per benchmark | count | descriptive | PROF-01 | P9 |
| **I-03** | Profile file size | `.prof` size on disk | KB | descriptive | PROF-09 | P9 |
| **I-04** | Profile write time | time in the `atexit` dump | ms | < 100 | PROF-07 | P9 |
| **I-05** | Counter accuracy | `measured count / hand-computed count` on a known program | ratio | **1.000** | PROF-02…05 | P9 |
| **I-06** | Instrumented output identity | instrumented program stdout == uninstrumented stdout | bool | **identical** | PROF-11 | P9 |
| **I-07** | Code size inflation | `instrumented .text / normal .text` | ratio | descriptive | — | P9 |

> **I-05 must be exactly 1.000, verified by hand on a small program** — a loop of known trip count, a branch of known bias. Every downstream PGO decision inherits any error here, and a systematically wrong profile produces confidently wrong optimizations.

> **I-01 has a subtlety worth recording:** instrumentation perturbs what it measures. Overhead concentrated in the hot loop distorts the *relative* frequencies, not just absolute time. Report both overhead and whether the hot-path *ranking* changed versus `perf`.

---

## G — PGO Effectiveness Metrics ★ The Project Thesis

These are the numbers the project is judged on.

| ID | Metric | Formula | Unit | Target | Traces to | From |
|---|---|---|---|---|---|---|
| **G-01** | ★★ **PGO speedup vs `-O2`** | `(O2 runtime / PGO runtime) − 1`, median across benchmarks | % | **≥ 10% on ≥3 benchmarks** | **NFR-10, PGO-14** | P11 |
| **G-02** | Per-benchmark PGO speedup | same, per benchmark | % | report all, including losses | PGO-14 | P11 |
| **G-03** | ★ Profile match rate | `IR entities found in profile / IR entities total` | % | **> 95%** on unchanged source | PGO-01, IR-11 | P10 |
| **G-04** | Hot-path detection accuracy | hot functions/loops identified vs hand-known ground truth | % | **100%** on designed benchmarks | PGO-03/05 | P10 |
| **G-05** | Decision attribution | speedup traced to a named PGO decision | % of G-01 | **> 80% attributed** | PGO-13 | P11 |
| **G-06** | PGO inline decisions changed | call sites inlined under PGO but not `-O2`, and vice versa | count | > 0 | PGO-06 | P11 |
| **G-07** | PGO unroll decisions changed | loops unrolled under PGO but not `-O2`, and vice versa | count | > 0 | PGO-07 | P11 |
| **G-08** | Spill-decision improvement | `Q-07 at -O2` vs `Q-07 with profile-weighted regalloc` | % | > 0 | PGO-08 | P11 |
| **G-09** | Layout improvement | fall-through rate on hot edges, `-O2` vs PGO | % | > 0 | PGO-09 | P11 |
| **G-10** | Cold code size reduction | `.text` of cold functions, `-O2` vs PGO | % | > 0 | PGO-10 | P11 |
| **G-11** | ★ Robustness | correct binary with missing / empty / corrupt / stale profile | bool | **4/4 pass** | **PGO-11** | P10 |
| **G-12** | Stale-profile detection | stale profile produces a warning, not a silent no-op | bool | **pass** | PGO-12 | P10 |
| **G-13** | Profile portability | speedup when profiled on workload A, run on workload B | % | descriptive | — | P12 |

> **G-05 is what separates a real result from a lucky one.** If PGO is 12% faster but you cannot say *which decision* produced the 12%, you have not demonstrated profile guidance — you have demonstrated run-to-run variance or an unrelated code-layout accident. Use `--pgo-remarks` and disable PGO passes one at a time to attribute the gain.

> **G-03 is the silent killer.** If block naming drifts (requirement IR-11), the match rate collapses, every PGO pass falls back to its no-profile path, and the build looks correct while doing nothing. G-03 must be checked *before* G-01 is believed — a PGO speedup of 0% with a match rate of 4% is a bug, not a finding.

> **G-13 is worth measuring even though it has no target.** Real PGO deployments profile on one workload and run on another. Showing you understand the limitation is worth more than a perfect number on the workload you profiled.

---

## R — Process Metrics

Project tracking. Reviewed at each phase boundary.

| ID | Metric | Formula | Unit | Target | Traces to |
|---|---|---|---|---|---|
| **R-01** | Requirement coverage | `requirements with ≥1 verifying test / total M-priority requirements` | % | **100%** at completion | requirement.md |
| **R-02** | Phase exit criteria met | criteria satisfied / total for the phase | % | 100% before advancing | roadmap.md |
| **R-03** | Milestone completion | milestones reached / 7 | count | 7/7 | roadmap.md §6 |
| **R-04** | Open risk count | risks in the register not yet mitigated | count | decreasing | roadmap.md §7 |
| **R-05** | Deliverable completion | delivered / 14 | count | 14/14 | requirement.md §2 |

---

## Adding a New Metric

1. Give it an ID in the right category, next free number.
2. Write the formula precisely enough that two people compute the same value.
3. State a target, or mark it **descriptive** and say what decision it informs.
4. Trace it to a requirement ID, or explain why it exists without one.
5. Note the phase it becomes measurable.
6. If the harness produces it, update `bench/harness/run.py` to emit it into `metrics/raw/`.

**A metric nobody will act on should not be collected.** Every entry above either gates a phase, guards a regression, or supports the central claim.
