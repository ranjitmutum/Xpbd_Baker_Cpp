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

## Quick start

# Library + tests + CLI
.\cpp\build.bat

# Same + fixture bake smoke
.\cpp\verify.bat

# Desktop app
.\cpp\run_app.bat
.\cpp\run_app.bat -gl
.\cpp\run_app.bat -vk
.\cpp\run_app.bat -d3d
```

Headless bake:

```powershell
.\cpp\run_cli.bat bake --model model.geo.json --anim idle.animation.json --out idle.baked.json --bones mid,tip
```

## Graphics backends

| Flag / env                | Backend                |
|---------------------------|------------------------|
| `-gl` / `XPBD_GFX=opengl` | OpenGL 3.3             |
| `-vk` / `XPBD_GFX=vulkan` | Vulkan (FIFO vsync)    |
| `-d3d` / `XPBD_GFX=dx11`  | Direct3D 11            |
| `auto`                    | Vulkan → DX11 → OpenGL |

The viewport is a GPU mesh preview. Use the toolbar to import a PNG/JPEG texture. Right-hand **Options**: show bones,
MCBE coordinates, UI language.

Camera:

- LMB drag — orbit
- RMB drag — pan on the ground plane (forward/back/left/right)
- MMB or Shift+RMB — height
- Wheel — zoom; Shift+wheel — height

## UI language

On startup the UI picks a language from the OS display language:

- English (default fallback)
- Simplified Chinese
- Traditional Chinese (Hong Kong)
- Traditional Chinese (Taiwan)

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
