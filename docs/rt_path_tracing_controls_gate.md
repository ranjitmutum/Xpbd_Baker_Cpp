# Path-Tracing Adjustable Controls Gate

Date: 2026-07-30
Branch: `RT`
Plan: `XPBD_独立计划_路径追踪可调参数_仅简中英文-2.md`

## Result

The adjustable path-tracing control surface is implemented through one
normalized state contract, transactional AppSession application, Vulkan
runtime/shader bindings, Renderer UI, English/Simplified Chinese resources,
versioned persistence, and immutable still-render snapshots.

Unavailable future subsystems remain explicit rather than pretending to be
active:

- Adaptive sampling is visible and disabled until variance/AOV support exists.
- Accumulate While Moving is visible and disabled until camera reprojection
  exists; camera motion safely resets incompatible history.
- Emissive-mesh direct sampling is visible and disabled until the remaining
  Phase 6 mesh-light distribution is bound to the GPU.
- NRD and DLSS modes preserve Requested state and expose Active `Raw / Off`
  until their later master phases provide capability.

## Implemented contract

- Engine Requested/Active/fallback/GPU/preview-resolution status.
- NVIDIA-only RT Core acceleration switch. On selects the Vulkan RT Pipeline;
  off selects the compatibility ray-query path. Unsupported devices disable
  the control.
- Realtime, Balanced, High Quality, and Reference presets; manual edits become
  Custom and retain a source preset for Restore.
- SPP `1/2/4/8/16/32`, Maximum Samples `0` or `32..65536`,
  automatic/fixed seeds, exact Reset Accumulation, total/per-lobe paths, and
  bounded Russian roulette.
- Transparent pass-through consumes both transparent and total path budgets.
- Analytic lights, emissive surfaces/multiplier, environment and Sun/Moon
  shared World switches, NEE, MIS, environment importance sampling, light
  samples per path, and zero-disables direct/indirect clamps.
- Requested/Active denoise and upscale state with unsupported fallback and
  Ray Reconstruction/SR conflict resolution.
- Display-only transparent background, exposure, white balance, ACES/Reinhard
  tone mapping, and a screen-space bloom kernel.
- Real preview target scaling, previous-GPU-time SPP throttling, interactive
  first-frame quality, pause/resume without discarding history, timing/memory
  status, and target/history generations.
- RT identity/material/normal/albedo/roughness/emission debug views plus
  developer compatibility-path control.
- JSON schema `xpbd-path-tracing/1`, transactional load, and immutable
  `PathTraceRenderSnapshot`.

## Change classes

| Change | Runtime action |
|---|---|
| SPP, maximum samples, target time, interactive schedule, pause | Schedule only; preserve compatible HDR history |
| Bounces, seed, Lighting/Advanced integrator controls, RT implementation | Reset accumulation |
| Preview resolution scale | Recreate target |
| Denoiser/upscale request | Reconfigure post-process |
| Film and display controls | Display only; preserve linear HDR history |

None of these controls rebuild BLAS/TLAS or compile shaders at runtime.

## Validation

- Debug `xpbd_baker_app`: builds.
- Debug `xpbd_viewport_regression_tests`: passes.
- Debug `xpbd_app_session_regression_tests`: passes.
- Embedded SPIR-V check: passes.
- Locale parity: `en.json` and `zh-CN.json` have 593 identical keys.
- Scoped `git diff --check`: passes; line-ending notices only.
- NVIDIA RT Pipeline smoke:
  `.tmp/pt-controls-log-smoke-20260730-120247-269`
  - NVIDIA GeForce RTX 4090 Laptop GPU.
  - Both RT slots use the genuine RT Pipeline.
  - Configured 0.5 preview scale resolves the 590x579 viewport to 295x290.
  - NEE/MIS/environment importance, two light samples, emissive `1.5`, and
    direct/indirect clamps `16/8` are accepted.
  - Both slots reach 32 spp, a PNG is captured, and `stderr.log` is empty.
- RT Core Off smoke:
  `.tmp/pt-rtcore-off-smoke-20260730-120329-415`
  - Startup records `rt_core=0`.
  - Dispatch selects the compatibility ray-query path.
  - `stderr.log` is empty.
