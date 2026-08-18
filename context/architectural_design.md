# OptiForge — Architectural Design

> **Doc version:** 1.0 · **Created:** 2026-08-18 · **Scope:** macro structure, module boundaries, dependency rules, and the decisions that are expensive to reverse.
> For component internals, data structures, and algorithms see `System_design.md`.

---

## 1. Architectural Goals

| Goal | Architectural consequence |
|---|---|
| **Replaceable stages** | Each stage communicates only through a well-defined data structure (Token stream, AST, IR Module, MachineFunction), never through shared mutable global state. |
| **New passes without surgery** | Passes are discovered through a registry, not hard-wired into a pipeline function. |
| **New targets without frontend changes** | The IR carries no target assumptions. Target facts live behind a `TargetInfo` interface. |
| **Profile is a first-class input, not a hack** | Profile data enters the compiler as an *analysis result*, sitting alongside dominators and loop info, so any pass can consult it without special plumbing. |
| **Debuggable at every boundary** | Every inter-stage data structure has a canonical textual form that can be dumped and diffed. |
| **Correct without a profile** | Profile-dependent code paths are always an *enhancement* of a working profile-free path. |

---

## 2. The Layer Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                         DRIVER  (optiforge)                         │
│      CLI parsing · pipeline construction · stage orchestration       │
└───────────────┬─────────────────────────────────────────────────────┘
                │
┌───────────────▼──────────────┐  ┌──────────────────────────────────┐
│          FRONTEND            │  │            SUPPORT               │
│  Lexer → Parser → Sema       │  │  Diagnostics · SourceManager     │
│  Output: Typed AST           │  │  Arena allocator · CLI options   │
└───────────────┬──────────────┘  │  Logging · Timers                │
                │                 └──────────────────────────────────┘
┌───────────────▼──────────────┐        ▲ used by every layer
│         IR BUILDER           │        │
│   Typed AST → IR Module      │        │
└───────────────┬──────────────┘        │
                │                       │
┌───────────────▼───────────────────────┴──────────────────────────────┐
│                          MIDDLE-END                                  │
│  ┌────────────────────┐         ┌──────────────────────────────┐     │
│  │  ANALYSIS MANAGER  │◄────────┤       PASS MANAGER           │     │
│  │  Dominators        │ queries │  pipeline · ordering          │     │
│  │  LoopInfo          │         │  invalidation                 │     │
│  │  Liveness          │         └───────────┬──────────────────┘     │
│  │  UseDef            │                     │ runs                   │
│  │  ProfileData ◄─────┼──── .prof file      ▼                        │
│  │  BlockFrequency    │         ┌──────────────────────────────┐     │
│  └────────────────────┘         │  TRANSFORM PASSES            │     │
│                                 │  CF·CP·DCE·CSE·LICM·Inline   │     │
│                                 │  Unroll·SimplifyCFG·SR       │     │
│                                 │  Instrumentation             │     │
│                                 └──────────────────────────────┘     │
└───────────────┬──────────────────────────────────────────────────────┘
                │  optimized IR Module
┌───────────────▼──────────────────────────────────────────────────────┐
│                            BACKEND                                    │
│  Instruction Selection → Register Allocation → Block Layout →         │
│  Prologue/Epilogue Insertion → Assembly Emission                      │
│  Behind: TargetInfo (register file, ABI, addressing modes)            │
└───────────────┬──────────────────────────────────────────────────────┘
                │ .s
┌───────────────▼──────────────────────────────────────────────────────┐
│                      EXTERNAL TOOLCHAIN                               │
│              system assembler + linker  →  executable                 │
│              links: libofprof (if instrumented) + libofrt             │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 3. Module Decomposition

Each module is a separate build target (a static library). The dependency rules below are enforced by the build system, not by convention.

