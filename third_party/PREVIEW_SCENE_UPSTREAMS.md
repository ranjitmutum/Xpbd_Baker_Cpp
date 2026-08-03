# Preview scene upstreams

The Desert and Ocean preview surfaces use small, frozen MIT-licensed upstream
boundaries. The application performs no runtime network access.

## FastNoiseLite

- Repository: https://github.com/Auburn/FastNoiseLite
- Commit: `785f37a9ad76e283586a379675085f2063ae03f7`
- Upstream version in header: 1.1.1
- Files retained verbatim:
  - `fast_noise_lite/FastNoiseLite.h`
  - `fast_noise_lite/LICENSE`
- Local use: Desert OpenSimplex2 fractal, cellular, and progressive domain-warp
  sampling. The upstream header itself is unchanged.
- SHA-256:
  - `FastNoiseLite.h`:
    `38FB03EC9F276CD4E3DEBCD25C4BD67B40E58A605E523A7BABC6DD7C96279BB4`
  - `LICENSE`:
    `E8ECD6D735C77F1921A99BB3339601FBA246CE330D0164F68B6CCF12F56989F9`

## osgw

- Repository: https://github.com/CaffeineViking/osgw
- Commit: `1d82fbeaabc1c04e8bed88b4dd27e0c690065dc8`
- Files retained verbatim:
  - `osgw/gerstner.glsl`
  - `osgw/LICENSE.md`
- Local use: the position-displacement and analytic-normal equations in
  `gerstner.glsl` are ported to C++ in `src/gfx/preview_scene.cpp`. Local
  additions are deterministic spectrum construction, per-component phase
  offsets, wave-group modulation, mesh emission, and colour/foam styling.
- SHA-256:
  - `gerstner.glsl`:
    `7AC0C2FC5ABA7BCBB56BF449BFF9644B9975DB13695812E26F5D2BFC37469287`
  - `LICENSE.md`:
    `C44C3C3B4A60CBFE380D2624FF3590E348728EBB41307ED5F23285E1204D1049`
