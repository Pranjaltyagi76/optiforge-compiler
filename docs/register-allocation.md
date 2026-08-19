# Register Allocation

> **Status:** complete for Phase 8. Written against `src/backend/RegAlloc.cpp`
> and the parts of `src/backend/CodeGen.cpp` that consume it.

---

## 1. Two allocators, one code generator

| Allocator | Flag | What it does |
|---|---|---|
| Naive | `--regalloc=naive` | Every value gets a frame slot. Operands are loaded into scratch registers, the operation runs, the result is stored straight back. |
| Graph | `--regalloc=graph` (default) | Chaitin–Briggs colouring. A coloured value lives in its register for its whole live range and is never written to memory; a spilled one behaves exactly as it does under the naive allocator. |

Keeping both is [ADR-08](../context/architectural_design.md). When the graph
allocator miscompiles — and it did, three times, during Phase 8 — one flag says
whether the allocator or something else is to blame. The whole end-to-end suite
runs through both (`ctest -R e2e`), so the fallback stays a working fallback.

They share every lowering rule. Three functions know where a value lives:

```
loadInt / loadFloat   materialize a value in a register
storeResult           put a computed result where the value belongs
```

Everything else in the instruction selector is written as if it did not know
about allocation at all.

---

## 2. What gets allocated

An **allocation unit**, not a value.

SSA destruction turns each phi into a group of copies that must share a
location: one per incoming edge, plus a root at the top of the block that names
the location for the phi's users. They are separate IR values, but they are one
storage cell, and they have to end up in one register or the phi's meaning is
lost. Every value whose `slotAlias` points at the same root is merged into one
unit before the graph is built.

Excluded from allocation:

- **Constants**, materialized inline at each use.
- **`alloca`**, whose *value* is the address of a frame slot. The slot has to
  exist and code addresses it directly.

---

## 3. Live ranges are over units, not values

`analysis::Liveness` answers a different question than the allocator asks, and
using it directly is a trap worth describing because it looks like it works.

Liveness tracks values. A location written by several edge copies and read
through a root copy has no single value whose range describes it, so the
obvious move — take the union of the members' ranges — over-approximates. The
root copy reads *itself*, which is how value-liveness carries the location back
to the predecessors that wrote it, and that self-read has no kill anywhere. The
location therefore appears live from the function entry, and two locations
belonging to unrelated loops both appear live everywhere and interfere for no
reason.

So the allocator runs its own backward dataflow over a domain of units, on the
same solver (`analysis::runDataflow`, requirement AN-11). In that domain a
location is killed by *any* of its member definitions, and the self-referencing
root copy is **transparent** — neither a definition nor a use — which makes the
answer exact.

---

## 4. The interference graph

Two units interfere when one is live at the point the other is defined. Built
by walking each block backwards from its live-out set.

Three details that are easy to get wrong, and all three were:

- **A copy's source and destination do not interfere at the copy.** They hold
  the same value there, which is what makes them coalescable.
- **Arguments are defined at the function entry**, by the calling convention.
  No walk over instructions ever reaches that definition point, so the edges
  from each argument to everything live at entry are added explicitly. Without
  it, two arguments live for a whole function had no edge between them at all.
- **A value live across a call** is marked as needing a callee-saved register,
  rather than being given an edge to each caller-saved one.

Integer and floating-point units never share a register file, so no edge is ever
added between them; one graph would only inflate degrees and cause spills that
are not needed.

---

## 5. Colouring

One loop, doing the cheapest thing still available:

```
repeat
    simplify   a low-degree node that no move cares about
  | coalesce   a copy whose two ends can safely share a register
  | freeze     give up coalescing one low-degree node, so it can simplify
  | spill      nothing is low-degree; push the cheapest node optimistically
until the graph is empty
then select
```

- **Simplify** removes a node of degree < K and pushes it on the select stack.
  It skips **move-related** nodes: removing one would throw away the chance to
  put a copy's two ends in the same register and delete the copy.
- **Coalesce** merges the two ends of a copy under Briggs' conservative test —
  merge only when the combined node has fewer than K neighbours of significant
  degree. Conservative in the sense that matters: it never turns a colourable
  graph into one that is not, which aggressive coalescing can.
- **Freeze** is reached when nothing simplifies and nothing coalesces, but
  low-degree nodes remain, held back only by a move. It gives up on one node's
  moves, which makes it ordinary and lets it simplify. The copy stays in the
  generated code; that is the price of not spilling, and it is a much better
  price.