| # | Module | Target name | Responsibility | May depend on |
|---|---|---|---|---|
| 1 | Support | `of_support` | Source management, diagnostics, arenas, string interning, CLI option types, timers | *(nothing)* |
| 2 | Frontend | `of_frontend` | Lexer, parser, AST, symbol table, type system, semantic analysis | `of_support` |
| 3 | IR | `of_ir` | Value model, instructions, blocks, functions, module, verifier, printer/parser | `of_support` |
| 4 | IR Builder | `of_irgen` | Typed AST → IR lowering | `of_frontend`, `of_ir` |
| 5 | Analysis | `of_analysis` | Analysis manager and all analyses, including `ProfileData` | `of_ir`, `of_support` |
| 6 | Transforms | `of_transforms` | All optimization passes and the instrumentation pass | `of_ir`, `of_analysis` |
| 7 | Pass Infra | `of_passes` | Pass base classes, registry, pass manager, pipeline definitions | `of_ir`, `of_analysis` |
| 8 | Profile | `of_profile` | `.prof` reader/writer, profile model, hot-path classifier | `of_support` |
| 9 | Backend | `of_backend` | Machine IR, instruction selection, register allocation, layout, emission | `of_ir`, `of_analysis`, `of_support` |
| 10 | Target | `of_target_x86_64` | x86-64 register file, ABI, encodings, assembly syntax | `of_backend` |
| 11 | Driver | `optiforge` | CLI, orchestration, toolchain invocation | all of the above |
| 12 | Runtime | `libofrt` | `print_int`, `print_float`, entry shim — **linked into compiled programs, not into the compiler** | *(C only)* |
| 13 | Profile Runtime | `libofprof` | Counter storage, `atexit` hook, `.prof` writer — **linked into instrumented programs** | `libofrt` |

### Dependency rules (hard constraints)

1. **`of_ir` does not depend on `of_frontend`.** The IR must be constructible without a source language. This is what makes the IR reusable and is the single most important boundary in the system.
2. **`of_frontend` does not depend on `of_ir`.** Lowering is `of_irgen`'s job; it is the only module that knows both.
3. **`of_backend` does not depend on `of_frontend`.** The backend sees IR and nothing else.
4. **`of_analysis` never mutates IR.** Analyses are read-only. Anything that mutates is a transform.
5. **`of_transforms` passes never reference each other.** All coordination is via the pass manager and the analysis cache.
6. **`of_profile` does not depend on `of_ir`.** The profile model is plain data — this lets the reader/writer be shared with `libofprof` and unit-tested standalone.
7. **`libofprof` and `libofrt` never link against compiler code.** They are C-level runtime, compiled for the *target*, not the host.

> **Rule 7 matters more than it looks.** The runtime is cross-compiled in spirit: it runs inside programs OptiForge produced. Keeping it a separate, dependency-free C library avoids an entire class of confusion.

---

## 4. Directory Layout

```
CompilerProject1/
├── context/                    # This document set — design of record
│   ├── roadmap.md
│   ├── requirement.md
│   ├── architectural_design.md
│   ├── System_design.md
│   └── deployment.md
├── docs/                       # Reference specs (user- and contributor-facing)
│   ├── language.md             # EBNF grammar, type rules, semantics
│   ├── ir.md                   # IR instruction set and textual syntax
│   ├── profile-format.md       # .prof format specification
│   └── adding-a-pass.md        # Extension guide
├── include/optiforge/          # Public headers, mirroring src/ layout
│   ├── support/
│   ├── frontend/
│   ├── ir/
│   ├── analysis/
│   ├── transforms/
│   ├── passes/
│   ├── profile/
│   └── backend/
├── src/
│   ├── support/
│   ├── frontend/
│   │   ├── lexer/
│   │   ├── parser/
│   │   ├── ast/
│   │   └── sema/
│   ├── ir/
│   ├── irgen/
│   ├── analysis/
│   ├── transforms/
│   ├── passes/
│   ├── profile/
│   ├── backend/
│   │   ├── isel/
│   │   ├── regalloc/
│   │   ├── layout/
│   │   └── emit/
│   ├── target/x86_64/
│   └── driver/
├── runtime/
│   ├── ofrt/                   # libofrt  — print_int, entry shim
│   └── ofprof/                 # libofprof — counters, .prof writer
├── examples/                   # *.of sample programs
├── tests/
│   ├── unit/                   # Per-component unit tests
│   ├── lexer/ parser/ sema/    # Golden-file tests per stage
│   ├── ir/ analysis/ opt/
│   ├── codegen/
│   ├── e2e/                    # Compile, run, compare output
│   └── pgo/                    # Full instrument→run→recompile cycle
├── metrics/                    # Measurement data and evaluation — see metrics/README.md
│   ├── metric-catalog.md       # Every metric: ID, formula, target, requirement traced
│   ├── methodology.md          # How to measure so the numbers are trustworthy
│   ├── machines/               # Machine specs — provenance for every result
│   ├── raw/                    # Immutable harness output
│   ├── results/                # Committed, citable result tables
│   └── reports/                # Written evaluation per phase
├── bench/                      # Benchmark CODE (harness writes into metrics/)
│   ├── programs/               # Benchmark .of sources
│   └── harness/                # Runner producing timings
├── tools/                      # Dev scripts (format, dump-diff, cfg-render)
└── CMakeLists.txt
```

