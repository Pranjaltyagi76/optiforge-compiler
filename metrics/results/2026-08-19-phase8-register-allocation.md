# Register Allocation — Phase 8

## 1. Provenance

| Field | Value |
|---|---|
| Date | 2026-08-19 |
| Git revision | `590c42a` + Phase 8 working tree |
| Machine | [windows-mingw](../machines/windows-mingw.md) |
| Host compiler | GCC 16.1.0 (MinGW-w64, UCRT) |
| Measurement | static count of frame-touching instructions, `bench/harness/count_memops.py` |
| Corpus | 18 programs from `tests/e2e/` and `examples/` |

A **static** count, like the Phase 7 table: it says how many memory instructions
exist, not how many execute. One inside a loop costs far more than one in a
prologue and this weights them the same. Runtime is metric Q-01 and waits for
the benchmark suite in Phase 12.

Both allocators compile the same source at the same optimization level, so the
IR reaching the backend is identical and the difference is the allocator alone.
That is the whole reason ADR-08 keeps the naive allocator: it is the control.

## 2. Metric BE-04 — memory traffic, `-O2`

Counted: any instruction with an `(%rbp)` or `(%rsp)` operand, plus every
`pushq`/`popq`. `leaq` is excluded — it computes an address and reads nothing.
Counting the pushes is the honest accounting: keeping a value in a callee-saved
register is not free, and the graph allocator is charged for every one it uses.

| Program | naive | graph | reduction |
|---|---:|---:|---:|
| arithmetic.of | 2 | 2 | 0.0% |
| bool_compare.of | 11 | 4 | 63.6% |
| calling_convention.of | 12 | 6 | 50.0% |
| comparisons.of | 2 | 2 | 0.0% |
| control_flow.of | 42 | 18 | 57.1% |
| fixed_registers.of | 80 | 34 | 57.5% |
| floats.of | 14 | 4 | 71.4% |
| frame_layout.of | 142 | 64 | 54.9% |
| loop_phi_edges.of | 31 | 6 | 80.6% |
| nan_compare.of | 35 | 4 | 88.6% |
| recursion.of | 69 | 29 | 58.0% |
| register_pressure.of | 174 | 76 | 56.3% |
| scoping.of | 28 | 10 | 64.3% |
| short_circuit.of | 44 | 14 | 68.2% |
| ssa_swap.of | 76 | 20 | 73.7% |
| ssa_uninitialized.of | 2 | 2 | 0.0% |
| fib.of | 24 | 11 | 54.2% |
| sum.of | 23 | 10 | 56.5% |
| **TOTAL** | **811** | **316** | **61.0%** |

At `-O0`, where the allocas are still there and every local is a real load and
store, the figure is **61.9%** (1705 → 650). Almost the same, for a different
reason: at `-O0` the win is that a load's result stays in a register instead of
being written straight back to a slot.

| Metric | Target | Measured | Met? |
|---|---|---|---|
| BE-04 — memory traffic versus the Phase-4 allocator | "measurable reduction" | **61.0%** at `-O2` | ✅ |

### The three programs at 0.0%

`arithmetic.of`, `comparisons.of` and `ssa_uninitialized.of` all optimize down
to a `main` that calls `print_int` on constants. There is nothing left to hold
in a register, and the two frame accesses each are the prologue and epilogue's
`push`/`pop` of `rbp`. A row of zeroes here is the measurement working, not
failing.

## 3. Milestone M5 — "generated assembly keeps values in registers across a loop"

`sum.of` at `-O2`, loop body only.

Phase 4:

```asm
.L_sum_while_body_2:
    movq    -24(%rbp), %r10        # %t5 = add
    movq    -16(%rbp), %rax
    addq    %r10, %rax
    movq    %rax, -40(%rbp)
    movq    $1, %r10               # %t7 = add
    movq    -24(%rbp), %rax
    addq    %r10, %rax
    movq    %rax, -48(%rbp)
    movq    -48(%rbp), %rax        # %t13 = copy
    movq    %rax, -24(%rbp)
    movq    -40(%rbp), %rax        # %t14 = copy
    movq    %rax, -16(%rbp)
    jmp     .L_sum_while_cond_1
```

Phase 8:

```asm
.L_sum_while_body_2:
    addq    %rsi, %rbx             # %t5 = add
    movq    $1, %r10               # %t7 = add
    addq    %r10, %rsi
    jmp     .L_sum_while_cond_1
```

Twelve instructions and eight memory accesses become three instructions and
none. The two phi copies disappeared entirely: coalescing put each phi's edge
copy in the same register as its destination, which turns the copy into a
self-move the code generator drops.

## 4. Allocation quality

From `--print-regalloc`, which prints one line per function. Reproduce with:

```
optiforge <program> -O2 --print-regalloc --emit=asm > /dev/null
```

| Function | units | coloured | spilled | coalesced | frozen | peak live |
|---|---:|---:|---:|---:|---:|---:|
| `sum.of @sum` | 6 | 4 | 0 | 2 | 0 | 3 |
| `fib.of @fib` | 7 | 7 | 0 | 0 | 0 | 1 |
| `ssa_swap.of @swapper` | 9 | 7 | 0 | 2 | 0 | 3 |
| `ssa_swap.of @rotate3` | 12 | 10 | 0 | 2 | 0 | 4 |
| `register_pressure.of @pressure` | 45 | 30 | 15 | 0 | 2 | 22 |
| `register_pressure.of @across_call` | 19 | 17 | 2 | 0 | 0 | 9 |
| `frame_layout.of @two` | 11 | 8 | 0 | 3 | 0 | 6 |
| `frame_layout.of @three` | 15 | 14 | 1 | 0 | 4 | 8 |
| `fixed_registers.of @mixed` | 15 | 14 | 1 | 0 | 2 | 8 |

Read the first row carefully: six units, four coloured, none spilled. The two
missing are not spills, they were *coalesced away* -- which is why the loop body
of `@sum` has no copies in it at all. Coloured plus spilled counts the units
that survived coalescing, not the units the function started with.

`@pressure` is the stress case and behaves as it should. Eight allocatable
integer registers against twenty-two values live at once, so fifteen units go to
memory and the ones that stay are the ones the spill heuristic values most: the
accumulator and the loop counter, both used on every iteration. A peak live
count above the register count is exactly the condition the roadmap's
"register-pressure stress tests do not miscompile" criterion is about.

**Freeze** fires in `@pressure`, `@three` and `@mixed`, and nowhere else. Both are at the
point where a low-degree node is held back only by a copy and nothing else can
make progress; the allocator gives up on the copy rather than spilling the node,
which costs one move instead of a memory access per use. Everywhere else the
graph is loose enough that coalescing succeeds outright and freeze never has to
choose.

`@two` shows the SSE side working: eight of eleven units coloured, three copies
coalesced, four callee-saved XMM registers saved to frame slots because the ABI
has no push for them.

## 5. Threats to validity

- A static count, as above. The number that matters is time, and this is not it.
- Sixteen small programs. This says what the allocator does to *this* code.
- The pool is deliberately small: eight integer registers out of sixteen, ten
  SSE out of sixteen. `rax`, `rcx`, `rdx`, `r8`, `r9` and `r10` are reserved for
  the code generator's fixed sequences (`idiv`, variable shifts, argument
  passing) so the allocator needs no pre-colouring. A bigger pool would spill
  less; it would also need every one of those constraints modelled, and the
  trade was made on the side of an allocator whose correctness argument fits on
  a page.
- Callee-saved pushes are counted but their *cost* is not modelled in the spill
  heuristic, which weights by loop depth only. A value used once in a leaf
  function can still earn a push and a pop.

## 6. Actions

| Action | Metric | Status |
|---|---|---|
| Runtime measurement, which is the number that actually matters | Q-01 | ☐ Phase 12 |
| Spill costs from measured execution counts rather than loop depth | PGO-08 | ☐ Phase 11 — the heuristic is already factored out for exactly this |
| Fold a small constant into the instruction instead of `movq $1, %r10` first | BE-04 | ☐ open — instruction selection, not allocation |
| Model the cost of a callee-saved push in the spill heuristic | BE-04 | ☐ open |
| Live-range splitting, so a value can be in a register in the loop and memory outside it | BE-04 | ☐ open, Phase 13 |
