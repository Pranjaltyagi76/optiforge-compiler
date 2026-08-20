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
| 7.5 | Correctness Sweep | Differential fuzzing; eight defects found and fixed | 0.5 week |
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

- [x] Repo layout (`src/`, `include/`, `tests/`, `bench/`, `metrics/`, `examples/`, `context/`, `docs/`).
- [x] CMake build (C++20), warnings-as-errors, sanitizer build variants.
- [x] `optiforge` CLI skeleton — argument parsing, `--help`, `--version`, `--emit=`, `-O<n>`, `-o`.
- [x] Diagnostics engine: `SourceLocation`, `DiagnosticEngine`, severity levels, caret-underline rendering.
- [x] Test harness: golden-file runner comparing stage dumps (CTest driver).
- [x] `docs/` skeleton + this `context/` set committed.

**Status: COMPLETE** (verified: clean build, `optiforge --version`, 2/2 test suites green)

**Exit criteria:** `optiforge --version` runs; the test runner executes with zero tests and passes; a clean build produces no warnings.

---

### Phase 1 — Lexer & Parser
**Goal:** Source text → AST.

- [x] Token definition (`TokenKind` enum, `Token` struct with location + lexeme).
- [x] Hand-written lexer: keywords, identifiers, int/float literals, all operators, punctuation, `//` and `/* */` comments, whitespace, EOF.
- [x] Lexical error recovery (unterminated comment, bad character, malformed number).
- [x] AST node hierarchy (`Node` base, `Expr`/`Stmt`/`Decl` branches, `std::unique_ptr` children).
- [x] Recursive-descent parser with precedence climbing for binary expressions.
- [x] Grammar coverage: variable declarations, assignment, `if`/`else`, `while`, `return`, function declarations, calls, unary/binary expressions, literals, parenthesized expressions.
- [x] Parser error recovery (panic-mode sync on `;` and `}`) so multiple errors are reported per run.
- [x] `--emit=tokens` and `--emit=ast` (indented tree printer).

**Status: COMPLETE** (verified: both examples parse, 79 unit tests + 15 golden cases green, clean -Werror build)

**Exit criteria:** All `examples/*.of` parse; `tests/parser/` golden AST dumps pass; malformed inputs produce sensible, located diagnostics without crashing.

---

### Phase 2 — Semantic Analysis
**Goal:** AST → Typed AST, with all static errors caught.

- [x] Scoped symbol table (stack of hash maps, or a scope tree).
- [x] Two-pass over functions so forward references and mutual recursion work.
- [x] Checks: undeclared variable, duplicate declaration, use-before-declaration, type mismatch in assignment/binop/condition, wrong argument count, wrong argument types, wrong or missing return type, calling an undeclared function, `void` misuse.
- [x] Implicit conversion rules (decide and document: `int` → `float` yes, everything else explicit or an error).
- [x] Annotate every expression node with a resolved `Type*`; annotate every identifier with its `Symbol*`.
- [x] `--emit=ast` shows types after this phase.

**Status: COMPLETE** (verified: 144 unit assertions + 18 golden cases green, clean -Werror build, both examples typecheck)

**Exit criteria:** `tests/sema/` — every negative test produces exactly the expected message and location; every positive test type-checks clean.

---

### Phase 3 — IR & CFG
**Goal:** Typed AST → three-address IR organized into a CFG.

- [x] IR value model: `Value` base → `Constant`, `Argument`, `Instruction`; `Instruction` has a type and an operand list.
- [x] Container hierarchy: `Module` → `Function` → `BasicBlock` → `Instruction`.
- [x] Instruction set v1: `Add Sub Mul Div Mod Neg`, `ICmp FCmp`, `Load Store Alloca`, `Br CondBr`, `Call Ret`, `Phi` (reserved for Phase 6), plus `SIToFP`/`FPToSI` if conversions are supported.
- [x] AST → IR lowering: expressions to temporaries, control flow (`if`/`while`) to blocks and branches, locals as `Alloca` in the entry block.
- [x] CFG: predecessor/successor lists, entry block, reachability, unreachable-block pruning.
- [x] IR verifier: every block ends in exactly one terminator, operands dominate uses (post-SSA), types agree per instruction.
- [x] `--emit=ir` (textual IR) and `--emit=cfg` (Graphviz DOT).

