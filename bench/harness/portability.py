#!/usr/bin/env python3
"""Metric G-13: how much of the PGO speedup survives a change of workload?

    python3 bench/harness/portability.py --optiforge build-release/bin/optiforge.exe \
                                         --machine windows-mingw --reps 12

Every speedup in `2026-08-20-phase12-benchmarks.md` was measured with the
profile applied to the *same* run it was collected from. That is the best case
for PGO and it is not the case anyone deploys: real builds profile a
representative workload and then run on whatever arrives. G-13 sizes the gap.

For each program with a workload-B variant in `bench/workloads/`, this builds
the B program three ways and times them against each other:

    baseline   B at -O2, no profile
    matched    B compiled against B's own profile   -- the upper bound
    crossed    B compiled against A's profile       -- the realistic case

and reports what fraction of the matched gain the crossed build keeps.

The B variants change only `main`. Every function the profile describes is
character-for-character identical, so the source hash differs -- the compiler
says so -- and matching falls back to names, which all still resolve. That is
deliberate: it isolates *the workload changed* from *the code changed*, which
would otherwise be two variables at once.

A negative retention is a real and expected outcome, not a bug. A profile that
points the optimizer at the wrong loop does not merely fail to help.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run import (TIMEOUT, compile_once, git_revision, run, speedup,  # noqa: E402
                 summarize)


def time_all(builds: list[dict], workdir: Path, reps: int, warmups: int) -> None:
    for _ in range(warmups):
        for build in builds:
            run([build["exe"]], cwd=workdir)
    samples: dict[str, list[float]] = {b["name"]: [] for b in builds}
    for _ in range(reps):
        for build in builds:
            start = time.perf_counter()
            subprocess.run([str(build["exe"])], capture_output=True,
                           timeout=TIMEOUT, cwd=str(workdir))
            samples[build["name"]].append(time.perf_counter() - start)
    for build in builds:
        build["timing"] = summarize(samples[build["name"]])


def collect_profile(optiforge: Path, source: Path, workdir: Path,
                    tag: str) -> Path | None:
    """Builds an instrumented binary, runs it, returns the profile it wrote."""
    prof = workdir / f"{tag}.prof"
    exe = workdir / f"{tag}_inst.exe"
    ok, _, err = compile_once(optiforge, source,
                              ["-O2", "--profile", f"--profile-out={prof}"], exe)
    if not ok:
        print(f"  instrumented build failed: {err[:200]}", file=sys.stderr)
        return None
    run([exe], cwd=workdir)
    return prof if prof.exists() else None


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--machine", required=True)
    parser.add_argument("--programs", type=Path, default=Path("bench/programs"))
    parser.add_argument("--workloads", type=Path, default=Path("bench/workloads"))
    parser.add_argument("--reps", type=int, default=12)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--metrics", type=Path, default=Path("metrics"))
    parser.add_argument("--label", default="pgo-portability")
    parser.add_argument("--noise-floor", type=float, default=1.0)
    opts = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    optiforge = opts.optiforge.resolve()
    if not (opts.metrics / "machines" / f"{opts.machine}.md").exists():
        print(f"error: no machine spec for '{opts.machine}'", file=sys.stderr)
        return 2

    pairs: list[tuple[Path, Path]] = []
    for variant in sorted(opts.workloads.glob("*_b.of")):
        original = opts.programs / f"{variant.stem[:-2]}.of"
        if original.exists():
            pairs.append((original.resolve(), variant.resolve()))
        else:
            print(f"warning: {variant.name} has no workload-A partner at {original}",
                  file=sys.stderr)
    if not pairs:
        print("error: no workload pairs found", file=sys.stderr)
        return 2

    workdir = Path(tempfile.mkdtemp(prefix="optiforge_port_"))
    revision, dirty = git_revision(root)
    started = datetime.now(timezone.utc).astimezone()
    rows: list[dict] = []

    print(f"{len(pairs)} workload pair(s), {opts.reps} interleaved reps after "
          f"{opts.warmups} warm-ups. Noise floor {opts.noise_floor:.1f}%.")
    print()

    try:
        for original, variant in pairs:
            print(f"{variant.name}  (profile from {original.name})")
            prof_a = collect_profile(optiforge, original, workdir, f"{original.stem}_a")
            prof_b = collect_profile(optiforge, variant, workdir, f"{variant.stem}_b")
            if prof_a is None or prof_b is None:
                print("  could not collect both profiles", file=sys.stderr)
                continue

            builds: list[dict] = []
            for name, args in (
                    ("baseline", ["-O2"]),
                    ("matched", ["-O2", f"--use-profile={prof_b}"]),
                    ("crossed", ["-O2", f"--use-profile={prof_a}"])):
                exe = workdir / f"{variant.stem}_{name}.exe"
                ok, _, err = compile_once(optiforge, variant, args, exe)
                if not ok:
                    print(f"  {name} build failed: {err[:200]}", file=sys.stderr)
                    continue
                builds.append({"name": name, "exe": exe, "args": args})

            if len(builds) != 3:
                continue

            # The crossed build is the one that could go wrong quietly, so its
            # match rate is recorded: a profile that matched nothing would show
            # up as "portability is fine", when in fact no profile was applied.
            remarks = compile_once(
                optiforge, variant,
                ["-O2", f"--use-profile={prof_a}", "--pgo-remarks"],
                workdir / f"{variant.stem}_remarks.exe")[2]

            outputs = {b["name"]: run([b["exe"]], cwd=workdir).stdout for b in builds}
            identical = len(set(outputs.values())) == 1
            if not identical:
                print("  *** builds disagree on output ***", file=sys.stderr)

            time_all(builds, workdir, opts.reps, opts.warmups)
            medians = {b["name"]: b["timing"]["median_ms"] for b in builds}
            matched_gain = speedup(medians["baseline"], medians["matched"])
            crossed_gain = speedup(medians["baseline"], medians["crossed"])
            retention = (crossed_gain / matched_gain * 100.0
                         if abs(matched_gain) > opts.noise_floor else None)

            print(f"  baseline {medians['baseline']:7.1f} ms")
            print(f"  matched  {medians['matched']:7.1f} ms   {matched_gain:+.1f}%"
                  "   (B profiled on B -- the upper bound)")
            print(f"  crossed  {medians['crossed']:7.1f} ms   {crossed_gain:+.1f}%"
                  "   (B compiled against A's profile)")
            if retention is None:
                print("  retention: not defined -- the matched gain is itself "
                      "within the noise floor")
            else:
                print(f"  retention: {retention:.0f}% of the matched gain survives")
            print()

            rows.append({
                "program": variant.name,
                "profile_from": original.name,
                "outputs_identical": identical,
                "baseline_ms": medians["baseline"],
                "matched_ms": medians["matched"],
                "crossed_ms": medians["crossed"],
                "matched_gain_pct": matched_gain,
                "crossed_gain_pct": crossed_gain,
                "retention_pct": retention,
                "crossed_remarks": [line for line in remarks.splitlines()
                                    if line.startswith(("profile:", "loop-unroll:",
                                                        "inline:", "layout:"))],
            })
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    date = started.strftime("%Y-%m-%d")
    raw_dir = opts.metrics / "raw" / f"{date}-{revision}"
    raw_dir.mkdir(parents=True, exist_ok=True)
    raw_path = raw_dir / f"{opts.label}.json"
    raw_path.write_text(json.dumps(
        {"meta": {"date": started.strftime("%Y-%m-%d %H:%M %z"), "revision": revision,
                  "dirty": dirty, "machine": opts.machine, "reps": opts.reps,
                  "warmups": opts.warmups, "noise_floor_pct": opts.noise_floor},
         "rows": rows}, indent=2), encoding="utf-8")

    print(f"raw -> {raw_path}")
    print()
    print(f"| {'Workload B':<22} | {'profile from':<20} | {'matched':>8} "
          f"| {'crossed':>8} | {'retained':>9} |")
    print("|" + "-" * 24 + "|" + "-" * 22 + "|" + "-" * 10 + "|" + "-" * 10 + "|"
          + "-" * 11 + "|")
    for row in rows:
        retained = ("n/a" if row["retention_pct"] is None
                    else f"{row['retention_pct']:.0f}%")
        print(f"| {row['program']:<22} | {row['profile_from']:<20} "
              f"| {row['matched_gain_pct']:>+7.1f}% | {row['crossed_gain_pct']:>+7.1f}% "
              f"| {retained:>9} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
