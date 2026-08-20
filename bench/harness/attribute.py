#!/usr/bin/env python3
"""Metric G-05: which profile-guided decision produced the speedup?

    python3 bench/harness/attribute.py --optiforge build-release/bin/optiforge.exe \
                                       --machine windows-mingw --reps 10

`methodology.md` §5, implemented literally:

    1. measure -O2                                     -> baseline
    2. measure -O2 --use-profile                       -> full gain
    3. for each decision P:
           measure -O2 --use-profile --disable-pgo=P   -> gain without P
           contribution(P) = full gain - gain without P
    4. the contributions should sum to about the full gain

Why this exists at all: a speedup nobody can name a cause for is as likely to be
an accidental code-layout shift as anything the profile bought. "PGO is 12%
faster" is a claim; "PGO is 12% faster and 11 of that is block layout, which
removed 40 taken branches per iteration" is a finding.

**A large residual is a result, not a bug in this script.** It means the
decisions do not simply add up -- most often because two of them interact, as
unrolling and register allocation do when the extra copies raise pressure past
what the register file holds. That interaction is exactly the thing worth
reporting, so the residual is printed rather than distributed away.

The decisions come from the compiler (`--help`) rather than a list here, so this
cannot silently miss one that was added later.
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
from run import (TIMEOUT, compile_once, config_keys, git_revision,  # noqa: E402
                 run, speedup, summarize)

DECISIONS = ["inline", "unroll", "regalloc", "layout", "cold-size"]


def time_all(builds: list[dict], workdir: Path, reps: int, warmups: int) -> None:
    """Interleaved timing across every variant of one program (rule 4)."""
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--machine", required=True)
    parser.add_argument("--programs", type=Path, default=Path("bench/programs"))
    parser.add_argument("--reps", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--metrics", type=Path, default=Path("metrics"))
    parser.add_argument("--label", default="pgo-attribution")
    parser.add_argument("--noise-floor", type=float, default=1.0)
    opts = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    optiforge = opts.optiforge.resolve()
    if not (opts.metrics / "machines" / f"{opts.machine}.md").exists():
        print(f"error: no machine spec for '{opts.machine}'", file=sys.stderr)
        return 2

    sources = sorted(opts.programs.glob("*.of"))
    workdir = Path(tempfile.mkdtemp(prefix="optiforge_attr_"))
    revision, dirty = git_revision(root)
    started = datetime.now(timezone.utc).astimezone()
    rows: list[dict] = []

    print(f"Attribution over {len(sources)} program(s), {opts.reps} interleaved "
          f"reps after {opts.warmups} warm-ups. Noise floor {opts.noise_floor:.1f}%.")
    print()

    try:
        for source in sources:
            resolved = source.resolve()
            print(source.name)
            prof = workdir / f"{source.stem}.prof"
            inst = workdir / f"{source.stem}_inst.exe"
            ok, _, err = compile_once(optiforge, resolved,
                                      ["-O2", "--profile", f"--profile-out={prof}"], inst)
            if not ok:
                print(f"  instrumented build failed: {err[:200]}", file=sys.stderr)
                continue
            run([inst], cwd=workdir)
            if not prof.exists():
                print("  no profile written", file=sys.stderr)
                continue

            builds: list[dict] = []
            base = workdir / f"{source.stem}_base.exe"
            ok, _, _ = compile_once(optiforge, resolved, ["-O2"], base)
            if not ok:
                continue
            builds.append({"name": "O2", "exe": base})

            full = workdir / f"{source.stem}_pgo.exe"
            ok, _, _ = compile_once(optiforge, resolved,
                                    ["-O2", f"--use-profile={prof}"], full)
            if not ok:
                continue
            builds.append({"name": "PGO", "exe": full})

            for decision in DECISIONS:
                exe = workdir / f"{source.stem}_no_{decision}.exe"
                ok, _, _ = compile_once(
                    optiforge, resolved,
                    ["-O2", f"--use-profile={prof}", f"--disable-pgo={decision}"], exe)
                if ok:
                    builds.append({"name": f"no-{decision}", "exe": exe})

            # Correctness first: disabling a decision must not change the answer.
            outputs = {b["name"]: run([b["exe"]], cwd=workdir).stdout for b in builds}
            if len(set(outputs.values())) != 1:
                print("  *** variants disagree on output; attribution is meaningless ***",
                      file=sys.stderr)
                for name, text in outputs.items():
                    print(f"      {name}: {text.strip()[:40]}", file=sys.stderr)

            time_all(builds, workdir, opts.reps, opts.warmups)
            medians = {b["name"]: b["timing"]["median_ms"] for b in builds}

            full_gain = speedup(medians["O2"], medians["PGO"])
            row = {
                "program": source.name,
                "outputs_identical": len(set(outputs.values())) == 1,
                "o2_ms": medians["O2"],
                "pgo_ms": medians["PGO"],
                "full_gain_pct": full_gain,
                "contributions": {},
                "without": {},
            }
            print(f"  full PGO gain {full_gain:+.1f}%  "
                  f"(-O2 {medians['O2']:.1f} ms -> PGO {medians['PGO']:.1f} ms)")

            attributed = 0.0
            for decision in DECISIONS:
                key = f"no-{decision}"
                if key not in medians:
                    continue
                without = speedup(medians["O2"], medians[key])
                contribution = full_gain - without
                row["without"][decision] = without
                row["contributions"][decision] = contribution
                attributed += contribution
                marker = "" if abs(contribution) > opts.noise_floor else "   (below noise)"
                print(f"    {decision:<10} contributes {contribution:+6.1f}%"
                      f"   [without it: {without:+.1f}%]{marker}")

            row["attributed_pct"] = attributed
            row["residual_pct"] = full_gain - attributed
            print(f"    {'sum':<10} {attributed:+6.1f}%    "
                  f"residual {row['residual_pct']:+.1f}%")
            print()
            rows.append(row)
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
    print(f"| {'Program':<20} | {'full':>7} |" +
          "".join(f" {d:>9} |" for d in DECISIONS) +
          f" {'residual':>8} |")
    print("|" + "-" * 22 + "|" + "-" * 9 + "|" +
          "".join("-" * 11 + "|" for _ in DECISIONS) + "-" * 10 + "|")
    for row in rows:
        line = f"| {row['program']:<20} | {row['full_gain_pct']:>+6.1f}% |"
        for decision in DECISIONS:
            value = row["contributions"].get(decision)
            line += f" {value:>+8.1f}% |" if value is not None else f" {'--':>9} |"
        line += f" {row['residual_pct']:>+7.1f}% |"
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
