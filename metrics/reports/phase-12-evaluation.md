# Phase 12 Evaluation — `Benchmarking & Evidence`

> Written at the phase boundary, before advancing. The question this phase
> exists to answer is the one in the project's Definition of Done:
> **what did profile guidance actually buy, and why?**

| Field | Value |
|---|---|
| Phase | 12 — Benchmarking & Evidence |
| Started / completed | 2026-08-20 / 2026-08-20 |
| Git revision at completion | `dd51bdf` + the Phase 12 commit |
| Milestone reached | M7 (`roadmap.md` §6) — *"PGO beats -O2"*, now with the full table behind it |

---

## 0. The answer, in one page

**Profile guidance bought one large win, two small ones, four nothings and one
substantial loss**, across eight benchmarks measured against a 1.0% noise floor:

| | Program | PGO vs `-O2` | Attributed to |
|---|---|---:|---|
| Win | `branch_machine.of` | **+15.8%** | block layout (+16.0) |
| Win | `nested_math.of` | +5.2% | unrolling (+5.0) |
| Win | `loop_sum.of` | +2.1% | unrolling (+2.6) |
| Null | `biased_branch.of` | +0.4% | — inside the floor |
| Null | `branchy.of` | −0.0% | — inside the floor |
| Null | `nbody.of` | −0.4% | regalloc (−1.5) |
| Null | `recursive_fib.of` | −0.7% | — inside the floor |
| **Loss** | `loop_kernel.of` | **−7.1%** | **unrolling (−6.8)** |

**And *why* it bought them is now a checked claim rather than an assertion.**
The `--disable-pgo=` attribution sweep says two of the five profile-guided
decisions do all of the work, each on exactly one program shape:

- **Block layout** pays when a function has one dominant path among several. It
  is worth +16.0 points on the one program in the corpus built that way, and is
  inside the noise on all seven others.
- **Unrolling** pays when a short loop body is dominated by loop control. It is
  worth +5.0 and +2.6 on two programs — and **−6.8 on a third**, because it has
  a trip-count threshold where it needs a cost model.
- **Inlining, cold-code size mode and profile-weighted spill costs never
  register outside the noise floor**, on any program, in either direction —
  except once, negatively.

The single most useful sentence this phase produced is not about a number:

> **Profile-guided layout pays in proportion to how *concentrated* the hot path
> is, not to how *accurately* the profile was measured.**

That came out of the portability run, where the same program driven so that no
single path dominates makes a perfectly accurate profile worth **−4.7%**.

---

## 1. Exit Criteria *(metric R-02)*

From `roadmap.md` Phase 12.

| # | Exit criterion | Met? | Evidence |
|---|---|---|---|
| 1 | Benchmark suite: matmul, n-body, sieve, recursive fib, an array/loop kernel, a branch-heavy state machine | ⚠️ **Partial** | 8 programs in `bench/programs/`. `recursive_fib`, `loop_kernel`, `branch_machine` and `nbody` are present. `matmul` and `sieve` **cannot be written** — see §2. |
| 2 | Harness: each benchmark at `-O0`, `-O1`, `-O2`, `-O2 --use-profile`, N reps, median and variance | ✅ | `bench/harness/run.py`, 15 reps after 3 warm-ups, interleaved; median, min, IQR and IQR/median in [the results](../results/2026-08-20-phase12-benchmarks.md) §3 |
| 3 | Metrics: wall time, instruction count, code size, compile time, instrumentation overhead | ✅ | Results §3 (Q-01/Q-02), §5 (Q-03/Q-05/Q-07/Q-08), §7 + §8 (P-01/P-02/P-03/P-05), §4 (I-01/I-03) |
| 4 | Results checked into `metrics/results/` with the machine specification recorded | ✅ | Four result files dated 2026-08-20; [machine spec](../machines/windows-mingw.md) completed with hardware and a **measured 1.0% noise floor**, which was the blocker |
| 5 | Write-up: which PGO decisions produced which gains, and where PGO lost | ✅ | [attribution](../results/2026-08-20-pgo-attribution.md), [portability](../results/2026-08-20-pgo-portability.md), and this document |
| ★ | **A results table showing PGO beating `-O2` on at least three benchmarks, with the wins explained mechanistically** | ✅ | **3 benchmarks** above the 1.0% floor: +15.8%, +5.2%, +2.1%. Each traced to a named decision by the disable-one-at-a-time sweep. |

