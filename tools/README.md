# tools/

Small helper scripts that **are** versioned in git. Large third-party binaries are **not**.

| Path | Tracked? | Purpose |
| --- | --- | --- |
| `export_i18n_json.py` | yes | Optional helper for i18n dumps |
| `setup_tools.ps1` / `setup_tools.bat` | yes | Download/extract glslang for SPIR-V rebuilds |
| `spirv_tool.py` | yes | Cross-platform rebuild, hash, and stale-artifact verification |
| `spirv_manifest.json` | yes | SHA-256 contract for GLSL, includes, `.spv`, and `.spv.inc` files |
| `build_spirv.ps1` / `build_spirv.bat` | yes | Legacy Windows-only rebuild/check helper |
| `glslang/` | **no** | Prebuilt glslangValidator (multi-MB) |
| `glslang.zip` | **no** | Cached download |

## Install binaries

From the repository root:

```powershell
.\tools\setup_tools.bat
# or
powershell -File .\tools\setup_tools.ps1
```

This fetches Khronos **main-tot** Windows Release zip and unpacks under `tools/glslang/bin/glslangValidator.exe`.

## Verify and rebuild Vulkan shaders

`spirv_tool.py` is the canonical shader entry point. It needs Python 3.9+ and
`glslangValidator`; it uses `tools/glslang/bin/glslangValidator.exe` when that
bundled Windows tool exists, and otherwise searches `PATH`. `GLSLANG_VALIDATOR`
or `--glslang` can select another installation.

Verify the entire checked-in chain without modifying it:

```sh
python3 tools/spirv_tool.py check
```

The check compiles every supported GLSL stage to a temporary directory and
byte-compares it with the checked-in `.spv`. It also decodes every
`.spv.inc` as little-endian 32-bit words, compares those exact words with the
`.spv`, and validates all source/artifact SHA-256 values in
`spirv_manifest.json`. Text hashes normalize CRLF/CR to LF so the manifest is
stable across Git checkout policies; `.spv` hashes cover the raw binary bytes.
Quoted GLSL includes are followed recursively.

After changing a shader, atomically rebuild every `.spv` and `.spv.inc`, then
refresh the hash manifest:

```sh
python3 tools/spirv_tool.py rebuild
```

If another trusted tool already regenerated both artifact forms, validate them
against a clean compile before updating only the manifest:

```sh
python3 tools/spirv_tool.py refresh-manifest
```

On Windows, use `python` instead of `python3` when that is the installed
command. The older PowerShell wrapper remains available, but it does not update
the hash manifest; follow it with `spirv_tool.py refresh-manifest`.

Every compile passes `--spirv-val` to `glslangValidator`, so its bundled
SPIRV-Tools validator always validates the generated module. When a standalone
`spirv-val` is installed, the script also finds it on `PATH` (or via
`SPIRV_VAL` / `--spirv-val`) and runs a second validation pass. Pass
`--require-spirv-val` when absence of that standalone executable must be an
error.

The same workflow is exposed through CMake:

```sh
cmake --build <build-dir> --target xpbd_check_spirv
cmake --build <build-dir> --target xpbd_rebuild_spirv
```

`XPBD_CHECK_SPIRV=ON` adds the consistency check to the default build. The
`s00-gate` configure/build/test presets enable it, and the test suite also runs
both the repository check and a synthetic stale-source/stale-binary/stale-include
regression. This is the CI/check-preset setting; ordinary developer builds keep
the option off by default.

CMake discovers Python with `find_package(Python3)` and discovers bundled or
system `glslangValidator` and `spirv-val`. Explicit paths can be supplied when
needed:

```sh
cmake -S . -B build -DXPBD_CHECK_SPIRV=ON \
  -DPython3_EXECUTABLE=/path/to/python3 \
  -DXPBD_GLSLANG_VALIDATOR=/path/to/glslangValidator \
  -DXPBD_SPIRV_VAL=/path/to/spirv-val
```

If Python or glslang is unavailable while `XPBD_CHECK_SPIRV=ON`, configure fails
with the missing tool and override variable. With the option off, CMake prints a
status message and ordinary application builds continue. No verification or
rebuild command downloads tools.

## Vulkan static UV A/B

The Vulkan viewport uses the static indexed UV path by default. To force the
legacy dynamic UV path for an A/B comparison:

```powershell
$env:XPBD_GFX = "vulkan"
$env:XPBD_VULKAN_LEGACY_UV = "1"
.\build-mingw-release\xpbd_baker_app.exe
Remove-Item Env:XPBD_VULKAN_LEGACY_UV
```

GPU bake is not implemented as a default execution path. Any future automatic
GPU bake remains gated on usable FP64 with numerical parity, at least **1.5x
end-to-end speedup** including transfers/readback, and graphics-frame p95
remaining within its approved budget. Until all gates pass, baking stays on
the CPU path rather than silently reducing precision or taking over the
graphics queue.
