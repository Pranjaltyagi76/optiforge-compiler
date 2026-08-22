#!/usr/bin/env python3
"""Metric G-09: what fraction of executed branches fall through?

    python3 bench/harness/fallthrough.py --optiforge build-release/bin/optiforge.exe \
                                         --dirs bench/programs

`metric-catalog.md` defines G-09 as "fall-through rate on hot edges, `-O2` vs
PGO", target "> 0". It has never been measured, because until now the only way
anyone tried to see block layout was on a stopwatch -- and layout's effect is
small enough that the stopwatch mostly measured the machine.

**This needs no clock.** Everything it uses is static or comes from the profile:
the emitted block order, each block's terminator, and how many times each edge
actually ran. So it is exactly reproducible, and a difference it reports is a
difference in the generated code rather than in the weather.

How an edge is counted
----------------------
For every conditional branch the compiler emitted:

    .L_a:                      the profile knows how many times each of a's two
        ...                    successors was reached
        jcc  .L_target         one successor costs a taken branch
    .L_next:                   the other costs nothing -- it falls through

The profile's `BRANCH f b taken=T not_taken=N` counts b's two *IR* successors,
in the order the `condbr` names them. Block layout is free to invert the
condition and swap which one the machine jumps to, so the IR order cannot be
assumed to match the assembly. The IR is read as well, and the two are matched
by block name -- which is the whole reason requirement IR-11 insists block names
are stable.

`fall-through rate` is then executed-fall-through-edges over executed-conditional
-edges. Higher is better: a fall-through costs no branch and does not consume a
predictor entry.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ASM_LABEL = re.compile(r"^\.L_(\S+):")
JCC = re.compile(r"^\s+(j\w+)\s+(\.L_\S+)")
IR_FN = re.compile(r"^fn @(\w+)\(")
IR_BLOCK = re.compile(r"^(\S+):")
IR_CONDBR = re.compile(r"^\s*condbr\s+\S+,\s*(\S+),\s*(\S+)")
PROF_BRANCH = re.compile(r"^BRANCH (\S+) (\S+) taken=(\d+) not_taken=(\d+)")


def ir_successors(text: str) -> dict[tuple[str, str], tuple[str, str]]:
    """(function, block) -> (taken successor, not-taken successor)."""
    out: dict[tuple[str, str], tuple[str, str]] = {}
    function = ""
    block = ""
    for line in text.splitlines():
        m = IR_FN.match(line)
        if m:
            function = m.group(1)
            continue
        if line and not line[0].isspace():
            m = IR_BLOCK.match(line)
            if m and not line.startswith(("fn ", "module", "}")):
                block = m.group(1)
            continue
        m = IR_CONDBR.match(line)
        if m and function and block:
            out[(function, block)] = (m.group(1), m.group(2))
    return out


def asm_layout(text: str) -> list[tuple[str, str | None]]:
    """Emitted blocks in order, each with its conditional-branch target."""
    blocks: list[tuple[str, str | None]] = []
    label = None
    target = None
    for line in text.splitlines():
        m = ASM_LABEL.match(line)
        if m:
            if label is not None:
                blocks.append((label, target))
            label, target = m.group(1), None
            continue
        m = JCC.match(line)
        if m and m.group(1) != "jmp" and label is not None:
            target = m.group(2)[3:]  # strip ".L_"
    if label is not None:
        blocks.append((label, target))
    return blocks


def branch_counts(profile: Path) -> dict[tuple[str, str], tuple[int, int]]:
    out: dict[tuple[str, str], tuple[int, int]] = {}
    for line in profile.read_text(encoding="utf-8", errors="replace").splitlines():
        m = PROF_BRANCH.match(line)
        if m:
            out[(m.group(1), m.group(2))] = (int(m.group(3)), int(m.group(4)))
    return out


def rate(optiforge: Path, source: Path, profile: Path,
         extra: list[str]) -> tuple[int, int]:
    """(executed fall-through edges, executed conditional edges)."""
    ir = subprocess.run([str(optiforge), str(source), "-O2", *extra, "--emit=ir"],
                        capture_output=True, text=True).stdout
    asm = subprocess.run([str(optiforge), str(source), "-O2", *extra, "--emit=asm"],
                         capture_output=True, text=True).stdout
    successors = ir_successors(ir)
    counts = branch_counts(profile)
    layout = asm_layout(asm)

    fell = 0
    total = 0
    for index, (label, target) in enumerate(layout):
        if target is None or index + 1 >= len(layout):
            continue
        # `.L_<fn>_<block with dots as underscores>` -- match by trying every
        # (function, block) the profile knows against this label.
        match = None
        for (function, block) in counts:
            if label == f"{function}_{block.replace('.', '_')}":
                match = (function, block)
                break
        if match is None or match not in successors:
            continue

        taken_count, not_taken_count = counts[match]
        taken_block, not_taken_block = successors[match]
        function = match[0]

        # **An edge only falls through if its successor is literally the next
        # block emitted.** Assuming "the successor the jcc does not name" falls
        # through is wrong and flatters the baseline: when neither successor is
        # next, the block ends with the conditional *and* an unconditional jump,
        # and both edges cost a branch.
        next_label = layout[index + 1][0]
        taken_label = f"{function}_{taken_block.replace('.', '_')}"
        not_taken_label = f"{function}_{not_taken_block.replace('.', '_')}"

        if next_label == taken_label:
            fell += taken_count
        elif next_label == not_taken_label:
            fell += not_taken_count
        # else: neither successor follows, so nothing falls through here.
        total += taken_count + not_taken_count
    return fell, total


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dirs", nargs="+", required=True, type=Path)
    parser.add_argument("--workdir", type=Path, default=Path("."))
    opts = parser.parse_args()

    optiforge = opts.optiforge.resolve()
    sources = sorted(s for d in opts.dirs for s in d.glob("*.of"))
    if not sources:
        print("no .of programs found", file=sys.stderr)
        return 1

    import tempfile
    import shutil
    work = Path(tempfile.mkdtemp(prefix="optiforge_ft_"))

    header = (f"| {'Program':<22} | {'-O2':>8} | {'PGO':>8} | {'G-09':>8} |")
    rule = "|" + "-" * 24 + "|" + "-" * 10 + "|" + "-" * 10 + "|" + "-" * 10 + "|"
    print(header)
    print(rule)

    improved = 0
    measured = 0
    try:
        for source in sources:
            resolved = source.resolve()
            profile = work / (source.stem + ".prof")
            instrumented = work / (source.stem + "_i.exe")
            build = subprocess.run(
                [str(optiforge), str(resolved), "-O2", "--profile",
                 f"--profile-out={profile}", "-o", str(instrumented)],
                capture_output=True, text=True)
            if build.returncode != 0 or subprocess.run(
                    [str(instrumented)], capture_output=True, cwd=str(work)) is None:
                continue
            subprocess.run([str(instrumented)], capture_output=True, cwd=str(work),
                           timeout=900)
            if not profile.exists():
                continue

            base_fell, base_total = rate(optiforge, resolved, profile,
                                         [f"--use-profile={profile}",
                                          "--disable-pgo=layout"])
            pgo_fell, pgo_total = rate(optiforge, resolved, profile,
                                       [f"--use-profile={profile}"])
            if base_total == 0 or pgo_total == 0:
                print(f"| {source.name:<22} | {'--':>8} | {'--':>8} | "
                      f"{'no branches':>8} |")
                continue

            base_rate = 100.0 * base_fell / base_total
            pgo_rate = 100.0 * pgo_fell / pgo_total
            measured += 1
            if pgo_rate > base_rate + 0.05:
                improved += 1
            print(f"| {source.name:<22} | {base_rate:>7.1f}% | {pgo_rate:>7.1f}% "
                  f"| {pgo_rate - base_rate:>+7.1f} |")
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(rule)
    print()
    print(f"G-09: layout raised the executed fall-through rate on {improved} of "
          f"{measured} program(s). Target: > 0.")
    print("Baseline is `--disable-pgo=layout`, so the comparison is layout and "
          "nothing else.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
