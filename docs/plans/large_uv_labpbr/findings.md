# Findings

## Repository facts

- Initial repository: `D:/XPBD_Baker_CPP`; branch `xpbdRT`; initial HEAD `3bd2109d6dfaf2ac257b27a7fbe26b0d2a53ebc0`; fetched upstream HEAD is `0a571c9cf195e88eea3c9df62eb7e2f046c52c44` (three commits ahead).
- The supplied plan was read completely from `C:/Users/shiromizu/Desktop/xpbdRT_大UV错位与LabPBR内存优化_无人值守精简版.md` (13,310 bytes).
- Session catch-up produced no unsynchronized planning report.
- The project is C++23 and primarily Windows/MSVC. The documented full gate uses configure preset `s00-gate`, Release build preset `s00-gate-release`, and test preset `s00-gate-release`.
- Existing regression executables are `xpbd_viewport_regression_tests`, `xpbd_nuklear_focus_regression_tests`, and `xpbd_app_session_regression_tests`; the Release gate also builds the app, CLI, and `xpbd_check_spirv`.
- Both root READMEs, every CMake/preset/vcpkg file, and all three existing test sources (5,753 lines total) have now been read completely.
- The preset pins `VCPKG_ROOT=C:/vcpkg`, shared dependencies under repository `vcpkg_installed`, and the Visual Studio 2022 x64 generator. Streamline is optional and never downloaded implicitly.
- `xpbd_viewport_regression_tests` already links the viewport, texture, LabPBR import/material/authoring/export, RT records, ray tracing, world, and preview sources. `xpbd_app_session_regression_tests` is the product-entry transaction test target. New S00/S01 coverage can therefore live in the existing test binary without creating heavyweight application launches.
- Relevant production file sizes and the complete loader/data model were inspected before baseline changes.

## Protected pre-existing working-tree baseline

- Tracked user change: `docs/long_term_scene_plan_manifest.json`.
- Untracked user/history evidence: `artifacts/**`, `docs/perf/**`, `docs/pre_cleanup_DC00_baseline.md`, `docs/pre_cleanup_handoff.md`, `docs/pre_cleanup_manifest.json`, `docs/pre_performance_handoff.md`, and `docs/pre_performance_manifest.json`.
- These paths are outside the planned implementation surface. They must not be edited, deleted, reset, cleaned, or staged. Phase-clean means the Git status equals this protected baseline after the phase commit.

## Execution decisions

- Stage files by explicit path only. Never use broad `git add` in this pre-dirty worktree.
- The fetched upstream range changes only four historical `docs` files and deletes `docs/long_term_scene_plan_manifest.json`, which has a protected local modification. Production source/CMake/test content has no net difference. Do not fast-forward, stash, reset, or merge this range; continue from the checked-out local HEAD so the user change is untouched.
- Root-level legacy planning files and `.planning/` exist but are outside this task. The only active task memory is `docs/plans/large_uv_labpbr/`.
- Synthetic PNG/model resources are generated in test temporary directories; no 2K/4K assets are committed.
- Relevant production surfaces are `model_loader`, `viewport_mesh`, `texture_image`, `labpbr_{material,import,authoring,export}`, AppSession material transactions/dialogs, RT material records/shaders, and the model-atlas sampler creation in `vulkan_backend.cpp` only.
- `LabPbrSourceFile::original_bytes` already uses `shared_ptr<const vector<uint8_t>>`, so S06 must extend and reduce ownership from the actual current model rather than assume every compressed snapshot is a direct vector.
- Current Win32 import dialogs in AppSession use `GetOpenFileNameW` with fixed `MAX_PATH`; `IFileOpenDialog` is absent. The static model-atlas Vulkan sampler is nearest + Repeat, while other Repeat samplers serve unrelated resources and must remain untouched.

## Existing test coverage discovered

