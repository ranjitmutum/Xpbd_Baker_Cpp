# RT Phase 2 Gate Evidence

Date: 2026-07-29
Authority: corrected 16-phase unattended implementation plan
Build policy: focused Debug targets and direct executables only; no Release
build or CTest was run.

## Completed Contract

- Canonical Cube triangles preserve cube, face, primitive, material, bone,
  group, UV, flat-normal, winding, pivot, rotation, inflate, mirror, hierarchy,
  overlap, and degenerate-face identity.
- Rigid bones use immutable local-space BLAS ranges and stable TLAS instance
  custom indices. Model and preview geometry share one TLAS.
- Current/previous rigid transforms and dynamic vertices are retained.
- Static, rigid-local, and dynamic-refit policies share scratch and report
  build reasons/counters.
- Normals use inverse-transpose under non-uniform or mirrored transforms with a
  finite deterministic fallback for singular transforms.
- The CPU identity resolver mirrors the shader mapping
  `instanceMetadata[instanceCustomIndex].x + localPrimitiveIndex`.
- Nearest-valid ordering covers cutout rejection, fractional Blend acceptance,
  finite bounds, two-sided winding, and a roughly 0.057-degree grazing ray.

## Focused Verification

Commands:

```powershell
cmake --build out\build\rt-migration --config Debug --target xpbd_viewport_regression_tests --parallel 1
out\build\rt-migration\Debug\xpbd_viewport_regression_tests.exe
cmake --build out\build\rt-migration --config Debug --target xpbd_baker_app --parallel 1
.\tools\build_spirv.ps1 -Check
git diff --check
```

Results:

- Focused viewport regression build/direct run: pass.
- Focused Debug application build: pass; pre-existing third-party/UI warnings
  only.
- Embedded SPIR-V check: pass.
- `git diff --check`: pass; line-ending conversion warnings only.

## Hardware Evidence

Rigid numeric two-bone smoke:

- Input model: ignored `.tmp/phase3-multiblas.geo.json`.
- Input animation: ignored `.tmp/phase2-rigid.animation.json`.
- Bone overlays disabled only for diagnostic isolation with
  `XPBD_SHOW_BONES=0`.
- Both frame slots initially built exactly two BLAS.
- Next 62 captured events:
  `reason=instance transforms`, `blas_full_delta=0`,
  `blas_refit_delta=0`, `blas=2`, `instances=2`, `tlas=1`.
- Graceful close, no forced termination, empty stderr.
- Capture:
  `.tmp/phase2-rigid-tlas-only-20260729-132130-226`.

Dynamic Ocean smoke:

- `XPBD_PREVIEW_SCENE=9`, `XPBD_PREVIEW_DYNAMIC=1`.
- Both frame slots initially performed full builds for two BLAS.
- Next 92 captured events:
  `reason=stable refit`, `blas_full_delta=0`,
  `blas_refit_delta=1`, `blas=2`, `instances=2`, `tlas=1`.
- Graceful close, no forced termination, empty stderr.
- Capture:
  `.tmp/phase2-ocean-refit-20260729-132224-250`.

The supplied `huilin` model remains valid for real large-model, PBR, and normal
validation. Its first slashblade clip expresses its dynamic motion mainly in
MoLang, which the numeric source-preview evaluator preserves but does not
execute, so the deterministic numeric fixture was used for the live-AS gate.

## Boundary

Phase 2 validates the AS records, identity contract, nearest-valid policy,
normals, and hardware build/refit behavior. Vulkan RT Pipeline/SBT creation and
GPU instance/cube/face/material/normal debug views are Phase 3 work.
