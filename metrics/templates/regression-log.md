# Regression Log

> Running record of every metric that moved the wrong way. Append-only — resolved entries stay, with their resolution.
> Copy into `metrics/reports/regression-log.md` and maintain there.

A regression is any metric that worsened beyond the noise floor. Every one gets an entry, even if the cause turns out to be trivial. The value is the pattern that emerges across entries.

---

## Open

| # | Date | Metric | Was | Now | Δ | Suspected cause | Bisected to | Owner | Status |
|---|---|---|---|---|---|---|---|---|---|
| R-001 | | | | | | | | | ☐ Investigating |

---

## Resolved

| # | Date found | Metric | Δ | Root cause | Fix | Resolved | Prevented by |
|---|---|---|---|---|---|---|---|
| | | | | | | | |

---

## Accepted (deliberately not fixed)

| # | Metric | Δ | Why accepted | Revisit when |
|---|---|---|---|---|
| | | | | |

---

## How to Handle a Regression

```
1. Confirm it is real:
      - exceeds the noise floor (methodology.md §3.2)
      - reproduces in a second session
      -> if not, it is noise. Log it as such and stop.

2. Bisect:
      git bisect run <script asserting the metric>
      For pipeline regressions, bisect the pass list instead:
      --print-after-all, or disable passes one at a time.

3. Classify:
      Correctness (C-*)      -> stop all other work. Nothing else matters
                                until the differential suite is green again.
      PGO effectiveness (G-*) -> check G-03 match rate FIRST. A collapsed
                                match rate explains most sudden PGO losses.
      Performance (Q-*, P-*)  -> attribute to a specific pass before fixing.

4. Record here with the root cause, not just the fix.
```

> **The match-rate check in step 3 is worth doing before anything else on a PGO regression.** When PGO stops helping, the cause is far more often that block naming drifted (breaking requirement IR-11) than that a PGO heuristic got worse. The symptom — "PGO no longer beats -O2" — looks like an optimization problem and is usually an identity problem.
