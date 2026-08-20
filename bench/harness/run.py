#!/usr/bin/env python3
"""The Phase 12 benchmark harness: every configuration, every metric, one run.

    python3 bench/harness/run.py --optiforge build-release/bin/optiforge.exe \
                                 --machine windows-mingw --label phase12

For each program in `--programs` it builds one binary per configuration, checks
that they all print the same thing, times them, and collects the static metrics
that do not need timing at all. It writes two files:

    metrics/raw/<date>-<sha>/<label>.json    immutable, append-only, machine-written
    metrics/results/<date>-<label>.md        the table a human cites

`metrics/README.md` section 4 requires the second to be generated rather than
typed, and requires this script to refuse to run without a machine spec. Both
are enforced below.

Configurations
--------------
`O0`, `O1`, `O2` are plain builds. `PGO` is the whole loop the project exists
to demonstrate: build instrumented at `-O2`, run it to produce a `.prof`,
then rebuild at `-O2 --use-profile=`. `INST` is the instrumented binary itself,
timed so instrumentation overhead (I-01) comes out of the same session as
everything else rather than a separate one on a differently-loaded machine.

Repeating a name -- `--configs O2,O2` -- is the noise-floor measurement from
methodology.md section 3.2: two identical configurations whose measured
difference is, by construction, entirely noise.

How the timing is done, and why
-------------------------------
**Interleaved, never batched.** One repetition runs every configuration once,
in order, and then the next repetition does the same. Batching all of `-O2`
before all of `PGO` turns a machine that warms up over the measurement into a
result, and it is the single most common way a benchmark becomes confidently
wrong (methodology.md rule 4).

**Median for the headline, minimum reported alongside.** Interference only ever
adds time, so the minimum is the closest estimate of the program's own cost --
but it is one sample and it moves. methodology.md section 4 asks for the median,
so the median is what the speedup is computed from, with min and IQR beside it
so a reader can see the spread the median came out of.

**Warm-ups discarded.** The first run of a freshly written executable pays for
paging it in; on this platform that alone can be most of a short benchmark.
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

# The configurations this harness knows how to build. PGO and INST need more
# than a flag, so they are handled by name in `build_config`.
PLAIN_LEVELS = {"O0": "-O0", "O1": "-O1", "O2": "-O2"}
KNOWN_CONFIGS = set(PLAIN_LEVELS) | {"PGO", "INST"}

TIMEOUT = 900


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------

def run(argv: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True,
                          timeout=TIMEOUT, cwd=None if cwd is None else str(cwd))


def git_revision(root: Path) -> tuple[str, bool]:
    """Short SHA and whether the tree is dirty.

    The dirty flag is recorded rather than hidden: a result taken from an
    uncommitted tree cannot be reproduced from the SHA alone, and saying so is
    cheaper than discovering it later (methodology.md section 7).
    """
    sha = run(["git", "-C", root, "rev-parse", "--short", "HEAD"])
    status = run(["git", "-C", root, "status", "--porcelain"])
    if sha.returncode != 0:
        return "unknown", True
    return sha.stdout.strip(), bool(status.stdout.strip())


def percentile(values: list[float], q: float) -> float:
    """Linear-interpolated percentile, so IQR is defined for any N."""
    if not values:
        return 0.0
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = q * (len(ordered) - 1)
    low = int(pos)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (pos - low)


def summarize(samples: list[float]) -> dict:
    """Median, min, IQR and spread for one configuration of one program.

    `iqr_over_median` is metric Q-02. Above 3% the environment is too noisy for
    any speedup below roughly twice that to be distinguishable, and the write-up
    has to say so rather than quoting the difference anyway.
    """
    median = statistics.median(samples)
    q1 = percentile(samples, 0.25)
    q3 = percentile(samples, 0.75)
    return {
        "n": len(samples),
        "median_ms": median * 1000.0,
        "min_ms": min(samples) * 1000.0,
        "max_ms": max(samples) * 1000.0,
        "q1_ms": q1 * 1000.0,
        "q3_ms": q3 * 1000.0,
        "iqr_ms": (q3 - q1) * 1000.0,
        "iqr_over_median_pct": ((q3 - q1) / median * 100.0) if median > 0 else 0.0,
        "samples_ms": [s * 1000.0 for s in samples],
    }


# ---------------------------------------------------------------------------
# Static metrics -- no timing, so no noise
# ---------------------------------------------------------------------------

def text_size(exe: Path) -> int | None:
    """`.text` bytes (metric Q-03), via binutils `size`. None if it is absent."""
    if shutil.which("size") is None:
        return None
    proc = run(["size", exe])
    if proc.returncode != 0:
        return None
    for line in proc.stdout.splitlines()[1:]:
        parts = line.split()
        if parts and parts[0].isdigit():
            return int(parts[0])
    return None


def count_asm_instructions(text: str) -> int:
    """Instructions in emitted assembly (metric Q-05).

    An instruction is an indented line that is not a directive, a label or a
    bare comment. Directives are excluded because `.globl` and `.def` are not
    things the processor executes.
    """
    total = 0
    for line in text.splitlines():
        if not line[:1].isspace():
            continue
        stripped = line.strip()
        if not stripped or stripped.startswith((".", "#", "//")):
            continue
        total += 1
    return total


def count_ir_instructions(text: str) -> int:
    """IR instructions after the pipeline (metric Q-04, feeding O-03/O-04).

    Same rule as `count_ir.py`: a line inside a function body that is not a
    label, a blank or a comment.
    """
    total = 0
    inside = False
    for line in text.splitlines():
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


REGALLOC_RE = re.compile(
    r"@(?P<fn>\S+): (?P<units>\d+) unit\(s\), (?P<coloured>\d+) coloured, "
    r"(?P<spilled>\d+) spilled, (?P<coalesced>\d+) coalesced, (?P<frozen>\d+) frozen, "
    r"peak (?P<peak>\d+) live")


def regalloc_totals(stderr: str) -> dict:
    """Spills (Q-07) and peak pressure (Q-08), summed over the module."""
    spilled = coloured = 0
    peak = 0
    for match in REGALLOC_RE.finditer(stderr):
        spilled += int(match.group("spilled"))
        coloured += int(match.group("coloured"))
        peak = max(peak, int(match.group("peak")))
    return {"spilled": spilled, "coloured": coloured, "max_pressure": peak}


MATCH_RATE_RE = re.compile(r"profile: match rate (\d+)/(\d+) function\(s\)")


def match_rate(stderr: str) -> float | None:
    """Metric G-03, read back from --pgo-remarks.

    Checked before any speedup is believed: if this collapses, every PGO pass
    silently takes its no-profile path and the build looks correct while doing
    nothing at all.
    """
    match = MATCH_RATE_RE.search(stderr)
    if match is None:
        return None
    total = int(match.group(2))
    return 100.0 * int(match.group(1)) / total if total else 100.0


# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------

def compile_once(optiforge: Path, source: Path, args: list[str],
                 exe: Path) -> tuple[bool, float, str]:
    """Builds one binary. Returns (ok, compile seconds, stderr).

    The compile time is metric P-01/P-02, and P-05 when the arguments include
    `--use-profile`: the same measurement in all three cases, taken here rather
    than in a separate script so the compiler being timed is definitely the one
    that produced the binary being timed.
    """
    start = time.perf_counter()
    proc = run([optiforge, source, *args, "-o", exe])
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        return False, elapsed, proc.stdout + proc.stderr
    return True, elapsed, proc.stderr


def build_config(optiforge: Path, source: Path, config: str, workdir: Path,
                 tag: str) -> dict | None:
    """Builds one configuration and collects everything static about it."""
    stem = f"{source.stem}_{tag}"
    exe = workdir / f"{stem}.exe"
    info: dict = {"config": config}

    if config in PLAIN_LEVELS:
        level = PLAIN_LEVELS[config]
        ok, seconds, err = compile_once(optiforge, source, [level, "--print-regalloc"], exe)
        if not ok:
            print(f"  build failed ({config}): {err.strip()[:300]}", file=sys.stderr)
            return None
        info["compile_s"] = seconds
        info["regalloc"] = regalloc_totals(err)
        asm = run([optiforge, source, level, "--emit=asm"])
        info["asm_instructions"] = count_asm_instructions(asm.stdout)
        ir = run([optiforge, source, level, "--emit=ir"])
        info["ir_instructions"] = count_ir_instructions(ir.stdout)

    elif config == "INST":
        ok, seconds, err = compile_once(
            optiforge, source, ["-O2", "--profile",
                                f"--profile-out={workdir / (stem + '.prof')}"], exe)
        if not ok:
            print(f"  build failed (INST): {err.strip()[:300]}", file=sys.stderr)
            return None
        info["compile_s"] = seconds

    elif config == "PGO":
        # The full loop, and the profile is collected here rather than reused
        # from the INST configuration so that this configuration is buildable
        # on its own.
        inst = workdir / f"{stem}_inst.exe"
        prof = workdir / f"{stem}.prof"
        ok, _, err = compile_once(optiforge, source,
                                  ["-O2", "--profile", f"--profile-out={prof}"], inst)
        if not ok:
            print(f"  build failed (PGO instrument): {err.strip()[:300]}", file=sys.stderr)
            return None
        run([inst], cwd=workdir)
        if not prof.exists():
            print("  PGO: the instrumented binary wrote no profile", file=sys.stderr)
            return None
        info["profile_bytes"] = prof.stat().st_size

        ok, seconds, err = compile_once(
            optiforge, source,
            ["-O2", f"--use-profile={prof}", "--pgo-remarks", "--print-regalloc"], exe)
        if not ok:
            print(f"  build failed (PGO): {err.strip()[:300]}", file=sys.stderr)
            return None
        info["compile_s"] = seconds
        info["regalloc"] = regalloc_totals(err)
        info["match_rate_pct"] = match_rate(err)
        info["remarks"] = [line for line in err.splitlines()
                           if line.startswith(("inline:", "loop-unroll:", "layout:",
                                               "profile:", "pgo:"))]
        asm = run([optiforge, source, "-O2", f"--use-profile={prof}", "--emit=asm"])
        info["asm_instructions"] = count_asm_instructions(asm.stdout)
        ir = run([optiforge, source, "-O2", f"--use-profile={prof}", "--emit=ir"])
        info["ir_instructions"] = count_ir_instructions(ir.stdout)

    else:
        raise ValueError(config)

    info["exe"] = exe
    info["text_bytes"] = text_size(exe)
    result = run([exe], cwd=workdir)
    info["stdout"] = result.stdout
    info["exit_code"] = result.returncode
    return info


# ---------------------------------------------------------------------------
# Timing
# ---------------------------------------------------------------------------

def time_interleaved(builds: list[dict], workdir: Path, reps: int,
                     warmups: int) -> None:
    """Times every configuration of one program, interleaved (rule 4)."""
    for _ in range(warmups):
        for build in builds:
            run([build["exe"]], cwd=workdir)

    samples: dict[int, list[float]] = {id(b): [] for b in builds}
    for _ in range(reps):
        for build in builds:
            start = time.perf_counter()
            subprocess.run([str(build["exe"])], capture_output=True,
                           timeout=TIMEOUT, cwd=str(workdir))
            samples[id(build)].append(time.perf_counter() - start)

    for build in builds:
        build["timing"] = summarize(samples[id(build)])


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def config_keys(configs: list[str]) -> list[str]:
    """The key each configuration is stored and rendered under.

    A repeated configuration -- `--configs O2,O2`, the noise-floor run -- needs
    two distinct keys, so the position is appended. One function decides this so
    the writer and the reader cannot disagree.
    """
    return [config if configs.count(config) == 1 else f"{config}#{index}"
            for index, config in enumerate(configs)]


def speedup(baseline_ms: float, treatment_ms: float) -> float:
    return (baseline_ms / treatment_ms - 1.0) * 100.0 if treatment_ms > 0 else 0.0


def render_markdown(meta: dict, rows: list[dict], configs: list[str]) -> str:
    out: list[str] = []
    add = out.append
    keys = config_keys(configs)

    add(f"# Benchmark Run - `{meta['label']}`")
    add("")
    add("> Generated by `bench/harness/run.py`. The analysis sections at the end")
    add("> are written by hand and the run is not citable until they are"
        " (`metrics/README.md` section 4).")
    add("")
    add("## 1. Provenance")
    add("")
    add("| Field | Value |")
    add("|---|---|")
    add(f"| Date | {meta['date']} |")
    add(f"| Git revision | `{meta['revision']}`"
        f"{' **+ uncommitted changes**' if meta['dirty'] else ''} |")
    add(f"| Machine | [`{meta['machine']}`](../machines/{meta['machine']}.md) |")
    add(f"| Host | {meta['host']} |")
    add(f"| Configurations | {', '.join(configs)} |")
    add(f"| Reps / warm-ups discarded | {meta['reps']} / {meta['warmups']} |")
    plural = "" if meta["program_count"] == 1 else "s"
    add(f"| Corpus | {meta['program_count']} program{plural} in `{meta['programs']}` |")
    add(f"| Raw data | [`../raw/{meta['raw_dir']}/`](../raw/{meta['raw_dir']}/) |")
    add("| Profile source (PGO rows) | the same workload the row is timed on |")
    add("")
    add("Timing is interleaved across configurations, one repetition of each in"
        " turn, so drift over the session hits every configuration equally.")
    add("")

    # --- Correctness gate, printed before any speed number ---
    add("## 2. Correctness gate (metric C-05)")
    add("")
    add("Every configuration of a program must print the same bytes. A faster"
        " binary that computes something else is not a result.")
    add("")
    add("| Program | Configurations agree | Output |")
    add("|---|---|---|")
    all_agree = True
    for row in rows:
        agree = row["outputs_identical"]
        all_agree = all_agree and agree
        shown = row["output"].strip().replace("|", r"\|")
        if len(shown) > 30:
            shown = shown[:27] + "..."
        add(f"| `{row['program']}` | {'yes' if agree else '**NO**'} | `{shown}` |")
    add("")
    add(f"**C-05: {'100% -- every configuration agrees.' if all_agree else 'FAILED. No performance number below is valid.'}**")
    add("")

    # --- Headline ---
    add("## 3. Runtime by configuration (metrics Q-01, Q-02, G-02)")
    add("")
    header = "| Program |" + "".join(f" {c} med (ms) |" for c in keys)
    if "O2" in configs and "PGO" in configs:
        header += " **PGO vs -O2** | above noise? |"
    add(header)
    add("|---" * (header.count("|") - 1) + "|")
    for row in rows:
        line = f"| `{row['program']}` |"
        for key in keys:
            timing = row["configs"].get(key, {}).get("timing")
            line += f" {timing['median_ms']:.1f} |" if timing else " -- |"
        if "O2" in configs and "PGO" in configs:
            gain = row.get("pgo_speedup_pct")
            floor = row.get("noise_floor_pct", 0.0)
            if gain is None:
                line += " -- | -- |"
            else:
                real = gain > floor
                line += f" **{gain:+.1f}%** | {'yes' if real else 'no'} |"
        add(line)
    add("")
    add("### Spread (metric Q-02 -- gates everything above)")
    add("")
    add("| Program | " + " | ".join(f"{c} IQR/med" for c in keys) +
        " | min (ms) | Under 3%? |")
    add("|---" * (len(keys) + 3) + "|")
    for row in rows:
        spreads = []
        worst = 0.0
        for key in keys:
            timing = row["configs"].get(key, {}).get("timing")
            if timing:
                spreads.append(f"{timing['iqr_over_median_pct']:.1f}%")
                worst = max(worst, timing["iqr_over_median_pct"])
            else:
                spreads.append("--")
        best = row["configs"].get(keys[-1], {}).get("timing")
        line = f"| `{row['program']}` | " + " | ".join(spreads)
        if best:
            line += f" | {best['min_ms']:.1f} | {'yes' if worst < 3.0 else '**no**'} |"
        else:
            line += " | -- | -- |"
        add(line)
    add("")

    # --- Profile health ---
    if "PGO" in configs:
        add("## 4. Profile health (metrics G-03, I-01, I-03)")
        add("")
        add("> Read this table before believing section 3. A collapsed match rate"
            " makes every PGO pass fall back to its no-profile path, which looks"
            " exactly like a null result.")
        add("")
        add("| Program | G-03 match rate | I-01 instrumentation overhead |"
            " I-03 profile size |")
        add("|---|---|---|---|")
        for row in rows:
            pgo = row["configs"].get("PGO", {})
            rate = pgo.get("match_rate_pct")
            overhead = row.get("instrumentation_overhead_pct")
            size = pgo.get("profile_bytes")
            add(f"| `{row['program']}` | {rate:.1f}% |" if rate is not None
                else f"| `{row['program']}` | -- |")
            out[-1] += (f" {overhead:+.1f}% |" if overhead is not None else " -- |")
            out[-1] += (f" {size / 1024.0:.1f} KB |" if size else " -- |")
        add("")

    # --- Code quality ---
    add("## 5. Generated code (metrics Q-03, Q-05, Q-07, Q-08)")
    add("")
    add("Static counts. They say how many instructions exist, not how many run.")
    add("")
    add("| Program | `.text` -O2 | `.text` PGO | asm instrs -O2 | asm instrs PGO |"
        " spills -O2 | spills PGO | peak live |")
    add("|---|---|---|---|---|---|---|---|")
    for row in rows:
        o2 = row["configs"].get("O2", {})
        pgo = row["configs"].get("PGO", {})

        def cell(source: dict, key: str, path: str | None = None) -> str:
            value = source.get(key) if path is None else source.get(key, {}).get(path)
            return "--" if value is None else str(value)

        add(f"| `{row['program']}` | {cell(o2, 'text_bytes')} | {cell(pgo, 'text_bytes')} |"
            f" {cell(o2, 'asm_instructions')} | {cell(pgo, 'asm_instructions')} |"
            f" {cell(o2, 'regalloc', 'spilled')} | {cell(pgo, 'regalloc', 'spilled')} |"
            f" {cell(o2, 'regalloc', 'max_pressure')} |")
    add("")

    # --- Optimization effectiveness ---
    levels = [c for c in ("O0", "O1", "O2") if c in configs]
    if len(levels) >= 2:
        add("## 6. Optimization effectiveness (metrics O-03, O-04, Q-04)")
        add("")
        add("| Program | " + " | ".join(f"IR {c}" for c in levels) +
            " | O-04 reduction |")
        add("|---" * (len(levels) + 2) + "|")
        totals = {c: 0 for c in levels}
        for row in rows:
            counts = {c: row["configs"].get(c, {}).get("ir_instructions") for c in levels}
            for c in levels:
                if counts[c]:
                    totals[c] += counts[c]
            cells = " | ".join("--" if counts[c] is None else str(counts[c]) for c in levels)
            if counts.get("O0") and counts.get("O2"):
                drop = f"{100.0 * (counts['O0'] - counts['O2']) / counts['O0']:.1f}%"
            else:
                drop = "--"
            add(f"| `{row['program']}` | {cells} | {drop} |")
        if totals.get("O0"):
            cells = " | ".join(str(totals[c]) for c in levels)
            add(f"| **TOTAL** | {cells} | "
                f"**{100.0 * (totals['O0'] - totals['O2']) / totals['O0']:.1f}%** |")
        add("")

    # --- Compiler performance ---
    add("## 7. Compiler performance (metrics P-01, P-02, P-05)")
    add("")
    add("| Program | compile -O0 (s) | compile -O2 (s) | compile PGO (s) |"
        " P-05 ratio |")
    add("|---|---|---|---|---|")
    for row in rows:
        def seconds(config: str) -> str:
            value = row["configs"].get(config, {}).get("compile_s")
            return "--" if value is None else f"{value:.2f}"
        o2 = row["configs"].get("O2", {}).get("compile_s")
        pgo = row["configs"].get("PGO", {}).get("compile_s")
        ratio = f"{pgo / o2:.2f}x" if o2 and pgo else "--"
        add(f"| `{row['program']}` | {seconds('O0')} | {seconds('O2')} |"
            f" {seconds('PGO')} | {ratio} |")
    add("")

    add("## 8. Analysis (written by hand -- required)")
    add("")
    add("**What moved, and why.**")
    add("")
    add("**What did not move, and why not.**")
    add("")
    add("**Where PGO lost.**")
    add("")
    add("## 9. Anomalies and threats to validity (written by hand -- required)")
    add("")
    add("## 10. Actions")
    add("")
    add("| Action | Metric | Status |")
    add("|---|---|---|")
    add("")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--machine", required=True,
                        help="machine id; metrics/machines/<id>.md must exist")
    parser.add_argument("--label", default="benchmark-run",
                        help="names the result file and the raw directory")
    parser.add_argument("--configs", default="O0,O1,O2,INST,PGO")
    parser.add_argument("--programs", type=Path, default=Path("bench/programs"))
    parser.add_argument("--reps", type=int, default=10)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--metrics", type=Path, default=Path("metrics"))
    parser.add_argument("--noise-floor", type=float, default=None,
                        help="known noise floor in %%, used to mark a result real")
    opts = parser.parse_args()

    root = Path(__file__).resolve().parents[2]
    optiforge = opts.optiforge.resolve()
    if not optiforge.exists():
        print(f"error: no compiler at {optiforge}", file=sys.stderr)
        return 3

    # Provenance is not optional (metrics/README.md section 4).
    spec = (opts.metrics / "machines" / f"{opts.machine}.md")
    if not spec.exists():
        print(f"error: no machine spec at {spec}.\n"
              "  A result without a machine specification is not admissible"
              " (methodology.md section 7).\n"
              f"  Copy {opts.metrics / 'machines' / 'TEMPLATE.md'} and fill it in.",
              file=sys.stderr)
        return 2

    configs = [c.strip() for c in opts.configs.split(",") if c.strip()]
    unknown = [c for c in configs if c not in KNOWN_CONFIGS]
    if unknown:
        print(f"error: unknown configuration(s) {unknown}; "
              f"known: {sorted(KNOWN_CONFIGS)}", file=sys.stderr)
        return 2

    sources = sorted(opts.programs.glob("*.of"))
    if not sources:
        print(f"error: no .of programs in {opts.programs}", file=sys.stderr)
        return 2

    revision, dirty = git_revision(root)
    started = datetime.now(timezone.utc).astimezone()
    date = started.strftime("%Y-%m-%d")
    raw_dir_name = f"{date}-{revision}"
    raw_dir = opts.metrics / "raw" / raw_dir_name
    raw_dir.mkdir(parents=True, exist_ok=True)

    workdir = Path(tempfile.mkdtemp(prefix="optiforge_bench_"))
    rows: list[dict] = []

    print(f"{len(sources)} program(s), configurations {configs}, "
          f"{opts.reps} reps after {opts.warmups} warm-ups")
    print()

    try:
        for source in sources:
            print(f"{source.name}")
            resolved = source.resolve()
            builds: list[dict] = []
            row: dict = {"program": source.name, "configs": {}}

            failed = False
            for index, config in enumerate(configs):
                # `tag` disambiguates a repeated configuration, which is what a
                # noise-floor run (`--configs O2,O2`) is made of.
                info = build_config(optiforge, resolved, config, workdir,
                                    f"{config}{index}")
                if info is None:
                    failed = True
                    break
                builds.append(info)
                row["configs"][config_keys(configs)[index]] = info
            if failed:
                continue

            outputs = {b["stdout"] for b in builds}
            row["outputs_identical"] = len(outputs) == 1
            row["output"] = builds[0]["stdout"]
            if not row["outputs_identical"]:
                print("  *** OUTPUTS DIFFER between configurations -- "
                      "every timing below is meaningless ***", file=sys.stderr)
                for build in builds:
                    print(f"      {build['config']}: {build['stdout'].strip()[:60]}",
                          file=sys.stderr)

            time_interleaved(builds, workdir, opts.reps, opts.warmups)

            for build in builds:
                print(f"  {build['config']:<5} median {build['timing']['median_ms']:8.1f} ms"
                      f"   min {build['timing']['min_ms']:8.1f}"
                      f"   IQR/med {build['timing']['iqr_over_median_pct']:5.1f}%")

            named = row["configs"]
            if "O2" in named and "PGO" in named:
                row["pgo_speedup_pct"] = speedup(named["O2"]["timing"]["median_ms"],
                                                 named["PGO"]["timing"]["median_ms"])
                print(f"  PGO vs -O2: {row['pgo_speedup_pct']:+.1f}%")
            if "O2" in named and "INST" in named:
                row["instrumentation_overhead_pct"] = speedup(
                    named["INST"]["timing"]["median_ms"],
                    named["O2"]["timing"]["median_ms"])
            if opts.noise_floor is not None:
                row["noise_floor_pct"] = opts.noise_floor
            repeated = [k for k in named if k.startswith(f"{configs[0]}#")]
            if len(repeated) == 2:
                # A noise-floor run: two builds of the same configuration, whose
                # measured difference is by construction entirely interference.
                row["noise_floor_measured_pct"] = abs(speedup(
                    named[repeated[0]]["timing"]["median_ms"],
                    named[repeated[1]]["timing"]["median_ms"]))
                print(f"  same-config difference: "
                      f"{row['noise_floor_measured_pct']:.2f}%")

            # The executable paths are scratch and are about to be deleted.
            for info in row["configs"].values():
                info.pop("exe", None)
            rows.append(row)
            print()
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    meta = {
        "label": opts.label,
        "date": started.strftime("%Y-%m-%d %H:%M %z"),
        "revision": revision,
        "dirty": dirty,
        "machine": opts.machine,
        "host": f"{platform.system()} {platform.release()}, Python {platform.python_version()}",
        "reps": opts.reps,
        "warmups": opts.warmups,
        "programs": str(opts.programs).replace("\\", "/"),
        "program_count": len(rows),
        "raw_dir": raw_dir_name,
        "configs": configs,
    }

    raw_path = raw_dir / f"{opts.label}.json"
    raw_path.write_text(json.dumps({"meta": meta, "rows": rows}, indent=2,
                                   default=str), encoding="utf-8")

    result_path = opts.metrics / "results" / f"{date}-{opts.label}.md"
    result_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.write_text(render_markdown(meta, rows, configs), encoding="utf-8")

    print(f"raw     -> {raw_path}")
    print(f"results -> {result_path}")
    print()
    print("Sections 8 and 9 of the result file are empty and have to be written"
          " before it is committed.")

    if any(not row["outputs_identical"] for row in rows):
        print("\nC-05 FAILED: at least one program disagreed across configurations.",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