---

## 5. Data Flow Contracts

Each arrow is a **contract**: a data structure with a canonical textual form and a validity invariant.

| Stage boundary | Data structure | Textual form | Invariant checked by |
|---|---|---|---|
| Source → Lexer | `SourceBuffer` | the file itself | `SourceManager` |
| Lexer → Parser | `std::vector<Token>` | `--emit=tokens` | Token stream ends with exactly one EOF |
| Parser → Sema | `AST` (untyped) | `--emit=ast` | AST structural validator |
| Sema → IRGen | `AST` (typed) + `SymbolTable` | `--emit=ast` (with types) | Every expression has a non-null type |
| IRGen → Middle-end | `ir::Module` | `--emit=ir` | `IRVerifier` |
| Middle-end → Backend | `ir::Module` (optimized, SSA-destructed) | `--emit=ir` | `IRVerifier` + non-SSA assertion |
| Backend → Toolchain | `.s` text | the file itself | Assembler acceptance |
| Runtime → Compiler | `.prof` text | the file itself | `ProfileReader` validation |

**Rule:** if a stage cannot print its output, it is not finished.

---

## 6. Key Architectural Decisions

Recorded in ADR style: decision, rationale, consequences, and what would make us revisit it.

---

### ADR-01 — Custom IR rather than LLVM IR
**Decision:** Design and implement a proprietary three-address IR.
**Rationale:** The educational value of the project *is* the IR. Using LLVM would reduce the project to a frontend exercise.
**Consequences:** We must build our own verifier, printer, and optimization infrastructure. Codegen quality will be far below LLVM's — this is accepted and must be stated in the report; benchmark comparisons are against *our own* `-O2`, never against GCC/Clang.
**Revisit if:** the goal changes from "understand compilers" to "produce fast code."

---

### ADR-02 — SSA form for the optimizer, introduced in Phase 6, not Phase 3
**Decision:** IRGen emits non-SSA IR with `Alloca`/`Load`/`Store` for locals; a `mem2reg` pass promotes to SSA later.
**Rationale:** Lowering directly to SSA requires dominance frontiers, which requires a CFG, which requires lowering — a circular dependency that stalls early progress. The alloca-then-promote route is how LLVM does it and lets Phase 3 and 4 ship without any SSA machinery at all.
**Consequences:** An extra pass and a temporary period of memory-heavy IR. `-O0` output is noticeably poor, which is fine and expected.
**Revisit if:** `mem2reg` proves harder than direct SSA construction, which is unlikely.

---

### ADR-03 — Analyses are cached, passes are stateless
**Decision:** An `AnalysisManager` owns all analysis results, keyed by (analysis kind, function). Passes hold no state between runs.
**Rationale:** Recomputing dominators for every pass is wasteful; letting passes cache results themselves creates invalidation bugs that are extremely hard to find.
**Consequences:** Every transform must declare what it invalidates. A wrong declaration is a correctness bug — mitigated by an option (`--verify-analyses`) that recomputes everything from scratch and compares.

---

### ADR-04 — Profile data enters as an analysis, not as a pipeline mode
**Decision:** `ProfileData` is registered with the `AnalysisManager` exactly like `DominatorTree`. Passes call `AM.getOrNull<ProfileData>(F)`.
**Rationale:** This is the decision that makes PGO clean instead of invasive. If profile access were a special pipeline mode, every pass would need two code paths threaded through the driver. As an analysis, a pass that wants profile data asks for it; a pass that does not, ignores it; and "no profile available" is naturally expressed as a null result.
**Consequences:** Every profile-aware pass has a documented fallback for the null case. This is also what makes requirement PGO-11 (never miscompile without a profile) structurally true rather than a thing we remember to test.

---

