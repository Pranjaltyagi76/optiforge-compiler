# OptiForge — Roadmap

> **Status:** Planning · **Owner:** Pranjal Tyagi · **Doc version:** 1.0 · **Created:** 2026-08-18

---

## 1. North Star

Build **OptiForge**, a from-scratch optimizing compiler for a small statically-typed C-like language (`.of`) that:

1. Compiles source → x86-64 machine code through a fully custom pipeline (no LLVM, no yacc/bison, no third-party codegen).
2. Can emit an **instrumented** binary that records runtime behaviour into a profile file.
3. Can **re-compile** the same source using that profile to make optimization decisions a static compiler cannot make.
4. Demonstrates a **measurable** speedup of the PGO build over the plain `-O2` build.

### The one-sentence success criterion

> `optiforge bench.of -O2 -o a.out` and `optiforge bench.of -O2 --use-profile=bench.prof -o b.out` both produce correct executables, and `b.out` is measurably faster than `a.out` on the workload the profile was collected from.

If that sentence is true at the end, the project succeeded. Everything below is the path to it.

---

## 2. Guiding Principles

| Principle | Meaning in practice |
|---|---|
| **Correctness before speed** | An optimization that breaks one test is worse than no optimization. Every pass ships with a differential test. |
| **Vertical slices** | Get a trivial program end-to-end to a running executable early, then widen the language. Never build the frontend for three months with no backend. |
| **Every stage is dumpable** | `--emit=tokens\|ast\|ir\|cfg\|asm` at all times. A compiler you cannot inspect is a compiler you cannot debug. |
| **Passes are plugins** | No pass may reference another pass by name. All coordination goes through the PassManager and the analysis cache. |
| **Profile is advisory** | The compiler must produce a correct binary with a stale, partial, empty, or missing profile. Profile data changes *decisions*, never *semantics*. |
| **Measure, do not assume** | Every optimization claim is backed by a benchmark number checked into `metrics/results/`. |

---

## 3. Phase Overview

| Phase | Name | Outcome | Est. effort |
|---|---|---|---|
| 0 | Foundation & Tooling | Repo, build system, test harness, CLI skeleton | 1 week |
| 1 | Frontend — Lexer & Parser | Source → AST, with error reporting | 2 weeks |
| 2 | Semantic Analysis | Typed AST, symbol tables, diagnostics | 1.5 weeks |
| 3 | IR & CFG Construction | Three-address IR, basic blocks, CFG | 2 weeks |
| 4 | **Naive Backend (Vertical Slice)** | **First running executable** — unoptimized, stack-based | 2.5 weeks |
| 5 | Analysis Framework | Dominators, loops, liveness, use-def, reaching defs | 2 weeks |
| 6 | SSA Form | SSA construction + destruction, phi nodes | 2 weeks |
| 7 | Classical Optimizations | CF, CP, DCE, CSE, copy prop, strength reduction, LICM | 3 weeks |
| 8 | Real Register Allocation | Interference graph, graph coloring, spilling | 2.5 weeks |
| 9 | Instrumentation & Runtime | `--profile` mode, counters, runtime library, `.prof` writer | 2 weeks |
| 10 | Profile Ingestion & Hot-Path Detection | `.prof` parser, hot/warm/cold classification | 1.5 weeks |
| 11 | **Profile-Guided Optimization** | **The defining feature** — PGO inlining, unrolling, layout | 3 weeks |
| 12 | Benchmarking & Evidence | Benchmark suite, speedup measurements, report | 1.5 weeks |
| 13 | Stretch Goals | Arrays, `for`/`break`/`continue`, more passes, more targets | open |

**Total to a defensible, complete project: ~26 weeks of part-time work (Phases 0–12).**
**Minimum viable demonstrable project: Phases 0–4 + 9 + 10 + a single PGO decision (~12 weeks).**

---

## 4. Phase Detail

### Phase 0 — Foundation & Tooling
**Goal:** Nothing compiles yet, but everything is ready to.

