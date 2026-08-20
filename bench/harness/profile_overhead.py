#!/usr/bin/env python3
"""Instrumentation overhead: how much slower is --profile? (metric PROF-13)

Compiles each program twice, plain and instrumented, runs both N times and
compares the medians. The target is under 40%.

Medians, not means: on a desktop the slowest run of a set is usually the
scheduler, not the program, and one outlier moves a mean a long way. Variance is
reported so a result that is mostly noise is visible as one.

Run:  python3 bench/harness/profile_overhead.py --optiforge <path> --dirs bench/programs
"""

from __future__ import annotations

import argparse
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def build(optiforge: Path, source: Path, workdir: Path, name: str,
          extra: list[str], level: str) -> Path | None:
    exe = workdir / name
    proc = subprocess.run([str(optiforge), str(source), level, *extra, "-o", str(exe)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"  compile failed: {proc.stdout}{proc.stderr}", file=sys.stderr)
        return None
    return exe


def timeRun(exe: Path, workdir: Path, repetitions: int) -> list[float]:
    # One discarded warm-up. The first run of a freshly written executable pays
    # for paging it in, and on this platform that alone can double the wall time
    # -- which would show up as instrumentation overhead it has nothing to do
    # with (methodology.md section 3).
    subprocess.run([str(exe)], capture_output=True, timeout=300, cwd=str(workdir))
    samples = []
    for _ in range(repetitions):
        start = time.perf_counter()
        subprocess.run([str(exe)], capture_output=True, timeout=300, cwd=str(workdir))
        samples.append(time.perf_counter() - start)
    return samples


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dirs", required=True, nargs="+", type=Path)
    parser.add_argument("--level", default="-O2")
    parser.add_argument("--reps", type=int, default=7)
    opts = parser.parse_args()

    optiforge = opts.optiforge.resolve()
    sources: list[Path] = []
    for directory in opts.dirs:
        sources.extend(sorted(directory.glob("*.of")))
    if not sources:
        print("no .of programs found", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="optiforge_overhead_"))
    header = f"| {'Program':<24} | {'plain ms':>9} | {'instr ms':>9} | {'overhead':>9} | {'spread':>7} |"
    rule = "|" + "-" * 26 + "|" + "-" * 11 + "|" + "-" * 11 + "|" + "-" * 11 + "|" + "-" * 9 + "|"
    print(header)
    print(rule)

    worst = 0.0
    try:
        for source in sources:
            plain = build(optiforge, source.resolve(), workdir, source.stem + "_p.exe",
                          [], opts.level)
            instrumented = build(optiforge, source.resolve(), workdir,
                                 source.stem + "_i.exe", ["--profile"], opts.level)
            if plain is None or instrumented is None:
                continue

            plainSamples = timeRun(plain, workdir, opts.reps)
            instrSamples = timeRun(instrumented, workdir, opts.reps)
            plainMedian = statistics.median(plainSamples)
            instrMedian = statistics.median(instrSamples)
            overhead = (instrMedian / plainMedian - 1.0) * 100.0 if plainMedian > 0 else 0.0
            worst = max(worst, overhead)

            # How far the plain runs spread, as a share of their own median. An
            # overhead smaller than this number is not a measurement.
            spread = ((max(plainSamples) - min(plainSamples)) / plainMedian * 100.0
                      if plainMedian > 0 else 0.0)

            print(f"| {source.name:<24} | {plainMedian * 1000:>9.1f} "
                  f"| {instrMedian * 1000:>9.1f} | {overhead:>8.1f}% | {spread:>6.1f}% |")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print(rule)
    print()
    print(f"PROF-13 worst-case instrumentation overhead at {opts.level}: "
          f"{worst:.1f}%  (target: under 40%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