### ADR-05 — Instrumentation runs late in the pipeline
**Decision:** The instrumentation pass runs after the standard optimization pipeline, immediately before codegen.
**Rationale:** If instrumentation ran first, optimizations would delete or move counters, and the measured CFG would not match the CFG the optimizer later sees. Instrumenting late means the profile describes the same block structure the PGO build will encounter.
**Consequences:** The instrumented build must use the *same* optimization level as the eventual PGO build, or block identities drift. The driver enforces this, and the chosen `-O` level is recorded in the profile header.
**Revisit if:** we adopt Ball–Larus edge profiling, which has its own placement constraints.

---

### ADR-06 — Stable symbolic IDs, not numeric indices, in the profile
**Decision:** Profile entries are keyed by `function_name:block_label` strings, with a source hash in the header.
**Rationale:** Numeric block indices shift with any IR change and would silently mismatch. Symbolic names degrade gracefully — an unmatched name is a detectable miss, not a wrong lookup.
**Consequences:** Larger profile files and slower lookup (irrelevant at our scale). Requires deterministic block naming — promoted to requirement IR-11 because PGO depends on it entirely.

---

### ADR-07 — Text-first profile format
**Decision:** `.prof` is a line-oriented text format, matching the format sketched in the project brief.
**Rationale:** Debuggability dominates at this scale. A profile you can `cat`, hand-edit to test a hypothesis, and diff across runs is worth more than a compact binary one.
**Consequences:** Larger files, slower parsing. Both are acceptable for programs of the size we compile. A binary format is a later optimization if profiles ever exceed a few megabytes.

---

### ADR-08 — Keep the naive register allocator forever
**Decision:** The Phase-4 stack allocator stays in the tree permanently behind `--regalloc=naive`.
**Rationale:** When the graph-coloring allocator miscompiles — and it will — the ability to flip one flag and determine whether the bug is in the allocator or elsewhere is worth far more than the code it costs to keep.
**Consequences:** Two allocators to keep working; the naive one is covered by the same end-to-end tests.

---

### ADR-09 — Emit assembly text, use the system assembler and linker
**Decision:** The backend emits `.s` and shells out to `gcc`/`clang` for assembling and linking.
**Rationale:** Object-file encoding and linking are large problems that teach little about *optimization*, which is this project's subject.
**Consequences:** A toolchain dependency on the build machine. Assembly text is also far easier to debug than emitted bytes — a real advantage during Phase 4.
**Revisit if:** direct ELF/COFF emission becomes a stretch goal.

---

### ADR-10 — Target platform
**Decision:** **RESOLVED — x86-64 Windows (Microsoft x64 ABI).** Recorded 2026-08-18, before Phase 4.

**Rationale:** the project is developed, built, and run on Windows. The complete toolchain is already present and working there — GCC 16.1.0 (MinGW-w64/UCRT), `as`, `ld`, `objdump`, `gdb` — so the backend targets the platform it actually runs on. The earlier recommendation of WSL2 was set aside deliberately: it buys System V and `perf` at the cost of a second environment to install and maintain, and the owner's platform is Windows.

**What this fixes:**

| Fact | Value |
|---|---|
| ABI | Microsoft x64 |
| Integer argument registers | `rcx rdx r8 r9` (4, not 6) |
| Float argument registers | `xmm0`–`xmm3` |
| Shadow space | **32 bytes reserved by the caller at every call** |
| Integer return | `rax` |
| Callee-saved | `rbx rbp rdi rsi rsp r12-r15`, `xmm6`–`xmm15` |
| Caller-saved | `rax rcx rdx r8-r11`, `xmm0`–`xmm5` |
| Stack alignment at `call` | 16 bytes |
| Assembler / linker | GNU `as` and `ld` via the MinGW `gcc` driver |
| Executable format | PE/COFF |

**Consequences, stated plainly rather than argued:**

- The brief specifies Linux. This deviates, and the final report should say so and why.
- `perf` is unavailable, so the profiler validation in Phase 12 rests on hand-verified counts (metric I-05) rather than an independent oracle. That check becomes more important, not less — see `metrics/methodology.md` §6, whose Level 2 cross-check is now unavailable.
- ASan/UBSan are unavailable on MinGW, so requirement QA-07 cannot be satisfied on this platform. Memory bugs must be caught by design and review instead — the teardown use-after-free found in Phase 3 was caught by inspection, which is the standard this project now has to hold itself to.
- Most compiler literature assumes System V; ABI details must be read from Microsoft's x64 calling-convention documentation instead.

