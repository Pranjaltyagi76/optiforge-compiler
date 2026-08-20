# OptiForge Documentation

Reference specifications. Design rationale lives in [`../context/`](../context/);
measurement data and evaluation live in [`../metrics/`](../metrics/).

| Document | Contents | Written in phase |
|---|---|---|
| [language.md](language.md) | Grammar (EBNF), types, semantics | 1-2 |
| [ir.md](ir.md) | IR instruction set, textual syntax, invariants | 3 |
| [profile-format.md](profile-format.md) | `.prof` format: grammar, what is counted, what is derived, validation, classification | 9-10 |
| [adding-a-pass.md](adding-a-pass.md) | Extension guide for new passes | 7 |
| [register-allocation.md](register-allocation.md) | Live ranges, interference, colouring, the ABI's share of the register file | 8 |

Each file is a stub until its phase. A stub is honest; a stale spec is worse
than none.
