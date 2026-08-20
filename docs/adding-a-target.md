# Adding a Target

> Written after Phase 12, with one target implemented. So unlike
> [`adding-a-pass.md`](adding-a-pass.md) — written after seven passes had gone
> through the process — this describes a road that has been surveyed but only
> partly walked. Where that distinction matters, it is marked.

---

## 1. Read this first: two very different jobs

"Adding a target" means one of two things here, and they are not the same size.

| | What it is | Effort | Behind `TargetInfo`? |
|---|---|---|---|
| **A new ABI on x86-64** | Same instructions, different calling convention — System V (Linux, macOS) alongside the existing Microsoft x64 | **One file.** A data change. | **Yes, entirely** |
| **A new instruction set** | ARM64, RISC-V — different registers, different mnemonics, different everything | **A new backend.** | **No.** See §5. |

`TargetInfo` was designed for the first (`architectural_design.md`: *"All ABI
facts behind `TargetInfo`"*). It does the job it was designed for, completely.
It was never designed for the second, and pretending otherwise would waste the
time of whoever tries.

ADR-10 fixed the target as x86-64 Windows and said the abstraction *"allows a
System V target to be added later as a data change rather than a rewrite"*.
That claim is true and §3 is the recipe. The claim was never that an ARM64
target is a data change.

---

## 2. What `TargetInfo` actually is

One abstract class, `include/optiforge/backend/TargetInfo.h`, with sixteen
virtual methods and no state. One implementation,
`src/target/x86_64/X86Target.cpp`, which is the worked example this document
keeps pointing at. Read it — it is 119 lines and it is the whole contract.

The methods fall into four groups:

**Calling convention.** `integerArgRegister(index)`, `floatArgRegister(index)`,
`maxRegisterArgs()`, `shadowSpaceBytes()`, `integerReturnRegister()`,
`floatReturnRegister()`.

**Registers the allocator may use.** `allocatableIntRegisters()`,
`allocatableFloatRegisters()`, `isCalleeSaved(reg)`.

**Registers the code generator reserves.** `scratchInt0/1()`,
`scratchFloat0/1()`.

**Layout and naming.** `stackAlignment()`, `symbolName(name)`, `name()`.

---

## 3. Adding an ABI: the System V worked example

This is the case the abstraction exists for. Six steps.

### 3.1 Write the class

Copy `src/target/x86_64/X86Target.cpp` to `X86SysVTarget.cpp` and change the
answers. For System V on x86-64 the differences from Microsoft x64 are:

| Fact | Microsoft x64 | System V |
|---|---|---|
| Integer argument registers | `rcx rdx r8 r9` | `rdi rsi rdx rcx r8 r9` |
| `maxRegisterArgs()` | 4 | 6 (integer), and float args are counted **separately** |
| Float argument registers | `xmm0`–`xmm3` | `xmm0`–`xmm7` |
| `shadowSpaceBytes()` | **32** | **0** |
| Callee-saved integer | `rbx rbp rsi rdi r12`–`r15` | `rbx rbp r12`–`r15` — **not** `rsi`/`rdi` |
| Callee-saved float | `xmm6`–`xmm15` | **none** — every XMM is caller-saved |
| `symbolName()` | bare name | bare name on ELF; `_` prefix on Mach-O |

Then export it next to the existing accessor in `TargetInfo.h`:

```cpp
const TargetInfo& x86_64WindowsTarget();   // ADR-10, the default
const TargetInfo& x86_64SysVTarget();      // new
```

### 3.2 The two traps in that table

**`maxRegisterArgs()` means something different under System V.** Read its
comment in `TargetInfo.h`:

> *Argument positions are shared between integer and float registers, so index 1
> is the second argument regardless of the first one's type.*

That is a Microsoft x64 rule, and the interface encodes it. Under System V the
two register classes are counted independently: `f(double, int)` passes the
double in `xmm0` and the int in `rdi` — **not** `rsi`. A single
`maxRegisterArgs()` returning one number cannot express that, so **this
interface needs one change to support System V**: either split it into
`maxIntegerRegisterArgs()` and `maxFloatRegisterArgs()`, or give the target the
whole argument-assignment job rather than one register at a time.

This is the one place the abstraction is genuinely x64-Windows-shaped, and it is
recorded here rather than discovered halfway through.

**Callee-saved float registers change the prologue's shape, not just its
contents.** The x86 target's comment explains why the Microsoft prologue spills
XMM registers to the frame rather than pushing them: *"there is no push for an
SSE register."* Under System V no XMM register is callee-saved, so that code
path stops being exercised — which means it stops being tested, not that it
stops existing.

### 3.3 Build it

```cmake
# src/target/x86_64/CMakeLists.txt
add_library(of_target_x86_64 STATIC
  X86Target.cpp
  X86SysVTarget.cpp)
```

No new `add_subdirectory` is needed — both ABIs share the instruction set, so
they belong in the same library.

### 3.4 Let the driver select it

The target is chosen in exactly one place,
[`src/driver/main.cpp:375`](../src/driver/main.cpp):

```cpp
backend::CodeGen codegen(backend::x86_64WindowsTarget(), allocator);
```

Add a `--target=` option in `src/driver/Options.cpp` next to `--regalloc=`,
which is the closest existing model: an enum, a `parse…` function that returns
false on an unknown name, and a usage error listing the valid ones. **Reject an
unknown target name rather than falling back to the default** — a silent
fallback would compile for the wrong ABI and produce a binary that crashes at
the first call.

### 3.5 The toolchain will not follow you

`src/driver/Toolchain.cpp` shells out to `gcc` to assemble and link, and adds
`-Wl,--whole-archive` for the profile runtime. Those flags are GNU-ld
spellings; the linker driver on a Mach-O host takes different ones, and the
runtime libraries in `runtime/` are built for the host by the same CMake
project. **A System V target that is cross-compiled is not runnable or
testable**, so the honest scope of §3 is: an ABI you can also *run*, i.e. build
the project on Linux and target System V there.

### 3.6 Test it

The suite is target-parameterized in one respect already and not in another:

- `tests/unit/test_codegen.cpp` and `test_regalloc.cpp` name
  `x86_64WindowsTarget()` in twelve places between them (five and seven).
  Those become the natural parameterization point.
- **The golden `.expected` files are target-specific and cannot be shared.**
  `tests/golden/asm_*.expected` contain Microsoft x64 assembly down to the
  `.def`/`.scl`/`.endef` directives. A second target needs its own golden set,
  not a merged one.
- **The end-to-end suite is where a new ABI is really tested**, because it
  checks what the program *prints*, which is ABI-independent. Every one of the
  48 runs in `tests/e2e/` should pass unchanged.

Two existing tests are worth pointing a new ABI at first, because they are
exactly the ones an ABI bug breaks:
`tests/e2e/calling_convention.of` and `tests/e2e/register_pressure.of` (22
values live against 8 registers).

---

## 4. What `TargetInfo` buys, precisely

Worth stating so the abstraction is not over-credited or under-credited. These
components are genuinely ABI-neutral today and need **no change** for §3:

| Component | Why it is neutral |
|---|---|
| `RegAlloc.cpp` | Asks the target for the register pool and for `isCalleeSaved`; never names a register. |
| Frame layout in `CodeGen.cpp` | Asks for `stackAlignment()` and `shadowSpaceBytes()`. |
| Call lowering | Asks for `integerArgRegister(i)` / `floatArgRegister(i)` and `maxRegisterArgs()` — modulo the §3.2 trap. |
| Prologue and epilogue | Driven by `usedCalleeSaved` in `RegisterAssignment`, which the allocator fills from `isCalleeSaved`. |

### The exceptions, counted

The code generator is *not* entirely free of named registers, and the useful
version of this section says exactly where. Outside `src/target/` and
`MachineIR.cpp` — the latter being the register *table*, so naming registers is
its job — `MReg::` appears 54 times: 46 in `CodeGen.cpp` and 8 in
`RegAlloc.cpp`. Twenty-five of those are `MReg::None`, which is not a register.
That leaves **29 real references, all in `CodeGen.cpp`, and zero in the register
allocator**:

| Registers | Uses | What they are | Does a new ABI care? |
|---|---:|---|---|
| `RBP`, `RSP` | 22 | The frame and stack pointers, in addressing modes | **No** — same registers under System V |
| `RAX`, `R10`, `RDX` | 5 | `idivq` writes the quotient to `rax` and the remainder to `rdx`, and needs its divisor somewhere ([CodeGen.cpp:497](../src/backend/CodeGen.cpp)) | **No** |
| `RCX` | 2 | A variable shift count must be in `cl` ([CodeGen.cpp:634](../src/backend/CodeGen.cpp)) | **No** |

This is the design working as documented rather than leaking: `TargetInfo.h`
says so in as many words — *"the code generator's fixed sequences own those --
`idiv` needs `rax`/`rdx`, a variable shift needs `cl`"* — and that is why those
registers are kept out of the allocatable pool.

The distinction that matters: these are **instruction-set** facts, not **ABI**
facts. x86-64 requires `idiv` to use `rax`/`rdx` no matter whose calling
convention is in force, so a System V port does not touch a line of it. A new
instruction set replaces all of it.

---

## 5. Adding an instruction set: what it actually costs

If the new target is ARM64 or RISC-V, `TargetInfo` is the smallest part of the
job. Here is the rest of it, measured against the current tree rather than
estimated.

**`MReg` is an x86 register file.**
[`MachineIR.h:12`](../include/optiforge/backend/MachineIR.h) enumerates `RAX`
through `R15` and `XMM0` through `XMM15`, and `regName()` returns AT&T spellings
with the `%` sigil. `isCalleeSaved` in the x86 target even relies on the
*ordering* of the enum (`reg >= MReg::XMM6 && reg <= MReg::XMM15`). A second
register file means either extending this enum with a second ISA's registers —
and every `switch` over it — or making it a target-supplied type.

**Instruction selection emits x86 mnemonics as raw strings.** `MInstr::mnemonic`
is a `const char*`, deliberately: its comment says a table of opcodes *"would add
indirection without adding checking"* for a backend whose only job is to be
correct. That was the right call for one target and it is exactly the decision
that a second ISA reopens. `CodeGen.cpp` emits about **fifty distinct
mnemonics** — `movq`, `addq`, `imulq`, `idivq`, `cqto`, `comisd`, `cvtsi2sdq`,
`setcc`, `movzbq`, and the rest. Every one is an instruction-selection decision
that ARM64 makes differently.

**Block layout knows x86 branch mnemonics.** `Layout.cpp` inverts conditions to
make the hot side fall through, via a hardcoded table:
`{"je","jne"}, {"jl","jge"}, {"jb","jae"}, {"jp","jnp"}` and their reverses,
and it detects branches with `strncmp(mnemonic, "j", 1)`. This is the
profile-guided layout pass that Phase 12 measured at **+15.8%** — the single
largest win in the project — so it is not optional, and it is string-matching
x86.

**`AsmPrinter.cpp` emits AT&T syntax and PE/COFF directives.** `.def … .scl 2
… .endef`, `.section .rdata,"dr"`, `%rip`-relative addressing for float
constants. ELF and Mach-O want different directives; ARM64 wants different
syntax entirely.

**The float-constant machinery is an x86 workaround.** `FloatConstant` and
`needsNegateMask` exist because, as `MachineIR.h` says, *"x86-64 has no
instruction to load an arbitrary 64-bit float as an immediate"*. On an ISA
without that limitation this is dead weight, not a service.

### The honest recommendation

A second instruction set is a **Phase 13 project in its own right**, and the
first step is not writing ARM64 code — it is introducing the opcode enum that
`MachineIR.h` explicitly deferred, so that instruction selection has somewhere
target-specific to live. Doing it the other way round means two backends sharing
a string-typed instruction format, and the compiler losing the one property that
made the x86 backend tractable: that being correct was the only thing it had to
be.

---

## 6. Checklist

For a new ABI on x86-64:

- [ ] New `TargetInfo` subclass in `src/target/x86_64/`, exported from `TargetInfo.h`
- [ ] Split `maxRegisterArgs()` if the ABI counts integer and float arguments separately (§3.2)
- [ ] Added to `src/target/x86_64/CMakeLists.txt`
- [ ] `--target=` in `Options.cpp`, rejecting unknown names rather than falling back
- [ ] Its own `tests/golden/asm_*.expected` set — goldens are not shareable across targets
- [ ] The whole `tests/e2e/` suite passes unchanged, at `-O0`, `-O1` and `-O2`
- [ ] `tests/e2e/calling_convention.of` and `register_pressure.of` pass first, before anything else is trusted
- [ ] The differential fuzzer (`tests/fuzz_differential.py`) run against it
- [ ] No *new* named registers outside `src/target/`. The 29 that exist are frame pointers and `idiv`/shift fixed operands (§4); a new ABI should add none:
      `grep -o 'MReg::[A-Z0-9]*' src/backend/*.cpp | grep -v 'MReg::None' | sort | uniq -c`

For a new instruction set: read §5 before starting.
