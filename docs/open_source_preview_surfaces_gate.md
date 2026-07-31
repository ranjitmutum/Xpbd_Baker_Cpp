# Open-source Desert and Ocean preview surfaces

Date: 2026-07-30

## Result

The project-authored Desert terrain and dynamic Ocean implementation were
removed and replaced by two frozen MIT-licensed upstream boundaries:

| Scene | Upstream | Frozen commit | Local integration |
|---|---|---|---|
| Desert | [Auburn/FastNoiseLite](https://github.com/Auburn/FastNoiseLite) | `785f37a9ad76e283586a379675085f2063ae03f7` | OpenSimplex2 FBm/ridged fields, cellular breakup, and progressive domain warp sampled into a static Vulkan/RT heightfield |
| Ocean | [CaffeineViking/osgw](https://github.com/CaffeineViking/osgw) | `1d82fbeaabc1c04e8bed88b4dd27e0c690065dc8` | C++ port of osgw's Gerstner position-displacement and analytic-normal equations, evaluated as a dynamic transparent Vulkan/RT mesh |

Exact files, source hashes, license hashes, and local adaptation notes are in
`third_party/PREVIEW_SCENE_UPSTREAMS.md`. Complete licenses are present in both
the source tree and the packaged `notices` directory.

The application performs no runtime network access.

## Implementation

### Desert

- 256x256 grid, 131,072 triangles.
- Deterministic FastNoiseLite seed per scene switch.
- Multi-scale warped/ridged dune systems with cellular regional breakup.
- Three small Gaussian passes remove grid-scale serration while preserving the
  upstream fields' broad domain-warped form.
- Natural sand palette with height, dune-band, slope, grain, and horizon
  variation.
- The model inspection core is flat at y=-0.08, with a smooth transition into
  the dunes. It does not add a separate plane at y=0.

### Ocean

- 176x176 grid, 61,952 transparent surface triangles plus a six-vertex deep
  backing surface at y=-5.
- Twelve deterministic deep-water wave components with physical dispersion,
  multiple scales/directions, horizontal choppiness, and osgw-derived analytic
  normals.
- Transparent depth/sky colour, restrained crest foam, and horizon blending.
- Static mode freezes at t=0. Dynamic mode retains the existing 20 Hz mesh
  update boundary.
- Stable topology preserves the RT `DynamicRefit` route. The calm inspection
  region covers the complete preview grid; no unrelated opaque plane is added
  at y=0.
- The packaged Poly Haven Day cubemap remains static and independent from
  physical World/Sky controls.

## Validation

- Debug `xpbd_baker_app`: build pass.
- Debug `xpbd_viewport_regression_tests`: full suite pass.
  - exact topology and solid/transparent routing;
  - finite positions/colours and unit analytic normals;
  - non-flat Desert relief with clear inspection core;
  - Ocean horizontal displacement and varied alpha;
  - dynamic position changes with stable topology;
  - unchanged CC0 sky during animation;
  - deterministic frozen static Ocean.
- RTX 4090 Laptop GPU Vulkan raster:
  - final Desert 256x256 capture: 693 frames / 2.895 s (~239.4 fps), static frame pair
    byte-identical, stderr empty;
  - final dynamic Ocean 176x176 run: 223 frames / 3.971 s (~56.2 fps),
    7,574,856-byte animated upload, stderr empty;
  - measured 224x224 (~13.9 fps) and 192x192 (~18.7 fps) candidates were
    rejected instead of becoming low-frame-rate defaults.
- RTX 4090 Laptop GPU dynamic path tracing:
  - 185,862 vertices, 61,954 triangles, 2 BLAS, 1 TLAS, 2 instances;
  - the initial two BLAS builds are followed by 72/72
    `reason=stable refit` updates for the animated Ocean BLAS;
  - 74 frames / 5.089 s (~14.5 fps), stderr empty, clean window shutdown.
- Dynamic path tracing intentionally remains at one sample per changing
  geometry state because 20 Hz mesh updates invalidate temporal accumulation.
  Denoising/reconstruction is not claimed by this gate.

Runtime evidence:

`D:/XPBD_Baker_CPP/.tmp/high-res-surfaces-20260730-1`

No Release build, CTest, commit, push, PR, external model/texture mutation, or
new SDK/license acceptance was performed.
