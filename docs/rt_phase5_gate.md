# RT Phase 5 Gate: Full BSDF and Direct Emission

Status: **PASS**
Date: 2026-07-29
Authority: corrected unattended master, Phase 5 (lines 899–909)

## Accepted scope

Phase 5 replaces the Phase 4 Lambert loop with a deterministic full material
transport contract:

- GGX distribution, exact Smith masking-shadowing, Schlick and dielectric
  Fresnel, dielectric/metal lobe separation, and rough reflection;
- transmission, refraction, total internal reflection, F0/IOR conversion, and
  bounded shading-normal correction;
- mutually consistent sample/eval/PDF behavior, energy checks, total/per-lobe
  bounce limits, and Russian roulette reweighting;
- direct-hit LabPBR emission, including the reserved alpha-255 no-emission
  rule and the existing normal/specular sidecar decode.

The CPU reference lives in `include/xpbd/gfx/ray_tracing.hpp` and
`src/gfx/ray_tracing.cpp`. The genuine Vulkan RT implementation is in
`src/gfx/spirv/rt_debug.rgen`, `rt_debug.rchit`, and `rt_debug.rmiss`, with
descriptor/payload/push integration in `VulkanPathTracer`.

## Gate matrix

| Gate | Result and evidence |
|---|---|
| GGX/Fresnel/Smith and F0/IOR | Focused Debug viewport regression passes distribution/masking limits, exact dielectric Fresnel/TIR, Schlick endpoints, and F0/IOR round trip. |
| sample/eval/PDF consistency | 4,096 deterministic reference samples pass matching lobe/PDF assertions with sufficient valid reflections. |
| Energy conservation | Opaque dielectric/metal white-furnace cases and transmissive/TIR Monte Carlo cases are finite and bounded. TIR adds only the Schlick complement beside GGX reflection. |
| Roughness and metal | `.tmp/phase5-material-gates-20260729-163541-197`: smooth/rough changes 5,867 matched pixels; dielectric/metal changes 12,883, including 6,231 strong changes. |
| Glass and TIR | CPU refraction/TIR and glass white-furnace gates pass. `.tmp/phase5-glass-final-20260729-164727-227` reaches 32 samples in both frame slots, writes a finite capture, has empty stderr, and shuts down gracefully. |
| Bounce boundaries | Pure total/per-lobe boundary tests pass. `.tmp/phase5-depth-gates-20260729-163958-391`: total 1/4 changes 226,771 pixels and diffuse 0/4 changes 226,767 pixels with exact logged settings. |
| Russian roulette | Boundary/reweight reference tests pass. `.tmp/phase5-rr-gate-20260729-164205-568`: on/off changes 143,477 pixels while mean luma remains 63.6925 versus 63.6927 at 32 samples. |
| Direct emission | `.tmp/phase5-material-gates-20260729-163541-197`: emission on/off changes 102,975 pixels, including 16,055 strong changes; mean luma increases by 2.7797/255. |
| Full LabPBR model | `.tmp/phase5-huilin-labpbr-20260729-163834-803` consumes base, normal, and normalized specular sidecars at 32 samples. External source/copy SHA-256 pairs are equal. |
| Default empty scene | A stale Studio environment added one 12-triangle cube that obscured Huilin. Defaults are now `None` with ground/grid/axes off. `.tmp/phase5-huilin-empty-20260729-170110-137` is visually clear and logs exactly 475 instances / 28,956 model triangles. Capture SHA-256: `AA141B5E84AE1FF18C278730FA2E6823D2EEE30028DA37937EB9F74B397CDA23`. |
| Determinism | `.tmp/phase5-repro-20260729-165129-198` produces two byte-identical captures: `BB0BA520F7B53BF9730A6DEE8D10938F5F8F5478F40AD1F8CCDAFC5764FD0D58`. |
| Phase 3 compatibility | `.tmp/phase5-debug-regression-20260729-165050-306`: instance, primitive, cube, face, material, and normal modes dispatch in both pipeline instances with no rejection and empty stderr. |
| Memory stability | `.tmp/phase5-memory-steady-20260729-170417-788`: the only capacity step coincides with a 590x579 to 1862x1185 resize. The final ten samples are flat, working set decreases, handles do not grow, stderr is empty, and shutdown is graceful. |

## Engineering gates

- `tools/build_spirv.ps1 -Check`: pass; embedded SPIR-V is current.
- Debug `xpbd_viewport_regression_tests`: build and direct executable pass.
- Debug `xpbd_app_session_regression_tests`: build and direct executable pass,
  including new empty-scene defaults.
- Debug `xpbd_baker_app`: build pass.
- `git diff --check`: pass; existing line-ending notices only.
- Staged diff: empty.
- No Release build and no CTest were run, per user instruction.

The optional `XPBD_PT_CAPTURE=<png>` path is a bounded unattended diagnostic:
it writes one GPU readback PNG only after a finite nonzero maximum sample
limit. Each frame slot owns its buffer; the previous slot fence is waited
before host access, and shutdown releases the mapped buffer. It is not the
Phase 10 still-render workflow.

## Source protection

`<user-test-assets>\huilin` remained read-only. The normalized suite
under `.tmp` retained exact source bytes:

- base: `0B9859DE934C9634359BA53FC943BA7F2226A07388DEAAC81E32745BA060912F`
- normal: `86EE943FCFF2579839D4F8B802994C2CA600AB4F5BA5A9D013D305E9F45D539B`
- specular: `8886D939E79237AF9B65002BFB8E5A7A0C29CFF898209EAFAB2A50CC9D401C6B`

## Frozen boundaries

- Phase 5 supports direct hits on emitters. Emissive-triangle distributions,
  mesh-light sampling, analytic-light NEE, and MIS belong to Phase 6.
- The narrow surviving-Blend dielectric bridge validates transmission here.
  Unified primary/shadow/emitter transparency and fractional stacking belong
  to Phase 7.
- The temporary analytic environment remains a safety path. Full procedural
  environment, user-HDRI importance sampling, celestial lighting, and clouds
  belong to Phase 6; their UI belongs to Phase 10.
- `max_transparent_bounces` is normalized, persisted, and history-keyed now,
  but its full consumption belongs to Phase 7.
