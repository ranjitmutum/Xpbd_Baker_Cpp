# NVIDIA RTX Path Tracing (RTXPT) integration

Target stack: **[NVIDIA-RTX/RTXPT](https://github.com/NVIDIA-RTX/RTXPT)** (v1.8.x).

This replaces older Kickstart-style “G-buffer + lighting cache” plans. RTXPT is the
project’s preferred real-time path-tracing reference and long-term RT backend.

## What RTXPT is (and is not)

| | |
| --- | --- |
| **Is** | A full **sample application** (Donut + NVRHI) with a modern path tracer, RTXDI, NRD, Streamline/DLSS, etc. |
| **Is not** | A single drop-in static library with a 5-call API. |

Core code lives under `Rtxpt/` and `Rtxpt/PathTracer/`. Dependencies (Donut, NRD, RTXDI, OMM, NVAPI, Streamline) are git submodules under `External/`.

Implications for XPBD Baker:

- The app today is **SDL3 + raw Vulkan + Nuklear**.
- RTXPT is **Donut windowing + NVRHI** (DX12 primary; Vulkan opt-in).
- Full in-viewport path tracing means either:
  1. **Interop**: share/import GPU resources between our Vulkan device and NVRHI, or
  2. **Migrate** the preview renderer onto Donut/NVRHI, or
  3. **Side-by-side process**: launch/build the RTXPT sample for reference only.

## Stages

### Stage 0 — Capability + settings (done)

- Detect NVIDIA + Vulkan RT extensions.
- Settings checkbox, default **off**.
- Disable option and force raster when unsupported.

### Stage 1 — Interim hybrid ray-query shadows (current fallback)

- Local BLAS/TLAS + `VK_KHR_ray_query` directional shadows.
- Used when the unified primary path cannot be created, so the app still has a
  useful RT path.

### Stage 2 — Optional RTXPT tree + bridge (this stage)

```powershell
# Developers only (optional sample sources; not required to run the app):
powershell -File tools/vendor_rtxpt.ps1
powershell -File tools/vendor_rtxpt.ps1 -WithAssets
```

CMake:

```text
-DXPBD_WITH_RTXPT=ON
-DXPBD_RTXPT_ROOT=<repo>/third_party/RTXPT
```

Defines:

- `XPBD_WITH_RTXPT=1` when the tree is found.
- `xpbd::gfx::RtxptBridge` reports availability (headers / not yet runtime-linked).

### Stage 3 — Viewport path-trace (current)

**Shipped:** local **RTXPT-aligned path tracer** on the app’s Vulkan device:

- Compute shader + `VK_KHR_ray_query` against one unified TLAS/BLAS.
- The animated model, ground/overlay triangles, and preview-environment
  triangles participate in the same closest-hit ordering.
- Stable primary rays, directional direct lighting, alpha-aware shadow
  transmittance, and a view-dependent sky reflection term on blend surfaces.
- Front-to-back compositing for texture/tint/vertex alpha.
- Premultiplied viewport composite plus a separate nearest-opaque RT depth
  target. Nuklear UI and bone/grid/axis lines remain raster, with the latter
  depth-tested against RT output.

Material semantics:

| Mode | RT behavior |
| --- | --- |
| Opaque | First accepted hit terminates primary and shadow traversal. |
| Cutout/mask | Alpha below `0.02` is skipped; surviving texels are opaque. |
| Blend | Color is accumulated front-to-back and the ray continues through remaining transmittance. Shadow visibility is attenuated by the same alpha. |

The static-model face classifier selects opaque/cutout/blend ranges from the
loaded texture. Bone tint RGBA is applied to model hits; preview-scene
triangles use interpolated vertex RGBA.

The unified BVH is what fixes camera-angle-dependent scene/model ordering.
Traversing the former model-only TLAS could not compare a model hit against a
raster-only floor or water surface. The explicit RT depth target also prevents
later grid/axis/skeleton draws from appearing through opaque RT surfaces.

Performance policy:

- Stable scenes are detected with model transform/tint hashes and preview
  geometry generation IDs, avoiding full large-mesh byte hashing each frame.
- Animated model/ocean geometry reuses CPU scratch allocations and performs a
  BLAS refit plus TLAS rebuild; topology changes trigger a full build.
- Path-trace targets are recreated only when viewport dimensions change.

Repeatable diagnostic startup:

```powershell
$env:XPBD_RT = "1"
$env:XPBD_PREVIEW_SCENE = "9"   # Ocean
$env:XPBD_PREVIEW_DYNAMIC = "1"
$env:XPBD_MODEL = "C:\path\to\model.geo.json" # optional
$env:XPBD_TEXTURE = "C:\path\to\texture.png"  # optional
$env:XPBD_CAMERA_YAW = "145"
$env:XPBD_CAMERA_PITCH = "-20"
$env:XPBD_CAMERA_DISTANCE = "70"
.\out\build\vscode-windows-app\Release\xpbd_baker_app.exe -vk
```

**Still future (full upstream sample):**

1. Build `Rtxpt` from the fetched tree (`DONUT_WITH_VULKAN` / `NVRHI_WITH_VULKAN`).
2. Map baker geometry into Donut/RTXPT scene resources.
3. Interop or migrate to full PathTracer + multi-bounce GI,
   RTXDI/NRD/DLSS.

## Build notes (upstream)

- Windows + VS2022, CMake 3.18+ (upstream docs also mention newer CMake for some packages).
- Driver: Game Ready **595.71+** (see RTXPT README).
- Vulkan in RTXPT is **off by default**; enable Donut/NVRHI Vulkan options and run with `--vk` for the standalone sample.
- Prefer **not** vendoring the multi-GB `Assets` submodule into XPBD unless needed for demos.

## License

RTXPT and its submodules ship under their own licenses (see `third_party/RTXPT/LICENSE.txt` and External/*). Redistribute notices accordingly under `notices/` when packaging.