**Status: COMPLETE** (verified: 203 unit assertions + 22 golden cases green, clean -Werror build, both examples lower to verified IR. Note: DOT output validated structurally, not rendered -- Graphviz is not installed on this machine.)

**Exit criteria:** Every example lowers to verified IR; `--emit=cfg` piped to `dot` renders correct graphs for nested `if`/`while`.

---

### Phase 4 — Naive Backend ⭐ *Vertical slice — the most important milestone*
**Goal:** **A real, running executable.** Correctness only; performance explicitly not a goal.

- [x] x86-64 instruction representation (`MachineInstr`, `MachineOperand`: register/immediate/memory/label).
- [x] Trivial instruction selection: one IR instruction → a small fixed sequence.
- [x] Trivial register allocation: every value lives in a stack slot; load operands into scratch registers, compute, store back.
- [x] Stack frame layout, prologue/epilogue, System V AMD64 calling convention (integer args in `rdi rsi rdx rcx r8 r9`, return in `rax`/`xmm0`).
- [x] Assembly emission to `.s`.
- [x] Assemble and link via the system toolchain (`gcc`/`clang` as assembler + linker driver).
- [x] Minimal runtime: `print_int`, `print_float`, program entry shim.

**Status: COMPLETE** (verified: `optiforge examples/fib.of -o fib.exe && ./fib.exe` prints 832040; 232 unit assertions, 23 golden cases, 8 end-to-end programs across -O0/-O1/-O2; clean -Werror build in both Debug and Release; installed compiler links standalone.)

**Exit criteria:** ⭐ `optiforge examples/fib.of -o fib && ./fib` prints the correct Fibonacci number. This is the moment the project becomes real.

---

### Phase 5 — Analysis Framework
**Goal:** Reusable, cached analyses that optimizations consume.

- [x] `AnalysisManager`: lazy computation, result caching, invalidation on IR mutation.
- [x] Dominator tree (Lengauer–Tarjan, or the iterative Cooper–Harvey–Kennedy algorithm).
- [x] Dominance frontiers (needed for SSA).
- [x] Post-dominator tree.
- [x] Natural loop detection via back edges; `LoopInfo` with nesting, headers, latches, exits, and preheader insertion.
- [x] Use-def and def-use chains.
- [x] Liveness analysis (backward dataflow, live-in/live-out per block).
- [x] Reaching definitions (forward dataflow).
- [x] A generic dataflow driver so future analyses are roughly 50 lines each.

**Status: COMPLETE** (verified: 270 unit assertions, 28 golden cases including five hand-checked analysis dumps, 8 end-to-end programs at three optimization levels; clean -Werror build in Debug and Release. Preheader insertion is implemented as a transform, not an analysis, to respect rule 4.)

**Exit criteria:** `tests/analysis/` golden dumps for dominator trees, loop nests, and live sets on hand-checked CFGs.

---

### Phase 6 — SSA Form
**Goal:** IR in SSA so optimizations become simple and precise.

- [x] `mem2reg`: promote non-address-taken `Alloca`s to SSA registers.
- [x] Phi insertion using dominance frontiers (Cytron et al.).
- [x] Variable renaming via a dominator-tree walk.
- [x] SSA verifier: single definition per value, all uses dominated by their definition, phi arity matches predecessor count.
- [x] SSA destruction before register allocation (phi → parallel copies, with the lost-copy and swap problems handled).

**Status: COMPLETE** (verified: 296 unit assertions, 30 golden cases, 10 end-to-end programs run at -O0/-O1/-O2 with byte-identical output -- which is the SSA round-trip test; clean -Werror build in Debug and Release. The swap problem is covered by a dedicated end-to-end regression.)

**Exit criteria:** All examples round-trip through SSA construction → destruction → codegen with identical program output. The verifier passes on every function.

---

### Phase 7 — Classical Optimizations
**Goal:** A real `-O1`/`-O2` pipeline, all profile-independent.

Pass order (initial proposal, to be tuned):
`mem2reg → constant folding → sparse conditional constant propagation → copy propagation → CSE (GVN) → DCE → strength reduction → LICM → simplify-CFG → DCE`

