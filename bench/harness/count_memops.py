#!/usr/bin/env python3
"""Memory traffic in generated code: graph allocator against naive.

Phase 8's exit criterion is "a measurable reduction in memory traffic versus
Phase 4". This is the measurement. For each program it emits assembly twice --
once with `--regalloc=graph`, once with `--regalloc=naive` -- and counts the
instructions that touch the frame.

What counts, and why:

  - Any instruction with an `(%rbp)` or `(%rsp)` operand is frame traffic.
  - `leaq` is excluded: it computes an address and reads nothing.
  - `pushq`/`popq` of callee-saved registers *are* counted. They are real
    memory accesses, and charging the graph allocator for them is the honest
    accounting -- keeping a value in a callee-saved register is not free, it is
    two accesses per call to this function rather than two per use inside it.

This is a static count. It says how many memory instructions exist, not how
many execute: one inside a loop costs far more than one in a prologue, and this
table weights them the same. Runtime is metric Q-01 and waits for Phase 12.

Run:  python3 bench/harness/count_memops.py --optiforge <path> --dirs tests/e2e examples
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

FRAME_OPERAND = re.compile(r"\((%rbp|%rsp)\)")


def count(assembly: str) -> int:
    total = 0
    for line in assembly.splitlines():
        text = line.split("#", 1)[0]
        if not text.strip():
            continue
        if text.lstrip().startswith(("pushq", "popq")):
            total += 1
            continue
        if "leaq" in text:
            continue
        if FRAME_OPERAND.search(text):
            total += 1
    return total


def emit(optiforge: Path, source: Path, level: str, allocator: str) -> str | None:
    proc = subprocess.run(
        [str(optiforge), str(source), level, f"--regalloc={allocator}", "--emit=asm"],
        capture_output=True, text=True, timeout=120,
    )
    return proc.stdout if proc.returncode == 0 else None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--optiforge", required=True, type=Path)
    parser.add_argument("--dirs", required=True, nargs="+", type=Path)
    parser.add_argument("--level", default="-O2")
    opts = parser.parse_args()

    sources: list[Path] = []
    for directory in opts.dirs:
        sources.extend(sorted(directory.glob("*.of")))
    if not sources:
        print("no .of programs found", file=sys.stderr)
        return 1

    header = f"| {'Program':<26} | {'naive':>7} | {'graph':>7} | {'reduction':>10} |"
    rule = "|" + "-" * 28 + "|" + "-" * 9 + "|" + "-" * 9 + "|" + "-" * 12 + "|"
    print(header)
    print(rule)

    naive_total = 0
    graph_total = 0
    for source in sources:
        naive_asm = emit(opts.optiforge, source, opts.level, "naive")
        graph_asm = emit(opts.optiforge, source, opts.level, "graph")
        if naive_asm is None or graph_asm is None:
            print(f"| {source.name:<26} | {'--':>7} | {'--':>7} | {'skipped':>10} |")
            continue
        naive_count = count(naive_asm)
        graph_count = count(graph_asm)
        naive_total += naive_count
        graph_total += graph_count
        drop = (1 - graph_count / naive_count) * 100 if naive_count else 0.0
        print(f"| {source.name:<26} | {naive_count:>7} | {graph_count:>7} | {drop:>9.1f}% |")

    print(rule)
    overall = (1 - graph_total / naive_total) * 100 if naive_total else 0.0
    print(f"| {'TOTAL':<26} | {naive_total:>7} | {graph_total:>7} | {overall:>9.1f}% |")
    print()
    print(f"BE-04 memory traffic at {opts.level}: {overall:.1f}% fewer frame accesses")
    return 0


if __name__ == "__main__":
    sys.exit(main())
