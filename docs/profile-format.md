# The `.prof` Profile Format

> **Status:** complete for Phases 9 and 10, format version 1. Written against
> `runtime/ofprof/ofprof.c`, which is the only thing that writes this file;
> `src/transforms/Instrument.cpp`, which decides what goes in it; and
> `src/profile/Profile.cpp`, which is the only thing that reads it.

---

## 1. What it is

Line-oriented text (ADR-07). A profile you can `cat`, hand-edit to test a
hypothesis, and diff across runs is worth more than a compact binary one at the
scale this compiler works at.

Produced by running a binary built with `--profile`. Written on normal exit, via
an `atexit` hook registered from a constructor in `libofprof`, so the program's
source needs no change at all (PROF-07).

```
optiforge prog.of -O2 --profile -o prog_inst
./prog_inst                       # writes prog_inst.prof
optiforge prog.of -O2 --use-profile=prog_inst.prof -o prog    # Phase 11
```

Where it lands, in order of precedence:

1. `$OPTIFORGE_PROFILE_OUT`
2. `--profile-out=<path>`, baked into the binary at compile time
3. the output binary's stem plus `.prof`

---

## 2. Grammar

```ebnf
profile     = header { blank | comment | record } ;
header      = "OPTIFORGE_PROFILE" version NL
              "SOURCE"  path   NL
              "SRCHASH" hex    NL
              "OPTLEVEL" int   NL
              "COMPILER" string NL
              [ "RUNS" int NL ]
              [ "TOTAL_SAMPLES" int NL ] ;
record      = func_rec | block_rec | branch_rec | loop_rec | time_rec ;
func_rec    = "FUNCTION" ident count NL ;
block_rec   = "BLOCK"    ident ident count NL ;
branch_rec  = "BRANCH"   ident ident "taken=" count "not_taken=" count NL ;
loop_rec    = "LOOP"     ident ident "entries=" count "iterations=" count NL ;
time_rec    = "TIME"     ident float NL ;
comment     = "#" { any } NL ;
```

Fields are separated by single spaces. Counts are unsigned 64-bit decimal
(PROF-12: a counter that wraps is a lie, and 64 bits will not wrap). `TIME` is
milliseconds with three decimal places.

Records are written grouped by kind, in the order above, with a blank line
between groups. Nothing depends on that order — it is for whoever reads the
file.

---

## 3. Example, hand-checkable

```of
fn sum(int n) -> int {
    int total = 0;
    int i = 0;
    while (i < n) { total = total + i; i = i + 1; }
    return total;
}
fn main() -> int { print_int(sum(4)); return 0; }
```

`optiforge sum.of -O1 --profile -o sum_inst && ./sum_inst`:

```
OPTIFORGE_PROFILE 1
SOURCE sum.of
SRCHASH 0x6d0ce770f654af73
OPTLEVEL 1
COMPILER optiforge-0.1.0
RUNS 1
TOTAL_SAMPLES 12

FUNCTION sum 1
FUNCTION main 1

BLOCK sum entry 1
BLOCK sum while.cond.1 5
BLOCK sum while.body.2 4
BLOCK sum while.end.3 1
BLOCK main entry 1

BRANCH sum while.cond.1 taken=4 not_taken=1

LOOP sum while.cond.1 entries=1 iterations=4
```

Every number is checkable from the source: the body runs four times, so the
condition runs five — four entries plus the check that fails. This exact program
is `tests/pgo/loop_counts.of`, with the counts written into it as assertions.

---

## 4. Header fields

| Field | Meaning |
|---|---|
| `OPTIFORGE_PROFILE` | Format version. A reader that does not know the version refuses the file. |
| `SOURCE` | Path the instrumented binary was compiled from. Informational. |
| `SRCHASH` | Hash of the source text. **The staleness check** (PROF-10). |
| `OPTLEVEL` | The `-O` level of the instrumented build. |
| `COMPILER` | `optiforge-<version>`. |
| `RUNS` | Number of runs merged into this file. Always 1 today; PROF-14 is deferred. |
| `TOTAL_SAMPLES` | Sum of every counter. A quick "did this program do anything" check. |

`OPTLEVEL` matters more than it looks. ADR-05 puts instrumentation *after* the
optimization pipeline, so block names describe the optimized CFG. A profile
collected at `-O1` and applied at `-O2` describes a different set of blocks, and
Phase 10 warns rather than silently matching the wrong ones.

---

## 5. What is counted, and what is derived

**One counter per basic block. That is all.**

Every record type is derived from block counters by the compiler, which encodes
the derivation in a static table the runtime walks. Four counter kinds would be
four times the hot-path cost and four ways for the numbers to disagree with each
other.

| Record | Derived how |
|---|---|
| `FUNCTION f` | the count of `f`'s entry block — a function is entered exactly as often as its entry block runs |
| `BLOCK f b` | that block's counter, directly |
| `BRANCH f b` | the counts of the two successors of `b`'s conditional branch |
| `LOOP f h` | `entries` is the preheader's count; `iterations` is `count(h) - entries`, because a header runs once per entry plus once per iteration |
| `TIME f` | accumulated wall time, when `--profile-time` was given |

