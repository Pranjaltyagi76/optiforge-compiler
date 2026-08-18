# OptiForge — Deployment & Environment

> **Doc version:** 1.0 · **Created:** 2026-08-18 · **Scope:** how OptiForge is built, where it runs, what it depends on, how compiled programs are produced and shipped, and how the whole thing is reproduced on another machine.

---

## 1. What "Deployment" Means for a Compiler

Three separate artifacts are deployed, and confusing them is the most common source of trouble in a project like this:

| Artifact | Runs on | Built by | Deployed as |
|---|---|---|---|
| **`optiforge`** — the compiler | the **host** (developer machine) | CMake + a host C++ compiler | An executable plus its runtime libraries |
| **`libofrt` / `libofprof`** — target runtime | inside **compiled programs** | CMake, compiled for the **target** | Static libraries installed alongside `optiforge` |
| **`a.out`** — a compiled user program | the **target** | `optiforge` | A standalone native executable |

`optiforge` must be able to find `libofrt` and `libofprof` at link time. That single requirement drives the whole install layout in §5.

---

## 2. Target Platform (ADR-10 — RESOLVED)

**Target: x86-64 Windows, Microsoft x64 ABI.** Decided 2026-08-18, before Phase 4.

The project is developed, built and run on Windows, and the full toolchain is
already installed and working there. The backend targets the platform it
actually runs on.

### What this means in practice

| | Value |
|---|---|
| ABI | Microsoft x64 |
| Integer argument registers | `rcx rdx r8 r9` — four, then the stack |
| Float argument registers | `xmm0`–`xmm3` |
| Shadow space | 32 bytes, reserved by the **caller**, at every call |
| Return | `rax` / `xmm0` |
| Callee-saved | `rbx rbp rdi rsi rsp r12-r15`, `xmm6`–`xmm15` |
| Stack alignment at `call` | 16 bytes |
| Assembler + linker | GNU `as` / `ld`, driven by MinGW `gcc` |
| Executable format | PE/COFF (`.exe`) |

The shadow space is the item most likely to bite: the caller must reserve 32
bytes below the return address even when the callee takes no arguments.
Omitting it produces crashes inside called functions that look like bugs in the
callee.

### Accepted costs

These are consequences, not objections — the decision is made.

- **The brief specifies Linux.** The final report should record the deviation.
- **No `perf`.** `metrics/methodology.md` §6 Level 2 (cross-checking our
  profiler against an independent sampler) is unavailable. Level 1 hand
  verification of counter accuracy (metric I-05) therefore has to carry the
  whole weight in Phase 9.
- **No ASan/UBSan.** MinGW ships no sanitizer runtimes, so requirement QA-07
  cannot be met here. The build already fails loudly at configure time if
  sanitizers are requested (§4.2), rather than at link time.
- **Sparser reference material.** Compiler literature overwhelmingly assumes
  System V; use Microsoft's x64 calling-convention documentation for ABI facts.

### What stays open

Every ABI fact lives behind `TargetInfo`. Adding a System V target later is a
data change, not a rewrite, so nothing here forecloses it.

---

## 3. Development Environment

### 3.1 Host Requirements (to build `optiforge`)

All of the following are free — see §3.5 for the licensing audit.

| Component | Minimum | Recommended | Notes |
|---|---|---|---|
| C++ compiler | GCC 11 / Clang 14 | GCC 13 or Clang 17 | C++20 required (concepts, ranges, `std::span`). MSVC is supported by the code but not used — see §3.5.2 |
| CMake | 3.20 | 3.27+ | `FetchContent`, presets |
| Build tool | Make | Ninja | Substantially faster incremental builds |
| Python | 3.9 | 3.11+ | Test runner, benchmark harness, golden-file tools |
| Git | 2.30 | latest | The project is not yet a git repo — see §9 |
| RAM | 4 GB | 8 GB+ | Debug builds with sanitizers are memory-hungry |

### 3.2 Target Requirements (to build and run compiled programs)

| Component | Purpose |
|---|---|
| `as` (GNU assembler, via `gcc`) | Assembles OptiForge's `.s` output |
| `ld` (via the `gcc` driver) | Links objects with `libofrt` / `libofprof` |
| `libc` | `libofrt` uses `printf`, `getenv`, `fopen`, `atexit` |

### 3.3 Optional but Strongly Recommended

