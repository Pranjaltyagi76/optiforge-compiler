# OptiForge — Requirements & Deliverables

> **Doc version:** 1.0 · **Created:** 2026-08-18 · **Companion docs:** `roadmap.md`, `architectural_design.md`, `System_design.md`, `deployment.md`

---

## 1. Purpose of This Document

This document defines **what OptiForge must do**, **what must be delivered**, and **how each requirement is verified**. It is the contract the implementation is checked against. `roadmap.md` says *when*; this document says *what* and *how we know it works*.

### Requirement ID scheme

| Prefix | Area |
|---|---|
| `LANG-` | Source language |
| `FE-` | Frontend (lexer, parser, semantic analysis) |
| `IR-` | Intermediate representation and CFG |
| `AN-` | Analysis framework |
| `OPT-` | Optimization passes |
| `BE-` | Backend / code generation |
| `PROF-` | Instrumentation and runtime profiling |
| `PGO-` | Profile-guided optimization |
| `CLI-` | Command-line interface and diagnostics |
| `QA-` | Testing, benchmarking, quality |
| `NFR-` | Non-functional requirements |
| `DOC-` | Documentation deliverables |

### Priority scheme

| Level | Meaning |
|---|---|
| **M** (Must) | The project is incomplete without it |
| **S** (Should) | Expected in the finished project, but the project still stands if one or two slip |
| **C** (Could) | Valuable if time permits |
| **W** (Won't, this version) | Explicitly out of scope — recorded so it is not silently attempted |

---

## 2. Deliverables

| # | Deliverable | Form | Priority |
|---|---|---|---|
| D1 | `optiforge` compiler binary | Native executable built from source | M |
| D2 | Complete source tree with modular architecture | C++20 source, CMake build | M |
| D3 | `libofprof` profiling runtime | Static library linked into instrumented builds | M |
| D4 | Minimal language runtime (`print_int`, entry shim) | Static library / object | M |
| D5 | Example programs (`examples/*.of`) | Source files exercising every language feature | M |
| D6 | Test suite (unit + golden-file + end-to-end + PGO) | Test sources plus a runner | M |
| D7 | Benchmark suite and harness | `bench/` with a repeatable runner | M |
| D8 | Benchmark results with machine specs | `metrics/results/*.md` or `.csv` | M |
| D9 | Language specification | `docs/language.md` — grammar (EBNF), types, semantics | M |
| D10 | IR specification | `docs/ir.md` — instruction set, textual syntax, invariants | M |
| D11 | Profile format specification | `docs/profile-format.md` | M |
| D12 | "How to add a pass" guide | `docs/adding-a-pass.md` | S |
| D13 | Final project report | Design decisions, results, honest analysis of what PGO bought | M |
| D14 | Architecture and design docs | This `context/` set, kept current | M |

---

## 3. Source Language Requirements

### 3.1 Types

| ID | Requirement | Priority |
|---|---|---|
| LANG-01 | Support `int` (64-bit signed) | M |
| LANG-02 | Support `float` (64-bit IEEE-754 double) | M |
| LANG-03 | Support `bool` (true/false, 1 byte in memory, register-width in registers) | M |
| LANG-04 | Support `void` as a function return type only | M |
| LANG-05 | Arrays (fixed size, single dimension) — **delivered in Phase 13**, local only, no bounds checking; see `docs/language.md` §9 | C |
| LANG-06 | Strings, structs, pointers | W |

> **Decision to record:** `int` is 64-bit. The spec's example assembly (`mov eax, edi`) implies 32-bit. Pick one and be consistent throughout the backend; the design docs assume **64-bit `int`** with 32-bit forms used only where provably safe.

### 3.2 Declarations and Variables

| ID | Requirement | Priority |
|---|---|---|
| LANG-10 | Variable declaration with mandatory type and optional initializer: `int x = 10;` | M |
| LANG-11 | Variables must be declared before use | M |
| LANG-12 | Block-scoped variables with shadowing rules defined and documented | M |
| LANG-13 | Assignment to a declared variable: `x = expr;` | M |

### 3.3 Expressions

| ID | Requirement | Priority |
|---|---|---|
| LANG-20 | Arithmetic operators `+ - * / %` with C-like precedence and left associativity | M |
| LANG-21 | Unary `-` and `!` | M |
| LANG-22 | Comparison operators `== != < > <= >=` yielding `bool` | M |
| LANG-23 | Parenthesized sub-expressions | M |
| LANG-24 | Integer and floating-point literals; `true`/`false` literals | M |
| LANG-25 | Logical `&&` and `\|\|` with short-circuit evaluation | S |
| LANG-26 | `%` is defined only for `int`; applying it to `float` is a semantic error | M |
| LANG-27 | Integer division by zero: behaviour defined and documented (trap with a runtime message) | S |

### 3.4 Control Flow

| ID | Requirement | Priority |
|---|---|---|
| LANG-30 | `if (cond) { ... }` and `if (cond) { ... } else { ... }`, arbitrarily nested | M |
| LANG-31 | `while (cond) { ... }`, arbitrarily nested | M |
| LANG-32 | Condition expressions must be of type `bool` (no implicit int-to-bool) | M |
| LANG-33 | `for`, `break`, `continue` — **delivered in Phase 13**; `continue` in a `for` runs the step clause, see `docs/language.md` §10 | C |

### 3.5 Functions

| ID | Requirement | Priority |
|---|---|---|
| LANG-40 | Function declaration: `fn name(int a, int b) -> int { ... }` (return-type syntax to be finalized in `docs/language.md`) | M |
| LANG-41 | Zero or more typed parameters | M |
| LANG-42 | Return values of any non-`void` type; `return;` for `void` | M |
| LANG-43 | Local variables inside function bodies | M |
| LANG-44 | Function calls with argument type and arity checking | M |
| LANG-45 | Direct recursion and mutual recursion | M |
| LANG-46 | A `main` function is the program entry point; its signature is fixed and documented | M |
| LANG-47 | All control paths in a non-`void` function must return — enforced or diagnosed | M |

---

## 4. Frontend Requirements

### 4.1 Lexer

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| FE-01 | Tokenize keywords, identifiers, integer literals, float literals, all operators, punctuation | M | Golden token dumps |
| FE-02 | Skip whitespace and both comment forms (`//`, `/* */`) | M | Golden token dumps |
| FE-03 | Every token carries file, line, and column | M | Dump inspection |
| FE-04 | Report lexical errors with location and continue lexing where possible | M | Negative tests |
| FE-05 | Correctly reject malformed numbers (`1.2.3`, `1e`, `0x` with no digits) | M | Negative tests |
| FE-06 | Handle unterminated block comments and unexpected characters without crashing | M | Negative tests |

### 4.2 Parser

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| FE-10 | Recursive-descent parser producing a typed AST hierarchy | M | Golden AST dumps |
| FE-11 | Correct operator precedence and associativity | M | Precedence test matrix |
| FE-12 | Parse every construct in §3 | M | `examples/` coverage |
| FE-13 | Report syntax errors with location and an expected-token hint | M | Negative tests |
| FE-14 | Error recovery: report multiple independent syntax errors in one run | S | Multi-error tests |
| FE-15 | AST owns children via smart pointers; no leaks under ASan/LSan | M | Sanitizer build |

### 4.3 Semantic Analysis

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| FE-20 | Build a scoped symbol table for globals, functions, parameters, and locals | M | Symbol-table dump |
| FE-21 | Detect use of an undeclared variable | M | Negative test |
| FE-22 | Detect duplicate declaration in the same scope | M | Negative test |
| FE-23 | Type-check assignments, binary and unary operations, and conditions | M | Negative test matrix |
| FE-24 | Detect calls to undeclared functions | M | Negative test |
| FE-25 | Check call arity and argument types | M | Negative test |
| FE-26 | Check return type against the declared type, including missing returns | M | Negative test |
| FE-27 | Support forward references between functions (two-pass resolution) | M | Mutual-recursion test |
| FE-28 | Annotate every expression node with its resolved type | M | Typed AST dump |
| FE-29 | Define and enforce implicit-conversion rules (`int` → `float` only) | M | Conversion tests |

---

## 5. IR and CFG Requirements

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| IR-01 | Custom three-address IR, independent of both source language and target | M | Design review; `docs/ir.md` |
| IR-02 | Hierarchy: `Module` → `Function` → `BasicBlock` → `Instruction` | M | Code structure |
| IR-03 | Instruction set covering arithmetic, comparison, memory, control flow, calls, returns, and phi | M | `docs/ir.md` |
| IR-04 | Every value is typed; type consistency is checked | M | Verifier |
| IR-05 | Textual IR printer whose output is human-readable and diff-stable | M | Golden IR dumps |
| IR-06 | CFG with predecessor and successor edges maintained incrementally | M | CFG dumps |
| IR-07 | Graphviz DOT export of the CFG | S | Manual render |
| IR-08 | IR verifier: exactly one terminator per block, valid operand types, no dangling uses, entry block has no predecessors | M | Verifier runs after every pass in debug builds |
| IR-09 | SSA form with correctly placed phi nodes | M | SSA verifier |
| IR-10 | SSA destruction that preserves semantics (lost-copy and swap problems handled) | M | End-to-end differential tests |
| IR-11 | Stable, deterministic naming of functions and basic blocks across compilations | M | Recompile-and-diff test — **prerequisite for PGO** |

---

## 6. Analysis Framework Requirements

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| AN-01 | Analysis manager with lazy computation, caching, and invalidation | M | Cache-hit instrumentation |
| AN-02 | Dominator tree | M | Golden dumps on hand-checked CFGs |
| AN-03 | Dominance frontiers | M | Golden dumps |
| AN-04 | Post-dominator tree | S | Golden dumps |
| AN-05 | Natural loop detection with nesting, headers, latches, and exits | M | Golden dumps on nested loops |
| AN-06 | Loop preheader creation | M | IR inspection |
| AN-07 | Use-def and def-use chains | M | Unit tests |
| AN-08 | Liveness analysis (live-in/live-out per block) | M | Golden dumps |
| AN-09 | Reaching definitions | S | Golden dumps |
| AN-10 | Static block-frequency estimation (used when no profile is available) | S | Comparison against profile data |
| AN-11 | Adding a new dataflow analysis requires no changes to existing analyses | M | Demonstrated by adding one |

---

## 7. Optimization Requirements

### 7.1 Framework

| ID | Requirement | Priority |
|---|---|---|
| OPT-01 | Pass-based architecture; each pass is an independent, separately testable module | M |
| OPT-02 | Pass manager supporting module-level and function-level passes with a declared order | M |
| OPT-03 | Passes declare which analyses they require and which they invalidate | M |
| OPT-04 | Optimization levels `-O0`, `-O1`, `-O2` with documented pass pipelines | M |
| OPT-05 | `--print-after-all` / `--print-after=<pass>` for debugging | S |
| OPT-06 | Individual passes can be enabled or disabled from the command line | S |
| OPT-07 | Every pass reports whether it changed the IR, for pipeline convergence | M |

### 7.2 Passes

| ID | Pass | Priority | Verified by |
|---|---|---|---|
| OPT-10 | Constant folding | M | Golden IR + differential output |
| OPT-11 | Constant propagation (SCCP preferred) | M | Golden IR + differential output |
| OPT-12 | Dead code elimination | M | Golden IR + instruction count |
| OPT-13 | Dead store elimination | S | Golden IR |
| OPT-14 | Copy propagation | M | Golden IR |
| OPT-15 | Common subexpression elimination / GVN | M | Golden IR |
| OPT-16 | Strength reduction | M | Generated assembly inspection |
| OPT-17 | Loop-invariant code motion | M | Golden IR — the hoisted instruction appears in the preheader |
| OPT-18 | CFG simplification (block merging, constant-branch folding) | M | Golden CFG |
| OPT-19 | Static function inlining with a size heuristic | S | Golden IR |
| OPT-20 | Loop unrolling (static heuristic) | S | Golden IR |
| OPT-21 | **Every pass preserves program semantics** | M | **Differential testing: output at `-On` is byte-identical to `-O0` for every test** |

---

## 8. Backend Requirements

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| BE-01 | Generate correct x86-64 assembly | M | End-to-end execution tests |
| BE-02 | Instruction selection covering every IR instruction | M | Coverage audit |
| BE-03 | Naive stack-slot register allocation (permanent fallback, `--regalloc=naive`) | M | End-to-end tests |
| BE-04 | Graph-coloring register allocation with spilling | M | End-to-end tests + memory-traffic comparison |
| BE-05 | Correct calling convention: argument passing, return values, callee/caller-saved registers, stack alignment | M | Interop test calling into the C runtime |
| BE-06 | Correct stack frame construction and teardown | M | Debugger inspection + tests |
| BE-07 | Branches, loops, and function calls generate correct control flow | M | End-to-end tests |
| BE-08 | Floating-point operations via SSE registers | M | Float test suite |
| BE-09 | Assembly output is commented with IR-level provenance | S | Manual inspection |
| BE-10 | Assemble and link into a runnable executable via the system toolchain | M | Executable runs |
| BE-11 | Basic-block layout is controllable by the caller (needed for PGO layout) | S | PGO layout test |
| BE-12 | Instruction scheduling | C | Benchmark |

---

## 9. Profiling Requirements

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| PROF-01 | `--profile` mode produces an instrumented executable | M | CLI test |
| PROF-02 | Count function entries (call counts) | M | Hand-verified small program |
| PROF-03 | Count basic block executions | M | Hand-verified small program |
| PROF-04 | Count branch outcomes (taken / not-taken per conditional) | M | Hand-verified small program |
| PROF-05 | Count loop entries and total iterations | M | Hand-verified small program |
| PROF-06 | Optional approximate per-function execution time | S | Sanity comparison against an external profiler |
| PROF-07 | Profile is written on normal program exit without any source change | M | Run and inspect |
| PROF-08 | Output path configurable via CLI and environment variable | S | CLI test |
| PROF-09 | Documented, versioned, human-readable `.prof` format | M | `docs/profile-format.md` |
| PROF-10 | Profile header records a source hash and compiler version for staleness detection | M | Stale-profile test |
| PROF-11 | Instrumented program produces the same output as the uninstrumented one | M | Differential test |
| PROF-12 | Counters do not overflow on long runs (64-bit counters) | M | Long-run test |
| PROF-13 | Instrumentation overhead measured and documented; target under 40% | S | Benchmark |
| PROF-14 | Multiple profile runs can be merged | C | Merge test |

---

## 10. Profile-Guided Optimization Requirements

| ID | Requirement | Priority | Verified by |
|---|---|---|---|
| PGO-01 | `--use-profile=<file>` loads and validates a profile | M | CLI test |
| PGO-02 | Profile data is exposed as an analysis any pass can query | M | Code structure |
| PGO-03 | Classify functions, blocks, and loops as Hot / Warm / Cold | M | `--profile-report` output |
| PGO-04 | Classification threshold is configurable (`--hot-threshold=`) | S | CLI test |
| PGO-05 | Human-readable hot-path report | M | Report matches a designed benchmark |
| PGO-06 | Hot call sites get an increased inlining budget; cold call sites are not inlined | M | Golden IR + benchmark |
| PGO-07 | Hot loops are unrolled with a profile-derived factor; cold loops are not | M | Golden IR + benchmark |
| PGO-08 | Register allocator spill costs are weighted by profile frequency | S | Generated assembly comparison |
| PGO-09 | Basic-block layout places the hot path on the fall-through edge | S | Generated assembly comparison |
| PGO-10 | Cold code is optimized minimally (size over speed) | S | Code-size measurement |
| PGO-11 | **A missing, empty, corrupt, or stale profile never produces an incorrect binary** | M | **Fault-injection tests** |
| PGO-12 | A stale profile produces a clear warning, not a silent no-op | M | Stale-profile test |
| PGO-13 | Every PGO decision is explainable via a `--pgo-remarks` style output | S | Manual inspection |
| PGO-14 | **PGO build measurably outperforms the `-O2` build on the profiled workload** | M | **Benchmark suite — the project's headline result** |

---

## 11. CLI and Diagnostics Requirements

| ID | Requirement | Priority |
|---|---|---|
| CLI-01 | `optiforge <input.of> -o <output>` compiles and links | M |
| CLI-02 | `-O0 \| -O1 \| -O2` selects the optimization pipeline | M |
| CLI-03 | `--emit=tokens\|ast\|ir\|cfg\|asm\|obj` stops after the named stage and dumps it | M |
| CLI-04 | `--profile` builds an instrumented binary | M |
| CLI-05 | `--use-profile=<file>` enables PGO | M |
| CLI-06 | `--profile-report=<file>` prints the hot-path report | M |
| CLI-07 | `--help` and `--version` | M |
| CLI-08 | Diagnostics show file, line, column, the source line, and a caret under the offending span | M |
| CLI-09 | Errors and warnings are distinguished; a non-zero exit code on error | M |
| CLI-10 | Compiler never crashes on malformed input — it reports and exits cleanly | M |
| CLI-11 | `--verbose` / `--time-passes` for compiler introspection | C |

---

## 12. Quality and Testing Requirements

| ID | Requirement | Priority |
|---|---|---|
| QA-01 | Unit tests for lexer, parser, symbol table, type checker, and each analysis | M |
| QA-02 | Golden-file tests for token, AST, IR, and CFG dumps | M |
| QA-03 | Negative tests: every diagnostic message has at least one test that triggers it | M |
| QA-04 | End-to-end tests: compile, run, compare program output against expected | M |
| QA-05 | **Differential tests: identical program output across `-O0`, `-O1`, `-O2`, instrumented, and PGO builds** | M |
| QA-06 | PGO integration test: instrument → run → profile → recompile → verify correctness and improvement | M |
| QA-07 | Sanitizer builds (ASan, UBSan) run clean over the whole test suite | S |
| QA-08 | A single command runs the entire suite | M |
| QA-09 | Benchmark harness with repetitions and variance reporting | M |
| QA-10 | Test suite runs in under 5 minutes so it is actually used | S |

---

## 13. Non-Functional Requirements

| ID | Requirement | Target |
|---|---|---|
| NFR-01 | Compile speed | A 1,000-line program compiles at `-O2` in under 2 seconds |
| NFR-02 | Compiler memory usage | Under 500 MB for a 1,000-line program |
| NFR-03 | Modularity | Each stage is a separate library target; the frontend does not link against the backend |
| NFR-04 | Extensibility | A new optimization pass can be added by creating one file and registering it — no edits to existing passes |
| NFR-05 | Portability of the compiler itself | Builds with GCC 11+ / Clang 14+ / MSVC 19.3+ under C++20 |
| NFR-06 | Determinism | The same input plus the same flags yields byte-identical assembly output |
| NFR-07 | Dependencies | No third-party compiler infrastructure (no LLVM, no bison/flex). Test and CLI helper libraries are permitted |
| NFR-08 | Code quality | Warnings-as-errors; consistent formatting via `clang-format` |
| NFR-09 | Instrumentation overhead | Under 40% runtime slowdown on the benchmark suite |
| NFR-10 | PGO speedup | At least 10% median improvement over `-O2` on at least three benchmarks |
| NFR-11 | **Zero cost** | Every tool, library, and service is free with no trial period, seat limit, registration wall, or paywalled feature. Any dependency added must record its license at the time of addition. Audited in `deployment.md` §3.5 |

---

## 14. Out of Scope (this version)

Recorded so they are not attempted by accident:

- Strings, structs, pointers, dynamic memory allocation, garbage collection.
- Separate compilation, a linker of our own, object-file emission without an external assembler.
- Multiple source files per compilation, a module or import system.
- Debug information (DWARF), source-level debugging support.
- Exception handling.
- Concurrency, threads, atomics.
- Auto-vectorization and SIMD (stretch goal only).
- Targets other than x86-64 (stretch goal only).
- Standard library beyond the minimal print/entry runtime.
- IDE integration, language server, syntax highlighting.

---

## 15. Acceptance Criteria — The Final Check

The project is accepted when all of the following are demonstrable in one sitting:

1. **Frontend:** A program with a deliberate type error produces a precise, located diagnostic; the corrected program compiles.
2. **IR:** `--emit=ir` and `--emit=cfg` show correct three-address code and a correct control-flow graph for a nested loop.
3. **Optimization:** `--emit=ir -O2` shows constants folded, dead code removed, and a loop-invariant computation hoisted into the preheader.
4. **Codegen:** The compiled executable runs and produces correct output for every example.
5. **Correctness under optimization:** Output is identical at `-O0`, `-O1`, and `-O2`.
6. **Profiling:** `--profile` produces a binary that writes a `.prof` file whose counts are correct.
7. **Hot-path detection:** `--profile-report` correctly identifies the hot function and hot loop.
8. **PGO:** `--use-profile` produces a binary that is correct and **measurably faster than the `-O2` build**, with the improvement traced to specific PGO decisions.
9. **Robustness:** A corrupt profile, a stale profile, and a missing profile each produce a correct binary and an appropriate message.
10. **Documentation:** Every deliverable in §2 is present and current.