**Verdict: ☑ Phase complete**, with criterion 1 partial for a reason recorded
below and accepted rather than quietly dropped.

### The target that was *not* met

**G-01 / NFR-10 — "≥10% speedup on ≥3 benchmarks" — is missed.** One benchmark
exceeds 10%. This is the project's headline performance target and it is not
met, which is stated here rather than buried: Phase 12's exit criterion (three
benchmarks beating `-O2`, explained) and NFR-10 (three benchmarks beating it by
ten points) are different bars, and only the first is cleared.

The reason is visible in the attribution table and is not mysterious. Two
decisions have real leverage; each applies to one narrow program shape; the
corpus contains one program of each shape. Making G-01 would need either more
decisions with leverage (multi-block inlining is the obvious candidate and has
been open since Phase 11) or a corpus of programs shaped to suit the two that
work — and the second would be measuring the benchmark writer, not the compiler.

**P-05 — compile-time PGO overhead < 1.3× — is missed on one program of eight**
(`nbody.of`, 1.73×). Cause and consequence in the results file §9.

---

## 2. Requirements Delivered *(metric R-01)*

| Requirement | Description | Verified by | Status |
|---|---|---|---|
| D7 | Benchmark suite and harness | `bench/programs/` (8), `bench/workloads/` (3), `bench/harness/` (7 scripts) | ☑ Done |
| D8 | Benchmark results with machine specs | 4 files in `metrics/results/`, machine spec completed | ☑ Done |
| PGO-14 | PGO measurably beats `-O2` on the profiled workload | Results §3 — three programs above the floor | ☑ Done |
| PGO-13 | Every profile-guided decision explains itself | `--pgo-remarks`, now also reporting match rate and disabled decisions | ☑ Done |
| NFR-10 | ≥10% PGO speedup on ≥3 benchmarks | One benchmark | ☒ **Not met** |
| G-05 | Speedup attributed to a named decision | `--disable-pgo=`, [attribution run](../results/2026-08-20-pgo-attribution.md) | ☑ Done |
| G-13 | Profile portability | [portability run](../results/2026-08-20-pgo-portability.md) | ☑ Done (descriptive) |

### Deferred, with reasons

| Item | Deferred to | Reason |
|---|---|---|
| `matmul` and `sieve` benchmarks | Phase 13 | **The language has no arrays** (`docs/language.md` §8), and the roadmap's own risk register freezes the feature set until Phase 12 completes: *"The language is frozen at the agreed feature set until Phase 12 is complete. Phase 13 only."* A matmul without arrays is not a matmul, and a sieve without them is not memory-bound, which is the only property that made it worth having. Rather than write two programs that carry the names without the shapes, the corpus records the gap. |
| A physically real `nbody` | Phase 13 | Same cause plus no `sqrt` builtin. `bench/programs/nbody.of` keeps the *shape* — pairwise force accumulation over fixed bodies, long float dependency chain, 35 values live against 10 allocatable XMM registers — and its header says plainly that the physics is not real. |
| A memory-bound benchmark of any kind | Phase 13 | Follows from the above, and it is the corpus's most consequential gap: layout and cold-code placement would be expected to pay most exactly where this corpus cannot go. |
| `perf` cross-check of the profiler (`methodology.md` §6, Level 2) | Never, on this target | `perf` does not exist on Windows/MinGW (ADR-10). Level 1 hand verification from Phase 9 is all that stands behind the profiler's ranking, and that is a permanent consequence of the target decision, not an oversight. |

---

## 3. Metrics at Phase Boundary

