# Progress

| Date | Phase | Status | Modified files | Tests / checks | Commit | Next |
|---|---|---|---|---|---|---|
| 2026-08-02 | Initialization | COMPLETE | `docs/plans/large_uv_labpbr/{task_plan,progress,findings}.md` | Plan read in full (13,310 bytes); branch/status/log inspected | pending S00 commit | Inspect README/CMake/tests/code, reconcile upstream, run untouched baseline |
| 2026-08-02 | S00 | COMPLETE | `CMakeLists.txt`; `include/xpbd/gfx/labpbr_memory.hpp`; `src/test_support/labpbr_synthetic_fixture.{hpp,cpp}`; viewport regression; planning files | Untouched Release baseline + CTest 5/5 PASS; affected Release targets PASS; viewport/AppSession CTest 2/2 PASS; `git diff --check` PASS | `4bdc38a` | S01: strict declarations, double Face UV, bounds, and pure Domain |
| 2026-08-02 | S01 | COMPLETE | `bedrock_model_data.hpp`, `model_loader.cpp`, `uv_domain.{hpp,cpp}`, `viewport_mesh.cpp`, AppSession declaration callers, CMake, synthetic fixture, viewport regression, planning files | Release App + viewport + AppSession targets PASS; related CTest 2/2 PASS (1.29 s); `git diff --check` PASS | `7cafe52` | S02: apply Domain to every material consumer and clamp model atlases |
| 2026-08-02 | S02 | COMPLETE | `viewport_mesh`, texture/material sampling, Coverage/Alpha, AppSession transactions, model Vulkan/PT samplers, viewport/AppSession regressions, planning files | One bounded repair fixed Alpha typing; final Release app + viewport + AppSession targets PASS; affected CTest 2/2 PASS (1.30 s); `git diff --check` PASS | `80970b6` | S03: Unicode/long-path snapshot-and-memory imports |
| 2026-08-02 | S03 | BLOCKED | planning files only after exact S03 rollback | AppSession CTest 1/1 PASS after repair 2, but affected viewport CTest then failed 1 assertion (`corrupt _s is rejected`); repair budget exhausted | none | Restore six S03 production/test files to `80970b6`; stop before Gate A |
| 2026-08-02 | S03 resumed | COMPLETE | texture snapshot/decode, strict Suite/Iris import, COM dialogs, AppSession/viewport regressions, planning files | Affected Release app + viewport + AppSession targets PASS; related CTest 2/2 PASS (1.45 s); source contracts and `git diff --check` PASS | `a9b955d` | Gate A: full Release build and full CTest |
| 2026-08-02 | Gate A | COMPLETE | planning files only after S03 commit `a9b955d` | Full Release preset PASS; SPIR-V 27-shader verification PASS; full CTest 5/5 PASS (4.30 s); CPU/reference/source contracts PASS | no gate commit by plan | S04: remove resolved-texel expansion and add final-model budget preflight |
| 2026-08-02 | S04 | COMPLETE | compact material images/on-demand decode; checked pre-decode/final-model budgets; Suite/Iris/cache accounting; AppSession/viewport regressions; planning files | Release app + viewport + AppSession targets PASS; related CTest 2/2 PASS (1.35 s); actual 2K + 8K arithmetic-only gates PASS; `git diff --check` and scope audit PASS | this isolated phase commit | S05: lazy Coverage/Composition and run encoding |

- Resumed implementation: restored the shared snapshot API declaration and removed the filename-based stb branch; subsequent patches are reapplying the previously compiled candidate without altering its scope.

- Resume recovery confirmed the newest test log is the historical final S03 viewport failure, `HEAD` and protected dirty baseline are unchanged, and no S03 code survived the mandated rollback. All three planning files, including the long findings file in bounded chunks, have now been re-read completely before editing.

- BLOCKED rollback verified: the six S03 production/test paths have no content diff from `80970b6`; `git diff --check` passes, and status again contains only the three planning files plus the protected pre-existing dirty baseline. No S03 commit was created and Gate A was not run.

- Recovery audit after context compaction: session catch-up contained only the current status/tool messages; all three planning files were re-read in full (with the long findings file read in bounded chunks), `HEAD` remains `80970b6`, the task-owned diff is limited to the three planning files, the protected pre-existing dirty baseline is unchanged, and the most recent affected CTest result remains 2/2 PASS (1.30 s).

