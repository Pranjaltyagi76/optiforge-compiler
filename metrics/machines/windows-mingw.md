# Machine Spec — `windows-mingw`

| Field | Value |
|---|---|
| **Machine ID** | `windows-mingw` |
| Recorded on | 2026-08-19 |

## Hardware

| Field | Value |
|---|---|
| Form factor | Laptop |
| RAM | see `free -h` at measurement time |

Full hardware detail is not recorded yet because no *timing* has been taken on
this machine. The static instruction counts in `results/` are hardware
independent. This file must be completed before any runtime figure is
published — see `methodology.md` §7.

## Software

| Field | Value |
|---|---|
| OS | Windows 11 (build 26200) |
| Host C++ compiler | GCC 16.1.0 (MinGW-w64, UCRT, POSIX threads, SEH) |
| Assembler / linker | GNU as / ld via the MinGW gcc driver |
| CMake / Ninja / Python | 4.3.3 / 1.13.2 / 3.14.6 |
| Target | x86-64 Windows, Microsoft x64 ABI (ADR-10) |

## ★ Noise Floor

**Not yet measured.** No timing has been taken on this machine, so no runtime
result may be published from it. Establish the floor per `methodology.md` §3.2
before the Phase 12 benchmarks.

| Measured on | Noise floor | Verdict |
|---|---|---|
| — | — | ☐ pending |

## Notes

- `perf` is unavailable on this target (ADR-10), so the Level 2 profiler
  cross-check in `methodology.md` §6 cannot run. Level 1 hand verification of
  counter accuracy carries that weight in Phase 9.
- Three `libstdc++-6.dll` copies sit on PATH; the build links the runtime
  statically to stay immune to load order. See `deployment.md` §3.4.