| Metric | Name | Value | Target | Met? | vs Phase 11 |
|---|---|---|---|---|---|
| C-05 | Differential pass rate | 100% (8 programs × 5 configurations) | 100% | ✅ | held |
| C-01/C-02 | Unit / golden pass rate | 383 / 46 | 100% | ✅ | 379 / 45 |
| Q-02 | Runtime spread | worst 1.8% | < 3% | ✅ | first measured |
| G-01 | PGO ≥10% on ≥3 benchmarks | 1 benchmark | 3 | ❌ | first measured |
| G-02 | Per-benchmark speedup | +15.8% … −7.1% | report all | ✅ | 5.5% best → **15.8%** best |
| G-03 | Profile match rate | **100%** on all 8 | > 95% | ✅ | first measured |
| G-05 | Decision attribution | 95% on the one gain large enough to attribute | > 80% | ✅ | first measured |
| G-13 | Profile portability | measured, 3 pairs | descriptive | ✅ | first measured |
| I-01 | Instrumentation overhead | worst +31.6% | < 40% | ✅ | held |
| O-04 | IR reduction at `-O2` | **50.4%** | > 40% | ✅ | 46.0% → 50.4% |
| BE-04 | Memory traffic, graph vs naive | **59.2%** fewer | lower | ✅ | 58.7% → 59.2% |
| P-01 | Compile time `-O2`, 1,000 lines | 0.10 s (0.38 s with `as`/`ld`) | < 2.0 s | ✅ | first measured |
| P-02 | Compile time `-O0` | 0.09 s | < 0.5 s | ✅ | first measured |
| P-03 | Compiler peak memory | 7.5 MB | < 500 MB | ✅ | first measured |
| P-05 | Compile-time PGO overhead | worst **1.73×** | < 1.3 | ❌ | first measured |

**Regressions since Phase 11:** none in correctness. O-04 and BE-04 both improved,
in both cases because the corpus grew rather than because a pass changed — the
new programs are larger and have more for the optimizer to remove. That is worth
naming so the improvement is not later mistaken for one.

---

## 4. What Worked

**Building the attribution flag before the measurement, not after.**
`methodology.md` §5 warned in Phase 0 that retrofitting `--disable-pgo=` would be
painful and that the temptation would be to skip attribution instead. It was
right. With the flag, `loop_kernel.of`'s −7.1% went from a mystery to
"unrolling, −6.8, here is the instruction count" in one 12-minute run. Without
it the honest write-up would have been *"PGO is slower on this program and we do
not know which decision did it"*.

**Rejecting a misspelled decision name.** `--disable-pgo=unrol` is a usage error,
not a no-op. An attribution run whose flag silently did nothing would measure the
full speedup and report it as one decision's contribution — a wrong number that
looks exactly like a right one. This cost four lines and removes a whole class of
silent bad results.

**Measuring the noise floor first, and adopting the worst case.** The floor came
out at 1.0%, which immediately reclassified three of the eight results as
"nothing happened" rather than leaving them to be quoted as small wins. Taking
the maximum rather than the mean was the right call for the same reason.

**Interleaving.** `branch_machine.of` drifted 330→364→333 ms *within* a
ten-repetition run. Batched, that drift would have landed entirely on whichever
configuration ran second and manufactured a 6% result out of nothing. Interleaved,
both configurations show the same drift and the medians agree to 0.48%.

**Printing the match rate under `--pgo-remarks`.** G-03 was defined in Phase 0 as
"the silent killer" and was, until this phase, uncheckable — the compiler only
warned below 50%. It is 100% on every program, which is what makes it legitimate
to read the four null results as real nulls rather than as a profile that quietly
failed to apply.

**Writing the showcase benchmark to make a decision fire, then reporting that it
fired and lost anyway.** `loop_kernel.of` did exactly what it was built to do:
the profile corrected a loop ranking that loop depth gets wrong by six orders of
magnitude. It is also the phase's worst result. Both halves of that are the
finding.

---

## 5. What Did Not Work

| Issue | Cost | Root cause | Prevented in future by |
|---|---|---|---|
| First benchmark timings were 5× too high | ~30 min | Timed with the shell's `time` around a subshell, so process startup and first-touch paging dominated a 90 ms program | Time inside the harness, with warm-ups discarded — which is what `methodology.md` §3.1 already said |
| Three new benchmarks sized below the 200 ms floor | ~20 min | Sized against those wrong timings | Same fix; the corrected sizes are 270–530 ms at `-O2` |
| `run_golden.py --update` rewrote all 45 goldens as error messages | ~5 min, no damage | Run without `--workdir`, so every relative input path failed to resolve. CTest passes `--workdir` and a manual invocation does not | Caught by reading the diff before committing, which is exactly what the script's own docstring instructs. `git checkout` restored it. |
| The unroller's cost model | not fixed | It has a trip-count threshold and a body-shape guard; neither can express "this body is already issue-limited" | Recorded as the phase's top action rather than patched with a number tuned against the benchmark that exposed it |