| Tool | Use | Available on this target? |
|---|---|---|
| `gdb` | Debugging generated assembly — indispensable in Phase 4 | ✅ installed |
| `objdump -d` | Verifying emitted instructions | ✅ installed |
| `clang-format` | Enforcing NFR-08 | ✅ ships with Clang |
| `graphviz` (`dot`) | Rendering `--emit=cfg` output | ❌ not installed — `winget install Graphviz.Graphviz` |
| `perf` | Validating the profiler against an independent sampler | ❌ Linux only — see §2 accepted costs |
| `valgrind` | Memory checking the compiler itself | ❌ Linux only |

### 3.4 Toolchain on This Machine

Already installed and verified working:

| Component | Version |
|---|---|
| GCC (MinGW-w64, UCRT, POSIX threads, SEH) | 16.1.0 |
| CMake | 4.3.3 |
| Ninja | 1.13.2 |
| Python | 3.14.6 |
| binutils (`as`, `ld`, `objdump`) | bundled with the GCC distribution |
| GDB | bundled |

Nothing further is required to build the compiler or to assemble and link the
programs it produces.

**One hazard specific to this machine.** Three different `libstdc++-6.dll`
copies sit on `PATH` (MSYS2, WinLibs, and a WinGet package). A binary built by
one GCC would load whichever the loader found first, and the ABI mismatch
produced silently wrong behaviour — an `ifstream` open that failed without
setting `failbit`. The build links the runtime statically (§4.1) so OptiForge
binaries import only Windows system DLLs. Do not remove those link options.

Note the space in `Pranjal Tyagi` — quote paths in every script.

---

## 3.5 Zero-Cost Constraint (hard requirement)

**Every tool, library, and service used by this project must be free of charge, with no trial period, no seat limit, no registration wall, and no feature gated behind payment.** This is a project constraint on the same footing as "no LLVM" (NFR-07). Any proposed addition that fails this test is rejected, not worked around.

### 3.5.1 Audit of the current stack

Everything specified in this document set already satisfies the constraint. No substitutions are needed.

| Tool | Role | License | Cost |
|---|---|---|---|
| **GCC** | Host C++ compiler; target assembler + linker driver | GPLv3 + GCC Runtime Exception | Free |
| **Clang / LLVM tools** | Alternative host compiler, `clang-format` | Apache 2.0 with LLVM Exception | Free |
| **CMake** | Build system | BSD-3-Clause | Free |
| **Ninja** | Build executor | Apache 2.0 | Free |
| **GNU Make** | Build executor (fallback) | GPLv3 | Free |
| **Python 3** | Test runner, benchmark harness, tooling | PSF License | Free |
| **Git** | Version control | GPLv2 | Free |
| **binutils** (`as`, `ld`, `objdump`, `readelf`) | Assembling, linking, inspecting output | GPLv3 | Free |
| **GDB** | Debugging generated assembly | GPLv3 | Free |
| **ASan / UBSan** | Sanitizers — unavailable on MinGW, see §2 | Compiler licence | Free |
| **Graphviz** (`dot`) | Rendering `--emit=cfg` | EPL-1.0 | Free |
| **doctest** | Unit-test framework (optional) | MIT | Free |
| **Catch2** | Unit-test framework (alternative) | BSL-1.0 | Free |
| **VS Code** | Editor | MIT source; free binary | Free |
| **GitHub** | Repository hosting | — | Free |
| **GitHub Actions** | CI | — | Free tier — see 3.5.3 |

Note that the C++ standard library, the sanitizers, and the profiling instrumentation are all part of GCC/Clang, so there is no separate acquisition step for any of them.

### 3.5.2 Deliberately avoided (paid, or free only under conditions)

Recorded so none of these creeps in later:

| Avoided | Why | Free alternative in use |
|---|---|---|
| Visual Studio (full IDE) | Community edition is free only under revenue and org-size eligibility rules | VS Code + MinGW GCC |
| MSVC toolset | Reachable free via Build Tools, but a second toolchain to keep working for no gain | MinGW GCC, already installed |
| Intel VTune / Advisor | Free tier requires an account and registration | `perf` |
| CLion, Sublime Text | Paid or nag-limited | VS Code |
| JetBrains toolchain, ReSharper C++ | Paid | — |
| Coverity, PVS-Studio, SonarQube (commercial tiers) | Paid for private use | `-Wall -Wextra -Werror`, `clang-tidy`, sanitizers |
| Compiler Explorer (self-hosted) | Free to use online; no dependency taken | `--emit=asm` and `objdump -d` |
| Any cloud build or benchmark service | Billable | Local machine + free CI tier |
| LLVM as a library | Free, but banned by NFR-07 for pedagogical reasons | Our own IR and backend |

### 3.5.3 The one item with a limit worth knowing

**GitHub Actions** is unlimited for **public** repositories and capped at 2,000 minutes per month for **private** ones on a free account. Both are workable:

- Making the repository public removes the cap entirely and costs nothing.
- If it stays private, the cap is generous for this project — the suite is targeted at under five minutes (QA-10), so roughly 400 CI runs per month.
- If the cap is ever hit, CI is not load-bearing: `ctest --test-dir build --output-on-failure` runs the identical suite locally, and a git pre-push hook can enforce it. CI is a convenience here, never a dependency.

**GitLab CI** (400 free minutes) and a **self-hosted runner** on your own machine (unlimited, free) are both fallbacks.

### 3.5.4 Free reference material

A compiler project needs literature, and the standard texts are expensive. These cover the same ground at no cost:

| Resource | Covers |
|---|---|
| LLVM documentation and source | IR design, pass infrastructure, SSA, register allocation — the model this architecture follows |
| *Crafting Interpreters* (Nystrom, free online) | Lexing, recursive-descent parsing, AST design |
| Cornell CS 6120 course materials (free online) | Dataflow analysis, SSA, LICM, optimization theory |
| *Static Single Assignment Book* (free PDF, community-authored) | SSA construction and destruction, including the swap and lost-copy problems |
| Agner Fog's optimization manuals (free PDFs) | x86-64 instruction costs, microarchitecture, alignment |
| Intel and AMD architecture manuals (free PDFs) | Authoritative x86-64 instruction semantics |
| System V AMD64 ABI specification (free PDF) | Calling convention — the authority for §11.5 of `System_design.md` |
| Cytron et al. (1991), Chaitin (1982), Briggs (1994) papers | The specific algorithms in `System_design.md` §8 and §12 — all freely available |

The Dragon Book and *Engineering a Compiler* are excellent but paid; nothing in this project's design depends on owning them.

---

## 4. Build System

### 4.1 Layout

```cmake
cmake_minimum_required(VERSION 3.20)
project(OptiForge VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / IDE tooling

add_subdirectory(src/support)      # of_support
add_subdirectory(src/frontend)     # of_frontend
add_subdirectory(src/ir)           # of_ir
add_subdirectory(src/irgen)        # of_irgen
add_subdirectory(src/analysis)     # of_analysis
add_subdirectory(src/passes)       # of_passes
add_subdirectory(src/transforms)   # of_transforms
add_subdirectory(src/profile)      # of_profile
add_subdirectory(src/backend)      # of_backend
add_subdirectory(src/target/x86_64)# of_target_x86_64
add_subdirectory(src/driver)       # optiforge (executable)
add_subdirectory(runtime)          # libofrt, libofprof  (C, target-compiled)
add_subdirectory(tests)
```

The module dependency rules in `architectural_design.md` §3 are enforced here with `target_link_libraries`. If `of_ir` ever needs `of_frontend` to compile, the build breaks — which is the point.

### 4.2 Build Configurations

| Preset | Flags | Use |
|---|---|---|
| `debug` | `-O0 -g -fno-omit-frame-pointer`, verifier always on | Daily development |
| `asan` | Debug + `-fsanitize=address,undefined` | Catching memory bugs in the compiler |
| `release` | `-O2 -DNDEBUG`, verifier behind `--verify-each` | Benchmarking, distribution |
| `relwithdebinfo` | `-O2 -g` | Profiling the compiler itself (NFR-01) |

Warnings-as-errors in every configuration (NFR-08): `-Wall -Wextra -Wpedantic -Werror`.

### 4.3 Standard Commands

Configure and build:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
```

```bash
cmake --build build -j
```

Run the full test suite:

```bash
ctest --test-dir build --output-on-failure
```

Build with sanitizers:

```bash
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DOPTIFORGE_SANITIZE=address,undefined && cmake --build build-asan -j
```

Run the benchmark suite:

```bash
python3 bench/harness/run.py --configs O0,O1,O2,PGO --reps 10
```

### 4.4 Third-Party Dependencies

Two rules apply: **no third-party compiler infrastructure** (NFR-07) and **nothing paid** (NFR-11, §3.5). Permitted dependencies, all header-only or vendored, all optional:

| Library | Purpose | License | Status |
|---|---|---|---|
| doctest | Unit-test framework | MIT | Optional — via `FetchContent`, replaceable by a 50-line assert harness |
| Catch2 | Unit-test framework (alternative) | BSL-1.0 | Optional |
| (none) | CLI parsing | — | Hand-written — 200 lines, avoids a dependency |
| (none) | Everything else | — | Standard library only |

**Rule for adding any future dependency:** it must be free for unrestricted use, OSI-approved, and permissively or copyleft licensed with no commercial tier gating the features we need. Record the license in this table at the time it is added, not later.

Keeping the dependency list this short means "clone and build" genuinely works offline, which matters for reproducibility — and it makes the zero-cost constraint trivially auditable.

---

## 5. Install Layout

`cmake --install build --prefix <dir>` produces:

```
<prefix>/
├── bin/
│   └── optiforge                  # the compiler
└── lib/optiforge/
    ├── libofrt.a                  # target runtime — linked into every compiled program
    └── libofprof.a                # profiling runtime — linked only into --profile builds