- Upstream reconciliation: fetched `origin/xpbdRT`; no production-code delta, but its docs deletion overlaps the protected modified manifest. Local HEAD retained without altering user work.

- S03 resume implementation: restored transactional bounded file snapshots and the memory-only Base texture decode entry point. Candidate assignment still occurs only after snapshot and decode both succeed; no caller-visible texture state is replaced on failure.

- S03 import audit: the strict suite importer still contained its legacy narrow-path `ifstream`, `.string()` metadata, and filename stb assumptions. The resumed patch will route all Base/Sidecar snapshots through the shared bounded byte snapshot while preserving native `filesystem::path` sidecar construction.

- S03 strict-suite candidate restored: discovery and Base/Normal/Specular/properties reads now use extended-length filesystem I/O, native path sidecar names, UTF-8 diagnostics/cache metadata, shared immutable snapshots, and explicit Sidecar/Header/Decode/Domain/budget stage labels.

- S03 Iris audit: the read-only Iris normal path was the remaining independent narrow-path stream. It will use the same bounded snapshot/decode transaction and retain its existing owned original-byte representation until the later S06 sharing phase.

- S03 Iris candidate restored: the Iris importer now snapshots and decodes transactionally from memory with UTF-8 metadata and explicit Decode/Domain/budget diagnostics. The AppSession audit confirms the remaining work is the legacy fixed-buffer Win32 dialogs plus user-facing narrow path conversions.

- S03 session metadata: Base/Suite/Specular/Iris texture paths and status filenames now use decoded texture metadata or explicit UTF-8 conversion. This avoids lossy narrow conversions while leaving unrelated session/UI behavior unchanged.

- S03 dialog candidate restored: all Windows open/folder/save selectors now use shell `IFileOpenDialog`/`IFileSaveDialog` with filesystem results and no fixed path buffer. Existing modal GPU pause, filters, default extensions, ownership, and open/save option semantics are retained.

- S03 regression harness preparation: binary fixture read/write helpers now use the production extended-length filesystem spelling, allowing the forthcoming runtime-generated >260-character Unicode fixture to exercise real file I/O instead of relying on repository assets.

- S03 Unicode transaction regression restored: runtime tests now create >330-character Chinese paths and verify Base/Suite/Iris success, exact source bytes, pre-allocation budget rejection, Header-vs-Decode diagnostics, missing/corrupt Sidecars, mismatched Iris Domain rollback, and COM/memory-only source contracts. No large fixture is committed.

- S03 resumed compatibility repair applied: the sole legacy viewport assertion now requires the stronger `Specular Sidecar` plus `Header stage` contract for the corrupt `_s` fixture. Rejection and rollback expectations are unchanged; this is the specifically authorized post-block repair.

- S03 verification preflight: forbidden legacy dialog/path-based stb symbols are absent, required COM/memory-snapshot symbols are present, `git diff --check` passes, and the only out-of-scope dirty paths remain the recorded protected user baseline.

- S03 build invocation correction: repository `build/` is not configured (`CMakeCache.txt` absent). Recorded as an orchestration/path error; locating the existing baseline Release preset tree before compilation, with no code-repair round consumed.

- S03 resumed repair 1/2: the correct Release tree exposed one compile-only error in the filesystem-exception diagnostic (`std::string` passed to a literal-only no-throw helper). The fix is confined to that catch block; no import, transaction, or rendering semantics change.

- S03 repair 1 applied: the filesystem exception now assigns its detailed message under a local exception guard and falls back to a literal diagnostic if allocation itself fails. Ready to rebuild the same affected targets.

- S03 affected Release build: `xpbd_baker_app`, `xpbd_viewport_regression_tests`, and `xpbd_app_session_regression_tests` all compile and link successfully after repair 1/2. Only pre-existing third-party compiler warnings remain.

- S03 resumed repair 2/2: affected CTest is viewport PASS; AppSession has one fixture-routing failure because the missing-Sidecar Base was itself named with reserved `_s`. Rename that runtime Base fixture only, rebuild AppSession, and rerun both tests; no further repair is permitted if they remain red.

