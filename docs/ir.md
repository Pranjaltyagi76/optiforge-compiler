# OptiForge IR

> **Status:** complete for Phase 3. Phi nodes exist in the instruction set but
> are only populated by `mem2reg` in Phase 6.
> Produced by `optiforge <file>.of --emit=ir`.

---

## 1. What the IR Is For

A three-address, language-independent representation sitting between the typed
AST and machine code. Everything the middle-end does — analysis, optimization,
instrumentation — happens here.

`of_ir` deliberately **does not depend on the frontend**. It has its own type
system and knows nothing about `.of` syntax. `of_irgen` is the only module that
sees both. That boundary is what makes the IR testable on its own and reusable
for another source language.

---

## 2. Types

| IR type | Frontend type | Size |
|---|---|---|
| `i64` | `int` | 8 bytes |
| `f64` | `float` | 8 bytes |
| `i1` | `bool` | 1 byte |
| `void` | `void` | 0 |
| `ptr` | — | 8 bytes |

`ptr` has no source-level counterpart; it is only what an `alloca` produces.
Types are interned, so identity is pointer comparison.

---

## 3. Structure

```
Module            one compilation unit; owns functions and interned constants
└── Function      signature + control-flow graph
    └── BasicBlock    straight-line run, one entry, one exit
        └── Instruction
```

A **Value** is anything usable as an operand: a constant, a function argument,
or an instruction result.

### Use tracking

Every `Value` records the instructions that use it, which makes
`replaceAllUsesWith` — the workhorse of constant folding, CSE and copy
propagation — a direct operation rather than a whole-function scan.

### Successors and predecessors

Successors are **derived** from the block's terminator, so they can never go
stale. Predecessors have nothing to derive from and are therefore stored, and
maintained in one place (`BasicBlock::append`). The verifier independently
recomputes them and compares, so a bookkeeping slip is caught rather than
silently miscompiled.

---

## 4. Instruction Set

Defined once in `include/optiforge/ir/Instruction.def`, from which the opcode
enum, mnemonic table, verifier and printer are all generated.

| Category | Instructions |
|---|---|
| Integer arithmetic | `add` `sub` `mul` `sdiv` `srem` `neg` |
| Float arithmetic | `fadd` `fsub` `fmul` `fdiv` `fneg` |
| Logical | `not` |
| Comparison | `icmp` `fcmp` — always produce `i1` |
| Conversion | `sitofp` |
| Memory | `alloca` `load` `store` `gep` (Phase 13) |
| Control flow | `br` `condbr` `ret` |
| Calls | `call` |
| SSA | `phi` (Phase 6), `copy` (Phase 6) |

Comparison predicates: `eq` `ne` `lt` `gt` `le` `ge`. Signedness and
float-ness come from the opcode, so one predicate set serves both.

---

## 5. Textual Syntax

```
module "examples/sum.of" hash=0x58e6b80ffffa7148

fn @print_int(i64 %value) -> void;

fn @sum(i64 %n) -> i64 {
entry:
  %n.addr = alloca i64
  store i64 %n, %n.addr
  %total.addr = alloca i64
  store i64 0, %total.addr
  br while.cond.1

while.cond.1:                                   ; preds = entry, while.body.2
  %t0 = load i64, %i.addr
  %t1 = icmp lt i64 %t0, %t2
  condbr %t1, while.body.2, while.end.3

while.end.3:                                    ; preds = while.cond.1
  %t8 = load i64, %total.addr
  ret i64 %t8
}
```

- `@name` is a function, `%name` a value.
- A function with no body ends in `;` — those are runtime builtins resolved at
  link time.
- The `; preds =` comment is informational; the edges live in the terminators.

The output is **diff-stable by construction**: no addresses, no hash-map
iteration, sequential temporary names. That is what makes golden-file testing
possible (NFR-06).

---

## 6. Block Naming (requirement IR-11)

The first block of a function is `entry`. Every other block is
`<prefix>.<n>`, with `n` a per-function counter incremented in creation order:

```
entry, if.then.1, if.else.2, if.end.3, while.cond.4, while.body.5, while.end.6
```

This is **part of the contract, not a printing detail.** Profile records are
keyed by `function:block` (see `profile-format.md` and ADR-06). A label that
shifted between compilations would make every profile lookup miss, PGO would
silently fall back to its no-profile path, and the build would look correct
while doing nothing. The counter is per function so that adding a function does
not renumber the blocks of any other.

---

## 7. Lowering Rules

