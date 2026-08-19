#!/usr/bin/env python3
"""Differential fuzzer: one program, three optimization levels, one answer.

Generates random well-typed OptiForge programs -- nested `if`/`while`, `int`,
`float` and `bool` locals, scope-correct references -- compiles each at every
optimization level, runs all three binaries and compares stdout and exit status.
`-O0` is the reference, so a divergence means an optimization changed what the
program does.

This is the `tests/e2e/` differential idea (QA-05) scaled up: those programs
cover the shapes someone thought to write, and this covers the ones nobody did.
It found both SCCP miscompiles recorded in
`metrics/results/2026-08-19-pre-phase8-correctness-sweep.md`, neither of which
any hand-written test caught.

Deliberate limits, so the results are not over-read: divisors are forced
non-zero and nothing relies on overflow, so undefined behaviour is out of reach
by construction; and comparing against `-O0` cannot see a bug the frontend or
the naive backend has at every level.

Run:  python3 tests/fuzz_differential.py <optiforge> [count] [first-seed]

A seed reproduces a program exactly, so a reported failure can be replayed with
`[count]=1` and the seed it printed.
"""
import random, subprocess, sys, tempfile, os, shutil
from pathlib import Path

# Resolved so a bare relative path works the same as an absolute one; on
# Windows subprocess does not search the current directory.
OPT = str(Path(sys.argv[1]).resolve())
N = int(sys.argv[2]) if len(sys.argv) > 2 else 200
SEED0 = int(sys.argv[3]) if len(sys.argv) > 3 else 1

class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.lines = []
        self.ints = []
        self.bools = []
        self.floats = []
        self.n = 0
        self.depth = 0

    def scope(self):
        return (list(self.ints), list(self.bools), list(self.floats))

    def restore(self, saved):
        self.ints, self.bools, self.floats = [list(x) for x in saved]

    def fresh(self, p):
        self.n += 1
        return f"{p}{self.n}"

    # --- expressions ---
    def int_expr(self, d=0):
        r = self.rng
        if d > 2 or not self.ints:
            c = r.choice([r.randint(-20, 20), r.randint(-1000, 1000)])
            if self.ints and r.random() < 0.6:
                return r.choice(self.ints)
            return str(c)
        k = r.random()
        if k < 0.30:
            return r.choice(self.ints)
        if k < 0.40:
            return str(r.randint(-50, 50))
        if k < 0.50:
            return f"(-{self.int_expr(d+1)})"
        op = r.choice(["+", "-", "*", "/", "%"])
        a = self.int_expr(d + 1)
        b = self.int_expr(d + 1)
        if op in ("/", "%"):
            # keep the divisor non-zero so behaviour is defined
            b = f"({b} * 2 + 1)"
        return f"({a} {op} {b})"

    def bool_expr(self, d=0):
        r = self.rng
        if d > 2:
            if self.bools and r.random() < 0.5:
                return r.choice(self.bools)
            return r.choice(["true", "false"])
        k = r.random()
        if k < 0.15 and self.bools:
            return r.choice(self.bools)
        if k < 0.25:
            return r.choice(["true", "false"])
        if k < 0.35:
            return f"(!{self.bool_expr(d+1)})"
        if k < 0.50:
            return f"({self.bool_expr(d+1)} {r.choice(['&&','||'])} {self.bool_expr(d+1)})"
        if k < 0.62 and self.bools:
            return f"({r.choice(self.bools)} {r.choice(['==','!='])} {self.bool_expr(d+1)})"
        if k < 0.75 and self.floats:
            return f"({self.float_expr(d+1)} {r.choice(['<','>','<=','>=','==','!='])} {self.float_expr(d+1)})"
        return f"({self.int_expr(d+1)} {r.choice(['<','>','<=','>=','==','!='])} {self.int_expr(d+1)})"

    def float_expr(self, d=0):
        r = self.rng
        if d > 2 or not self.floats:
            if self.floats and r.random() < 0.5:
                return r.choice(self.floats)
            return f"{r.randint(-100,100)}.{r.randint(0,9)}"
        k = r.random()
        if k < 0.35:
            return r.choice(self.floats)
        if k < 0.45:
            return f"{r.randint(-100,100)}.{r.randint(0,9)}"
        if k < 0.55:
            return f"(-{self.float_expr(d+1)})"
        op = r.choice(["+", "-", "*"])
        return f"({self.float_expr(d+1)} {op} {self.float_expr(d+1)})"

    # --- statements ---
    def emit(self, s):
        self.lines.append("    " * (self.depth + 1) + s)

    def stmt(self, budget):
        r = self.rng
        k = r.random()
        if k < 0.22:
            v = self.fresh("i")
            self.emit(f"int {v} = {self.int_expr()};")
            self.ints.append(v)
        elif k < 0.34:
            v = self.fresh("b")
            self.emit(f"bool {v} = {self.bool_expr()};")
            self.bools.append(v)
        elif k < 0.44:
            v = self.fresh("f")
            self.emit(f"float {v} = {self.float_expr()};")
            self.floats.append(v)
        elif k < 0.56 and self.ints:
            self.emit(f"{r.choice(self.ints)} = {self.int_expr()};")
        elif k < 0.63 and self.bools:
            self.emit(f"{r.choice(self.bools)} = {self.bool_expr()};")
        elif k < 0.68 and self.floats:
            self.emit(f"{r.choice(self.floats)} = {self.float_expr()};")
        elif k < 0.82 and budget > 0:
            self.emit(f"if ({self.bool_expr()}) {{")
            self.depth += 1
            saved = self.scope()
            for _ in range(r.randint(1, 3)):
                self.stmt(budget - 1)
            self.restore(saved)
            self.depth -= 1
            if r.random() < 0.5:
                self.emit("} else {")
                self.depth += 1
                saved = self.scope()
                for _ in range(r.randint(1, 3)):
                    self.stmt(budget - 1)
                self.restore(saved)
                self.depth -= 1
            self.emit("}")
        elif k < 0.93 and budget > 0 and self.ints:
            c = self.fresh("k")
            self.emit(f"int {c} = 0;")
            self.ints.append(c)
            self.emit(f"while ({c} < {r.randint(1,6)}) {{")
            self.depth += 1
            saved = self.scope()
            for _ in range(r.randint(1, 3)):
                self.stmt(budget - 1)
            self.restore(saved)
            self.emit(f"{c} = {c} + 1;")
            self.depth -= 1
            self.emit("}")
        else:
            if self.ints:
                self.emit(f"print_int({r.choice(self.ints)});")