```

### Runtime library discovery

`optiforge` locates its runtime libraries in this order:

1. `--runtime-dir=<path>` if given on the command line.
2. `$OPTIFORGE_RUNTIME_DIR` if set.
3. `<dir-of-executable>/../lib/optiforge` — the installed layout.
4. `<dir-of-executable>/../runtime` — the build-tree layout, so an uninstalled build works directly.

If none resolve, the error is explicit and actionable:

```
optiforge: error: cannot locate libofrt.a
  searched: /usr/local/lib/optiforge, /home/p/of/build/runtime
  hint: set OPTIFORGE_RUNTIME_DIR or pass --runtime-dir=<path>
```

Silent failure here would surface as a baffling linker error, so the check is explicit and early.

---

## 6. How Compiled Programs Are Produced

### 6.1 Normal build

```bash
optiforge program.of -O2 -o program
```

Internally:

```
program.of ──[optiforge]──> program.s ──[as]──> program.o ──[ld]──> program
                                                     +  libofrt.a
```

The assembler and linker are invoked through the `gcc` driver, which handles CRT startup files and libc linking correctly on every distribution — significantly more robust than invoking `ld` directly.

### 6.2 Instrumented build

```bash
optiforge program.of -O2 --profile -o program_inst
```

Links `libofprof.a` in addition to `libofrt.a`. The resulting binary writes its profile on exit:

```bash
./program_inst < workload.txt
```

Produces `program.prof` in the working directory (override with `OPTIFORGE_PROFILE_OUT` or `--profile-out=`).

### 6.3 PGO build

```bash
optiforge program.of -O2 --use-profile=program.prof -o program_pgo
```

Links exactly like a normal build — `libofprof` is **not** linked, and no counters exist in the final binary. The profile influenced compile-time decisions only. This is worth stating explicitly, because it is the property that makes PGO free at runtime.

### 6.4 The complete PGO cycle

```bash
optiforge bench.of -O2 --profile -o bench_inst && ./bench_inst && optiforge bench.of -O2 --use-profile=bench.prof -o bench_pgo && ./bench_pgo
```

**Constraint (enforced by the driver):** the `-O` level in step 1 must match step 3, or block identities drift and the profile match rate collapses. The level is recorded in the `.prof` header (`OPTLEVEL`) and a mismatch produces a loud warning.

---

## 7. Continuous Integration

Even as a solo project, CI is worth the hour it costs — it catches "works on my machine" and enforces the differential-testing discipline that keeps optimizations honest.

### Pipeline

| Stage | Runs | Gate |
|---|---|---|
| Build (debug) | GCC + Clang | Zero warnings |
| Build (release) | GCC | Zero warnings |
| Unit tests | debug build | All pass |
| Golden-file tests | debug build | All pass |
| Negative tests | debug build | Diagnostics match exactly |
| End-to-end tests | release build | Program output matches |
| **Differential tests** | release build | **Output identical across all `-O` levels and PGO** |
| PGO integration | release build | Full cycle succeeds, decisions applied |
| Sanitizer run | asan build | Clean over the whole suite |
| Format check | — | `clang-format --dry-run -Werror` |
| Benchmarks | release, nightly | Regression alert if median worsens by more than 5% |

CI runs on `ubuntu-latest`, which matches the Option-A target exactly — one more argument for Option A.

Benchmarks on shared CI runners are noisy; treat CI benchmark numbers as regression *alarms* only. The numbers that go into `metrics/results/` and the final report come from a quiet local machine with the CPU governor pinned.

---

## 8. Reproducibility

Requirement NFR-06 says identical input plus identical flags yields byte-identical output. This must hold for golden tests to be meaningful and for profile IDs to be stable.

| Threat to determinism | Mitigation |
|---|---|
| Iterating `unordered_map`/`unordered_set` where order affects output | Use insertion-ordered or sorted containers on any output path |
| Printing pointer values | Never print addresses; use deterministic `%tN` names |
| Address-based sorting | Sort by ID or name, never by pointer |
| Timestamps or paths embedded in output | Only the source path, which is an explicit input |
| Parallel pass execution | Passes run sequentially — no plans to change this |
| Uninitialized memory affecting a decision | Sanitizer builds in CI |

**Verification:** a CI step compiles every example twice and diffs the `.s` output. Any difference fails the build.

For benchmark reproducibility, `metrics/results/` entries record CPU model, core count, frequency governor, kernel version, compiler version, and the OptiForge git revision. Without those, a number six months old means nothing.

---

## 9. Repository Setup

**The project directory is not currently a git repository.** Initialize before writing any code — a compiler is exactly the kind of project where bisecting to find which pass broke correctness saves days.

```bash
git init && git add context && git commit -m "Add project design documents"
```

### `.gitignore` essentials

```
build*/
*.o
*.s
*.prof
a.out
*.exe
compile_commands.json
__pycache__/
```

Note that `*.prof` is ignored: profiles are machine- and workload-specific generated data. The **benchmark results** derived from them are committed; the raw profiles are not. Test fixtures under `tests/pgo/fixtures/` are the exception and should be force-added.

### Branching

`main` stays green — the full test suite passes on every commit. Phase work happens on `phase-N-<name>` branches and merges when the phase's exit criteria are met. Tag each completed phase (`v0.4-first-executable`, `v0.11-pgo`) so the progression is legible in the final report.

---

## 10. Release & Distribution

The compiler is a project deliverable rather than a product, so distribution is deliberately minimal.

| Channel | Contents | When |
|---|---|---|
| Source (git) | Full tree; builds with the §4.3 commands | Continuously |
| Tagged release | Source tarball + `docs/` + benchmark results | At each phase completion |
| Prebuilt binary | `optiforge` + `lib/optiforge/*.a` for x86-64 Linux | Optional, at v1.0 |

A prebuilt binary needs `libofrt.a` and `libofprof.a` shipped alongside it in the §5 layout, and it still requires `gcc` on the target machine for assembling and linking. That external dependency should be stated in the release notes rather than discovered by a user.

---

## 11. Environment Variables

| Variable | Consumed by | Purpose |
|---|---|---|
| `OPTIFORGE_RUNTIME_DIR` | compiler | Override runtime library search path |
| `OPTIFORGE_PROFILE_OUT` | compiled program (`libofprof`) | Override the `.prof` output path |
| `OPTIFORGE_ASSEMBLER` | compiler | Override the assembler/linker driver (default `gcc`) |
| `OPTIFORGE_DEBUG_PASSES` | compiler | Comma-separated pass names to trace |

Every one has a working default. None is required for normal use.

---

## 12. Operational Runbook

| Symptom | Likely cause | Resolution |
|---|---|---|
| `cannot locate libofrt.a` | Compiler run from an unexpected location | Set `OPTIFORGE_RUNTIME_DIR` or use `--runtime-dir` |
| Linker: `undefined reference to main` | The generated entry symbol does not match what the CRT expects | Check the entry-shim naming in `libofrt` |
| Compiled program segfaults immediately | Stack misalignment at a `call` (needs 16-byte alignment) | Inspect the prologue; a classic Phase-4 bug |
| Segfault only at `-O2`, not `-O0` | An optimization broke semantics | Bisect the pipeline with `--print-after-all`; run `--verify-each` |
| Wrong results only with the graph allocator | Register allocation bug | Compare against `--regalloc=naive` to confirm, then inspect live ranges |
| `.prof` file is not written | Program crashed or called `_exit` | `atexit` never ran; confirm the program exits normally |
| PGO produces no speedup | Profile did not match the IR | Check the match rate in `--profile-report`; verify the `-O` levels match between the two builds |
| PGO makes things *slower* | Over-aggressive unrolling causing instruction-cache pressure | Reduce the unroll size cap; report the finding — a negative result with an explanation is a legitimate deliverable |
| Golden tests fail everywhere after a refactor | Non-determinism introduced | Compile twice, diff the output; look for unordered-container iteration |

---

## 13. Deployment Checklist per Phase

| Phase | Deployment-relevant work |
|---|---|
| 0 | CMake skeleton, presets, CI pipeline, `.gitignore`, `git init` |
| **Pre-4** | **⚠ Resolve the target-platform decision (§2) and update ADR-10** |
| 4 | `libofrt` builds and links; assembler/linker invocation works; install layout established |
| 8 | `--regalloc` selection wired through the driver |
| 9 | `libofprof` builds and links; `.prof` written to a configurable path |
| 10 | `--profile-report` available as a standalone mode |
| 11 | Driver enforces matching `-O` levels between instrumented and PGO builds |
| 12 | Benchmark harness records full machine specifications; results committed |