| Source | IR |
|---|---|
| `int x = e;` | `%x.addr = alloca i64` in the entry block, then `store` |
| `x` | `%t = load i64, %x.addr` |
| `int a[4];` | `%a.addr = alloca i64 x 4` in the entry block, **no store** |
| `a[i]` | `%p = gep i64, %a.addr, i64 %i` then `%t = load i64, %p` |
| `a[i] = e;` | `%p = gep i64, %a.addr, i64 %i` then `store` |
| `a + b` | `add` / `fadd`, with `sitofp` inserted on the narrower side |
| `a < b` | `icmp lt` / `fcmp lt` |
| `f(a)` | `%t = call <type> @f(...)` |
| `if` / `while` | blocks plus `br` / `condbr` |
| `a && b`, `a \|\| b` | branch plus a stack slot for the result |

### Two invariants worth stating

**Every `alloca` lives in the entry block**, wherever the declaration appears.
One inside a loop would grow the stack on each iteration, and `mem2reg` (Phase
6) only promotes entry-block slots. `IRBuilder::createEntryAlloca` owns this
rule so no call site has to remember it.

**Parameters get stack slots too.** Assigning to a parameter then needs no
special case; `mem2reg` removes the traffic later.

### Arrays and `gep` (Phase 13)

`alloca` gained an element count and the instruction set gained one opcode:

```
%a.addr = alloca i64 x 4              ; four consecutive elements
%p      = gep i64, %a.addr, i64 %i    ; &a[i]
%t      = load i64, %p
```

The count is 1 for every scalar, so nothing about an ordinary local changed.

**Why an opcode rather than open-coded arithmetic.** `gep` could have been a
`mul` by the element size and an `add`, and GVN would still have commoned two
indexings of the same element. What that loses is checkability: the verifier can
say "a pointer base and an integer index" about a `gep` and can say nothing
useful about an `add` whose operands happen to be a pointer and a number. It
also leaves the IR saying *what* is being computed rather than how, which is the
property that makes `--emit=ir` worth reading.

**An array is never promoted to a register.** `mem2reg` refuses any `alloca`
whose users are not exclusively `load` and `store`, and every array use is a
`gep`, so the existing rule excludes arrays without needing to know they exist.

**Element size is the slot size, not the natural size.** Every element occupies
eight bytes, `bool` included, so the backend scales the index by a constant 8.
Packing would make element addressing disagree with the frame layout that every
other value uses.

### Short-circuit evaluation

`a && b` becomes control flow so `b` is not evaluated when `a` is false:

```
entry:      %t1 = <a>;  store %t1, %and.addr;  condbr %t1, and.rhs.1, and.end.2
and.rhs.1:  %t3 = <b>;  store %t3, %and.addr;  br and.end.2
and.end.2:  %t4 = load i1, %and.addr
```

Without SSA the result travels through a stack slot; `mem2reg` turns that into
the phi node the shape really wants.

---

## 8. Verifier

Runs after IR generation and, from Phase 7, after every pass. A failure is an
**internal compiler error**, never a user error.

- Every block ends in exactly one terminator, and it is last
- Entry block has no predecessors
- Every block is reachable (unreachable ones are pruned first)
- Predecessor lists agree with the terminators that target them
- Operand counts and types match the opcode
- Every operand lists its user
- No operand refers to an instruction outside the function
- `alloca` only in the entry block, and its element count is at least 1
- `gep` produces a `ptr` from a `ptr` base and an `i64` index, and carries the
  element type it addresses
- `condbr` condition is `i1`; `br` has one successor, `condbr` two
- `ret` matches the function's return type
- Call arity, argument types and result type match the callee
- Phi nodes only at the top of a block

---

## 9. Control-Flow Graph Export

```bash
optiforge program.of --emit=cfg | dot -Tpng -o cfg.png
```

Emits Graphviz DOT, one cluster per function, with `true`/`false` labels on
conditional edges. Requires [Graphviz](https://graphviz.org) to render.

---

## 10. Deviations From the Original Design

Both recorded because `System_design.md` §5 describes something different.

| Design | Built | Why |
|---|---|---|
| Intrusive `Use` nodes with prev/next links | `std::vector<Instruction*>` of users | Same capability including `replaceAllUsesWith`; removal is a linear scan instead of O(1), which is irrelevant at this scale. Revisit if metric P-04 says so. |
| Arena allocation | `std::unique_ptr` ownership | Consistent with the AST decision in Phase 1; no trivially-destructible constraint, and NFR-01 is met with two orders of magnitude to spare. |

### Teardown order is load-bearing

Instructions hold raw pointers into each other's user lists, and `Module`
members are destroyed in reverse declaration order. Constant storage is
therefore declared **before** `functions_`, so constants outlive every
instruction that references them; and `Function::dropAllReferences()` clears
all operands before any block is destroyed. Reordering those members would
reintroduce a use-after-free that no test would reliably catch.
