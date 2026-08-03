# XPBD Baker (C++)

C++ port of the XPBD Bone Baker for Minecraft Bedrock physics animation baking. The Java tree in this repository remains
the behavioral reference; this directory is a standalone C++23 build.

## Credits

| Role            | Name                                          |
|-----------------|-----------------------------------------------|
| C++ port        | 卡门线                                        |
| Repository      | https://github.com/ranjitmutum/xpbd_baker     |

Thanks to the authors of Nuklear, SDL, Bullet, stb, spdlog, and other open-source projects used here. Full third-party
texts ship in the **`notices/`** folder next to the built app (see below).

中文说明：[README.md](README.md)

## Requirements

- Windows x64 (primary target)
- CMake 3.21+
- MSVC 2022+ or another supported C++23 toolchain
- [vcpkg](https://vcpkg.io/) with `VCPKG_ROOT` set

Source builds with DLSS SR/RR/FG and Reflex require the official NVIDIA
Streamline 2.12.0 SDK. Configure CMake with
`-DXPBD_STREAMLINE_SDK_ROOT=<sdk-root>`. SDK headers, DLLs, and licenses are
not committed to this repository; runnable packages contain only the
signature-verified production DLLs and their licenses.

## Quick start

```powershell
cmake --preset vscode-windows-app
cmake --build --preset vscode-windows-app-release

# Vulkan raster + RT
.\out\build\vscode-windows-app\Release\xpbd_baker_app.exe -vk

# OpenGL 3.3 raster fallback
.\out\build\vscode-windows-app\Release\xpbd_baker_app.exe -gl
```

Headless bake:

```powershell
cmake --build out\build\vscode-windows-app --config Release --target xpbd_cli
.\out\build\vscode-windows-app\Release\xpbd_cli.exe bake --model model.geo.json --anim idle.animation.json --out idle.baked.json --bones mid,tip
```

## Graphics backend

The app retains only **Vulkan** and **OpenGL 3.3**. Vulkan provides raster and
hardware RT rendering; OpenGL is the raster fallback. DX11 and Metal have been
removed.

| Flag / env                  | Notes                              |
|-----------------------------|------------------------------------|
| `-vk` / `XPBD_GFX=vulkan`   | Force Vulkan                       |
| `-gl` / `XPBD_GFX=opengl`   | Force OpenGL 3.3                   |
| `auto` (default)            | Try Vulkan, then fall back to GL   |
| `XPBD_VULKAN_VALIDATION=1`  | Request Khronos Validation; log and fall back safely if unavailable |
| `XPBD_VULKAN_DIAGNOSTICS=1` | Log Vulkan layers, extensions, and waits |
| `XPBD_ANIMATION=<path>`      | Load an animation at startup for unattended validation |
| `XPBD_AUTOPLAY=1`            | Autoplay after model and animation load successfully |
| `XPBD_SHOW_BONES=0`          | Hide bone overlays for unattended RT validation |
| `XPBD_RT_DEBUG=<mode>`       | RT Pipeline view: `instance`/`primitive`/`cube`/`face`/`material`/`normal` |
| `XPBD_PT_SPP=<1..64>`        | Path samples appended per frame slot per frame |
| `XPBD_PT_MAX_SAMPLES=<n>`    | Maximum samples per independent history; `0` is unlimited |
| `XPBD_PT_BOUNCES=<1..64>`    | Maximum total path-bounce count |
| `XPBD_PT_DIFFUSE_BOUNCES=<0..16>` | Maximum diffuse bounces |
| `XPBD_PT_GLOSSY_BOUNCES=<0..16>` | Maximum glossy/GGX bounces |
| `XPBD_PT_TRANSMISSION_BOUNCES=<0..32>` | Maximum transmission/refraction bounces |
| `XPBD_PT_TRANSPARENT_BOUNCES=<0..64>` | Maximum transparent bounces (fully consumed in Phase 7) |
| `XPBD_PT_RR=<0|1>`           | Enable Russian roulette |
| `XPBD_PT_RR_START=<1..Bounces>` | Bounce where Russian roulette starts |
| `XPBD_PT_SEED=<uint32>`      | Fixed deterministic sampling seed |
| `XPBD_PT_ENVIRONMENT=<0..16>` | Temporary analytic clear-sky strength; default `0` (Sky Rendering Off) |
| `XPBD_PT_CAPTURE=<png>`      | Unattended diagnostic: write one PT PNG after reaching a finite nonzero `XPBD_PT_MAX_SAMPLES`; not the still-render workflow |

On supported NVIDIA hardware, enable **Advanced preview lighting (NVIDIA)** in
Options to use the unified RT primary path. Model, ground, water, and preview
environment triangles share one alpha-aware BVH; opaque, cutout, and blended
materials retain distinct visibility and shadow behavior.

DLSS Frame Generation defaults to Off and its plugin is loaded only after a
manual request. Vulkan FG turns the application's own VSync off and activates
only when the swapchain can use `VK_PRESENT_MODE_IMMEDIATE_KHR`. G-SYNC/VRR
remains controlled by the NVIDIA driver and display; for F11 borderless mode,
enable G-SYNC for windowed and full-screen mode in NVIDIA Control Panel. The
application does not modify driver settings.

To avoid driver instability from third-party overlays inserted into the Vulkan
call chain, the app disables the GamePP and RTSS implicit Vulkan layers for its
own process by default. This does not change system settings or uninstall
anything. Set `XPBD_VULKAN_ALLOW_THIRD_PARTY_LAYERS=1` to opt back in.

The viewport is a GPU mesh preview. Use the toolbar to import a PNG/JPEG texture. Right-hand **Options**: show bones,
MCBE coordinates, UI language.

Camera:

- LMB drag — orbit
- RMB drag — pan on the ground plane (forward/back/left/right)
- MMB or Shift+RMB — height
- Wheel — zoom; Shift+wheel — height

## UI language

On startup the UI picks a language from the OS display language:

- English
- Simplified Chinese (default)

You can override it under Options.

## CLI flags (bake)

| Flag                      | Meaning                      |
|---------------------------|------------------------------|
| `--model`                 | Geometry JSON                |
| `--anim`                  | Animation JSON               |
| `--out`                   | Baked output                 |
| `--bones a,b`             | Physics bones (default: all) |
| `--mode xpbd\|bullet`     | Solver                       |
| `--loop auto\|once\|loop` | Loop policy                  |
| `--velocity path`         | Velocity cache               |
| `--dt`                    | Step size (default 1/60)     |
| `--assume-molang-zero`    | Explicitly sample unsupported position/rotation Molang as zero for this headless bake; scale checks still apply |

Fixtures: `tests/fixtures/chain.geo.json`, `chain.animation.json`.

## Molang and selected-only baking

Export replaces only `position` and `rotation` on selected physics bones with numeric baked keys. Every channel on unselected bones, plus `scale` on selected bones, is serialized exactly from the authored source animation.

Before baking, the GUI checks the source animation and any active transition target:

- Position/rotation Molang on an unselected ancestor or collision dependency is listed as a physics-only neutral-zero sample; the authored Molang remains unchanged in the export.
- Position/rotation Molang on a selected bone is listed by animation, bone, and channel as an expression that numeric baked keys will replace.
- Confirmation is valid for the immediately following bake only. Cancelling or finishing restores the safe default.