**On not fixing `loop_kernel.of`.** Phase 11 found a regression on `branchy.of`
and fixed it the same day, and this phase deliberately did not do the same. The
difference is the kind of fix available. Phase 11's was categorical — never
unroll a loop whose body contains a call — and derivable without reference to
the program that exposed it. Here, any threshold that rescues `loop_kernel.of`
would be a constant chosen by looking at `loop_kernel.of`, which is
`methodology.md` §2's warning running in reverse: instead of writing a benchmark
to fit the pass, tuning the pass to fit the benchmark. The loss is reported at
full size and the real fix is on the list.

---

## 6. Risks

**Materialized:**

| Risk | What happened | Mitigation effective? |
|---|---|---|
| *"PGO shows no measurable win"* — was **partly realized** in Phase 11 | Still partly realized, and now quantified: 3 wins, 4 nulls, 1 loss of 7%. Every one is explained by a named decision | Yes — the mitigation was "report rather than quietly drop", and that is what §0 does |
| *"Scope creep in the language"* | The array question came up immediately and was resolved **by the register's own rule** rather than by re-litigating it: frozen until Phase 12 completes | Yes — the rule did its job without a discussion |
| *"Profile IDs unstable across recompiles, so PGO silently no-ops"* | Did **not** materialize. G-03 is 100% on all eight programs, and 100% even across the deliberately-mismatched source hashes of the portability run | Yes — the Phase 9 ID scheme holds |

**New risks:**

| Risk | Impact | Likelihood | Mitigation | In register? |
|---|---|---|---|---|
| The unroller has no cost model, so profile-driven unrolling is a coin flip on any body it has not been tested against | High — it is simultaneously the largest positive and largest negative contributor | **Realized** on `loop_kernel.of` | Cost model, or a much more conservative firing rule | ☑ added |
| The benchmark corpus is entirely compute-bound, so layout and cold-code decisions are measured only where they matter least | Medium — the phase's biggest win is layout, and the corpus cannot show its best case | Certain, until arrays exist | Arrays, Phase 13 | ☑ added |
| Profile-guided layout can lose on programs with no dominant path | Medium — measured at −4.7% | Realized on `branch_machine_b.of` | Decline to commit when no path dominates | ☑ added |

---

## 7. Estimate Accuracy

| | Estimated | Actual | Ratio |
|---|---|---|---|
| Duration | 1.5 weeks | 1 session | — |

The estimate assumed the benchmark suite would be written from scratch. Five of
eight programs already existed from Phase 11, and — more significantly — the
measurement *discipline* (`methodology.md`, `metric-catalog.md`) had been written
in Phase 0 and did not have to be invented under the pressure of wanting a good
number. Most of this phase was executing a plan that already existed.

The part that was not in the plan, and took the most thought, was deciding **not**
to fix `loop_kernel.of`.

---

## 8. Readiness for Phase 13

| Question | Answer |
|---|---|
| Blocking dependencies satisfied? | ☑ Yes. Phases 0–12 complete. |
| Decisions to make first? | **Arrays.** Four of this phase's open actions and three of its recorded gaps are one language feature. It should be the first thing Phase 13 does, before any new pass. |
| Technical debt that compounds if unpaid? | **The unroller's missing cost model.** It is the largest correctable loss measured anywhere in this project, and every future benchmark added to the corpus is a fresh chance for it to fire on a body it hurts. |
| Does `context/` still describe reality? | ☑ Updated — `roadmap.md` Phase 12 marked complete with its results; the risk register gained three entries. |

### The three things Phase 13 should do first, in order

1. **Arrays.** Unblocks `matmul`, `sieve`, a real `nbody`, and the corpus's only
   structural gap — no memory-bound program.
2. **An unroller cost model.** Worth up to 7 points on shapes it currently
   hurts, and up to 11 on the short-trip loops it currently refuses
   (measured in the portability run, §4).
3. **Multi-block inlining.** The one profile-guided decision that is fully
   implemented, demonstrably correct, and has nothing to apply to. Open since
   Phase 11; the attribution table is now the evidence that it contributes
   nothing measurable today.