- [ ] Repo layout (`src/`, `include/`, `tests/`, `bench/`, `metrics/`, `examples/`, `context/`, `docs/`).
- [ ] CMake build (C++20), warnings-as-errors, sanitizer build variants.
- [ ] `optiforge` CLI skeleton — argument parsing, `--help`, `--version`, `--emit=`, `-O<n>`, `-o`.
- [ ] Diagnostics engine: `SourceLocation`, `DiagnosticEngine`, severity levels, caret-underline rendering.
- [ ] Test harness: golden-file runner comparing stage dumps (CTest driver).
- [ ] `docs/` skeleton + this `context/` set committed.

**Exit criteria:** `optiforge --version` runs; the test runner executes with zero tests and passes; a clean build produces no warnings.

---

### Phase 1 — Lexer & Parser
**Goal:** Source text → AST.

- [ ] Token definition (`TokenKind` enum, `Token` struct with location + lexeme).
- [ ] Hand-written lexer: keywords, identifiers, int/float literals, all operators, punctuation, `//` and `/* */` comments, whitespace, EOF.
- [ ] Lexical error recovery (unterminated comment, bad character, malformed number).
- [ ] AST node hierarchy (`Node` base, `Expr`/`Stmt`/`Decl` branches, `std::unique_ptr` children).
- [ ] Recursive-descent parser with precedence climbing for binary expressions.
- [ ] Grammar coverage: variable declarations, assignment, `if`/`else`, `while`, `return`, function declarations, calls, unary/binary expressions, literals, parenthesized expressions.
- [ ] Parser error recovery (panic-mode sync on `;` and `}`) so multiple errors are reported per run.
- [ ] `--emit=tokens` and `--emit=ast` (indented tree printer).

**Exit criteria:** All `examples/*.of` parse; `tests/parser/` golden AST dumps pass; malformed inputs produce sensible, located diagnostics without crashing.

---

### Phase 2 — Semantic Analysis
**Goal:** AST → Typed AST, with all static errors caught.

- [ ] Scoped symbol table (stack of hash maps, or a scope tree).
- [ ] Two-pass over functions so forward references and mutual recursion work.
- [ ] Checks: undeclared variable, duplicate declaration, use-before-declaration, type mismatch in assignment/binop/condition, wrong argument count, wrong argument types, wrong or missing return type, calling an undeclared function, `void` misuse.
- [ ] Implicit conversion rules (decide and document: `int` → `float` yes, everything else explicit or an error).
- [ ] Annotate every expression node with a resolved `Type*`; annotate every identifier with its `Symbol*`.
- [ ] `--emit=ast` shows types after this phase.

**Exit criteria:** `tests/sema/` — every negative test produces exactly the expected message and location; every positive test type-checks clean.

---

### Phase 3 — IR & CFG
**Goal:** Typed AST → three-address IR organized into a CFG.

- [ ] IR value model: `Value` base → `Constant`, `Argument`, `Instruction`; `Instruction` has a type and an operand list.
- [ ] Container hierarchy: `Module` → `Function` → `BasicBlock` → `Instruction`.
- [ ] Instruction set v1: `Add Sub Mul Div Mod Neg`, `ICmp FCmp`, `Load Store Alloca`, `Br CondBr`, `Call Ret`, `Phi` (reserved for Phase 6), plus `SIToFP`/`FPToSI` if conversions are supported.
- [ ] AST → IR lowering: expressions to temporaries, control flow (`if`/`while`) to blocks and branches, locals as `Alloca` in the entry block.
- [ ] CFG: predecessor/successor lists, entry block, reachability, unreachable-block pruning.
- [ ] IR verifier: every block ends in exactly one terminator, operands dominate uses (post-SSA), types agree per instruction.
- [ ] `--emit=ir` (textual IR) and `--emit=cfg` (Graphviz DOT).

**Exit criteria:** Every example lowers to verified IR; `--emit=cfg` piped to `dot` renders correct graphs for nested `if`/`while`.

---

### Phase 4 — Naive Backend ⭐ *Vertical slice — the most important milestone*
**Goal:** **A real, running executable.** Correctness only; performance explicitly not a goal.