- S03 repair 2 applied: the missing-Sidecar runtime Base now has a valid non-sidecar stem, so the test reaches mandatory Specular Sidecar discovery as intended. Production code is unchanged by this repair.

- S03 affected verification is GREEN: repaired AppSession target rebuild PASS; `xpbd_viewport_regression` and `xpbd_app_session_regression` pass 2/2 in 1.34 s. Proceeding to final diff/scope audit and isolated S03 commit.

- S03 scope audit: no forbidden/Vulkan files changed and `git diff --check` passes. One pre-commit budget-preservation correction is required so the new outer path-load candidate continues counting the caller's resident output capacity exactly as the old transactional decoder did; checked arithmetic and output preservation will be retained.

- S03 budget audit correction applied: path loads add the caller's prior RGBA capacity to retained bytes with checked arithmetic before decoding the local candidate. A deterministic 101-byte peak regression now proves a 100-byte limit rejects without replacing the output.

- S03 final affected verification remains GREEN after scope-audit correction: all three Release targets PASS; viewport/AppSession CTest 2/2 PASS in 1.45 s, including the exact resident-output peak assertion.

- S03 final scope review: task-owned changes are confined to the texture snapshot/decode API, Suite/Iris importers, AppSession file dialogs and UTF-8 metadata, and the two affected regression sources plus planning files. No Vulkan backend, staging, queue, rendering protocol, physics, BRDF, or UI-layout file changed; the protected pre-existing dirty baseline remains untouched.

- Gate A core verification: complete `s00-gate-release` Release build PASS, including app/CLI/all regression targets and verification of 27 shaders; full CTest 5/5 PASS in 4.30 s. Hardware availability classification is the only remaining Gate A record before closing the gate.

- Gate A hardware inventory: Windows PnP reports a started RTX 4090 Laptop GPU, so this host is not `SKIPPED_NO_SUPPORTED_GPU`. A malformed scalar-array `nvidia-smi` command is recorded and will be retried directly; no repository state changed.

- Gate A hardware CLI correction: the guessed legacy Program Files NVSMI path is absent; retrying the verified System32 candidate. This remains a read-only probe issue, not a code or test failure.

- Gate A hardware classification: System32 `nvidia-smi` confirms an RTX 4090 Laptop GPU, driver 596.36, compute capability 8.9. The original Gate A requires Release/full CTest and source contracts; inventorying any repository-provided automated RT/RR hardware test before recording `PASSED`, and will not infer hardware validation from device presence alone.

- Gate A hardware-test inventory: no CTest/CMake/README-registered RT/RR hardware exercise exists; historical protected smoke artifacts are not current evidence. Gate A's mandatory correctness criteria are green, while current-commit hardware RR/RT is not yet claimed.

- Gate A COMPLETE: every mandatory item in the compact plan is covered by the full Release/full CTest run. An RTX 4090 Laptop GPU is present, but device detection and historical artifacts are not reported as current RR/RT hardware validation; this task changed no Vulkan/RT implementation and retains hardware smoke as unclaimed manual validation.

- S04 inventory started: persistent `ResolvedMaterialTable::texels` is built in import, resolver, and authoring paths and sampled through the table API; S00 already provides checked compact-memory estimates. A read-only wildcard error is recorded, and the remaining inventory will use exact paths.

- S04 data-flow inventory: remove three full-atlas decode loops, store Base alongside optional sidecars, and make `sample` decode one clamped texel by value. Production table equality can compare metadata/flags and Base/Normal/Specular pixels; GPU/RT consumers already read images and feature flags rather than the expanded texel vector.

- S04 budget specification audit: the plan defines required peak components and checked rejection, but deliberately provides no new hard process-memory threshold. Implementation will reuse the existing 1 GiB texture peak ceiling as the default and expose injectable checked preflight inputs for exact 2K/8K transaction tests.

- S04 implementation route fixed: preserve the material `sample` reference ABI via thread-local one-texel decode so forbidden `vulkan_backend.cpp` remains untouched. Add checked product preflight at the existing AppSession candidate boundary before Coverage/Composition, with injectable budget only for deterministic transaction tests.

