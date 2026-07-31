# RT Phase 6 Gate — Environment, Clouds, Lights, NEE/MIS

Date: 2026-07-30

Status: **PASS**

## Implemented scope

### Procedural and imported environments

- Astronomy Engine C produces topocentric Sun/Moon direction, altitude,
  azimuth, apparent diameter, lunar distance, phase, illuminated fraction,
  magnitude, libration, sidereal orientation, and twilight class.
- The Bruneton Vulkan compute adaptation owns transmittance, scattering,
  irradiance, and multiple-scattering LUTs. Its cache identity contains the
  implementation revision, physical parameters, dimensions, format, and
  frozen upstream shader hashes.
- The dynamic procedural environment contains continuous atmosphere,
  analytic circular Sun, finite phased Moon, generated lunar surface, stars,
  Milky Way, horizon transition, and generated volumetric clouds.
- Procedural and Radiance HDR environments build a solid-angle-correct alias
  distribution. Background, lighting, rotation, finite Sun/Moon sampling, and
  the evaluated PDF use the same linear-space coordinate convention.
- Sky rendering remains independent from scene selection. The below-horizon
  result is radiance rather than geometry and cannot cover the scene `y=0`
  plane.

### Volumetric clouds

- `atmosphere_environment_cache.comp` ray marches generated weather, base,
  detail, and erosion noise in Vulkan compute.
- Sun and Moon are evaluated independently with anisotropic phase, light-ray
  transmittance, multiple-scattering approximation, and adjustable shadow
  strength.
- Cloud history is a dedicated RGBA16F radiance/transmittance image. A new
  frame reads the previous image, reprojects it by the effective wind/weather
  displacement, clips invalid history, and applies the bounded history weight.
- Sky playback advances cloud advection and the deterministic temporal frame.
  Structural changes, size changes, disabled reprojection, or incompatible
  cache identity reset history.
- Full cloud/environment resolution is **2048x1024 by default**. The runtime
  ratio is real: `1.0 = 2048x1024`, `0.5 = 1024x512`, and
  `0.25 = 512x256`. The UI displays the resolved dimensions.
- Shadow resolution affects the cloud-light sampling grid. Cloud changes
  invalidate environment/PT history without rebuilding BLAS/TLAS.

### Analytic and emissive lighting

- Analytic light and environment NEE, environment importance sampling, MIS,
  light samples per path, emissive multiplier, and direct/indirect clamps run
  in the RT Pipeline ray-generation shader.
- Static LabPBR emissive triangles build a dense world-space GPU record table.
  Power is `world area * average premultiplied emitted luminance`.
- The table exports alias acceptance, alias index, PMF, transformed triangle
  vertices, area, and emitted radiance. Bone transforms and bone tint are
  applied before upload.
- NEE samples triangle area uniformly, converts the area PDF to solid angle,
  traces bounded shadow rays, evaluates the same BSDF, and applies the power
  heuristic. BSDF paths that hit an emissive primitive recover the same
  selection PDF for reciprocal MIS.
- Exact LabPBR emission still contributes on direct hits. GPU mesh-emitter
  direct sampling is capability-gated to the NVIDIA RT Pipeline; compatibility
  Ray Query retains direct-hit emission and the UI reports the limitation.

## Gate evidence

| Gate | Evidence |
|---|---|
| Astronomy reference values | `xpbd_viewport_regression_tests`: frozen Sun/Moon azimuth, altitude, lunar fraction, magnitude, distance, diameter, and libration |
| Day/night continuity | one-minute celestial continuity, twilight classification, procedural cache hardware runs |
| Moon phase and moonlight | finite Moon cache/importance diagnostics and lunar phase regression |
| Cloud occlusion/shadow | real compute Sun/Moon light transmittance; cloud cache is background radiance so scene depth remains authoritative |
| Cloud temporal | `.tmp/phase6-cloud-temporal-smoke`: first frame `history=reset`, then 340 consecutive `history=reprojected` frames with monotonic frame IDs and wind deltas; stderr empty |
| 2K cloud default | `.tmp/phase6-cloud-2k-smoke`: `ratio=1.00`, `2048x1024`, 2,097,152 environment texels, 33,554,528-byte GPU alias buffer; stderr empty |
| PDF/MIS | alias normalization, environment PDF integration, power heuristic, and area-to-solid-angle CPU regressions; RT shader SPIR-V validation |
| Mesh-emitter NEE/MIS | `.tmp/phase6-emissive-smoke`: RTX 4090 Laptop GPU, LabPBR specular active, 17,216 primitives, 11,558 emitters, two RT Pipeline slots, 8-spp capture, stderr empty |
| Low-SPP convergence | the mesh-light hardware capture completed at 8 spp with direct NEE/MIS and finite output |
| Build/regression | Debug app build, viewport regressions, AppSession regressions, embedded SPIR-V check |

## License audit

| Component | Use | License/notice result |
|---|---|---|
| Astronomy Engine C v2.1.19 | compiled C source and thin C++ state wrapper | MIT; `third_party/astronomy/LICENSE` exactly matches `notices/ASTRONOMY_ENGINE.txt` SHA-256 `F74B2D...7695C6` |
| Bruneton atmosphere, frozen commit `34f14e7...` | adapted physical GLSL and Vulkan LUT orchestration | BSD-3-Clause; `third_party/bruneton/LICENSE` exactly matches `notices/BRUNETON_ATMOSPHERE.txt` SHA-256 `5EC344...98270` |
| Meteoros / NUBIS | design reference for independently written cloud concepts | no source, textures, models, or redistributables are vendored or shipped |
| Generated sky/cloud assets | deterministic project shaders/noise | project-owned; no external HDRI, cloud texture, star catalogue, or lunar texture is bundled |

`CMakeLists.txt` copies the complete `notices/` directory beside the application
binary. The hardware smoke directories contain that copied notice set.

## Validation boundary

- Local Vulkan validation layers were not available. The gate uses successful
  SPIR-V compilation plus real NVIDIA Vulkan/RT hardware execution with empty
  stderr.
- The Phase 6 mesh-light sampler intentionally targets the NVIDIA RT Pipeline.
  It does not claim to be active on the compatibility Ray Query path.
- Cloud rendering is a generated sky/environment volume, not an unbiased
  participating-medium path tracer.
