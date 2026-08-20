#!/usr/bin/env python3
"""The North Star: does the profile-guided build beat -O2 on the profiled workload?

For each program this does the whole loop the project exists to demonstrate:

    1. optiforge prog.of -O2 -o baseline
    2. optiforge prog.of -O2 --profile -o instrumented
    3. ./instrumented                                    -> prog.prof
    4. optiforge prog.of -O2 --use-profile=prog.prof -o guided
    5. time baseline and guided, and check they print the same thing

Step 5 is not a formality. A speedup from a binary that computes something else
is not a speedup, and the comparison is worthless without it.

**Minimum of N runs, not the median.** Interference from the rest of the machine
only ever *adds* time, so the fastest run of a set is the closest thing to the
program's own cost; a median moves with whatever else the scheduler was doing.
Runs are interleaved between the two binaries so any drift over the measurement
affects both equally.

`jitter` is how far the baseline's median sat above its own minimum. It is the
amount of interference present, and a speedup below it should be read with
suspicion even though the estimator is more robust than a median.

Run:  python3 bench/harness/pgo_speedup.py --optiforge <path> --dirs bench/programs
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
        print(f"  build failed ({' '.join(extra)}): {proc.stdout}{proc.stderr}",
              file=sys.stderr)
        return None
    return exe


def run(exe: Path, workdir: Path) -> str:
    proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=600,
                          cwd=str(workdir))
    return proc.stdout


def timeOnce(exe: Path, workdir: Path) -> float:
    start = time.perf_counter()
    subprocess.run([str(exe)], capture_output=True, timeout=600, cwd=str(workdir))
    return time.perf_counter() - start


def timeBoth(first: Path, second: Path, workdir: Path,
             repetitions: int) -> tuple[list[float], list[float]]:
    """Times two binaries alternately, after a discarded warm-up of each.

    Interleaved rather than one after the other: a machine that gets busier
    halfway through a measurement would otherwise make whichever binary ran
    second look slower, and that is exactly the shape of error that produces a
    speedup nobody can reproduce.
    """
    timeOnce(first, workdir)
    timeOnce(second, workdir)
    firstSamples: list[float] = []
    secondSamples: list[float] = []
    for _ in range(repetitions):
        firstSamples.append(timeOnce(first, workdir))
        secondSamples.append(timeOnce(second, workdir))
    return firstSamples, secondSamples


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dirs", required=True, nargs="+", type=Path)
    parser.add_argument("--level", default="-O2")
    parser.add_argument("--reps", type=int, default=9)
    opts = parser.parse_args()

    optiforge = opts.optiforge.resolve()
    sources: list[Path] = []
    for directory in opts.dirs:
        sources.extend(sorted(directory.glob("*.of")))
    if not sources:
        print("no .of programs found", file=sys.stderr)
        return 1

    workdir = Path(tempfile.mkdtemp(prefix="optiforge_pgo_"))
    header = (f"| {'Program':<20} | {'-O2 ms':>8} | {'PGO ms':>8} | {'speedup':>8} "
              f"| {'jitter':>7} | {'same output':>11} |")
    rule = ("|" + "-" * 22 + "|" + "-" * 10 + "|" + "-" * 10 + "|" + "-" * 10 + "|"
            + "-" * 9 + "|" + "-" * 13 + "|")
    print(header)
    print(rule)

    wins = 0
    measured = 0
    try:
        for source in sources:
            resolved = source.resolve()
            baseline = build(optiforge, resolved, workdir, source.stem + "_base.exe",
                             [], opts.level)
            instrumented = build(optiforge, resolved, workdir,
                                 source.stem + "_inst.exe", ["--profile"], opts.level)
            if baseline is None or instrumented is None:
                continue

            run(instrumented, workdir)
            profile = workdir / (source.stem + "_inst.prof")
            if not profile.exists():
                print(f"| {source.name:<20} | {'--':>8} | {'--':>8} | "
                      f"{'no profile':>8} | {'':>7} | {'':>11} |")
                continue

            guided = build(optiforge, resolved, workdir, source.stem + "_pgo.exe",
                           [f"--use-profile={profile}"], opts.level)
            if guided is None:
                continue

            # Correctness before speed. A faster binary that computes something
            # else is not a result.
            baseOutput = run(baseline, workdir)
            guidedOutput = run(guided, workdir)
            same = baseOutput == guidedOutput

            baseSamples, guidedSamples = timeBoth(baseline, guided, workdir, opts.reps)
            baseBest = min(baseSamples)
            guidedBest = min(guidedSamples)
            speedup = (baseBest / guidedBest - 1.0) * 100.0 if guidedBest > 0 else 0.0
            jitter = ((statistics.median(baseSamples) - baseBest) / baseBest * 100.0
                      if baseBest > 0 else 0.0)

            measured += 1
            if same and speedup > 1.0:
                wins += 1

            print(f"| {source.name:<20} | {baseBest * 1000:>8.1f} "
                  f"| {guidedBest * 1000:>8.1f} | {speedup:>7.1f}% | {jitter:>6.1f}% "
                  f"| {('yes' if same else 'NO'):>11} |")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print(rule)
    print()
    print(f"PGO-14: {wins} of {measured} program(s) are more than 1% faster under "
          f"profile guidance, with identical output.")
    print("Minimum of "
          f"{opts.reps} interleaved runs each; jitter is how far the baseline's "
          "median sat above its own minimum.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