- [x] Constant folding (peephole on constant operands, including `x*1`, `x+0`, `x*0`).
- [x] Constant propagation (SCCP preferred — folds and prunes branches together).
- [x] Copy propagation.
- [x] Common subexpression elimination (dominator-based GVN).
- [x] Dead code elimination (mark-and-sweep over SSA use lists). **Dead store elimination deferred** (OPT-13, priority S): mem2reg already removes the stores that matter, leaving nothing measurable for it to do.
- [x] Strength reduction (`*2^k` → shift, `/2^k` → shift, `%2^k` → mask, `*2` → `x+x`).
- [x] Loop-invariant code motion (uses `LoopInfo` and preheaders).
- [x] CFG simplification: fold constant branches, prune unreachable blocks, and merge a
      block into its only predecessor. **Threading an empty block with several
      predecessors is not done** (OPT-14, priority S) -- it needs phi merging in every
      predecessor and buys nothing the merge above does not already get.
- [x] Static (non-PGO) function inlining, **restricted to single-block callees**, plus module-level removal of functions left uncalled.
- [x] `-O0`/`-O1`/`-O2` pipeline definitions; `--print-after-all` for pass debugging.

**Status: COMPLETE** (verified: 334 unit assertions, 32 golden cases, 10 end-to-end programs byte-identical at -O0/-O1/-O2; clean -Werror in Debug and Release. Metric O-04 = 46.0% against a 40% target, recorded in metrics/results/2026-08-19-phase7-ir-reduction.md.)

**Deferred, both priority S:** dead store elimination (nothing left for it after mem2reg) and static loop unrolling (better done with the trip counts Phase 11 measures than with a static guess).

**Exit criteria:** For every optimization level, all tests produce output identical to `-O0`. Instruction counts drop measurably. `tests/opt/` holds golden IR per pass.

---

### Phase 7.5 — Correctness sweep before Phase 8
**Goal:** Do not build a register allocator on top of a miscompiling optimizer.

A review of Phases 3-7 plus a differential fuzzer -- random well-typed programs
compiled at `-O0`/`-O1`/`-O2`, run, and compared against `-O0` -- turned up eight
defects. Full write-up with the numbers:
[`metrics/results/2026-08-19-pre-phase8-correctness-sweep.md`](../metrics/results/2026-08-19-pre-phase8-correctness-sweep.md).

- [x] **SCCP tracked reachable blocks, not executable edges.** A phi in a block
      already visited was never re-evaluated when its second predecessor came
      alive, so it kept the value from the first edge. Miscompile.
- [x] **SCCP left values at `Unknown` forever.** `bool == bool` is legal source its
      evaluator declined to fold; the result never settled, a branch on it marked
      neither successor executable, and both arms were deleted as dead. Miscompile.
- [x] **Float comparison ignored NaN.** `comisd` sets ZF, PF and CF together for an
      unordered pair, so `sete` could not tell "equal" from "unordered" and the
      below family reported true where IEEE-754 says false.
- [x] **Liveness charged phi operands to the wrong block.** They were added after
      the backward walk, so a value was live across the block defining it --
      exactly the over-approximation that gives Phase 8 interferences that are not
      real.
- [x] **`simplify-cfg` mishandled `condbr %c, X, X`**, dropping both of the block's
      phi entries instead of the one edge that actually went away.
- [x] **`insertLoopPreheaders` redirected edges without rewriting the header's
      phis.** Now merges the incoming values, with a phi in the preheader when they
      differ.
- [x] **SSA destruction iterated an `unordered_map`**, making copy emission order --
      and the temporary names that follow from it -- depend on heap addresses (NFR-06).
- [x] **`--verify-each` did not check phi arity**, which is the invariant a
      CFG-editing pass is most likely to break. Moved into `ir::Verifier` so the
      pass responsible gets named.

Three new differential programs (`bool_compare.of`, `loop_phi_edges.of`,
`nan_compare.of`) and eight unit tests cover the lot.

**Status: COMPLETE** (verified: 341 unit assertions, 32 golden cases, 39 end-to-end
runs across three optimization levels, `--verify-each` clean on every test program,
and ~400 fuzzer-generated programs with no divergence between levels. Metric O-04
rose from 46.0% to 47.7%.)

---

### Phase 8 — Real Register Allocation
**Goal:** Replace the Phase-4 stack allocator with graph coloring.

- [x] Live-range construction. **Over allocation units, not values** — the two
      answer different questions. SSA destruction leaves several values sharing
      one location, and no single value's range describes that; taking the union
      over-approximates so badly that two locations in unrelated loops appear
      live everywhere. Same dataflow driver (AN-11), different domain.