- **Spill** picks a victim by lowest `useWeight / degree` and pushes it
  optimistically.
- **Select** pops the stack and assigns each node a colour no live neighbour
  holds. A node with nothing available is spilled for real.

Each branch either removes a node, merges two, or settles a move, so the loop
terminates. Degrees are recomputed rather than maintained incrementally: that
is O(n²) where the textbook is O(n), and at tens of units per function it is not
measurable — while the incremental version is where this algorithm is usually
got wrong.

`useWeight` is Σ 10^loopDepth over the blocks that mention the unit — the static
stand-in for "how often does this run". Phase 11 replaces the base with a
measured execution count (PGO-08); it is factored out for exactly that.

### Spilling needs no rewrite round

The textbook loop is build → colour → *insert spill code* → repeat, because
spill code introduces new short live ranges the graph has to account for.

Here it does not. The code generator already computes with values that live in
memory — that is all the naive allocator ever did — so "spill" means only
"assign no register". The reloads use the reserved scratch registers, which are
not allocatable and so add no live range at all. One pass is exact.

---

## 6. Which registers, and why so few

Microsoft x64 (ADR-10). The pool lives in `TargetInfo`, never in the allocator.

| Class | Allocatable | Reserved, and for what |
|---|---|---|
| Integer | `r11 rbx rsi rdi r12 r13 r14 r15` | `rcx rdx r8 r9` carry arguments; `rax` returns and is scratch; `r10` is the second scratch; `rsp rbp` are the frame |
| SSE | `xmm6`–`xmm15` | `xmm0`–`xmm3` carry arguments and return values; `xmm4 xmm5` are scratch |

Eight integer registers out of sixteen is a deliberate trade. `idiv` demands
`rax`/`rdx`, a variable shift demands `cl`, a call writes every argument
register in turn. The textbook answer is to model those as pre-coloured nodes;
the answer here is to keep them out of the pool. Fewer registers, and an
allocator whose correctness argument fits on a page.

`r11` is listed first because it is the one caller-saved register in the pool:
handing it out before a callee-saved one avoids a push and a pop. It is also the
one register a value spanning a call may never use.

---

## 7. Prologue and epilogue

```asm
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   <each callee-saved GPR the assignment used>
    subq    $frameSize, %rsp
    movups  %xmmN, -k(%rbp)        # SSE has no push
    ...
    movups  -k(%rbp), %xmmN
    leaq    -8N(%rbp), %rsp        # N = number of GPRs pushed
    popq    <the same GPRs, in reverse>
    popq    %rbp
    ret
```

Two things load-bearing enough to state:

- **Local slots start below the pushed registers.** The pushes happen after
  `rbp` is established, so they occupy the first `8N` bytes below it.
- **An odd number of pushes costs eight bytes of frame.** `rsp` must be 16-byte
  aligned at every call; `push rbp` plus N pushes plus the frame is what gets it
  there, and the frame is the only part still free to correct the parity.

The SSE saves come *after* `subq`, never before: Windows x64 has no red zone.

---

## 8. Verification

`verifyAssignment` runs on **every** compilation, not behind a flag. It
recomputes live ranges from the IR and checks two rules:

1. Two locations live at the same point never share a register.
2. A location live across a call is never in a caller-saved register.

The allocator adds an edge where a location is *defined*; the verifier checks
every pair live at every point. Those are equivalent when the live ranges are
exact and diverge exactly where they are not, which is the failure it exists to
catch — it is how the missing argument edges were found. An assignment that
fails is not emitted: the backend falls back to frame slots for that function
and the driver reports an internal compiler error.

---

## 9. Debugging a suspected allocation bug

1. **Bisect the allocator.** `--regalloc=naive`. If the bug survives, it is not
   the allocator.
2. **Look at what it decided.** `--print-regalloc` prints one line per function:
   units, coloured, spilled, coalesced, peak pressure, and the callee-saved
   registers used.
   ```
   @swapper: 9 unit(s), 7 coloured, 0 spilled, 2 coalesced, 0 frozen, peak 3 live, saves %rbx %rsi %rdi
   ```
   Coloured plus spilled counts what survived coalescing, not what the function
   started with.
3. **Read the assembly.** `--emit=asm`. Every machine instruction carries the IR
   instruction that produced it as a comment.
4. **Widen the search.** `python tests/fuzz_differential.py build/bin/optiforge <count>`
   compares four configurations — `-O1`, `-O2`, and both at `--regalloc=naive` —
   against `-O0`, which localizes a divergence to the allocator or to a pass.
