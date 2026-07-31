# RT Phase 10 gate — World/Sky audit and Still Render Result

Status: **pass** (2026-07-30, Debug).

## Scene and World/Sky boundary

- New sessions still start with `SceneSelection::Empty` and
  `SkyRendering::Off`.
- SceneSelection and World/Sky remain independent transactions. A loaded model
  or preview preset never silently changes the selected sky source.
- The existing procedural day/night sky, astronomical UTC/observer controls,
  artistic Sun/Moon angles, atmosphere, clouds, HDRI, persistence, and
  unavailable-resource rollback remain covered by the Phase 6-8 regressions.
- A still job freezes the selected Scene, resolved World/Sky and clouds,
  camera matrices, animation/preview time, model/material generations, and
  normalized physical path-tracing settings before GPU accumulation begins.

## Still Render contract

- The Renderer page exposes file name, width, height, target samples, samples
  per submit, PNG/EXR, transparent background, start/cancel, progress, failure,
  and the final Render Result path.
- Width/height are bounded to 64-4096. Target samples are bounded to
  32-65,536 and default to 1,024. Submit batches are bounded to 1-32.
- Output is fixed to `<application directory>/output`. File names are
  sanitized, extensions follow the selected format, and existing files receive
  collision-free numeric suffixes. The UI cannot redirect a result outside
  this folder.
- The job owns explicit Queued, Rendering, Saving, Completed, Failed, and
  Cancelled states. Playback is paused for the immutable snapshot and restored
  after every terminal state.
- RT scene preparation is asynchronous: the job remains Queued while model
  buffers and BLAS/TLAS are being prepared. It fails only for a real missing
  NVIDIA Vulkan RT capability, disabled RT-core path, software fallback, target
  allocation failure, rejected capture, or file-write failure.
- Cancellation is fence-safe and never publishes a partial Render Result.

## Raw accumulation and output encoding

- Still Render is deliberately a Cycles-style raw, full-resolution sample
  accumulation. Its snapshot forces denoiser `Raw`, upscale `Off`, adaptive
  sampling off, resolution scale 1.0, Full quality, and accumulation enabled.
- Future NRD/DLSS preview integration must not silently alter still-render
  pixels.
- A third `VulkanPathTracer` owns still accumulation independently from both
  in-flight viewport accumulators. It records on slot zero and consumes its
  mapped readback only after that slot's fence, so still rendering neither
  resets viewport history nor races another command buffer.
- PNG is display-referred sRGB and applies the frozen exposure, white balance,
  bloom, and tone mapping.
- EXR is an uncompressed scanline OpenEXR with scene-linear RGBA16F channels in
  the required A/B/G/R order; display transforms are not baked into it.
- Transparent output reads device depth and clears only far-plane pixels.
  Model/material alpha remains intact instead of deleting the visible
  procedural sky indiscriminately.

## Automated evidence

- Debug `xpbd_app_session_regression_tests`: pass.
  - fixed application/output routing and collision-free sanitized names;
  - numeric bounds and format selection;
  - immutable World/Sky, camera, model/material, and path-tracing snapshot;
  - forced raw/full-resolution still settings;
  - playback restoration after terminal states;
  - OpenEXR byte-stream generation and file writing;
  - depth-aware transparent PNG masking.
- Debug `xpbd_viewport_regression_tests`: pass, including all prior RT,
  LabPBR, transparency, Scene/Sky, preview-surface, and PT-control coverage.
- OpenEXR 3.4.8 decoded the regression and hardware files as
  `scanlineimage`, `NO_COMPRESSION`, RGBA float16 with finite samples.
- English/Simplified Chinese locale parity: **620/620**.
- Debug `xpbd_baker_app`: build pass.
- `git diff --check`: pass; line-ending notices only.

## NVIDIA hardware evidence

Evidence directory: `.tmp/phase10-still-20260730-2`.

Read-only user inputs:

- `<user-test-assets>\白水绘\models\main.json`;
- `<user-test-assets>\白水绘\textures\tex.png`;
- `<user-test-assets>\白水绘\textures\pbr\blue_s.png`;
- `<user-test-assets>\白水绘\textures\pbr\bule_n.png`.

On the NVIDIA GeForce RTX 4090 Laptop GPU:

- direct base/PBR/Iris-normal imports succeeded;
- the unified alpha-aware PT scene built 506 BLAS, one TLAS, 506 instances,
  25,008 vertices, and 12,504 triangles;
- transparent 640x360 PNG completed in the executable's `output` folder and
  contains both alpha 0 and alpha 255 pixels;
- opaque 640x360 raw PNG completed at exactly **1,024/1,024** samples with no
  denoiser or reconstruction;
- linear 640x360 EXR decoded as finite RGBA16F with alpha 1 and a finite HDR
  range;
- all hardware stderr logs are empty and shutdown completed normally.

The evidence directory contains copies only. No source model or texture file
was modified.

## Deferred boundary

NRD and DLSS SR/FG/RR runtime integration belongs to Phase 11 and later. The
Phase 10 still renderer already isolates its raw Render Result from those
future interactive-preview paths.