- [x] Interference graph construction, from the def-point rule: two units
      interfere when one is live where the other is defined. Arguments are
      defined by the calling convention at the function entry, which is a
      definition point no walk over instructions ever reaches, so it is added
      explicitly.
- [x] Chaitin–Briggs coloring: simplify / coalesce / freeze / spill / select, as
      one interleaved loop rather than separate phases. Simplify skips
      move-related nodes so a copy keeps its chance to be coalesced; freeze is
      what breaks the deadlock when nothing simplifies and nothing coalesces,
      giving up a copy rather than spilling the node holding it. Degrees are
      recomputed rather than maintained incrementally — O(n²) against the
      textbook's O(n), immeasurable at tens of units per function, and it is
      where this algorithm is usually got wrong.
- [x] Spilling, with a cost heuristic of Σ 10^loopDepth over uses and defs,
      divided by degree. **No spill-code insertion pass, deliberately:** the code
      generator already computes with values that live in memory — that is all
      the naive allocator ever did — so "spill" here means only "assign no
      register". The reloads use the reserved scratch registers, which are not
      allocatable and so add no live range the graph would need to know about,
      which is what makes the usual build/spill/rebuild loop unnecessary.
- [x] Callee-saved and caller-saved discipline: a value live across a call is
      refused every caller-saved register, and the prologue saves exactly the
      callee-saved registers the assignment used — `push`/`pop` for the
      general-purpose ones, `movups` to a frame slot for the SSE ones, which
      have no push.
- [x] Register-pressure verification, run on **every** compilation rather than
      behind a flag. It recomputes live ranges independently and checks every
      pair live at every point, where the allocator only adds edges at
      definition points; the two are equivalent when the ranges are exact and
      diverge exactly where they are not. It found the missing argument edges,
      and it refuses to emit an assignment it cannot vouch for.
- [x] `--regalloc=naive|graph` (ADR-08), and the whole end-to-end suite runs
      through both.

**No pre-colouring.** `idiv` wants `rax`/`rdx`, a variable shift wants `cl`, a
call writes every argument register in turn. Rather than model those as
pre-coloured nodes, the target keeps them out of the allocatable pool: eight
integer registers instead of fourteen, and an allocator whose correctness
argument fits on a page. Recorded as a deliberate trade, not an oversight.

**Status: COMPLETE** (verified: 356 unit assertions, 36 golden cases, 48
end-to-end runs at three optimization levels plus 32 more through
`--regalloc=naive`, clean -Werror build in Debug and Release, and fuzzer
programs comparing four configurations -- `-O1`, `-O2`, and both allocators --
against `-O0`. Metric BE-04 = 61.0% fewer frame accesses at `-O2`, recorded in
metrics/results/2026-08-19-phase8-register-allocation.md.)

**Exit criteria:** All tests still pass; measurable reduction in memory traffic versus Phase 4; register-pressure stress tests do not miscompile.

---

### Phase 9 — Instrumentation & Runtime Profiling
**Goal:** `optiforge prog.of --profile -o prog_inst` produces a binary that writes `prog.prof` on exit.

- [x] Instrumentation pass over IR, run late (ADR-05) so the measured CFG is the
      optimized one the profile-guided build will also see. New `profinc` opcode,
      which is why the pass needed no special case anywhere in the printer,
      verifier or code generator.
- [x] Counter allocation. **One counter per basic block, and nothing else.** The
      function entry count, both branch outcomes and the loop entry and iteration
      counts are all *derived* from block counters by the compiler, which writes
      the derivation into a static table the runtime walks. Four counter kinds
      would be four times the hot-path cost and four numbers that can disagree
      with each other; deriving them makes the four arithmetically consistent by
      construction. Branch outcomes are derivable because critical edges are
      split first, after which each successor of a conditional branch has exactly
      one predecessor and its block counter *is* that edge's count.
- [x] Efficient increments: one `incq __ofprof_counters+8n(%rip)`. No call, no
      register saved, nothing the register allocator has to know about.
- [x] Optional per-function wall time behind `--profile-time`, off by default
      because unlike a counter increment it is a real call. Recursion is charged
      for its whole call tree once rather than once per level.
- [x] `libofprof`: counter storage, `atexit` dump registered from a constructor
      so the program's source needs no change (PROF-07), the `.prof` writer, and
      `$OPTIFORGE_PROFILE_OUT` overriding the path baked in at compile time.
      Linked with `--whole-archive` and only for `--profile` builds: it works
      entirely through that constructor, so an ordinary archive link would find
      no undefined symbol, drop the object, and silently write no profile.
