# Progress

| Date | Phase | Status | Modified files | Tests / checks | Commit | Next |
|---|---|---|---|---|---|---|
| 2026-08-02 | Initialization | COMPLETE | `docs/plans/large_uv_labpbr/{task_plan,progress,findings}.md` | Plan read in full (13,310 bytes); branch/status/log inspected | pending S00 commit | Inspect README/CMake/tests/code, reconcile upstream, run untouched baseline |
| 2026-08-02 | S00 | COMPLETE | `CMakeLists.txt`; `include/xpbd/gfx/labpbr_memory.hpp`; `src/test_support/labpbr_synthetic_fixture.{hpp,cpp}`; viewport regression; planning files | Untouched Release baseline + CTest 5/5 PASS; affected Release targets PASS; viewport/AppSession CTest 2/2 PASS; `git diff --check` PASS | `4bdc38a` | S01: strict declarations, double Face UV, bounds, and pure Domain |
| 2026-08-02 | S01 | COMPLETE | `bedrock_model_data.hpp`, `model_loader.cpp`, `uv_domain.{hpp,cpp}`, `viewport_mesh.cpp`, AppSession declaration callers, CMake, synthetic fixture, viewport regression, planning files | Release App + viewport + AppSession targets PASS; related CTest 2/2 PASS (1.29 s); `git diff --check` PASS | `7cafe52` | S02: apply Domain to every material consumer and clamp model atlases |
| 2026-08-02 | S02 | COMPLETE | `viewport_mesh`, texture/material sampling, Coverage/Alpha, AppSession transactions, model Vulkan/PT samplers, viewport/AppSession regressions, planning files | One bounded repair fixed Alpha typing; final Release app + viewport + AppSession targets PASS; affected CTest 2/2 PASS (1.30 s); `git diff --check` PASS | `80970b6` | S03: Unicode/long-path snapshot-and-memory imports |
| 2026-08-02 | S03 | BLOCKED | planning files only after exact S03 rollback | AppSession CTest 1/1 PASS after repair 2, but affected viewport CTest then failed 1 assertion (`corrupt _s is rejected`); repair budget exhausted | none | Restore six S03 production/test files to `80970b6`; stop before Gate A |
| 2026-08-02 | S03 resumed | COMPLETE | texture snapshot/decode, strict Suite/Iris import, COM dialogs, AppSession/viewport regressions, planning files | Affected Release app + viewport + AppSession targets PASS; related CTest 2/2 PASS (1.45 s); source contracts and `git diff --check` PASS | this isolated phase commit | Gate A: full Release build and full CTest |

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

## Resume protocol

Read all three planning files, then `git status --short`, `git log -10 --oneline`, `git diff`, `git diff --cached`, and the newest recorded test result. Resume the first non-complete phase; never infer repository state from chat history.
