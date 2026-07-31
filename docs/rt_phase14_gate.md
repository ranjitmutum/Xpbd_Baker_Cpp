# RT Phase 14 Gate: Reflex and DLSS Frame Generation

## Outcome

Phase 14 integrates NVIDIA Streamline 2.12.0 Reflex, PCL, and DLSS Frame
Generation into the Vulkan path-tracing preview. DLSS Super Resolution and
Ray Reconstruction retain their accepted reconstruction paths. NRD/NRI is no
longer a runtime, build, shader, deployment, or UI dependency.

## Official runtime contract

- Streamline is initialized before Vulkan and all mandatory Vulkan presentation
  entry points use the Streamline manual-hooking proxies.
- DLSS-G, Reflex, and PCL are requested during `slInit`; feature requirements,
  Vulkan support, and the selected adapter are checked before availability is
  exposed.
- DLSS-G remains user opt-in. Because requested Streamline features initially
  load by default, the plugin is explicitly unloaded after device discovery
  and before the first application swapchain. It is loaded only for a manual
  FG request.
- Production Streamline, NGX, and low-latency binaries are Authenticode
  validated before loading and are shipped beside the application.
- One application frame token is shared by Reflex Sleep, PCL markers,
  `slSetConstants`, SR/RR evaluation, FG tags, and intercepted Present.
- Reflex defaults to On and supports Off, On, and On + Boost. `slReflexSleep`
  and PCL markers remain active even when the user selects Off.
- The SDL Win32 message hook forwards the official PCL latency-ping message
  using the current application frame token.

## Frame Generation inputs and lifecycle

- Depth is non-inverted Vulkan device depth in `[0,1]`.
- Dense RG32F motion vectors include camera and animated-object motion, use the
  current-to-previous convention, and are normalized by
  `{1/renderWidth, 1/renderHeight}` in common constants.
- The full scene is copied to a swapchain-sized HUD-less image after the
  path-traced result and white/yellow 3D selection guides are drawn.
- Nuklear overlay UI is rendered separately into a transparent,
  premultiplied UI Color+Alpha image and recomposited onto the intercepted
  backbuffer.
- Depth, motion, HUD-less color, UI Color+Alpha, and the null-resource
  backbuffer subrect tag use `eValidUntilPresent`.
- Frames without valid RT depth/motion/color explicitly submit null FG tags.
- FG-only frames provide common constants once; SR/RR+FG frames reuse the
  constants already submitted by reconstruction.
- Enabling/disabling follows NVIDIA's section-18 ordering: disable FG, drain
  GPU/present work, destroy the old swapchain, load or unload the DLSS-G
  feature, then create the replacement swapchain. Plugin-owned entry points
  are reacquired after every load.
- Resizing, minimizing, file dialogs, VSync changes, and F11 borderless
  transitions disable FG before swapchain manipulation and recreate the
  swapchain.
- Streamline 2.12 does not support VSync with Vulkan FG. A manual FG request
  therefore turns the visible VSync setting Off before the hooked swapchain
  is created. FG availability additionally requires the replacement
  swapchain to use `VK_PRESENT_MODE_IMMEDIATE_KHR`; FIFO or mailbox fallbacks
  remain native presentation rather than entering an unsupported FG mode.
- G-SYNC/VRR remains a driver-and-display feature rather than an application
  toggle. With application VSync off and immediate presentation selected, the
  NVIDIA driver may apply G-SYNC when it is enabled for the app's window mode.
- The conservative Vulkan queue mode blocks the presenting client queue.
  Availability is restricted to a shared graphics/present queue so tagged
  resources cannot be reused on an unsynchronized non-presenting queue.
- The current UI guide path is restricted to 8-bit RGBA/BGRA swapchains,
  avoiding unsupported FP16/scRGB color and insufficient two-bit UI alpha.
- `DLSSGState::status`, minimum dimensions, generated-frame support, and actual
  presented frame count are checked. The UI reports original application FPS
  and actual DLSS-FG presented FPS separately.

## SR/RR re-audit

- SR tags render-resolution RGBA16F HDR color, R32F depth, RG32F motion, and a
  separately allocated full-resolution RGBA16F output.
- The user-facing modes are exactly Off, DLAA, Quality, Balanced, Performance,
  and Ultra Performance. Official presets are K/K/K/M/L.
- Jitter is supplied in input-pixel units. Projection constants are converted
  from the application's OpenGL convention to Vulkan Y and depth conventions
  before being passed as row-vector Streamline matrices.
- RR continues to provide linear diffuse/specular albedo,
  packed normal/linear roughness, and world-space R32F specular hit distance.
- Still Render remains raw, full-resolution accumulation with SR, RR, Reflex,
  and FG disabled.

## User-facing fixes included in this gate

- Still Render controls use unique Nuklear widget identities and release stale
  filename text focus before pointer interaction.
- Parent-to-child bone pivot connection lines are no longer rendered.
  Joint markers and deliberate white/yellow selection outlines remain.
- F11 toggles borderless desktop fullscreen and restores the previous
  windowed geometry/maximized state.

## Verification

- Serialized Debug build:
  `cmake --build out/build/vscode-windows-app --config Debug --target
  xpbd_baker_app --parallel 1`
- `xpbd_viewport_regression_tests.exe`: passed.
- `xpbd_app_session_regression_tests.exe`: passed.
- The maintained viewport suite explicitly verifies that DLSS Frame
  Generation defaults to Off.
- English and Simplified Chinese JSON resources parse as UTF-8.
- Required Streamline/NGX/Reflex binaries are present and have valid NVIDIA
  Authenticode signatures.
- Static scans found no NRD/NRI implementation or deployment reference; only
  legacy setting migration, regression assertions, and historical gate
  documentation remain.
- `git diff --check` reported no whitespace errors in the changed Phase 14
  sources.

The application itself was not launched during this gate, as requested.
Hardware FG/Reflex QA therefore remains a user-run verification on a supported
NVIDIA system; this gate claims source, build, package, and non-GUI regression
conformance.

## User-run startup finding

The first pre-gate desktop package reached the first intercepted Vulkan
`Present()` and then stopped producing frame logs. Its log showed a redundant
same-frame `slDLSSGSetOptions(eOff)` call and a 0x0 optional Backbuffer extent
while DLSS-G was loaded despite the user-facing default being Off. The final
source removes the redundant options call, supplies a validated Backbuffer
extent for null-tag frames, and—most importantly—keeps the DLSS-G plugin
unloaded until the user explicitly enables FG.