- [x] **Stable IDs:** counters are numbered in block order per function, and the
      records name `function block` exactly as the IR labels them. The header
      carries the source hash, the `-O` level and the compiler version, which is
      what lets Phase 10 tell a stale profile from a current one.
- [x] `.prof` format specification: [`docs/profile-format.md`](../docs/profile-format.md),
      line-oriented text (ADR-07), versioned, with the validation table Phase 10
      will implement.

**Deferred, priority C:** merging several runs into one profile (PROF-14). The
`RUNS` header field exists and is always 1, so adding it later is a reader
change rather than a format change.

**Status: COMPLETE** (verified: 4 profile tests whose counts are written into the
source by hand before the compiler is run, PROF-11 checked on every end-to-end
program at every optimization level and on fuzzer-generated programs, clean
-Werror build in Debug and Release. Metric PROF-13 = 12.9% worst-case overhead
against a 40% target, recorded in
metrics/results/2026-08-19-phase9-instrumentation.md.)

**Exit criteria:** Running the instrumented binary yields a `.prof` whose counts are hand-verifiable on a small loop program. Instrumentation overhead documented (target: under 40% slowdown).

---

### Phase 10 — Profile Ingestion & Hot-Path Detection
**Goal:** Turn a `.prof` file into decisions the optimizer can query.

- [x] `.prof` parser and validator in `of_profile`, which depends on `of_support`
      and nothing else (architectural_design.md rule 6). Version check, source
      hash, unknown record types ignored once for forward compatibility,
      malformed lines dropped one warning per kind rather than one per line.
- [x] `ProfileData`: per-function counts, per-block counts, branch probabilities,
      loop trip counts, and per-function wall time when it was collected.
- [x] Flow conservation, checked from the profile alone: a branch's two outcomes
      must sum to its block's count, and a loop's entries plus iterations must
      sum to its header's. Both follow from where the counters are placed, so a
      violation means the file is not describing a real execution. Reported;
      counts kept as advisory. **Block-frequency propagation is not needed and
      not implemented** — every block carries its own counter, so there are no
      gaps to fill.
- [x] Hot/Warm/Cold classification, relative with an absolute floor, threshold
      configurable by `--hot-threshold=` (PGO-04). `Unknown` is a fourth state
      and not a synonym for cold: "never ran" and "never measured" justify
      opposite decisions.
- [x] **Functions are weighted by the work done inside them, not by their entry
      count.** A departure from §15.1, and the reason for it is in the fixture:
      `compute` is called twenty times, runs five thousand iterations each, holds
      99.98% of the execution — and the entry-count rule calls it cold.
- [x] Staleness: hash mismatch, `-O` level mismatch and a sub-50% match rate each
      produce their own warning, and all of them still produce a correct binary.
- [x] `--profile-report=<file>` as a mode of its own, needing no input program.
- [x] `ProfileAnalysis` binds a loaded profile into the AnalysisManager (PGO-02).
      Passes reach it through `getCached`, which returns null when no profile was
      supplied — the fallback path every profile-guided pass must have.

**Status: COMPLETE** (verified: 21 profile unit tests, 7 golden cases covering
the report and every rejection path, and fault injection in `tests/run_profile.py`
that recompiles every profile program with a missing, corrupt and mismatched
profile and requires byte-identical output. Clean -Werror build in Debug and
Release.)

**Exit criteria:** The report correctly names the hot function and hot loop in a benchmark designed to have exactly one of each. A corrupt or stale profile produces a warning and a correct binary.

Both demonstrated: `tests/pgo/fixtures/hotpath.of` has exactly one hot function
(`compute`, 99.98% of executions from twenty calls) and one hot loop
(`compute:while.cond.1`, trip count 5000), with `rarely` cold and one block never
executed. The report on it is checked in as `tests/golden/profile_report.expected`.

---

### Phase 11 — Profile-Guided Optimization ⭐ *The defining feature*
**Goal:** The optimizer changes its decisions based on measured behaviour.

- [x] `ProfileData` queryable by any pass through `getCached<ProfileAnalysis>`.
      **A supplied profile is not invalidated when the IR changes**, unlike every
      derived analysis: nothing a pass does can change what the program did when
      it ran. Before that distinction existed the first pass to report a change
      threw the profile away and every decision after it silently took the
      no-profile path while appearing to work.
