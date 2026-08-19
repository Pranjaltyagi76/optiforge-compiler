#!/usr/bin/env python3
"""End-to-end test runner: compile, run, compare output.

Each test is a .of program annotated with its expected result:

    // EXPECT-OUTPUT: 832040
    // EXPECT-EXIT: 0

Every EXPECT-OUTPUT line is one expected line of stdout, in order.
EXPECT-EXIT is optional and defaults to 0.

The suite is run once per optimization level. Because the *same* program must
produce the *same* output at every level, this doubles as the differential
test category (requirement QA-05) -- the highest-value tests in the project,
since every future optimization pass is covered by every existing program the
moment it is added.

Run:  python3 tests/run_e2e.py --optiforge <path> --dir tests/e2e
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECT_OUTPUT = re.compile(r"^\s*//\s*EXPECT-OUTPUT:\s?(.*)$")
EXPECT_EXIT = re.compile(r"^\s*//\s*EXPECT-EXIT:\s*(-?\d+)\s*$")


def parse_expectations(path: Path) -> tuple[list[str], int]:
    lines: list[str] = []
    exit_code = 0
    for line in path.read_text(encoding="utf-8").splitlines():
        m = EXPECT_OUTPUT.match(line)
        if m:
            lines.append(m.group(1).rstrip())
            continue
        m = EXPECT_EXIT.match(line)
        if m:
            exit_code = int(m.group(1))
    return lines, exit_code


def normalize(text: str) -> list[str]:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return [ln.rstrip() for ln in text.split("\n") if ln.strip() != ""]


def run_case(optiforge: Path, source: Path, workdir: Path, opt: str,
             extra: list[str]) -> tuple[bool, str]:
    tag = "".join(c for c in opt.lstrip("-") + "".join(extra) if c.isalnum())
    exe = workdir / (source.stem + "_" + tag + ".exe")

    compile_proc = subprocess.run(
        [str(optiforge), str(source), opt, *extra, "-o", str(exe)],
        capture_output=True, text=True,
    )
    if compile_proc.returncode != 0:
        return False, (f"compilation failed (exit {compile_proc.returncode})\n"
                       f"{compile_proc.stdout}{compile_proc.stderr}")
    if not exe.exists():
        return False, "compiler reported success but produced no executable"

    try:
        run_proc = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return False, "program did not terminate within 30s"

    expected_lines, expected_exit = parse_expectations(source)
    actual_lines = normalize(run_proc.stdout)

    if run_proc.returncode != expected_exit:
        return False, (f"exit code {run_proc.returncode}, expected {expected_exit}\n"
                       f"stderr: {run_proc.stderr}")
    if actual_lines != expected_lines:
        return False, ("output mismatch\n"
                       f"  expected: {expected_lines}\n"
                       f"  actual  : {actual_lines}")
    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dir", required=True, type=Path)
    parser.add_argument("--levels", default="-O0",
                        help="comma-separated optimization levels to run")
    parser.add_argument("--extra-arg", action="append", default=[],
                        help="extra compiler flag, repeatable. Used to run the "
                             "whole suite through --regalloc=naive as well, "
                             "since ADR-08 keeps that allocator supported.")
    opts = parser.parse_args()

    if not opts.optiforge.exists():
        print(f"error: optiforge not found: {opts.optiforge}", file=sys.stderr)
        return 3

    sources = sorted(opts.dir.glob("*.of"))
    if not sources:
        print(f"warning: no .of programs in {opts.dir}")
        return 0

    levels = [lv.strip() for lv in opts.levels.split(",") if lv.strip()]
    workdir = Path(tempfile.mkdtemp(prefix="optiforge_e2e_"))
    failures = 0
    total = 0

    try:
        for source in sources:
            expected, _ = parse_expectations(source)
            if not expected:
                print(f"SKIP    {source.name}: no EXPECT-OUTPUT annotation")
                continue
            for level in levels:
                total += 1
                ok, detail = run_case(opts.optiforge, source, workdir, level,
                                      opts.extra_arg)
                suffix = "".join(" " + a for a in opts.extra_arg)
                label = f"{source.name} [{level}{suffix}]"
                if ok:
                    print(f"ok      {label}")
                else:
                    failures += 1
                    print(f"FAIL    {label}: {detail}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"{failures} of {total} end-to-end test(s) failed.")
        return 1
    print(f"All {total} end-to-end test(s) passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
