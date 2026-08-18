# Machine Spec — `<machine-id>`

> Copy to `metrics/machines/<machine-id>.md`. Every result file references one of these by ID.
> Without this, a benchmark number is uninterpretable six months later.

| Field | Value |
|---|---|
| **Machine ID** | `wsl2-ryzen5600` *(short, stable, used in result files)* |
| Recorded on | YYYY-MM-DD |

## Hardware

| Field | Value | How to find |
|---|---|---|
| CPU model | | `lscpu \| grep "Model name"` |
| Physical cores / threads | | `lscpu \| grep -E "^CPU\(s\)\|Thread"` |
| Base / boost clock | | `lscpu \| grep MHz` |
| L1d / L1i / L2 / L3 cache | | `lscpu \| grep cache` |
| RAM | | `free -h` |
| Storage type | SSD / NVMe / HDD | |
| Form factor | Laptop / desktop | *laptops throttle — this matters* |

## Software

| Field | Value | How to find |
|---|---|---|
| OS | | `lsb_release -d` |
| Kernel | | `uname -r` |
| WSL2? | yes / no | `uname -r` contains `microsoft` |
| Windows host build (if WSL2) | | `winver` |
| Host C++ compiler | | `g++ --version` |
| Assembler / linker | | `as --version` |
| Python | | `python3 --version` |

## Measurement Environment

| Control | Setting | Verified |
|---|---|---|
| CPU governor | `performance` | ☐ |
| Turbo boost | enabled / disabled | ☐ |
| On AC power | yes / no | ☐ |
| Core pinning | e.g. `taskset -c 2` | ☐ |
| Background load | none / describe | ☐ |
| Warmup reps discarded | 3 | ☐ |
| Settle time before timing | 30s | ☐ |

## ★ Noise Floor

Measured per `methodology.md` §3.2 — identical config against itself, 10 reps.

| Measured on | Noise floor (IQR/median) | Verdict |
|---|---|---|
| YYYY-MM-DD | __._% | ☐ under 3% — results trustworthy / ☐ over 3% — **fix environment first** |

> **No result from this machine is reportable until the noise floor is under 3%.** Any measured difference smaller than the figure above is indistinguishable from noise and must not be cited as a result.

## Notes

*Anything that would surprise someone reproducing these numbers — thermal behaviour, known interference, a flaky sensor, unusual BIOS settings.*
