# CC0 preview-scene replacement gate

Date: 2026-07-30

## Result

The preview scene selector now exposes a curated offline set:

1. None
2. Studio
3. Day
4. Night
5. Sunset
6. Desert
7. Ocean
8. Overcast

Studio, Day, Night, Sunset, and Overcast use fixed Poly Haven CC0 1K Radiance
HDR assets. Desert and Ocean use the separately gated FastNoiseLite/osgw
open-source surfaces and the CC0 Day sky. Dawn, Space, End, and Storm remain
valid legacy numeric inputs but resolve to Sunset, Night, Night, and Overcast
respectively.

Scene selection remains independent from physical World/Sky rendering.
Preview assets never change the World sky source, environment-light toggle,
HDRI selection, exposure, time, atmosphere, Sun, Moon, or path-tracing state.

## Geometry and horizon policy

- The old project-authored Studio box room was removed.
- Studio contributes no floor, walls, ceiling, collision, RT geometry, or
  depth at `y=0`.
- Pure-sky sources contain an intentionally dark lower hemisphere. Automatic
  exposure now measures only the useful upper sky.
- Cubemap conversion synthesizes a continuous, non-black lower radiance shell
  from the upper sky. It is skybox color only, writes no depth, and cannot cover
  or occlude the `y=0` plane.
- Ocean alone retains dynamic preview animation; its CC0 cubemap is static
  while its water surface and matching preview light animate.

## Provenance

Exact source pages, download URLs, byte counts, official MD5 values, local
SHA-256 values, and authors are recorded in:

- `assets/preview_scenes/SOURCES.md`
- `notices/POLY_HAVEN_CC0.txt`
- `third_party/PREVIEW_SCENE_UPSTREAMS.md`
- `docs/open_source_preview_surfaces_gate.md`

Poly Haven's license page states that its assets are CC0:
https://polyhaven.com/license

The application performs no runtime network or Poly Haven API access.

## Validation

- Debug `xpbd_baker_app`: build pass.
- Debug `xpbd_viewport_regression_tests`: pass.
  - all five assets decode and convert transactionally;
  - curated and legacy mappings pass;
  - RGB range/chroma pass;
  - all pure-sky nadir faces are non-black;
  - Studio has no environment geometry or solid ground;
  - dynamic Ocean retains a static CC0 sky and animated surface.
- Debug `xpbd_app_session_regression_tests`: pass.
- English/Simplified Chinese locale parity: 594/594.
- Source/package SHA-256 parity: all five assets pass.
- Scoped `git diff --check`: pass; repository line-ending notices only.
- Real Vulkan/RTX 4090 bounded launches: Studio, Day, Night, Sunset, Overcast,
  and dynamic Ocean all load the exact packaged HDR and exit cleanly.
- Real compositor captures confirm upper sky detail, non-black lower horizon,
  distinct retained presets, and the dynamic Ocean surface.

Runtime evidence:

`D:/XPBD_Baker_CPP/.tmp/cc0-preview-smoke-20260730-141931`

No Release build, CTest, commit, push, PR, external model/texture mutation, or
new SDK/license acceptance was performed.