- `src/gfx/viewport_regression_tests_main.cpp` (3,933 lines) has been read completely.
- `src/app/app_session_regression_tests_main.cpp` (1,310 lines) has been read completely. External suite execution is optional via a positional path; the normal CTest path is fully synthetic and temporary-directory based.
- `src/app/nuklear_focus_regression_tests_main.cpp` (510 lines) has been read completely. It is outside this task's implementation scope and confirms the UI test drives the real product helper across English/Chinese and DPI scales; no UI layout changes are needed.
- `viewport_regression_tests_main.cpp` already has source-contract assertions for explicit LOD 0 and nearest filtering across Raster/RT/PT material samplers, plus a 28-binding RT descriptor ABI contract. S02 must extend these contracts for model-atlas clamp without weakening current assertions.
- Texture decode tests already verify checked RGBA arithmetic, 16,384 side/134,217,728-pixel limits, pre-decode header rejection, decoded/peak budgets, overflow rejection, and preservation of the caller's output object on failure.
- Existing LabPBR discovery tests directly assert `ResolvedMaterialTable::texels`, confirming the persistent decoded table is current behavior and that S04 must replace those expectations with on-demand reference checks.
- Strict suite import currently retains exact Base/Normal/Specular/properties compressed snapshots and caches successive checksum versions as separate entries. Tests cover missing/corrupt/mismatched sidecars, format confirmation, optional normals, source-change detection, and failure without cache damage; S06/S07 must deliberately update ownership and cache assertions.
- Coverage currently stores per-group flat texel-index vectors (`group_texels`), rasterizes from float mesh UVs, deduplicates overlaps, and Composition immediately owns a full `TextureImage`. These tests provide the reference set/conflict oracle for S05 run encoding.
- Iris normal currently owns `original_file_bytes` directly and failure-preservation is already asserted. Export round-trips those exact bytes; S06 must preserve that behavior while changing ownership to shared immutable storage.
- RT tests already model material generation as one of six independent scene-generation inputs and assert history-reset classification. S02 should reuse that generation path; it must not alter RR/SR/FG protocols or descriptor topology.
- Existing canonical cube tests cover Box UV mirror/tangent handedness, Per-Face north rotation, and the Bedrock Up-face opposite-corner rule. They currently assert normalized float UVs divided by one 16×16 declaration, so S01/S02 should add a double raw-UV/domain layer while preserving these GPU-facing results.
- Static alpha classification currently samples the texture from normalized mesh floats and assigns Opaque/Cutout/Blend before draw-plan ordering. This is a concrete S02 consumer that must take the resolved domain and clamp semantics transactionally.
- Hardware eligibility has deterministic CPU tests for NVIDIA vendor/features/generation. The suite can report unsupported hardware without treating it as a correctness failure; shader/source contracts remain the non-hardware Gate A evidence.
- AppSession tests define a full material transaction snapshot: model texture, resolved material, coverage, overrides/draft, composition, Iris normal, suite source, texture path, material generation, source-change flag, and cache-hit flag. New failure tests should reuse/extend this equality oracle so no partial state replacement is missed.
- The real AppSession import test already proves suite/relink/confirmation/direct-specular failures preserve all committed state, successful material changes advance generation exactly once, and same-size Base reload preserves independently selected PBR slots. Unicode/domain/budget failures should be added through these same product entry points.
- Existing optional external-suite validation assumes long-lived compressed bytes for all images; because the new task has no real model and S06 intentionally drops some resident snapshots, that optional expectation must be replaced by synthetic/shared-ownership invariants rather than retained as an architectural constraint.

## Errors and blockers

- Planning update attempt 1 failed because one multi-hunk patch expected `## Execution decisions` after a later section. No partial edit occurred; resolved by re-reading the file and applying smaller exact-context hunks.
- One PowerShell `rg` sampler inventory used Unix-style wildcard path arguments (`src/gfx/*.cpp`), which Windows rejected with error 123. The two explicitly named Vulkan files and recursive shader search still returned useful results; subsequent inventories will use `-g` filters or directory roots.
- The first baseline-record patch used mojibake copied from PowerShell's default decoding and did not match the UTF-8 em dash in `progress.md`. No partial edit occurred; re-read with explicit UTF-8 and patched the exact text.
- The first explicit S00 staging command reported that `docs/plans/` is ignored, so none of the three required planning files were staged. Production/test paths staged correctly; the planning paths are intentionally force-added by exact filename, without broad staging or touching other ignored evidence.

## Remaining risks

- No real user model is available; correctness evidence must come from runtime-generated fixtures.
- NVIDIA RT/RR hardware availability has not yet been probed.
- The task's untouched baseline is green at local HEAD `3bd2109`: `cmake --preset s00-gate`, complete `s00-gate-release` build (app, CLI, all regression targets, and 27-shader SPIR-V verification), and CTest 5/5 passed in 4.27 seconds on 2026-08-02.

## Current implementation facts

