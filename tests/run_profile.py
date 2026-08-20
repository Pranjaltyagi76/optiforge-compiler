#!/usr/bin/env python3
"""Profile tests: compile instrumented, run, check the .prof against the source.

Each test is a .of program annotated with the records it must produce:

    // EXPECT-OUTPUT: 6
    // EXPECT-PROF: FUNCTION sum 1
    // EXPECT-PROF: BLOCK sum while.body.2 4
    // EXPECT-PROF: BRANCH sum while.cond.1 taken=4 not_taken=1
    // EXPECT-PROF: LOOP sum while.cond.1 entries=1 iterations=4

EXPECT-PROF lines must appear verbatim in the profile; other records may also be
present and are ignored. That keeps a test about the counts it cares about
rather than about every block the optimizer happened to leave behind.

This is Phase 9's exit criterion made executable: the counts on a small loop
program are worked out by hand, written into the source, and checked. It also
covers PROF-11 by running the *uninstrumented* build of the same program and
requiring identical output — instrumentation that changes what a program prints
has measured something other than the program.

Phase 10 adds fault injection for PGO-11. Every program is compiled a fourth,
fifth and sixth time with a profile that is missing, corrupt, and describing a
different program, and each build must print exactly what the plain one did. The
requirement is that profile data changes decisions and never semantics, and the
only way to have any confidence in that is to break the profile on purpose.

Run:  python3 tests/run_profile.py --optiforge <path> --dir tests/pgo
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECT_OUTPUT = re.compile(r"^\s*//\s*EXPECT-OUTPUT:\s?(.*)$")
EXPECT_PROF = re.compile(r"^\s*//\s*EXPECT-PROF:\s?(.*)$")
LEVEL = re.compile(r"^\s*//\s*LEVEL:\s*(-O\d)\s*$")
FLAGS = re.compile(r"^\s*//\s*FLAGS:\s*(.*)$")


def parse(path: Path) -> tuple[list[str], list[str], str, list[str]]:
    output: list[str] = []
    records: list[str] = []
    level = "-O1"
    flags: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if m := EXPECT_OUTPUT.match(line):
            output.append(m.group(1).rstrip())
        elif m := EXPECT_PROF.match(line):
            records.append(m.group(1).rstrip())
        elif m := LEVEL.match(line):
            level = m.group(1)
        elif m := FLAGS.match(line):
            flags.extend(m.group(1).split())
    return output, records, level, flags


def normalize(text: str) -> list[str]:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return [ln.rstrip() for ln in text.split("\n") if ln.strip() != ""]


def compile_and_run(optiforge: Path, source: Path, workdir: Path, exe_name: str,
                    args: list[str]) -> tuple[bool, str, str]:
    exe = workdir / exe_name
    proc = subprocess.run([str(optiforge), str(source), *args, "-o", str(exe)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return False, "", (f"compilation failed (exit {proc.returncode})\n"
                           f"{proc.stdout}{proc.stderr}")
    run = subprocess.run([str(exe)], capture_output=True, text=True,
                         timeout=60, cwd=str(workdir))
    if run.returncode != 0:
        return False, "", f"program exited {run.returncode}\nstderr: {run.stderr}"
    return True, run.stdout, ""


# Profiles that are wrong in three different ways. Each must produce a working
# binary indistinguishable from one built with no profile at all (PGO-11).
def broken_profiles(workdir: Path, good_profile: Path) -> list[tuple[str, str]]:
    corrupt = workdir / "corrupt.prof"
    corrupt.write_text("this is not a profile\nneither is this\n", encoding="utf-8")

    stale = workdir / "stale.prof"
    stale.write_text(
        "OPTIFORGE_PROFILE 1\n"
        "SOURCE somewhere/else.of\n"
        "SRCHASH 0x0000000000000001\n"
        "OPTLEVEL 2\n"
        "COMPILER optiforge-0.0.0\n"
        "FUNCTION not_a_function_here 999999\n"
        "BLOCK not_a_function_here entry 999999\n"
        "BRANCH not_a_function_here entry taken=1 not_taken=999998\n",
        encoding="utf-8")

    return [
        ("missing", str(workdir / "does_not_exist.prof")),
        ("corrupt", str(corrupt)),
        ("stale", str(stale)),
        ("valid", str(good_profile)),
    ]


def run_case(optiforge: Path, source: Path, workdir: Path) -> tuple[bool, str]:
    expected_output, expected_records, level, flags = parse(source)

    ok, instrumented_out, detail = compile_and_run(
        optiforge, source, workdir, source.stem + "_prof.exe",
        [level, "--profile", *flags])
    if not ok:
        return False, detail

    actual = normalize(instrumented_out)
    if actual != expected_output:
        return False, ("output mismatch under instrumentation\n"
                       f"  expected: {expected_output}\n"
                       f"  actual  : {actual}")

    # PROF-11: instrumentation must not change what the program does.
    ok, plain_out, detail = compile_and_run(
        optiforge, source, workdir, source.stem + "_plain.exe", [level])
    if not ok:
        return False, "uninstrumented build: " + detail
    if normalize(plain_out) != actual:
        return False, ("instrumented and uninstrumented output differ\n"
                       f"  plain       : {normalize(plain_out)}\n"
                       f"  instrumented: {actual}")

    profile_path = workdir / (source.stem + "_prof.prof")
    if not profile_path.exists():
        return False, f"no profile written at {profile_path.name}"

    # PGO-11: a profile that is missing, corrupt, stale or simply correct must
    # all produce the same running program. Only the last of those is expected
    # to be useful; none of them may be wrong.
    for label, path in broken_profiles(workdir, profile_path):
        ok, guided_out, detail = compile_and_run(
            optiforge, source, workdir, f"{source.stem}_{label}.exe",
            [level, f"--use-profile={path}"])
        if not ok:
            return False, f"{label} profile: {detail}"
        if normalize(guided_out) != actual:
            return False, (f"output changed under a {label} profile\n"
                           f"  without profile: {actual}\n"
                           f"  with {label}    : {normalize(guided_out)}")

    lines = normalize(profile_path.read_text(encoding="utf-8"))
    missing = [record for record in expected_records if record not in lines]
    if missing:
        return False, ("profile is missing expected records:\n" +
                       "".join(f"    {m}\n" for m in missing) +
                       "  profile was:\n" +
                       "".join(f"    {ln}\n" for ln in lines))

    # The header has to be there and has to say which build produced it, or a
    # stale profile cannot be detected in Phase 10.
    for required in ("OPTIFORGE_PROFILE 1", "SRCHASH 0x", "OPTLEVEL ", "COMPILER "):
        if not any(ln.startswith(required.rstrip()) for ln in lines):
            return False, f"profile header is missing '{required.strip()}'"

    return True, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dir", required=True, type=Path)
    opts = parser.parse_args()

    optiforge = opts.optiforge.resolve()
    if not optiforge.exists():
        print(f"error: optiforge not found: {optiforge}", file=sys.stderr)
        return 3

    sources = sorted(p for p in opts.dir.glob("*.of"))
    if not sources:
        print(f"warning: no .of programs in {opts.dir}")
        return 0

    workdir = Path(tempfile.mkdtemp(prefix="optiforge_prof_"))
    failures = 0
    try:
        for source in sources:
            ok, detail = run_case(optiforge, source.resolve(), workdir)
            if ok:
                print(f"ok      {source.name}")
            else:
                failures += 1
                print(f"FAIL    {source.name}: {detail}")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    total = len(sources)
    if failures:
        print(f"\n{failures} of {total} profile test(s) failed.")
        return 1
    print(f"\nAll {total} profile test(s) passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