`BRANCH` works because instrumentation **splits critical edges first**. After
splitting, each successor of a conditional branch has exactly one predecessor,
so its block counter *is* that edge's count. This is why the instrumented build
can contain `crit.edge.N` blocks the ordinary build does not: they are where the
edge counts live.

`LOOP` is emitted only for loops with a single preheader. A loop entered from
several blocks has no one place to read the entry count from, and gets no record
rather than a wrong one.

---

## 6. Validation on load

Implemented in Phase 10, in `src/profile/Profile.cpp`.

| Condition | Action |
|---|---|
| Unknown version | Refuse the profile, warn, compile without it |
| `SRCHASH` mismatch | **Warn**, then match by name; report the match rate |
| Match rate below 50% | Warn: the profile appears stale and PGO will do little |
| `-O` level differs from the build | Warn: block names will not line up (ADR-05) |
| Unknown record type | Ignore the line, warn **once** — forward compatibility |
| Malformed line | Ignore the line, warn once per kind, continue |
| Flow conservation violated | Warn, treat counts as advisory |
| Missing or unreadable file | Warn naming the path; compile without a profile |
| Empty file | Warn; compile without a profile |

Every row of that table ends in a **correct binary**. Profile data changes
decisions, never semantics (PGO-11). That is designed in, not tested in: nothing
the reader returns can reach code generation, and `tests/run_profile.py`
recompiles every profile test with a missing, a corrupt and a mismatched profile
and requires byte-identical output from all three.

### Flow conservation

Two invariants follow from where the counters are placed, so a file that
violates either is not describing a real execution:

```
BRANCH f b taken=T not_taken=N     =>   T + N  ==  BLOCK f b
LOOP   f h entries=E iterations=I  =>   E + I  ==  BLOCK f h
```

Checked on load, from the profile alone — no IR needed. A violation is reported
and the counts are kept as advisory, because a profile that is 99% consistent is
still worth more than none.

---

## 7. Classification

`--profile-report` and every Phase 11 pass read heat, not raw counts. Thresholds
in executions do not transfer between programs; one that did would be measuring
the workload rather than the code.

```
total = sum of all block counts
max   = the largest block count

HOT   blocks in the smallest prefix (sorted by count, descending) whose
      cumulative count reaches --hot-threshold percent of total,
      AND whose own count is at least 1,000
COLD  count == 0, or count < 0.01% of max
WARM  everything else
```

The **floor of 1,000** is why a small test program has no hot blocks, and the
report says so in as many words. Without it every trivial program has a "hot"
block that ran four times and the whole notion stops meaning anything.

`--hot-threshold=<percent>` moves the 80% (PGO-04). Lower narrows the hot set;
higher widens it.

### Functions are weighted by work, not by call count

**A deliberate departure from `System_design.md` §15.1**, which specifies that
"functions inherit heat from their entry block count".

Entry count measures how often you arrive, not how much happens once you do. In
`tests/pgo/fixtures/hotpath.of`, `compute` is called twenty times and runs five
thousand iterations each; it holds 99.98% of the program's execution. The
entry-count rule labels it cold. This compiler weights a function by the summed
executions of its blocks, which answers the question an inlining or layout
decision is actually asking — where does the time go.

`Heat::Unknown` is a fourth state and not a synonym for `Cold`. "The profile
says this never ran" and "the profile says nothing about this" justify opposite
decisions, and a pass that cannot tell them apart will make the wrong one.

---

## 8. The report

`optiforge --profile-report=prog.prof` needs no source file. It prints hot,
warm and cold functions; every loop with its trip count and the unroll factor
Phase 11 would choose; branches that went one way more than 90% of the time; and
`--profile-time` wall clocks when they were collected.

The unroll factors and layout notes are **suggestions, not decisions** — nothing
in this mode changes anything. That is the point: a PGO decision becomes
reviewable before any pass exists to make it.

See `tests/golden/profile_report.expected` for the output on the designed
benchmark, and section 3 above for a profile small enough to check by hand.

---

## 9. Limits, stated plainly

- **A program that dies by signal or calls `_exit` writes nothing.** That is
  inherent to `atexit`. `__ofprof_flush()` is exported for anyone who needs the
  data out before then.
- **`RUNS` is always 1.** Merging several runs (PROF-14, priority C) is not
  implemented; the field exists so that adding it does not change the format.
- **Counts are exact, not sampled.** Every block increments on every execution.
  That costs measured 12.9% worst case (PROF-13, target 40%) and buys numbers
  that can be checked by hand, which sampling cannot.
- **`TIME` is opt-in and coarse.** It is a call at each entry and exit, so it is
  the one part of instrumentation that is not free. Recursive functions are
  charged for their whole call tree once, not once per level.
