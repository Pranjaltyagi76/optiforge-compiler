# Phase <N> Evaluation — `<Phase Name>`

> Copy to `metrics/reports/phase-<N>-evaluation.md`. Written at each phase boundary, before advancing.
> Purpose: decide whether the phase is genuinely done, using evidence rather than a feeling that it works.

| Field | Value |
|---|---|
| Phase | <N> — <name> |
| Started / completed | YYYY-MM-DD / YYYY-MM-DD |
| Git revision at completion | `<short SHA>` |
| Milestone reached | M__ (`roadmap.md` §6) |

---

## 1. Exit Criteria  *(metric R-02)*

From `roadmap.md` — every criterion, with evidence. "It seems to work" is not evidence.

| # | Exit criterion | Met? | Evidence |
|---|---|---|---|
| 1 | | ☐ | *test name, result file, or command output* |
| 2 | | ☐ | |
| 3 | | ☐ | |

**Verdict:** ☐ Phase complete · ☐ Incomplete — items outstanding below

---

## 2. Requirements Delivered  *(metric R-01)*

| Requirement ID | Description | Verified by | Status |
|---|---|---|---|
| | | | ☐ Done ☐ Partial ☐ Deferred |

**Deferred requirements** — what was pushed, to which phase, and why:

| ID | Deferred to | Reason |
|---|---|---|
| | | |

---

## 3. Metrics at Phase Boundary

Only metrics measurable at this phase (see the "From" column in `metric-catalog.md`).

| Metric ID | Name | Value | Target | Met? | Trend vs last phase |
|---|---|---|---|---|---|
| C-05 | Differential pass rate | | 100% | ☐ | |
| | | | | ☐ | |

**Regressions since the last phase:**

| Metric | Was | Now | Cause | Accepted or fixed? |
|---|---|---|---|---|
| | | | | |

> A regression must be either fixed or explicitly accepted with a reason. Silently carrying one forward is how a compiler ends up slow with nobody able to say when it happened.

---

## 4. What Worked

*Design decisions that paid off. Be specific enough to reuse the insight — "the arena allocator made AST teardown a non-issue" is useful; "the design was good" is not.*

---

## 5. What Did Not Work

*Approaches abandoned, bugs that cost disproportionate time, estimates that were wrong.*

| Issue | Time cost | Root cause | Prevented in future by |
|---|---|---|---|
| | | | |

> This section is the most valuable part of the document for the final report, and the one most tempting to skip. A compiler project's real story is in what turned out to be harder than expected.

---

## 6. Risks

**Risks that materialized** (from `roadmap.md` §7):

| Risk | What happened | Mitigation effective? |
|---|---|---|
| | | |

**New risks identified:**

| Risk | Impact | Likelihood | Mitigation | Added to register? |
|---|---|---|---|---|
| | | | | ☐ |

---

## 7. Estimate Accuracy  *(informs future phase planning)*

| | Estimated | Actual | Ratio |
|---|---|---|---|
| Duration | __ weeks | __ weeks | _._× |

**Why the difference:**

---

## 8. Readiness for the Next Phase

| Question | Answer |
|---|---|
| Are all blocking dependencies satisfied? | ☐ Yes ☐ No — |
| Any decisions that must be made first? | *e.g. ADR-10 target platform before Phase 4* |
| Any technical debt that will compound if not paid now? | |
| Does `context/` still describe reality? | ☐ Yes ☐ Updated ☐ Needs updating |

> The last question matters. Design documents that drift from the implementation become actively misleading — worse than having none. Update them at the phase boundary while the differences are still fresh.
