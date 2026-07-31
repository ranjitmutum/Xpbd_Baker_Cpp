# RT Phase 12 Gate: Streamline Core and DLSS Super Resolution

## Result

Phase 12 is complete for the interactive Vulkan path-traced preview. Official
signed NVIDIA Streamline 2.12.0 and DLSS Super Resolution execute on the
target RTX hardware. The central 3D viewport is reconstructed to its native
output extent; Nuklear UI pixels and the Cycles-style Still Render remain
outside Streamline.

## Frozen dependency boundary

- Streamline: 2.12.0.
- DLSS model runtime: 310.7.0.
- Project identity:
  `50504244-4241-4b45-92ab-c0d1e2f3a4b5`.
- The build consumes a prepared SDK selected by
  `XPBD_STREAMLINE_SDK_ROOT`; it never downloads or accepts SDK terms from
  CMake.
- Only NVIDIA-signed production binaries are deployed:
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.pcl.dll`,
  `NvLowLatencyVk.dll`, and `nvngx_dlss.dll`.
- Streamline OTA plugin loading is disabled.
- Streamline and NGX/DLSS notices are copied beside packaged object code.

## Implemented render path

```text
Preview PT linear HDR + depth + pixel-space motion
  -> optional official NRD REBLUR/RELAX
  -> emission + premultiplied transparency linear-HDR composition
  -> official DLSS SR/DLAA
  -> preview display transform
  -> native-resolution Nuklear UI
```

- Vulkan feature requirements are queried before instance/device creation.
- Manual Vulkan hooks route instance, physical-device enumeration, device,
  Win32 surface, swapchain, acquire, present, and device-wait-idle calls
  through the Streamline interposer.
- Every evaluation uses one frame token and viewport handle for options,
  tagged resources, constants, and evaluation.
- Tagged inputs are render-resolution RGBA16F HDR color, R32 depth, and the
  path tracer's motion/disocclusion AOV. Output is double-buffered RGBA16F at
  the native central-preview extent.
- Current/previous Vulkan view-projection transforms, motion-vector scale,
  camera-cut/reset state, and automatic exposure are supplied each frame.
- Streamline manual mode does not restore Vulkan command state. XPBD therefore
  explicitly binds the following compositor pipeline and descriptor set.
- `PT -> NRD -> DLSS SR` is a real chain: SR reads an NRD-composed linear-HDR
  image containing denoised diffuse/specular plus emission and transparent
  overlay. RR and NRD remain mutually exclusive.
- Output/viewport resize waits for device idle, frees the old DLSS feature
  resources, recreates output images, and invalidates temporal history.
  Minimized, folded, or zero-sized previews skip evaluation and force reset on
  the next valid frame.
- Still Render freezes upscale Off and makes zero Streamline feature calls.

## User controls and compatibility

The user-visible choices are exactly:

- Off
- DLAA
- Quality
- Balanced
- Performance
- Ultra Performance

Early development values Auto and Ultra Quality remain reserved numeric JSON
compatibility values. Loading either migrates it to Quality; neither is
displayed in the UI. `XPBD_PT_UPSCALE` supports the five user tiers for
unattended diagnostics and accepts the two retired spellings only as warned
Quality migrations.

Requested and Active denoiser/upscale states remain visible. A one-time
no-Streamline fallback probe passed during Phase 12, but it is not a supported
delivery configuration: the user requires Streamline for RR, Reflex, and FG,
so subsequent phases and final packaging always include Streamline.

## Verification

Serialized Debug builds and both maintained regression executables pass.
Coverage includes legacy-mode migration, unavailable requested/active
resolution, RR/SR conflict resolution, settings round-trip, viewport mapping,
resize history, and Still Render isolation. English and Simplified Chinese
JSON are valid with 621/621 key parity.

RTX 4090 Laptop GPU evidence:

- Quality: official optimal `393x386 -> 590x579`, 80 frames, clean release.
- DLAA: `590x579 -> 590x579`, clean release.
- Balanced: `342x336 -> 590x579`, clean release.
- Performance: `295x290 -> 590x579`, clean release.
- Ultra Performance: `197x193 -> 590x579`, clean release.
- Resize/minimize/restore:
  `590x579 -> 312x509 -> 708x665`, matching context rebuilds, 760 frames,
  clean release.
- NRD-to-SR chain: REBLUR `393x386 -> 590x579`, 80 frames, explicit active
  chain log, clean release.
- Still isolation: raw 64x64/32-sample PNG completes without increasing the
  global official DLSS-context creation count.
- One-time fallback probe: a no-Streamline diagnostic build resolved requested
  Quality visibly to Off and exited cleanly. This variant is retired from
  subsequent gates and packaging.

Gate artifacts are under `.tmp/phase12-*`; the application runtime log records
the official context extents and releases.

The local Vulkan validation layer is not installed, so no validation-layer
claim is made. Official Streamline/NGX logs contain benign cache-probe misses
before loading the packaged production model and manual-hook warnings for
commands that XPBD explicitly rebinds. They report no evaluation or shutdown
failure in the passing gates.

DLSS Ray Reconstruction is Phase 13. NVIDIA's current Streamline Vulkan
feature report does not expose Frame Generation; Phase 14 must keep a truthful
unavailable reason rather than presenting a nonfunctional switch.