- [ ] x86-64 instruction representation (`MachineInstr`, `MachineOperand`: register/immediate/memory/label).
- [ ] Trivial instruction selection: one IR instruction → a small fixed sequence.
- [ ] Trivial register allocation: every value lives in a stack slot; load operands into scratch registers, compute, store back.
- [ ] Stack frame layout, prologue/epilogue, System V AMD64 calling convention (integer args in `rdi rsi rdx rcx r8 r9`, return in `rax`/`xmm0`).
- [ ] Assembly emission to `.s`.
- [ ] Assemble and link via the system toolchain (`gcc`/`clang` as assembler + linker driver).
- [ ] Minimal runtime: `print_int`, `print_float`, program entry shim.

**Exit criteria:** ⭐ `optiforge examples/fib.of -o fib && ./fib` prints the correct Fibonacci number. This is the moment the project becomes real.

---

### Phase 5 — Analysis Framework
**Goal:** Reusable, cached analyses that optimizations consume.

- [ ] `AnalysisManager`: lazy computation, result caching, invalidation on IR mutation.
- [ ] Dominator tree (Lengauer–Tarjan, or the iterative Cooper–Harvey–Kennedy algorithm).
- [ ] Dominance frontiers (needed for SSA).
- [ ] Post-dominator tree.
- [ ] Natural loop detection via back edges; `LoopInfo` with nesting, headers, latches, exits, and preheader insertion.
- [ ] Use-def and def-use chains.
- [ ] Liveness analysis (backward dataflow, live-in/live-out per block).
- [ ] Reaching definitions (forward dataflow).
- [ ] A generic dataflow driver so future analyses are roughly 50 lines each.

**Exit criteria:** `tests/analysis/` golden dumps for dominator trees, loop nests, and live sets on hand-checked CFGs.

---

### Phase 6 — SSA Form
**Goal:** IR in SSA so optimizations become simple and precise.

- [ ] `mem2reg`: promote non-address-taken `Alloca`s to SSA registers.
- [ ] Phi insertion using dominance frontiers (Cytron et al.).
- [ ] Variable renaming via a dominator-tree walk.
- [ ] SSA verifier: single definition per value, all uses dominated by their definition, phi arity matches predecessor count.
- [ ] SSA destruction before register allocation (phi → parallel copies, with the lost-copy and swap problems handled).

**Exit criteria:** All examples round-trip through SSA construction → destruction → codegen with identical program output. The verifier passes on every function.

---

### Phase 7 — Classical Optimizations
**Goal:** A real `-O1`/`-O2` pipeline, all profile-independent.

Pass order (initial proposal, to be tuned):
`mem2reg → constant folding → sparse conditional constant propagation → copy propagation → CSE (GVN) → DCE → strength reduction → LICM → simplify-CFG → DCE`

- [ ] Constant folding (peephole on constant operands, including `x*1`, `x+0`, `x*0`).
- [ ] Constant propagation (SCCP preferred — folds and prunes branches together).
- [ ] Copy propagation.
- [ ] Common subexpression elimination (dominator-based GVN).
- [ ] Dead code elimination (mark-and-sweep over SSA use lists) and dead store elimination.
- [ ] Strength reduction (`*2^k` → shift, `/2^k` → shift, `%2^k` → mask, `*2` → `x+x`).
- [ ] Loop-invariant code motion (uses `LoopInfo` and preheaders).
- [ ] CFG simplification (merge blocks, remove empty blocks, fold constant branches).
- [ ] Static (non-PGO) function inlining with a size heuristic.
- [ ] `-O0`/`-O1`/`-O2` pipeline definitions; `--print-after-all` for pass debugging.

**Exit criteria:** For every optimization level, all tests produce output identical to `-O0`. Instruction counts drop measurably. `tests/opt/` holds golden IR per pass.

---

### Phase 8 — Real Register Allocation
**Goal:** Replace the Phase-4 stack allocator with graph coloring.

- [ ] Live-range construction from liveness.
- [ ] Interference graph construction.
- [ ] Chaitin–Briggs coloring: simplify / coalesce / freeze / spill / select.
- [ ] Spill code insertion with reload minimization; spill-cost heuristic using loop depth (and later, profile weight).
- [ ] Callee-saved and caller-saved register discipline; ABI-correct clobber handling around calls.
- [ ] Register-pressure-aware verification.