- S04 compact material implementation: `ResolvedMaterialTable` now stores Base/optional sidecar images, validates aligned active resources, and decodes one clamped texel through unchanged `decodeLabPbrTexel`; all three full-atlas construction loops are removed. The existing sample ABI is preserved and no Vulkan file changed.

- S04 reference migration: legacy tests now sample the compact material API instead of indexing removed storage, including clamp, RGB-specular no-emission, authored override, and AppSession semantics. No assertion was removed or weakened; expected texel values still come from the unchanged decoder.

- S04 checked budget core: added a default 1 GiB peak ceiling, explicit zero resolved-texel stride, and an output-preserving `preflightLabPbrMemory`. Authored material construction now rejects checked resident+candidate image peaks before copying and converts allocation failures into transactional budget diagnostics.

- S04 strict importer convergence: strict Suite material construction now delegates to the same budgeted compact authored-image builder, retaining strict metadata and sidecar validation while avoiding a second copy/decode implementation.

- S04 structural gates added: compact 1K/2K/4K arithmetic records a zero resolved stride and exact legacy delta; 8K performs budget arithmetic/fast rejection without image allocation; one runtime 2K three-image compact build is exercised; every texel of the small synthetic atlas is compared against the unchanged decoder reference.

- S04 product preflight core inserted at the AppSession candidate boundary, before mesh Coverage/Composition allocation. It checks atlas arithmetic and compact resident/candidate images, decoder scratch, full Composition, and Coverage against the Session's default/injectable peak ceiling, then forwards the same ceiling to material copying.

- S04 call-site wiring paused after a no-op multi-hunk context mismatch at the Iris call; exact call endings are being re-read and updated in smaller groups. No source call site was partially edited.

- S04 product wiring complete: all ten model/Base/Suite/override/specular/Iris candidate paths pass the Session peak ceiling through the same preflight and compact material allocator. The public budget field exists solely for deterministic product-transaction tests and has no UI exposure.

- S04 transaction/cache accounting: AppSession regression now injects a one-byte product budget and requires complete Session/Generation/material rollback. The existing cache exposes saturating resident-byte accounting over its actual deep image and encoded-snapshot ownership so preflight can include it before S07 replaces the cache design.

- S04 resident accounting helper added: current model/resolved/composition/Iris/Coverage/snapshot allocations and strict-suite encoded snapshot bytes can be accumulated separately from cache ownership and fed into the existing estimator fields. Call-site context wiring is next.

- S04 full context wiring complete: every product material transaction includes current Session residency and cache bytes; strict Suite additionally includes its encoded snapshots. Repository search confirms the persistent `ResolvedMaterialTable::texels` member and all indexing/build loops are gone; `git diff --check` passes.

- S04 compile checkpoint: affected Release viewport and AppSession regression targets compile and link with the compact table, preflight API, cache accounting, and all product call sites. No repair round has been consumed.

- S04 related verification checkpoint: `xpbd_viewport_regression` and `xpbd_app_session_regression` pass 2/2 in 1.53 s, including the runtime 2K three-image compact stress, 8K arithmetic-only rejection, and exhaustive small-fixture texel comparison. Final affected build/test verification remains pending after the pre-decode budget audit.

- Authorized resume recovery: session catch-up reported seven unsynchronized status/tool messages; `HEAD` remains `a9b955d` on `xpbdRT`, task-owned S04 changes remain uncommitted, and the protected user baseline remains present and separable. The first combined planning-file read command failed because a PowerShell `Join-Path` expression accidentally supplied an object array as `ChildPath`; this was a read-only orchestration error, and the retry uses an explicit path array.

- Recovery planning read: `task_plan.md` and `progress.md` were re-read completely and the staged diff is empty. The 59,188-byte `findings.md` exceeded one tool-output projection despite a bounded line split, so complete recovery is continuing with smaller exact line ranges before any design edit.

- Recovery findings read: exact lines 1–160 have been re-read without truncation, confirming the protected baseline, S03/Gate A evidence, S04 design decisions, failure history, and zero consumed S04 repair rounds. Lines 161–end and the task-owned diff remain to be re-read before editing.

- Recovery findings complete: overlapping exact windows through the end of `findings.md` have now been re-read. All three planning files are fully recovered; S04 remains the first incomplete phase, its repair count is 0/2, and the next recovery step is a bounded per-file review of the uncommitted task diff.

