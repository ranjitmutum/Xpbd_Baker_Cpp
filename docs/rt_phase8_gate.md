# RT Phase 8 gate — Preview, Renderer parameters, and UI separation

Status: **pass** (2026-07-30, Debug).

## Delivered contract

- `SceneSelectionState` is the authoritative app-level scene identity:
  `Empty`, `Preset`, `UserBuilt`, or `Loaded`.
- A new session starts in `Empty`. Loading a model makes it `Loaded` unless
  the user already selected a preset. Returning to `Empty` only suppresses
  render submission; it does not destroy the loaded geometry or source path.
- Curated presets commit through `selectPresetScene`. Invalid/non-curated
  values are rejected without mutating the previous selection.
- Scene settings use versioned `xpbd-scene-selection/1` JSON and round-trip
  selection, preset, source identity, grid, axes, and dynamic-surface state.
- `SceneSelection` never calls or mutates `setSkyRendering`. Preview preset
  assets and the user-owned World/Sky source remain separate controls.
- The Physics page owns SceneSelection, the Renderer page owns
  raster/path-tracing/integrator/post settings, and the Sky page owns the one
  persisted Sky Rendering option.
- The renderer consults `sceneRendersLoadedContent()` before submitting
  dynamic or static model data. `Empty` therefore submits neither retained
  model geometry nor a preset.
- Logical UI viewport coordinates cross one explicit
  `logicalViewportToFramebuffer` DPI boundary. Per-axis DPI, resize clipping,
  and collapsed one-pixel targets are deterministic.
- Vulkan path tracing renders into preview-sized offscreen targets and
  composites them before the overlay UI pass. UI pixels are not path-tracer
  input and do not enter accumulated history.

## Change classification

| Change | Required work |
| --- | --- |
| Film/exposure/background display | Display-only; keep raw PT history |
| SPP / maximum samples | Sampling schedule only |
| Integrator, physical light, or material | Reset accumulation |
| Preview resolution / DPI extent | Recreate preview target and history |
| Scene identity / loaded geometry | Reset history; AS work follows geometry policy |
| Ordinary UI layout / UI pixels | No shader or AS rebuild |

Renderer presets, Custom/source-preset restoration, requested/active
fallbacks, persistence, immutable render snapshots, and the complete control
classification remain covered by `docs/rt_path_tracing_controls_gate.md`.

## Automated evidence

- Debug `xpbd_app_session_regression_tests`: pass.
  - explicit Empty default;
  - curated preset transaction and invalid-preset rollback;
  - Scene/Sky generation independence;
  - Preset/custom JSON round-trip;
  - Empty retains loaded model data;
  - Loaded/User-Built restoration;
  - invalid JSON state preservation.
- Debug `xpbd_viewport_regression_tests`: pass.
  - exact mixed 1.5x/2.0x DPI conversion;
  - framebuffer clipping after resize;
  - invalid DPI and collapsed-viewport one-pixel fallback;
  - all existing viewport, RT, LabPBR, transparency, sky, and material tests.
- English/Simplified Chinese locale parity: **599/599**.
- `git diff --check`: pass; line-ending notices only.
- Debug `xpbd_baker_app`: build pass.

## Hardware evidence

Evidence directory:
`.tmp/phase8-scene-ui-20260730-1`.

- GPU: NVIDIA GeForce RTX 4090 Laptop GPU; Vulkan RT Pipeline ready.
- Empty + loaded model: 6,146 frames completed with empty stderr and no
  current-run RT-AS event or PT capture, demonstrating that retained model
  data was not submitted while SceneSelection was Empty.
- Ocean preset + loaded `白水绘/models/main.json`: 509 BLAS, 509 instances,
  one TLAS, and a 590x579 offscreen path-traced capture at exactly 32 samples.
- `preset-ocean-aov.json`: zero non-finite statistics, zero negative-variance
  pixels, sample range `[32, 32]`.
- `preset-ocean-pt.png`: 1,367,187 bytes and contains only the preview render;
  no Nuklear UI or HUD pixels are present.
- Both hardware stderr logs are empty. Shutdown waits completed successfully.

## Frozen boundaries

- This phase does not create the Phase 10 still-render job. `XPBD_PT_CAPTURE`
  remains a bounded diagnostic readback only.
- Scene presets may carry a preview background asset. That asset is not the
  user's World/Sky selection and never changes its persisted mode or controls.
- NRD and DLSS integration remains a later phase; Phase 8 only preserves the
  requested/active UI and fallback contract.