**Exit criteria:** All tests still pass; measurable reduction in memory traffic versus Phase 4; register-pressure stress tests do not miscompile.

---

### Phase 9 — Instrumentation & Runtime Profiling
**Goal:** `optiforge prog.of --profile -o prog_inst` produces a binary that writes `prog.prof` on exit.

- [ ] Instrumentation pass over IR, run late (after optimization) so IDs are stable and the instrumented layout matches the optimized one.
- [ ] Counter allocation: one per function entry, one per basic block, one per branch edge (taken/not-taken), one per loop header.
- [ ] Efficient counter increments — a direct memory increment on a static counter array, no function call in the hot path.
- [ ] Optional lightweight timing for function-level wall time (opt-in).
- [ ] Runtime library `libofprof`: counter storage, `atexit` dump hook, `.prof` writer, environment overrides (`OPTIFORGE_PROFILE_OUT`).
- [ ] **Stable IDs:** deterministic `function:block` naming that survives recompilation, plus a source-hash stamp in the profile header.
- [ ] `.prof` format specification (documented in `architectural_design.md`), text-first for debuggability.

**Exit criteria:** Running the instrumented binary yields a `.prof` whose counts are hand-verifiable on a small loop program. Instrumentation overhead documented (target: under 40% slowdown).

---

### Phase 10 — Profile Ingestion & Hot-Path Detection
**Goal:** Turn a `.prof` file into decisions the optimizer can query.

- [ ] `.prof` parser and validator (version check, source-hash check, malformed-file tolerance).
- [ ] `ProfileData` analysis: per-function counts, per-block counts, per-edge probabilities, loop trip counts.
- [ ] Block-frequency propagation to fill gaps and sanity-check flow conservation.
- [ ] Hot/Warm/Cold classification with a configurable threshold (`--hot-threshold=`), defaulting to a percentile rule rather than an absolute count.
- [ ] Staleness handling: if the source hash differs, warn and either degrade to partial matching or ignore the profile — **never miscompile**.
- [ ] `optiforge --profile-report=prog.prof` — human-readable hot function, hot loop, and branch-bias report.

**Exit criteria:** The report correctly names the hot function and hot loop in a benchmark designed to have exactly one of each. A corrupt or stale profile produces a warning and a correct binary.

---

### Phase 11 — Profile-Guided Optimization ⭐ *The defining feature*
**Goal:** The optimizer changes its decisions based on measured behaviour.

- [ ] `ProfileData` wired into the `AnalysisManager` and queryable by any pass.
- [ ] **PGO inlining:** raise the size budget for hot call sites, refuse to inline cold ones.
- [ ] **PGO loop unrolling:** unroll only hot loops; choose the factor from measured trip counts.
- [ ] **PGO LICM aggressiveness:** spend more compile effort on hot loops.
- [ ] **Profile-guided register allocation:** spill cost weighted by real execution counts instead of static loop depth.
- [ ] **Basic block layout / branch reordering:** lay out the likely path to fall through; sink cold blocks to the end of the function.
- [ ] **Cold-code size mode:** minimal optimization on cold paths.
- [ ] Every PGO pass must have a documented fallback for when no profile exists.

**Exit criteria:** ⭐ The North Star sentence in §1 is demonstrably true, with numbers.

---

### Phase 12 — Benchmarking & Evidence
**Goal:** Prove it. Numbers, not claims.

- [ ] Benchmark suite: matrix multiply, n-body, sieve, recursive fib, an array/loop kernel, a branch-heavy state machine.
- [ ] Harness: run each benchmark at `-O0`, `-O1`, `-O2`, and `-O2 --use-profile`, N repetitions, reporting median and variance.
- [ ] Metrics: wall time, instruction count, generated code size, compile time, instrumentation overhead.
- [ ] Results checked into `metrics/results/` with the machine specification recorded.
- [ ] Write-up: which PGO decisions produced which gains, and where PGO *lost* (honesty is part of the deliverable).