- Recovery diff review: compact material/memory/import/authoring headers and implementations plus AppSession/product transaction changes were re-read completely. The review confirms the remaining audit items are checked Coverage-capacity multiplication and moving budget enforcement ahead of Base/Specular/Iris/Suite decode allocations; the viewport regression diff is the last unread task-owned code diff.

- Recovery diff complete: the viewport regression diff and protected tracked-manifest diff were re-read; staged diff remains empty. The original compact plan reconfirms the S04 gate and isolated commit message `perf: remove persistent resolved material texel expansion`. A broad API/call inventory was partially truncated, so exact texture/Suite/Iris implementations will be inspected in bounded ranges before patching.

- S04 pre-decode design audit complete: direct Base/Specular can reuse `TextureDecodeLimits` with current Session+cache residency; Iris will accept the same optional limits and preflight both snapshot/decode and its still-required encoded-byte copy; strict Suite will gain explicit import limits, cumulative snapshot caps, header-only inspection, and one checked final-model estimate before any pixel decode. Existing callers retain defaults.

- S04 exact call audit: Base and direct Specular decode into empty locals before later compact material/Coverage/Composition candidates; Iris likewise imports into a local; strict Suite still deep-copies decoded images into result/cache. The implementation will keep default public APIs source-compatible while AppSession supplies budget, resident, and cache context at these four product entry points.

- S04 pre-decode implementation part 1: texture decode now exposes transactional header-only inspection through the same stb memory/header and checked-budget path; file snapshots subtract retained state from the effective peak before allocating bytes. Suite/Iris APIs gained optional source-compatible budget contexts; implementations and product call wiring are next.

- S04 pre-decode implementation part 2: strict Suite now caps each cumulative encoded snapshot against resident+cache state, validates all image headers/channels/domains without allocating pixels, runs a conservative compact-result/cache/Coverage/Composition estimate, and supplies exact retained context to each subsequent decoder. Iris and AppSession wiring remain.

- S04 pre-decode implementation part 3: Iris now checks old output residency, snapshot capacity, decoder peak, and the later encoded-byte copy before each allocation; AppSession Base/Specular/Iris/Suite calls now supply the Session ceiling and residency context, and Coverage capacity multiplication is checked. A pre-compile access audit found file-local helpers must receive private cache bytes from member call sites rather than access `labpbr_import_cache_` directly; correcting that next.

- S04 pre-compile audit: private cache bytes are now passed from member call sites; all new signatures and calls are source-compatible and `git diff --check` passes. Remaining edits are strict-Suite accounting for AppSession's temporary Iris copy, moving properties parsing after preflight, and regression assertions for header/output/budget preservation before the first build.

- S04 pre-decode tests added: header-only inspection success/failure preserves output; strict Suite snapshots and validates headers then fails a deliberately tight final-model budget before pixel decode without touching cache; Iris allows decoder peak but rejects its later encoded-byte copy before allocation while preserving the active asset. AppSession's transaction oracle accepts either pre-decode or final-model `budget` diagnostics.

- S04 final affected Release build checkpoint: `xpbd_baker_app`, `xpbd_viewport_regression_tests`, and `xpbd_app_session_regression_tests` all compile and link successfully with the pre-decode budget paths. Only pre-existing third-party warnings were emitted; S04 repair rounds remain 0/2.

- S04 affected verification is GREEN: viewport/AppSession CTest passes 2/2 in 1.35 s, including actual 2K compact allocation, 8K arithmetic rejection, exhaustive small-texel equality, header-only Suite budget rejection, Iris copy rejection, Unicode paths, and Session rollback. `git diff --check` passes; no persistent material `.texels` access or task file >=1 MiB exists.

- S04 COMPLETE scope audit: the task diff is limited to compact material/texture/import/authoring/AppSession code, the two related regression sources, and the three planning files. No Vulkan/backend/shader/physics/UI file changed, no large image was added, staged state is empty, and the protected user baseline is unchanged and excluded from the pending phase commit.

## Resume protocol

Read all three planning files, then `git status --short`, `git log -10 --oneline`, `git diff`, `git diff --cached`, and the newest recorded test result. Resume the first non-complete phase; never infer repository state from chat history.
