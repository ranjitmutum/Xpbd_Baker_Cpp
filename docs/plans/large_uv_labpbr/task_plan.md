# Large UV / LabPBR execution plan

## Current phase

S06 — shared immutable texture/Iris ownership and copy-on-write Composition (`COMPLETE; COMMIT PENDING`).

## Phases and dependencies

| Phase | Scope | Depends on | Status |
|---|---|---|---|
| S00 | Baseline; runtime synthetic fixtures; checked compact-memory estimate | — | COMPLETE |
| S01 | Pure double-precision face UV parsing, bounds, and `ResolvedUvDomain` | S00 | COMPLETE |
| S02 | Apply one domain to mesh/material consumers; atlas clamp; transaction/history behavior | S01 | COMPLETE |
| S03 | Unicode/long-path snapshot-and-memory image import | S02 | COMPLETE |
| Gate A | Release app build and full CTest correctness gate | S00–S03 | COMPLETE |
| S04 | Remove persistent resolved-texel expansion; final-model budget preflight | Gate A | COMPLETE |
| S05 | Lazy Coverage/Composition and compact run encoding | S04 | COMPLETE |
| S06 | Shared immutable texture and Iris byte ownership; copy-on-write composition | S05 | COMPLETE |
| S07 | Shared-asset byte-bounded LRU import cache | S06 | PENDING |
| Gate B | Release app build, full CTest, 2K and one 4K memory gate | S07 | PENDING |
| Final | Final Release/CTest regression, scope audit, clean task diff, report | Gate B | PENDING |

## Gates

- Every phase: affected build/tests, `git diff --check`, scope review, planning update, one isolated commit, then verify that only the protected pre-existing dirty baseline remains.
- Gate A must pass before S04. Missing NVIDIA RT hardware is recorded as `SKIPPED_NO_SUPPORTED_GPU`; CPU reference and shader source contracts remain mandatory.
- 2K stress runs only in memory-structure work and Gate B; 4K runs once immediately before/at Gate B; 8K performs checked arithmetic and fast rejection only.
- Any import/domain/coverage/composition/budget/cache failure must preserve the prior Session, Generation, and GPU material state.
- A failed phase gets at most two evidence-based local repair rounds. A second failed round restores only this phase's agent-owned changes to its last green commit, records `BLOCKED`, and stops.
- Excluded throughout: Vulkan staging/submit/fence/transfer/async/resource-ownership work, PBR/BRDF or LabPBR semantics, nearest/LOD-0 changes, RR/SR/FG protocols, XPBD/Bullet, UI layout, and incremental Composition updates.