- [x] **PGO inlining:** budget 12 with no profile, 250 at a hot call site, and a
      cold call site is not inlined at any size. Honest limit: the inliner still
      only handles single-block callees, so the *block count* restriction binds
      long before the budget does. Multi-block inlining is recorded as open.
- [x] **PGO loop unrolling**, and it does nothing at all without a profile —
      which is what Phase 7 deferred static unrolling for. Every copy re-tests
      the exit condition, so the measured trip count being an *average* rather
      than a bound costs code size and never correctness.
- [ ] **PGO LICM aggressiveness.** Not implemented, and on reflection not worth
      implementing: LICM here is already exhaustive rather than budgeted, so
      "spend more effort on hot loops" has no effort left to spend. Recorded as
      dropped rather than left looking undone.
- [x] **Profile-guided register allocation:** spill cost from measured block
      executions, falling back to 10^loopDepth when there is no profile.
- [x] **Basic block layout:** greedy hot chains, cold blocks sunk, conditions
      inverted so the hot side falls through.
- [x] **Cold-code size mode:** a cold function gets the `-O1` pipeline, derived
      from `pipelineFor(1)` rather than a second hand-maintained list.
- [x] `--pgo-remarks` explains every decision, including the ones *not* taken.
- [x] Every pass's no-profile behaviour is its normal behaviour, and is what the
      whole non-PGO test suite exercises.

**Status: COMPLETE** (verified: 379 unit assertions, 45 golden cases, 48
end-to-end runs at three levels plus 32 through `--regalloc=naive`, 5 profile
programs across 4 profile states each, and a differential fuzzer that now
includes a full profile round-trip. Clean -Werror build in Debug and Release.
Metric PGO-14 recorded in
metrics/results/2026-08-19-phase11-pgo-speedup.md.)

**Exit criteria:** ⭐ The North Star sentence in §1 is demonstrably true, with numbers.

**Demonstrated.** `bench/programs/nested_math.of` compiled with
`--use-profile` is **5.5% faster** than the same source at `-O2`, with identical
output, against a 1.4% noise floor and reproduced an hour later. The mechanism
was predicted before it was measured: the profile puts the inner loop's trip
count at 400, the unroller replicates it eight times, and 7 of every 8 back-edge
jumps disappear — 5.8% fewer instructions per iteration by arithmetic, 5.5%
measured.

Two of five programs beat `-O2` above their own noise. The other three are
explained rather than excused in §4 of the metrics write-up, and one of them
(`branchy.of`) is there because PGO first made it *slower* and the unroller now
refuses the shape that did it.

---

### Phase 12 — Benchmarking & Evidence
**Goal:** Prove it. Numbers, not claims.

- [x] Benchmark suite: eight programs in `bench/programs/`, plus three
      workload-B variants in `bench/workloads/` for profile portability.
      **Two of the six shapes `methodology.md` §2 asks for could not be
      written:** `matmul` and `sieve` both need arrays, which the language does
      not have and which the risk register below freezes until this phase
      completes. `nbody` is present with the *shape* of an n-body inner loop —
      pairwise accumulation over fixed bodies, 35 floats live against 16
      registers — and says in its own header that the physics is not real, there
      being no `sqrt`. The gap is recorded rather than papered over with two
      programs carrying the names but not the properties.
- [x] Harness: `bench/harness/run.py` builds and times every program at `-O0`,
      `-O1`, `-O2`, instrumented and `-O2 --use-profile`, 15 repetitions after
      three discarded warm-ups, **interleaved** across configurations, reporting
      median, minimum, IQR and IQR/median. It refuses to run without a machine
      spec, and writes both the immutable raw JSON and the citable table.
- [x] Metrics: wall time (Q-01), spread (Q-02), code size (Q-03), IR and machine
      instruction counts (Q-04, Q-05), spills and pressure (Q-07, Q-08), compile
      time and peak memory (P-01, P-02, P-03, P-05), instrumentation overhead
      (I-01), profile match rate (G-03).
- [x] Results checked into `metrics/results/`, and the **machine specification
      completed** — hardware, toolchain, and a measured **1.0% noise floor**,
      which was a hard blocker: the spec previously said in as many words that
      no runtime figure could be published from this machine.
