# RT Phase 11 Gate: NVIDIA NRD

## Result

Phase 11 is complete for the interactive Vulkan path-traced preview.
Official NVIDIA NRD 4.17.4 REBLUR and RELAX execute on the target RTX
hardware. The Cycles-style still-render path remains raw, full-resolution,
and independent from NRD.

## Frozen dependency boundary

- NRD: 4.17.4, commit
  `9a3fe938a7558fd16b6c91a1c0456305cdcd9f16`
- NRI: 180, commit
  `4b485316c98a746e6e5c5e7f6832d88065461e95`
- ShaderMake:
  `18f5a344e7ca8fa65daaf079d07bc8ce38453e05`
- MathLib v11:
  `974e1387aa82d752b0f9a6c927293c3dba57ceaf`
- NRD is compiled with linear roughness and RGBA16 signed-normal numeric
  encoding. The Vulkan inputs are floating-point images.
- CMake never downloads or accepts the SDK implicitly. A prepared SDK is
  selected with `XPBD_NRD_SDK_ROOT`.
- The deployed object code includes `NRI.dll` and
  `output/notices/NVIDIA-RTX-SDK-LICENSE.txt`.

Official references:

- <https://github.com/NVIDIA-RTX/NRD>
- <https://github.com/NVIDIA-RTX/NRD/blob/master/LICENSE.txt>

## Implemented render path

```text
Preview PT HDR + AOV array
  -> NRD input preparation
     normal/roughness, viewZ, diffuse/specular radiance + hit distance,
     screen-space motion/disocclusion
  -> official NRDIntegration + NRI Vulkan dispatch
     REBLUR or RELAX
  -> emission and premultiplied transparency post-composite
  -> preview display transform
  -> native-resolution Nuklear UI
```

- NRD owns preview-sized resources with origin `(0,0)`.
- Permanent/transient resources are created by the official integration.
- The host enables only the synchronization2 Vulkan extension required by the
  compute-only NRI wrapper. Host swapchain and ray-tracing extensions are not
  exposed to NRD.
- First frame, invalid motion, camera cut, resize, method, scene, material,
  and explicit reset generations invalidate history.
- Stable frames preserve history and use the motion/disocclusion input.
- Transparent surfaces, emission, sky, and clouds are not fed through NRD;
  they are composed outside the opaque geometry denoiser.
- RR and NRD resolve as mutually exclusive. RR owns reconstruction when it is
  available and selected.

## Controls and truthful fallback

- Renderer UI exposes Requested and Active denoiser state:
  `Auto`, `DLSS RR`, `NRD REBLUR`, `NRD RELAX`, and `Raw`.
- Developer controls expose the official NRD `OUT_VALIDATION` overlay.
- Path-tracing JSON persists the validation option.
- Builds without a prepared SDK compile and run without `NRI.dll`; an NRD
  request resolves visibly to Raw and reports that NRD was not built.
- `StillRenderJob` always freezes `Raw`, `Upscale Off`, validation off,
  full resolution, and raw accumulation.

## Verification

Debug build and regression executables pass:

```text
cmake --build out/build/rt-migration --config Debug
  --target xpbd_baker_app xpbd_viewport_regression_tests
           xpbd_app_session_regression_tests --parallel 1

xpbd_viewport_regression_tests.exe
xpbd_app_session_regression_tests.exe
```

Coverage includes AOV mapping, unavailable fallback, requested/active
resolution, validation routing, first-frame reset, dynamic motion,
disocclusion, resize, camera cut, scene/material/user reset, method changes,
256 stable history frames, settings round-trip, and still-render isolation.

RTX 4090 Laptop GPU evidence:

- `.tmp/phase11-nrd-relax`: RELAX, 295x290, clean exit.
- `.tmp/phase11-nrd-reblur`: REBLUR, 295x290, clean exit.
- `.tmp/phase11-nrd-longstable`: REBLUR reaches 120 continuous stable
  history frames with motion and disocclusion enabled.
- `.tmp/phase11-nrd-validation-output`: official RELAX validation output is
  enabled and dispatched.
- `.tmp/phase11-nrd-nosdk`: separately configured no-SDK app, no `NRI.dll`,
  requested RELAX, clean Raw fallback and clean exit.

The local Vulkan validation layer is not installed, so a validation-layer
claim is deliberately not made. The official NRD validation output and the
NRI runtime report no execution errors. Full validation-layer and packaging
audits remain part of Phase 15.