**Still open, deliberately:** every ABI fact lives behind `TargetInfo`, so adding a System V target later remains a data change rather than a rewrite. Nothing about this decision forecloses that.

---

## 7. Cross-Cutting Concerns

### 7.1 Diagnostics
A single `DiagnosticEngine` in `of_support` is threaded through every stage. No module writes to `stderr` directly. Every diagnostic carries a `SourceLocation`; IR-level and backend-level diagnostics carry the location propagated from the originating AST node. The engine owns error counting, the warnings-as-errors policy, and the process exit code.

### 7.2 Memory Management
- **AST and IR:** arena-allocated per compilation unit. Nodes are raw pointers into the arena; the arena is freed wholesale. This avoids reference-counting overhead and makes the common "many small nodes" pattern fast.
- **Cross-references (use lists, symbol references):** non-owning raw pointers, valid for the arena's lifetime.
- **Owning containers outside arenas:** `std::unique_ptr` (as the brief specifies for the AST — the arena is the implementation of that ownership, with `unique_ptr` used where lifetimes genuinely differ).
- **Rule:** no `shared_ptr` in the compiler core. If ownership is unclear, the design is unclear.

### 7.3 Determinism
Requirement NFR-06 (identical input → identical output) is a prerequisite for golden-file testing *and* for stable profile IDs. Therefore: no iteration over `unordered_map` where order affects output, no pointer values in any printed form, no address-based sorting. Where a map must be iterated for output, it is a sorted or insertion-ordered container.

### 7.4 Verification
`IRVerifier` runs after every pass in debug builds and behind `--verify-each` in release builds. This converts "some later pass crashed mysteriously" into "pass X produced invalid IR," which is the difference between a ten-minute bug and a two-day bug.

### 7.5 Extension Points
| To add… | You touch |
|---|---|
| A new analysis | One new file in `src/analysis/`, register it. No existing file changes. |
| A new transform pass | One new file in `src/transforms/`, register it, add it to a pipeline definition. |
| A new IR instruction | `ir/Instruction.def` (the X-macro list), plus a case in the verifier, printer, and instruction selector. |
| A new target | A new `src/target/<arch>/` implementing `TargetInfo` and the emitter interface. |
| A new profile metric | The `.prof` grammar, `ProfileData`, and the writer in `libofprof`. |

---

## 8. Build-Time vs Run-Time Architecture

A recurring source of confusion in PGO projects is that **two different programs** are involved. Stated explicitly:

| | Compiler (`optiforge`) | Compiled program (`a.out`) |
|---|---|---|
| Built for | the **host** (developer machine) | the **target** (x86-64) |
| Written in | C++20 | generated assembly |
| Links against | `of_*` libraries | `libofrt`, and `libofprof` if instrumented |
| Reads `.prof` | yes (PGO build) | no |
| Writes `.prof` | no | yes (instrumented build only) |

The `.prof` file is the *only* channel between them, and it crosses a process boundary and a time boundary. This is why the format is versioned, hashed, and validated (ADR-06, ADR-07).

---

## 9. The Two-Phase PGO Workflow

```
  ┌──────────── PHASE A: PROFILE COLLECTION ────────────┐
  │                                                      │
  │  prog.of ──> optiforge -O2 --profile -o prog_inst    │
  │                          │                           │
  │                          ├─ standard -O2 pipeline    │
  │                          ├─ InstrumentationPass      │
  │                          └─ codegen + link libofprof │
  │                                                      │
  │  ./prog_inst <representative workload>               │
  │                          │                           │
  │                          └──> prog.prof              │
  └──────────────────────────────────────────────────────┘
                             │
  ┌──────────── PHASE B: PROFILE-GUIDED BUILD ───────────┐
  │                          ▼                           │
  │  prog.of ──> optiforge -O2 --use-profile=prog.prof   │
  │                          │              -o prog_pgo  │
  │                          ├─ ProfileReader + validate │
  │                          ├─ ProfileData analysis     │
  │                          ├─ hot/warm/cold classify   │
  │                          ├─ PGO inliner              │
  │                          ├─ PGO unroller             │
  │                          ├─ profile-weighted regalloc│
  │                          ├─ hot-path block layout    │
  │                          └─ codegen + link libofrt   │
  │                                                      │
  │  ./prog_pgo  — same output as prog, measurably faster│
  └──────────────────────────────────────────────────────┘
```

