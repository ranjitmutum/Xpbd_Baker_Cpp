# Phase 3 Gate: Vulkan RT Pipeline and SBT

Date: 2026-07-29
Branch: `RT`

Phase 3 is complete. This document freezes the implementation and validation
evidence so completed process detail can be removed from active planning files.

## Implemented Contract

- A dedicated `VulkanRtPipeline` owns the Vulkan RT procedures, descriptor
  layout/pool/set, pipeline layout, four shader groups, SBT, and teardown.
- Shader groups are RayGen, primary Miss, shadow Miss, and a triangle hit group
  containing Closest Hit plus alpha-aware Any Hit.
- Explicit `XPBD_RT_DEBUG` modes select `instance`, `primitive`, `cube`, `face`,
  `material`, or `normal`. Default `off` preserves the compute ray-query path.
- RT dispatch shares `VulkanPathTracer` color/depth targets and composite.
  Unavailable or rejected RT dispatches emit a one-shot diagnostic and fall
  back to compute without failing application startup.
- Static packed primitives carry `{cube, face, material, sourcePrimitive}`;
  dynamic primitives receive deterministic sentinel metadata. Stable TLAS
  instance metadata resolves the global packed primitive in Closest/Any Hit.
- BLAS triangles are non-opaque so alpha Any Hit can execute. Compute ray-query
  continues to request opaque traversal explicitly and keeps its manual alpha
  loop.
- Vulkan-independent helpers validate SBT handle/base alignment, stride,
  allocation size, device-address overflow, and every shader-indexed
  vertex/primitive/instance buffer range. The Vulkan runtime uses the same
  helpers as focused regressions.

## Principal Files

- `include/xpbd/gfx/vulkan_rt_pipeline.hpp`
- `src/gfx/vulkan_rt_pipeline.cpp`
- `include/xpbd/gfx/vulkan_path_tracer.hpp`
- `src/gfx/vulkan_path_tracer.cpp`
- `include/xpbd/gfx/vulkan_rt_scene.hpp`
- `src/gfx/vulkan_rt_scene.cpp`
- `include/xpbd/gfx/ray_tracing.hpp`
- `src/gfx/ray_tracing.cpp`
- `src/gfx/spirv/rt_debug.rgen`
- `src/gfx/spirv/rt_debug.rmiss`
- `src/gfx/spirv/rt_shadow.rmiss`
- `src/gfx/spirv/rt_debug.rchit`
- `src/gfx/spirv/rt_debug.rahit`

## Focused Gates

The following commands passed:

```powershell
.\tools\build_spirv.ps1 -Check
cmake --build out\build\rt-migration --config Debug --target xpbd_viewport_regression_tests
.\out\build\rt-migration\Debug\xpbd_viewport_regression_tests.exe
cmake --build out\build\rt-migration --config Debug --target xpbd_baker_app
git diff --check
```

The direct regression covers stable debug names, a valid observed-style SBT
layout, non-power-of-two alignment rejection, maximum-stride rejection,
undersized allocation rejection, device-address overflow rejection, complete
dispatch buffers, and undersized normal/index/primitive-identity/
instance-identity buffers.

No Release build or CTest was run, per user instruction.

## Hardware Gates

GPU execution created one pipeline/SBT per frame slot with:

- 4 shader groups
- 32-byte group handle
- 32-byte handle alignment
- 64-byte base alignment
- 32-byte group stride
- 223-byte worst-case-safe allocation after the shared layout helper

The deterministic 36-primitive/3-instance fixture dispatched all six debug
modes. Each bounded run produced empty stderr, no VUID/error signature, and a
graceful device-idle/fence shutdown. `face` and `normal` captures are retained
as ignored diagnostics under:

`<repository>\.tmp\phase3-visual-20260729-135244-534`

Visual QA confirmed stable discrete face colors and axis-oriented normal colors
including a flat +Y green ground; neither result was black, constant, or
corrupted.

The user's read-only real model also passed:

`<user-test-assets>\huilin\models\main.json`

Its normal debug dispatch contained 28,968 primitives and 476 instances with
the same 32-byte stride/223-byte SBT, empty stderr, no error signature, and
graceful shutdown. The source directory was not modified. Its base/PBR/normal
texture trio was validated separately by the completed Phase 1 gate using an
ignored normalized copy because `textures/pbr/red.png` does not follow the
strict same-stem `_s.png` convention.

The machine does not have the Khronos validation layer installed. The
opt-in validation request reported that fact and safely continued with Vulkan
diagnostics; hardware runs still had empty stderr and no validation/VUID
signatures.
