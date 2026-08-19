# Adding an Optimization Pass

> Written after the Phase 7 pipeline existed, so the steps below are the ones
> actually taken rather than the ones planned.

---

## 1. The shape of a pass

A pass is one class in one file under `src/transforms/`. It sees a function
and reports whether it changed anything.

```cpp
class MyPass final : public Pass {
public:
  std::string_view name() const override { return "my-pass"; }
  std::string_view description() const override { return "what it does"; }

  bool run(ir::Function& function, analysis::AnalysisManager& manager) override {
    // ... return true only if the IR actually changed
  }
};

std::unique_ptr<Pass> makeMyPass() { return std::make_unique<MyPass>(); }
const PassRegistration kMyPass{"my-pass", makeMyPass};
```

Then add the file to `src/transforms/CMakeLists.txt` and the pass name to a
pipeline in `pipelineFor()` (`src/passes/PassManager.cpp`).

**No existing pass is touched.** That is the point of the registry: passes
never reference one another (`architectural_design.md` §3, rule 5).

### The anchor

Static libraries drop object files whose symbols nobody names, and that would
take the registration with them. Each transform file ends with a no-op anchor
that the driver and the tests call:

```cpp
void anchorMyPass() {}
```

Add it to `keepPassRegistrations()` in `src/driver/main.cpp` and to the
`KeepRegistrations` struct in `tests/unit/test_passes.cpp`. Forgetting this
does not fail the build — the pass simply never runs, which is worse.

---

## 2. Getting analyses

Ask the manager; never compute your own.

```cpp
const analysis::DominatorTree& tree =
    manager.get<analysis::DominatorTreeAnalysis>(function);
```

Results are cached across passes (ADR-03). Available: `DominatorTreeAnalysis`,
`PostDominatorTreeAnalysis`, `DominanceFrontierAnalysis`, `LoopAnalysis`,
`LivenessAnalysis`, `ReachingDefinitionsAnalysis`, `UseDefAnalysis`.

The manager invalidates every analysis for a function whenever a pass reports a
change. Declaring preserved analyses individually would be faster; being wrong
about one is a miscompile, so the blunt rule stands until a measurement says it
matters.

---

## 3. Rules that are not negotiable

### Report changes honestly

Returning `true` when nothing changed costs a pipeline sweep. Returning `false`
when something did changed leaves stale analyses behind — a miscompile.

### Leave the IR valid

The verifier runs after every pass under `--verify-each`, and a pass that
leaves invalid IR gets the *next* pass blamed for it.

This was learned the hard way: SCCP originally folded a constant branch and
left the orphaned block for `simplify-cfg` to remove. Run on its own, it
produced IR the verifier rejected. It now calls `removeUnreachableBlocks`
itself.

### Do not iterate a container you are mutating

Collect first, mutate second:

```cpp
std::vector<ir::Instruction*> dead;
for (const auto& block : function.blocks()) {
  for (const auto& instruction : block->instructions()) {
    if (isDead(*instruction)) dead.push_back(instruction.get());
  }
}
for (ir::Instruction* instruction : dead) instruction->eraseFromParent();
```

### Watch ownership when moving instructions

`BasicBlock::detach` hands ownership *back*. Discarding the returned
`unique_ptr` destroys the instruction:

```cpp
parent_->detach(this);                    // WRONG: deleted at the semicolon
auto owned = parent_->detach(this);       // right
```

That exact mistake in LICM corrupted the heap, and the crash surfaced far away
in an unrelated allocation.

### Keep it deterministic

No iteration over `unordered_map` where the order reaches the IR, no sorting by
pointer where the result is printed. Identical input must give identical output
(NFR-06), or every golden test becomes noise.

---

## 4. Tests a pass must ship with

Three kinds, in `tests/unit/test_passes.cpp`:

1. **It fires** on the shape it targets.
2. **It does not fire** where it would be wrong. This is the half that catches
   real bugs — see the tests asserting that division is *not* strength-reduced,
   that a call survives DCE, and that GVN does not reuse a value across sibling
   blocks.
3. **It leaves valid SSA**, via `analysis::verifySSA`.

Then add a program to `tests/e2e/` if the pass targets a shape not already
covered. Every end-to-end program runs at `-O0`, `-O1` and `-O2` and the output
must be byte-identical, so **a new pass is covered by every existing program
the moment it is added** (QA-05). That suite has caught every miscompile in
this project so far.

---

## 5. Debugging a pass

```bash
optiforge prog.of -O2 --print-after=my-pass --emit=ir
```

| Flag | Use |
|---|---|
| `--print-after=<pass>` | dump IR after one pass |
| `--print-after-all` | dump after every pass that changed something |
| `--verify-each` | verify after every pass, naming the culprit |
| `--disable-pass=<pass>` | leave a pass out — the fastest way to bisect a miscompile |

To find which pass broke a program: disable them one at a time until the output
is correct again.

---

## 6. Where a pass goes in the pipeline

Order is declared in `pipelineFor()`. Two things drive it:

- **Enabling.** Folding first means later passes see constants. Inlining
  exposes the callee's body to the caller's constants, which is why `-O2` runs
  `sccp` and `gvn` *again* immediately after it.
- **Cleanup.** `dce` and `simplify-cfg` run last because every other pass
  leaves debris.

The manager sweeps the pipeline until nothing changes, capped at
`kMaxIterations`. A pipeline that runs to the cap means two passes are undoing
each other's work.

---

## 7. Module-level work

The `Pass` interface is per-function on purpose: almost everything is. Work
that genuinely spans functions — `removeUnusedFunctions`, for instance, which
cannot see from inside a function that nothing calls it — is a free function
called by the driver rather than a contortion of the interface.
