#!/usr/bin/env python3
"""Golden-file test runner for the optiforge CLI.

Each test is a .cmd file describing one invocation; the expected stdout,
stderr and exit code live alongside it in a .expected file.

  tests/golden/version.cmd       ->  arguments, one per line
  tests/golden/version.expected  ->  expected output block

Run:      python3 tests/run_golden.py --optiforge <path> --dir tests/golden
Update:   ... --update      (then READ the diff before committing; blind
                             regeneration is how golden tests stop catching
                             anything -- System_design.md 18.1)
"""

from __future__ import annotations

import argparse
import difflib
import subprocess
import sys
from pathlib import Path

EXIT_MARKER = "=== exit ==="
STDOUT_MARKER = "=== stdout ==="
STDERR_MARKER = "=== stderr ==="


def normalize(text: str) -> str:
    """Windows and Linux disagree about line endings; the compiler's behaviour
    does not. Compare on normalized text so goldens are platform-neutral."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def read_args(cmd_path: Path) -> list[str]:
    args = []
    for line in cmd_path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            args.append(line)
    return args


def block(marker: str, text: str) -> str:
    """Render one labelled section.

    Output that does not end in a newline would otherwise run into the next
    marker on the same line, making the golden file ambiguous and its diffs
    confusing. Flag it explicitly, the way diff does.
    """
    text = normalize(text)
    if text and not text.endswith("\n"):
        return f"{marker}\n{text}\n\\ No newline at end of output\n"
    return f"{marker}\n{text}"


def render(exit_code: int, stdout: str, stderr: str) -> str:
    return (
        f"{EXIT_MARKER}\n{exit_code}\n"
        + block(STDOUT_MARKER, stdout)
        + block(STDERR_MARKER, stderr)
    )


def run_case(optiforge: Path, cmd_path: Path, workdir: Path) -> str:
    args = read_args(cmd_path)
    proc = subprocess.run(
        [str(optiforge)] + args,
        capture_output=True,
        text=True,
        cwd=str(workdir),
    )
    return render(proc.returncode, proc.stdout, proc.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dir", required=True, type=Path)
    parser.add_argument("--workdir", type=Path, default=None,
                        help="Directory to run from; defaults to --dir")
    parser.add_argument("--update", action="store_true")
    opts = parser.parse_args()

    if not opts.optiforge.exists():
        print(f"error: optiforge binary not found: {opts.optiforge}", file=sys.stderr)
        return 3

    workdir = opts.workdir or opts.dir
    cases = sorted(opts.dir.glob("*.cmd"))
    if not cases:
        print(f"warning: no .cmd cases found in {opts.dir}")
        return 0

    failures = 0
    for cmd_path in cases:
        name = cmd_path.stem
        expected_path = cmd_path.with_suffix(".expected")
        actual = run_case(opts.optiforge, cmd_path, workdir)

        if opts.update:
            expected_path.write_text(actual, encoding="utf-8", newline="\n")
            print(f"UPDATED {name}")
            continue

        if not expected_path.exists():
            print(f"FAIL    {name}: no .expected file (run with --update)")
            failures += 1
            continue

        expected = normalize(expected_path.read_text(encoding="utf-8"))
        if actual == expected:
            print(f"ok      {name}")
        else:
            failures += 1
            print(f"FAIL    {name}")
            diff = difflib.unified_diff(
                expected.splitlines(keepends=True),
                actual.splitlines(keepends=True),
                fromfile=f"{name}.expected",
                tofile=f"{name}.actual",
            )
            sys.stdout.writelines(diff)
            print()

    total = len(cases)
    if opts.update:
        print(f"Updated {total} golden file(s).")
        return 0
    if failures:
        print(f"{failures} of {total} golden test(s) failed.")
        return 1
    print(f"All {total} golden test(s) passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