**Exit criteria:** A results table showing PGO beating `-O2` on at least three benchmarks, with the wins explained mechanistically.

---

### Phase 13 — Stretch Goals *(only after 0–12 are done)*
- **Language:** arrays, `for`, `break`/`continue`, strings, structs, pointers.
- **Optimization:** vectorization, tail-call elimination, jump threading, inter-procedural constant propagation, partial redundancy elimination.
- **Backend:** instruction scheduling, an ARM64 target, direct object-file emission (no external assembler).
- **Profiling:** sampling-based profiler, value profiling for indirect-call specialization, Ball–Larus edge-profile minimization.
- **Infrastructure:** fuzzing the frontend, differential testing against a reference C compiler.

---

## 5. Dependency Graph

```
P0 ──> P1 ──> P2 ──> P3 ──> P4 ⭐ (first executable)
                       │      │
                       └──> P5 ──> P6 ──> P7 ──> P8
                                            │      │
                                            └──────┴──> P9 ──> P10 ──> P11 ⭐ ──> P12
```

**Critical path to a demo:** P0 → P1 → P2 → P3 → P4. Everything after that improves an already-working compiler.
**Critical path to the thesis of the project:** P9 → P10 → P11.

---

## 6. Milestones

| # | Milestone | Phase | Demonstrable proof |
|---|---|---|---|
| M1 | "It parses" | P1–P2 | `--emit=ast` on a typed program; errors on a broken one |
| M2 | "It has an IR" | P3 | `--emit=ir` and `--emit=cfg` render a loop's CFG |
| M3 | ⭐ **"It runs"** | P4 | `./fib` prints 832040 |
| M4 | "It optimizes" | P7 | Same output, roughly 30–50% fewer IR instructions at `-O2` |
| M5 | "It allocates registers" | P8 | Generated assembly keeps values in registers across a loop |
| M6 | "It profiles" | P9–P10 | A `.prof` file plus a hot-path report identifying the real hot loop |
| M7 | ⭐ **"PGO beats -O2"** | P11–P12 | Benchmark table with measured speedup |

---

## 7. Risk Register

| Risk | Impact | Likelihood | Mitigation |
|---|---|---|---|
| **Target-OS mismatch** — the spec says x86-64 **Linux**, the dev machine is **Windows 11** | High | Certain | Decide early: WSL2 as the canonical target (recommended), or add a Windows x64 / Microsoft-ABI backend. See `deployment.md` §2. Do not defer past Phase 4. |
| Scope creep in the language (structs, pointers, generics) | High | High | The language is frozen at the agreed feature set until Phase 12 is complete. Phase 13 only. |
| SSA construction/destruction bugs | High | Medium | Verifier after every pass; differential testing `-O0` vs `-On`; keep SSA behind a flag until stable |
| Register allocator miscompiles under pressure | High | Medium | Keep the Phase-4 stack allocator as a permanent fallback (`--regalloc=naive`); stress tests with more than 16 simultaneously live values |
| Profile IDs unstable across recompiles, so PGO silently no-ops | High | High | Deterministic ID scheme, source hash in the header, and a loud warning on mismatch (Phases 9–10) |
| Instrumentation perturbs the behaviour it measures | Medium | Medium | Instrument late in the pipeline; measure and document overhead; prefer edge counters over block counters where equivalent |
| PGO shows no measurable win | Medium | Medium | Design benchmarks with genuinely biased branches and hot loops; if a pass shows no win, report that honestly — a negative result with analysis is still a result |
| Debugging generated assembly consumes all available time | Medium | High | Invest early in `--emit=asm` readability, IR-to-source line comments, and a small assembly test harness |

---

## 8. Definition of Done (project level)

- [ ] `optiforge` builds clean from a fresh clone with one documented command.
- [ ] All Phase 0–12 exit criteria met.
- [ ] Test suite green: lexer, parser, sema, IR, analysis, opt, codegen, end-to-end, PGO.
- [ ] Benchmark results committed with machine specifications.
- [ ] `docs/` explains: the language, the IR, the `.prof` format, how to add a new pass, how to add a new target.
- [ ] A written report answering: *what did profile guidance actually buy, and why?*
