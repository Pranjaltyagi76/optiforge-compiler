#!/usr/bin/env python3
"""Counts IR instructions per optimization level.

Produces metrics O-03 and O-04 from metrics/metric-catalog.md: the reduction
in IR instruction count at -O1 and -O2 relative to -O0.

An IR instruction is a line inside a function body that is not a label, a
blank, or a brace.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def count(optiforge: Path, source: Path, level: str) -> int | None:
    proc = subprocess.run([str(optiforge), str(source), level, "--emit=ir"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return None
    total = 0
    inside = False
    for line in proc.stdout.splitlines():
        if line.startswith("fn @") and line.rstrip().endswith("{"):
            inside = True
            continue
        if inside and line.startswith("}"):
            inside = False
            continue
        if not inside:
            continue
        stripped = line.strip()
        if not stripped or stripped.endswith(":") or stripped.startswith(";"):
            continue
        total += 1
    return total


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dirs", nargs="+", required=True, type=Path)
    opts = parser.parse_args()

    sources = sorted(s for d in opts.dirs for s in d.glob("*.of"))
    rows = []
    for source in sources:
        counts = {lv: count(opts.optiforge, source, lv) for lv in ("-O0", "-O1", "-O2")}
        if any(v is None for v in counts.values()):
            continue
        rows.append((source.name, counts["-O0"], counts["-O1"], counts["-O2"]))

    print(f"| {'Program':<26} | {'-O0':>5} | {'-O1':>5} | {'-O2':>5} | {'O-04 (-O2 vs -O0)':>18} |")
    print(f"|{'-'*28}|{'-'*7}|{'-'*7}|{'-'*7}|{'-'*20}|")

    t0 = t1 = t2 = 0
    for name, c0, c1, c2 in rows:
        t0, t1, t2 = t0 + c0, t1 + c1, t2 + c2
        pct = 100.0 * (c0 - c2) / c0 if c0 else 0.0
        print(f"| {name:<26} | {c0:>5} | {c1:>5} | {c2:>5} | {pct:>17.1f}% |")

    if t0:
        print(f"|{'-'*28}|{'-'*7}|{'-'*7}|{'-'*7}|{'-'*20}|")
        print(f"| {'TOTAL':<26} | {t0:>5} | {t1:>5} | {t2:>5} | "
              f"{100.0*(t0-t2)/t0:>17.1f}% |")
        print()
        print(f"O-03 (-O1 vs -O0): {100.0*(t0-t1)/t0:.1f}%   target > 25%")
        print(f"O-04 (-O2 vs -O0): {100.0*(t0-t2)/t0:.1f}%   target > 40%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