- [x] Write-up: which PGO decisions produced which gains, and where PGO lost.
      `--disable-pgo=<decision>` was added to the driver so `methodology.md` §5's
      attribution protocol could run literally, one decision switched off at a
      time.

**Status: COMPLETE** (verified: 384 unit assertions, 46 golden cases, 48
end-to-end runs at three levels plus 32 through `--regalloc=naive`, 5 profile
programs across 4 profile states each, 8 benchmark programs whose five
configurations all agree byte-for-byte, and 120 randomly generated programs
through the differential fuzzer with 0 mismatches. Clean -Werror build in Debug
and Release. Four result files dated 2026-08-20 and the phase evaluation in
`metrics/reports/phase-12-evaluation.md`.)

**Exit criteria:** A results table showing PGO beating `-O2` on at least three
benchmarks, with the wins explained mechanistically.

**Met.** Three benchmarks beat `-O2` above the 1.0% noise floor, and the
attribution sweep names the decision responsible for each:

| Program | PGO vs `-O2` | Attributed to |
|---|---:|---|
| `branch_machine.of` | **+15.8%** | block layout, +16.0 |
| `nested_math.of` | +5.2% | unrolling, +5.0 |
| `loop_sum.of` | +2.1% | unrolling, +2.6 |
| `loop_kernel.of` | **−7.1%** | unrolling, **−6.8** |

Four further programs sit inside the noise floor and are listed with the rest in
`metrics/results/2026-08-20-phase12-benchmarks.md` §8.

**Three things this phase established that were previously assertions:**

1. **Two of the five profile-guided decisions do all the work**, and each on one
   program shape. Layout is worth +16 points where a function has one dominant
   path and nothing anywhere else. Unrolling is worth +5.0 and +2.6 where a short
   body is dominated by loop control — and −6.8 where it is not. Inlining,
   cold-code size mode and profile-weighted spill costs never register outside
   the noise floor in either direction, except once, negatively.
2. **PGO lost 7% on the showcase benchmark, having made the right decision.**
   `loop_kernel.of` was built so loop depth ranks its two loops backwards by six
   orders of magnitude; the profile corrects the ranking and unrolls the loop
   that really is hot, and the program gets slower, because the unroller has a
   trip-count threshold where it needs a cost model. This was **not fixed before
   reporting**, and §5 of the phase evaluation says why: any threshold that
   rescued it would be a number tuned against the one benchmark that exposed it.
3. **Layout pays in proportion to how concentrated the hot path is, not to how
   accurately the profile was measured.** The portability run drives the same
   state machine so that no single path dominates, and a *perfectly accurate*
   profile of it is worth **−4.7%**.

**The headline target NFR-10 / G-01 — ≥10% on ≥3 benchmarks — is NOT met.** One
benchmark exceeds 10%. The phase exit criterion and NFR-10 are different bars and
only the first is cleared; the evaluation says so in §1 rather than letting the
met criterion stand in for the missed target.

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
| M3 | ⭐ **"It runs"** | P4 | ✅ **REACHED** — `./fib.exe` prints 832040 |
| M4 | "It optimizes" | P7 | Same output, roughly 30–50% fewer IR instructions at `-O2` |
| M5 | **"It allocates registers"** | P8 | ✅ **REACHED** — `sum.of`'s loop body is three instructions and no memory access, down from twelve and eight |
| M6 | **"It profiles"** | P9–P10 | ✅ **REACHED** — `.prof` written and hand-verified, and the report names the hot function and hot loop of a benchmark built to have one of each |
| M7 | ⭐ **"PGO beats -O2"** | P11–P12 | ✅ **REACHED** — three benchmarks beat `-O2` above a measured 1.0% noise floor (+15.8%, +5.2%, +2.1%), each traced to a named decision by the `--disable-pgo=` attribution sweep. One benchmark loses 7%, and that is reported too. |

---

## 7. Risk Register

