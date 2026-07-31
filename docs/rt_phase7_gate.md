# RT Phase 7 Gate — Transparency, Dynamics, AOV, and Motion

Date: 2026-07-30

Status: **PASS**

## Implemented scope

### Unified transparency

- Primary visibility, shadow visibility, and emissive-triangle weighting use
  the same base-alpha contract and `0.02` cutoff.
- Cutout rejects alpha below the cutoff and treats surviving texels as opaque.
  Blend uses stochastic coverage in the path integrator and the matching
  opacity rule for shadows.
- Mesh-emitter power is premultiplied by accepted opacity, so invisible texels
  cannot emit.
- The denoiser-facing transparency AOV is deterministic and premultiplied. It
  contains fractional Blend layers in front of the first opaque/cutout surface;
  rejected Cutout texels, including fully transparent Huilin flame texels, do
  not become a white or emissive overlay.

### Dynamic AS and one-frame history

- Static scene geometry uses build/compact, rigid cube groups keep immutable
  local-space BLAS plus TLAS transforms, and deformed model/XPBD/ocean geometry
  uses topology-compatible BLAS refit.
- Topology or incompatible content changes select a full rebuild. Rigid
  transforms rebuild only the TLAS.
- Both in-flight frame slots consume one authoritative previous rendered-frame
  CPU snapshot. Previous packed positions, bone transforms, and per-instance
  transforms are no longer two rendered frames old.
- The same dynamic vertex stream covers source animation, skinned deformation,
  live XPBD output, and procedural ocean vertices.

### History, AOVs, motion, and disocclusion

- The path-tracing compatibility key explicitly includes the resolved World
  generation. Target recreation and incompatible accumulation reset advance
  an independent history generation.
- Full-resolution targets now contain:
  - RGBA16F noisy HDR beauty and R32F device depth;
  - a 9-layer RGBA16F AOV array for geometric normal/linear depth, shading
    normal/linear roughness, diffuse albedo, specular albedo/normalized hit
    distance, diffuse noisy radiance/hit distance, specular noisy radiance/hit
    distance, motion/disocclusion, emission, and transparency overlay;
  - RGBA32F online variance, path length, exact sample count, and linear depth.
- Motion is dense and uses the documented current-to-previous pixel-space
  convention. Surface motion reconstructs the previous barycentric vertex
  position and previous instance transform; sky motion uses the previous
  camera matrix.
- Missing history, invalid/non-finite clip data, behind-camera history, and
  previous samples outside the viewport produce `disocclusion=1, valid=0`.
  Valid motion produces `disocclusion=0, valid=1`.
- Diagnostic readback writes `xpbd-pt-aov-summary/1` JSON with finite counts,
  ranges, nonzero coverage, variance checks, depth range, motion validity, and
  disocclusion counts.

## Gate evidence

| Gate | Evidence |
|---|---|
| Cube and rigid transforms | `xpbd_viewport_regression_tests`: canonical cube identity, rigid per-bone BLAS ranges, stable TLAS IDs, and exact current/previous transform checks |
| Dynamic BLAS policy | CPU regression: first dynamic update is full build, stable topology/content change is refit, incompatible rigid-local content fails safe to full build |
| Ocean refit on hardware | `.tmp/phase2-ocean-refit-20260729-132224-250`: two initial BLAS builds followed by 92 `stable refit` events, no extra full builds, two BLAS/one TLAS |
| Bone/skinned motion | `.tmp/phase7-huilin-dynamic-aov-smoke-20260730`: 32,554 primitives, 476 instances, 2-spp AOV readback, nonzero dense motion, empty stderr |
| XPBD/deformed motion | The live `frame.scene` solid/transparent vertex stream is classified `DynamicRefit` and uses the same explicit previous packed-position snapshot; update-policy and previous-position regressions pass |
| Disocclusion | CPU shader-reference regression covers absent history, valid static/translated motion, behind-camera history, viewport exit, and non-finite clip values |
| Complete AOV numeric output | `.tmp/phase7-aov-numeric-smoke-20260730`: 295x290 at exactly 32 spp, 7,486 surface pixels, zero non-finite statistics, zero negative variance, finite device depth, and nonzero geometry/material/radiance/motion layers |
| Fractional transparency | `.tmp/phase7-ocean-transparent-aov-smoke-above-20260730`: RTX 4090, 32,770 triangles, two BLAS/one TLAS, 298,664 nonzero transparency pixels, overlay alpha max `0.725586` |
| Dynamic ocean motion | Same Ocean capture: 341,610 valid motion pixels, finite nonzero X/Y motion, zero non-finite values |
| Build and shader ABI | Debug app, viewport regressions, AppSession regressions, embedded SPIR-V `-Check`, AOV layer static assertion, and scoped `git diff --check` pass |

The Huilin and Ocean runs use the NVIDIA Vulkan RT Pipeline on an
`NVIDIA GeForce RTX 4090 Laptop GPU`. All referenced Phase 7 stderr logs are
empty. English/Simplified Chinese parity remains 594/594 keys.

## Validation boundary

- Local Vulkan validation layers are unavailable; the gate uses successful
  SPIR-V compilation plus real NVIDIA Vulkan/RT execution.
- The deterministic disocclusion boundary is regression-tested on the CPU
  against the shader contract. A moving object remaining inside the viewport
  correctly reports valid motion rather than fabricating disocclusion.
- Dynamic scenes intentionally invalidate multi-frame radiance accumulation
  when their geometry changes; AOV diagnostics can capture an early rendered
  frame without weakening that history rule.
