# Phase 4 Gate: Minimal Path Tracing and Accumulation

Date: 2026-07-29
Branch: `RT`

Phase 4 is complete. This document freezes the implementation and validation
evidence so completed process detail can be removed from active planning files.

## Implemented Contract

- Genuine Vulkan RT RayGen performs deterministic jittered primary rays and a
  sequential 2–4-bounce path loop without increasing pipeline recursion depth.
- Closest Hit returns world-space interpolated normal and linear Lambert
  albedo. Base-texture RGB uses the same explicit IEC sRGB-to-linear conversion
  as Raster/compute because the atlas is uploaded as UNORM.
- RayGen uses cosine-weighted hemisphere sampling. At every accepted surface it
  evaluates the existing directional preview light through an alpha-aware
  shadow ray, then continues with Lambert throughput.
- A temporary analytic clear-sky environment supplies miss radiance when its
  strength is positive. Its default strength is zero, preserving the corrected
  `Sky Rendering = Off` product boundary.
- Each of the two in-flight `VulkanPathTracer` instances owns an independent
  history key, sample counter, and RGBA16F in-place average. No shared history
  image or cross-slot write exists.
- History compatibility includes exact RT-scene content/transform hash,
  matrices, viewport, model/material/geometry generations, lighting/clear
  state, material/debug features, seed, bounce cap, environment strength, and
  explicit reset generation. SPP and Maximum Samples remain statistically
  compatible and do not reset the average.
- Target resize, explicit reset/key change, debug output, or legacy compute
  fallback invalidates path history. This prevents an immediate debug/fallback
  image from being reused as accumulated radiance.
- Core settings provide SPP, Maximum Samples (`0` unlimited), Max Bounces,
  deterministic seed, analytic-environment strength, and an explicit
  `AppSession::resetPathTraceAccumulation()` generation bump. Phase 8 owns the
  eventual renderer-settings UI.
- Analytic-environment misses are opaque; environment-Off misses are
  zero-radiance/alpha-zero. This preserves the fullscreen composite's
  premultiplied-alpha contract.
- RT push constants are checked against the device limit and remain 208 bytes.
  RayGen/Miss/Closest Hit share a 32-byte payload; shadow rays keep the separate
  float payload and use `TerminateOnFirstHit | SkipClosestHit`.

## Principal Files

- `include/xpbd/gfx/ray_tracing.hpp`
- `src/gfx/ray_tracing.cpp`
- `include/xpbd/gfx/gpu_backend.hpp`
- `include/xpbd/app/app_session.hpp`
- `src/app/main_sdl3.cpp`
- `include/xpbd/gfx/vulkan_path_tracer.hpp`
- `src/gfx/vulkan_path_tracer.cpp`
- `include/xpbd/gfx/vulkan_rt_pipeline.hpp`
- `src/gfx/vulkan_rt_pipeline.cpp`
- `src/gfx/vulkan_backend.cpp`
- `src/gfx/spirv/rt_debug.rgen`
- `src/gfx/spirv/rt_debug.rchit`
- `src/gfx/spirv/rt_debug.rmiss`
- `src/gfx/spirv/rt_shadow.rmiss`
- `src/gfx/spirv/rt_debug.rahit`
- `src/gfx/viewport_regression_tests_main.cpp`
- `README.md`
- `README.en.md`

## Focused Gates

The following commands passed:

```powershell
.\tools\build_spirv.ps1
.\tools\build_spirv.ps1 -Check
cmake --build out\build\rt-migration --config Debug --target xpbd_viewport_regression_tests
.\out\build\rt-migration\Debug\xpbd_viewport_regression_tests.exe
cmake --build out\build\rt-migration --config Debug --target xpbd_baker_app
git diff --check
```

The direct regression covers settings clamps, fixed-seed integer repeatability,
dimension decorrelation, finite normalized cosine-hemisphere samples, invalid
normal fallback, new-history reset, compatible SPP/maximum changes, exact
Maximum Samples stopping, lowered-limit preservation, and key-change reset.

SPIR-V generation/check passed after the final depth finite guard and
premultiplied environment-alpha correction. The Debug application build passed
with only pre-existing third-party/UI warnings. `git diff --check` reported
only the existing line-ending conversion notices. The staged diff is empty.

No Release build or CTest was run, per user instruction.

## Fixed-Seed Hardware Reproducibility

On the NVIDIA GeForce RTX 4090 Laptop GPU, two fresh fixed-window runs used the
same deterministic 36-primitive/3-instance fixture, seed `424242`, four
bounces, two samples per dispatch, and eight samples per frame slot.

Both slots independently reset at sample zero and stopped at sample eight with
the same history key. The captured client PNGs were byte-identical:

`E5453123CE71DB449D38B02B265A1A7B08C9F6CBB8851CF97502A389BE4951B6`

Post-closeout-fix evidence is retained under:

`.tmp/phase4-final-gates-20260729-154740-435`

All four runs in that directory have empty stderr, exit code 0, bounded sample
diagnostics, and successful `vkDeviceWaitIdle.shutdown`.

## Enclosed-Box Indirect-Light Gate

An ignored reversible thin-wall room with an open front and two interior blocks
was rendered from a fixed yaw-180 camera with `PreviewSceneId::None`, seed
`20260729`, four bounces, and 128 samples per frame slot.

The only difference between the matched captures was analytic-environment
strength 1 versus 0. Conservative interior-region measurements were:

| Region | Strength 1 mean luma | Strength 0 mean luma | Delta |
|---|---:|---:|---:|
| Back-wall shadow (141,600 pixels) | 17.3559/255 | 0 | +17.3559/255 |
| Tall interior block | 28.6128/255 | 4.0789/255 | +24.5338/255 |
| Short interior block | 26.8474/255 | 0 | +26.8474/255 |

All 141,600 tested back-wall pixels changed. The strength-0 room is black apart
from unrelated raster grid pixels, while the strength-1 room has stable
nonzero wall/block illumination. This isolates transported environment
radiance inside the enclosure rather than direct-light or raster leakage.

## Finite Output and Memory Stability

RayGen sanitizes every stored RGB/alpha average and verifies projected clip/
depth values before the color/depth image stores. Invalid throughput, sampled
radiance, payload normal, accumulated color, or depth resolves to a finite safe
fallback; RGBA values are then bounded to the finite RGBA16F range. The CPU
reference also exercises non-finite settings and invalid cosine-sampling input.
Final SPIR-V checks, deterministic hardware captures, empty stderr, and clean
Vulkan shutdowns passed after these guards.

For the memory-growth gate, both slots reached a bounded 32 samples before a
12-second process series:

- private bytes: `485,216,256 → 481,742,848` (last nine samples flat)
- working set: `266,244,096 → 265,265,152`
- handles: `490 → 490`

Evidence is retained under:

`.tmp/phase4-memory-20260729-153610-519`

No extra accumulation image is allocated; compatible frames update the
slot-local RGBA16F target in place.

## Real Model Stress and Boundary

The user's read-only model and base texture passed the final minimal path gate:

- model: `<user-test-assets>\huilin\models\main.json`
- base texture: `<user-test-assets>\huilin\textures\texture.png`
- 28,956 model primitives
- 475 instances
- four bounces
- both slots reached eight samples
- empty stderr, exit code 0, graceful shutdown

Evidence is retained under:

`.tmp/phase4-huilin-20260729-154207-611`

The source directory was not modified. Phase 4 uses base-color Lambert input
only. Full GGX/dielectric/metal/transmission/emission and the user's LabPBR
normal/specular response remain Phase 5 work and are not claimed here.