**Critical invariant:** Phase A and Phase B must use the same `-O` level and the same compiler version, or block identities drift. The driver checks this against the profile header and warns loudly on mismatch (PGO-12).

---

## 10. Pass Pipeline Architecture

Pipelines are **declarative lists**, defined in one place, not code paths scattered across the driver.

```
-O0 :  mem2reg? no.  verify.                       (debuggability first)

-O1 :  mem2reg → simplify-cfg → constant-fold → sccp → copy-prop
       → dce → simplify-cfg → verify

-O2 :  mem2reg → simplify-cfg → sccp → copy-prop → gvn/cse → dce
       → inline(static) → sccp → licm → strength-reduce
       → unroll(static) → dce → simplify-cfg → verify

-O2 + profile :
       mem2reg → simplify-cfg → sccp → copy-prop → gvn/cse → dce
       → [ProfileData attach + classify]
       → inline(PGO budget) → sccp → licm(hot-weighted)
       → unroll(PGO trip counts) → strength-reduce
       → dce → simplify-cfg → [block layout hints] → verify

--profile (instrumented) :
       <same as the -O level requested> → instrument → verify
```

Two properties make this workable: passes are idempotent enough to be repeated, and every pass reports whether it changed anything, so pipeline segments can iterate to a fixed point without infinite loops.

---

## 11. Error Handling Architecture

| Class | Example | Handling |
|---|---|---|
| **User error** | Type mismatch, undeclared variable | `DiagnosticEngine` error, continue to find more, non-zero exit |
| **User warning** | Unused variable, stale profile | `DiagnosticEngine` warning, compilation proceeds |
| **Environment error** | Missing input file, assembler not found | Clear message naming the remedy, non-zero exit |
| **Profile fault** | Corrupt, stale, or partial `.prof` | Warning, discard the affected data, **compile correctly without it** |
| **Internal error** | Verifier failure, unreachable branch | Assert in debug; in release, an "internal compiler error" message with the pass name and a request to report it |

**Non-negotiable:** no code path in the compiler calls `exit()` or throws past the driver. The driver owns process exit.

---

## 12. Testing Architecture

Tests mirror the module structure so a failure localizes immediately.

| Layer | Style | What it catches |
|---|---|---|
| Unit | Direct API calls per component | Logic errors inside one component |
| Golden-file | Compare `--emit=<stage>` against a checked-in expected dump | Unintended changes to any stage output |
| Negative | Compile a broken program, compare diagnostics | Missing or wrong error messages |
| End-to-end | Compile, run, compare program stdout | Codegen and runtime bugs |
| **Differential** | Same program at `-O0/-O1/-O2/instrumented/PGO`, outputs must match | **Any optimization that changes semantics — the single highest-value test category** |
| PGO integration | Full instrument → run → recompile → verify cycle | Profile format drift, ID instability, PGO regressions |
| Benchmark | Timed runs across configurations | Performance regressions, and the headline PGO result |

The differential category deserves emphasis: it is the only test type that scales automatically as passes are added, because every new pass is immediately covered by every existing program.

---

## 13. Architectural Risks

| Risk | Architectural mitigation |
|---|---|
| Passes accumulate hidden interdependencies | Registry-based discovery; a pass may not name another pass; pipelines defined declaratively in one file |
| Analysis invalidation bugs | Explicit invalidation declarations plus a `--verify-analyses` mode that recomputes and compares |
| Profile IDs drift, PGO silently no-ops | Symbolic IDs, source hash, `-O` level recorded in the header, loud mismatch warning, and a PGO integration test that fails if the match rate drops |
| ABI decision made too late | All ABI facts behind `TargetInfo`; decision forced before Phase 4 by ADR-10 |
| Backend complexity swallows the schedule | Naive allocator retained permanently as a fallback; assembly text rather than object emission; external assembler and linker |
| The IR grows target assumptions | `of_ir` is forbidden from depending on `of_backend` or `of_target_*`, enforced by the build graph |