| Risk | Impact | Likelihood | Mitigation |
|---|---|---|---|
| ~~Target-OS mismatch~~ **RETIRED 2026-08-18** | — | — | Resolved: target is x86-64 Windows, Microsoft x64 ABI (ADR-10). Deviation from the brief is recorded and accepted; `TargetInfo` keeps a System V target open. |
| Scope creep in the language (structs, pointers, generics) | High | High | The language is frozen at the agreed feature set until Phase 12 is complete. Phase 13 only. |
| SSA construction/destruction bugs | High | Medium | Verifier after every pass; differential testing `-O0` vs `-On`; keep SSA behind a flag until stable |
| An optimization miscompiles a shape nobody wrote a test for | High | **Realized in Phase 7.5** | Hand-written tests found none of the two miscompiles the differential fuzzer found in minutes. Fuzz before declaring a phase complete, not after. |
| An optimistic analysis treats "not yet known" as "cannot happen" | High | **Realized in Phase 7.5** | Both SCCP miscompiles were this. Any lattice-based pass must guarantee every value leaves the bottom element, and every edge that can fire is eventually marked. |
| No sanitizers on MinGW, so QA-07 cannot be met | Medium | Certain | Consequence of ADR-10. Memory bugs must be caught by design and review; Phase 3 found a teardown use-after-free by inspection, which is the standard this now requires. |
| ~~Register allocator miscompiles under pressure~~ **RETIRED 2026-08-19** | — | — | Mitigated as planned: `--regalloc=naive` is kept and the whole end-to-end suite runs through it, `tests/e2e/register_pressure.of` holds 22 values live against 8 registers, and every compilation verifies the assignment against independently recomputed live ranges rather than trusting it. |
| Profile IDs unstable across recompiles, so PGO silently no-ops | High | High | Deterministic ID scheme and a source hash in the header, both landed in Phase 9. **One known gap:** instrumentation splits critical edges, so an instrumented build contains `crit.edge.N` blocks an ordinary build does not. The BRANCH and FUNCTION records that Phase 11 actually consumes name blocks that exist in both; the extra BLOCK records simply will not match, and Phase 10's match rate must not count them as staleness. |
| Instrumentation perturbs the behaviour it measures | Medium | Medium | Instrument late in the pipeline; measure and document overhead; prefer edge counters over block counters where equivalent |
| PGO shows no measurable win | Medium | **Partly realized, now quantified** | Phase 12: three of eight benchmarks win (+15.8%, +5.2%, +2.1%), four are inside the noise floor, one loses 7%. Every one is attributed to a named decision. The mitigation was "report rather than quietly drop", and that is what `metrics/reports/phase-12-evaluation.md` §0 does. **NFR-10's ≥10% on three benchmarks is missed**, with one benchmark over 10%. |
| **The unroller has no cost model** | High | **Realized in Phase 12** | It has a trip-count threshold and a body-shape guard, and neither can express "this body is already issue-limited". It is simultaneously the largest positive contributor in the attribution table (+5.0) and the largest negative (−6.8 on `loop_kernel.of`). Not patched with a constant tuned against the benchmark that exposed it; recorded as Phase 13's second priority. |
| The benchmark corpus is entirely compute-bound | Medium | Certain until arrays exist | Layout and cold-code placement would pay most on memory-bound code, and no program in the corpus is memory-bound, because `matmul` and `sieve` both need arrays. Phase 13's first priority. |
| Profile-guided layout can lose where no path dominates | Medium | **Realized in Phase 12** | Measured at −4.7% on `branch_machine_b.of`, whose state machine cycles through three paths in rotation so a *perfectly accurate* profile still gives layout nothing to commit to. Mitigation — decline to commit when no path dominates — is open. |
| Debugging generated assembly consumes all available time | Medium | High | Invest early in `--emit=asm` readability, IR-to-source line comments, and a small assembly test harness |

---

## 8. Definition of Done (project level)

- [x] `optiforge` builds clean from a fresh clone with one documented command (`deployment.md` §2).
- [x] All Phase 0–12 exit criteria met. **NFR-10's ≥10%-on-three target is separately missed** — see Phase 12 above; the exit criteria and that target are different bars.
- [x] Test suite green: lexer, parser, sema, IR, analysis, opt, codegen, end-to-end, PGO — 384 unit assertions, 46 golden cases, 5 CTest suites.
- [x] Benchmark results committed with machine specifications — four files in `metrics/results/` dated 2026-08-20, `metrics/machines/windows-mingw.md` complete with a measured noise floor.
- [ ] `docs/` explains: the language, the IR, the `.prof` format, how to add a new pass, **how to add a new target** — the last is still missing.
- [x] A written report answering: *what did profile guidance actually buy, and why?* — `metrics/reports/phase-12-evaluation.md` §0, with the evidence in `metrics/results/2026-08-20-pgo-attribution.md`.
