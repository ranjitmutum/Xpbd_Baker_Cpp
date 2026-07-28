# tools/

Small helper scripts that **are** versioned in git. Large third-party binaries are **not**.

| Path | Tracked? | Purpose |
| --- | --- | --- |
| `export_i18n_json.py` | yes | Optional helper for i18n dumps |
| `setup_tools.ps1` / `setup_tools.bat` | yes | Download/extract glslang for SPIR-V rebuilds |
| `build_spirv.ps1` / `build_spirv.bat` | yes | Deterministically rebuild or verify embedded Vulkan SPIR-V |
| `glslang/` | **no** | Prebuilt glslangValidator (multi-MB) |
| `glslang.zip` | **no** | Cached download |

## Install binaries

From `cpp/`:

```powershell
.\tools\setup_tools.bat
# or
powershell -File .\tools\setup_tools.ps1
```

This fetches Khronos **main-tot** Windows Release zip and unpacks under `tools/glslang/bin/glslangValidator.exe`.

## Rebuild Vulkan shaders

After changing a GLSL file under `src/gfx/spirv`, rebuild all checked-in
`.spv` binaries and their embedded `.spv.inc` arrays:

```powershell
.\tools\build_spirv.bat
```

To verify that both checked-in forms match the GLSL sources without modifying
them:

```powershell
.\tools\build_spirv.bat -Check
# equivalent CMake target for an existing build tree
cmake --build build-mingw-release --target xpbd_check_spirv
```

The command uses the locally installed `tools/glslang/bin/glslangValidator.exe`; it never downloads tools.

To regenerate them through CMake, run
`cmake --build build-mingw-release --target xpbd_rebuild_spirv`. A stale check
failure means the GLSL, `.spv`, and `.spv.inc` files disagree; run the rebuild
command and rebuild the application.

## Vulkan static UV A/B

The Vulkan viewport uses the static indexed UV path by default. OpenGL and
Direct3D keep the legacy dynamic path. To force the Vulkan legacy path for an
A/B comparison:

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