- `GeometryDescription` has one combined `has_texture_size` flag and default 16×16 integers. The loader truncates numeric declarations to `int`, clamps them to at least 1, and sets the shared flag when either axis exists. A one-axis declaration therefore silently treats the other default as declared, while fractional/negative/too-large values are not strictly rejected.
- Cube/face authored UV values are already parsed as finite doubles. Per-Face supports negative `uv_size`, 90-degree rotations, and four-number corner arrays; Box UV supports mirror. S01 can centralize interpretation without changing loader-side LabPBR/PBR semantics.
- `StaticModelVertex` currently stores only normalized float `u/v`; `StaticModelFace` stores no raw UV bounds or domain identity. `viewport_mesh.cpp` also begins UV work with a private float `FaceTexelUV`, confirming precision and domain information are discarded before consumer stages.
- Static and dynamic meshes do call the same private `resolveCubeFaceUV`, but it converts authored doubles/default cube extents to float immediately. Both then choose one divisor from the combined declaration flag; no UV-bounds scan or recoverable atlas domain exists.
- Dynamic preview subdivides each textured face into 8×8 cells and classifies them through generic `TextureImage::sample`; the static mesh stores normalized UVs and later alpha classification samples independently. These are the two consumer paths that S01/S02 must keep aligned.
- `viewport_mesh.cpp` has been read completely. All rest/animation/baked paths funnel through the same builder, so a builder-owned resolved domain can cover static and dynamic generation without touching XPBD/Bullet or animation semantics.
- `TextureImage` exposes only the generic `sample(float,float)` API today. S02 needs an explicit model-atlas clamp sampler rather than changing this global API's established Repeat behavior.
- Generic `TextureImage::sample` explicitly wraps with fractional Repeat before nearest texel selection. File loading passes `path.string()` to `stbi_info/stbi_load`, so Windows Unicode/long-path failures occur before the otherwise-transactional candidate swap; S03 should route filesystem bytes through the existing memory decoder.
- `ResolvedMaterialTable` currently owns `vector<ResolvedMaterialTexel> texels` plus by-value Normal/Specular `TextureImage`s. Its removal and image sharing are distinct later phases as the plan requires.
- `resolveLabPbrMaterial` loads compatible sibling maps and then decodes every pixel into the persistent texel vector. `valid()`, `sample()`, and resource equality all depend on/deep-compare that expansion, and `sample()` repeats UVs. S04 must replace all four behaviors together while retaining `decodeLabPbrTexel()` unchanged.
- Legacy best-effort sidecar discovery uses filesystem-aware names but still reaches the path-string stb loader; the strict suite importer must become the canonical Unicode-safe snapshot/decode path without changing LabPBR 1.3 channel math.
- Strict suite import already snapshots with `filesystem::path` + bounded `ifstream`, validates size/time before and after the read, hashes the shared byte vector, and decodes from memory. Its material builder nevertheless copies decoded images and performs the same full texel expansion; errors use narrow `.string()` only for diagnostics/metadata.
- `LabPbrSuiteImportCache` is currently an unbounded ordered map whose `find` and `store` return/store `ImportedLabPbrSuite` by value, causing deep image/material copies even though encoded snapshots are shared.
- Cache lookup happens only after snapshotting and decoding all candidate images, so a hit avoids material construction but not decoder allocations. S07 needs shared cache entries, LRU byte accounting, and an earlier safe lookup point based on snapshot identities.
- Strict discovery/import still converts filenames/stems/extensions to narrow `.string()` for suffix logic and error text. Unicode-safe exact path construction should use native `filesystem::path` components; ASCII folding may remain only for ASCII extensions/suffixes.
- Authoring currently represents Coverage as `map<string, vector<uint32_t>>`, Composition as an always-owned `TextureImage`, and Iris normal as a second owned encoded vector plus a by-value decoded image. These match the planned S05/S06 conversion targets.
- Coverage construction currently uses a per-group `std::set<uint32_t>` and derives pixel coordinates by multiplying already-normalized float mesh UVs by atlas width/height. It clamps triangle bounds to the atlas, which can mask true out-of-domain UVs rather than reject the candidate.
- Composition allocates/fills a complete default Specular before checking whether overrides exist and deep-copies imported pixels. `buildAuthoredResolvedMaterial` copies the source/images and re-decodes every texel. These are the exact S04/S05 peak and residency sources.
- Iris import already decodes from an `ifstream` byte buffer and commits transactionally, but the read is unbounded until EOF and the byte vector is independently owned. S03 adds the same bounded snapshot discipline; S06 shares the strict suite snapshot and decoded image.
- LabPBR export stages and validates all outputs, backs up existing targets, installs, and rolls back on exception. S03's Unicode-safe global file decode also protects staged validation; later ownership changes must preserve this transaction protocol. Export stem manipulation currently narrows paths and should be made native-path-safe where touched.
- `static_model_draw_plan.hpp` deliberately computes wrapped alpha texel spans; a span of at least one normalized cycle scans the whole atlas and smaller cross-cycle spans split into two. This is the S02 behavior to replace with clamped, resolved-domain spans.
- RT primitive records copy the static mesh's normalized float UVs verbatim. Correcting the mesh/domain once will align Raster and RT records without modifying BLAS/TLAS topology or RR/FG ABI.
- `rt_scene_records.cpp` was read completely. Invalid record construction clears every candidate vector before return, so S02 needs no RT scene ownership changes; it only supplies already-correct UV/material metadata.
- AppSession currently owns Base, resolved material, Coverage, Composition, Iris asset, suite source, source material, and cache as separate value fields. Material generation is the single texture/material generation exposed to renderers, and its existing transaction helper is the correct commit boundary.
- `native_dialog.cpp` only manages prepare/finish lifecycle hooks; Win32 picker implementations and public declarations are in AppSession. S03 can replace picker internals without changing overall UI layout or dialog lifetime hooks.
- `buildSessionLabPbrMaterial` builds a candidate mesh, Coverage, Composition, and resolved material in locals and moves them only after every step succeeds, preserving the session transaction boundary. It currently does all Coverage/Composition work even with no overrides; S05 can make this helper lazy while retaining its commit order.
- `loadModel` currently commits geometry/model UI state before calling `refreshLabPbrAuthoring`; a later UV Domain/Coverage failure only appends a warning. S01/S02 must validate a candidate model against the active texture/material before replacing the current Session.
- Strict suite import builds candidate material/coverage/composition first, then commits all fields and generation. It nevertheless deep-copies suite Normal bytes into Iris and copies decoded images into multiple fields, directly motivating S06.
- `loadTexture` also constructs texture, resolved-material, Coverage, Composition, and authoring candidates before committing them, so decode/material/coverage errors already preserve its prior Session and generation. Domain validation should be inserted into that existing candidate boundary.
- `refreshLabPbrAuthoring` commits only after its local candidate build succeeds, but `loadModel` invokes it after replacing geometry and related UI/model state. A Domain/Coverage failure during model import therefore cannot currently roll back the model replacement and must be moved ahead of the Session commit in S01/S02.
- LabPBR draft initialization reads the first flat Coverage texel for each group. S05's compact run encoding therefore needs a deterministic first-texel accessor in addition to membership/iteration behavior.
- The newest available CTest log predates this task (`2026-08-01 21:53`) and reports the prior AppSession suite passing; it is historical evidence only, not the required untouched S00 baseline.
- Direct specular/normal import and removal also build all Coverage/Composition/material candidates before mutation. S03/S05/S06 can preserve these established commit points while changing loaders and ownership.
- The Win32 folder picker already uses `IFileDialog`, but file-open and save pickers still use `OPENFILENAMEW` with `MAX_PATH` buffers. S03 should replace the relevant file picker(s) with filesystem-returning `IFileOpenDialog`/`IFileSaveDialog` behavior while preserving `NativeDialogScope` and avoiding UI layout changes.
- The one model-atlas sampler in `vulkan_backend.cpp` is nearest, fixed nearest mip, and Repeat on U/V/W at the static albedo/normal/specular descriptor set. This is the only allowed backend address-mode edit. Other sky, atmosphere, font, and world samplers remain out of scope.
- Shader consumers use the same interpolated mesh UV for Base, Normal, and Specular with explicit `textureLod(..., 0.0)`. Correct atlas clamping at CPU reference sampling plus the model sampler therefore aligns all three GPU material slots without changing shader math or fixed LOD 0.
- The path tracer creates a generic clamp sampler first, then mutates the same create-info back to Repeat for its albedo/normal/specular material samplers. S02 must update those three material samplers as well as the forward/RT static sampler; the unrelated generic sampler stays unchanged.
- Existing test targets each compile their own graphics source list rather than link a shared graphics library. A test-only synthetic-fixture implementation can be compiled into both viewport and AppSession regression executables without changing application rendering behavior.
- Existing AppSession tests duplicate PNG encode/write helpers and use only tiny fixtures. The shared S00 generator should own runtime PNG/model-suite creation and later be reused by both the domain/material consumer tests and the real AppSession Unicode import entry-point test.
- `ResolvedMaterialTexel` is a large float-rich value and the current table is one element per atlas pixel. The S00 estimate will keep its bytes-per-pixel input explicit so S04 can prove the final estimate uses zero legacy resolved-texel bytes after the type's persistent vector is removed.
- S00 measured `sizeof(ResolvedMaterialTexel) == 108` on the MSVC x64 Release baseline. The explicit legacy estimates were: 1K resident 125,829,120 / peak 171,966,464 bytes; 2K resident 503,316,480 / peak 687,865,856 bytes; 4K resident 2,013,265,920 / peak 2,751,463,424 bytes.
- The checked estimator preserves its output on multiplication/addition overflow and keeps Coverage and Cache bytes separately auditable. The synthetic generator writes all PNGs only beneath a runtime temporary directory and includes large-eye, high-resolution protection, true out-of-bounds, Box, mirror, per-face rotation, negative size, Up/Down, 16,384-boundary, and non-finite cases.