def make_program(seed):
    r = random.Random(seed)
    g = Gen(r)
    g.emit("int seed = %d;" % r.randint(0, 50))
    g.ints.append("seed")
    for _ in range(r.randint(6, 16)):
        g.stmt(2)
    body = "\n".join(g.lines)
    prints = []
    for v in g.ints[:8]:
        prints.append(f"    print_int({v});")
    for v in g.bools[:6]:
        prints.append(f"    print_bool({v});")
    for v in g.floats[:6]:
        prints.append(f"    print_float({v});")
    return "fn main() -> int {\n" + body + "\n" + "\n".join(prints) + "\n    return 0;\n}\n"

def run(src, workdir, level, idx):
    f = workdir / f"p{idx}.of"
    f.write_text(src)
    exe = workdir / f"p{idx}_{level[1:]}.exe"
    c = subprocess.run([OPT, str(f), level, "-o", str(exe)],
                       capture_output=True, text=True, timeout=120)
    if c.returncode != 0:
        return ("COMPILE_FAIL", c.returncode, (c.stdout + c.stderr)[:800])
    try:
        p = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", -1, "")
    return ("OK", p.returncode, p.stdout)

def main():
    tmp = Path(tempfile.mkdtemp(prefix="offuzz"))
    bad = 0
    try:
        for i in range(SEED0, SEED0 + N):
            src = make_program(i)
            res = {}
            for lvl in ("-O0", "-O1", "-O2"):
                res[lvl] = run(src, tmp, lvl, i)
            base = res["-O0"]
            for lvl in ("-O1", "-O2"):
                if res[lvl] != base:
                    bad += 1
                    print("=" * 70)
                    print(f"MISMATCH seed={i} at {lvl}")
                    print(f"  -O0: {base}")
                    print(f"  {lvl}: {res[lvl]}")
                    print(src)
                    break
            if bad >= 6:
                break
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print(f"done: {bad} mismatching programs")

main()
