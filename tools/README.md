# tools/

Small helper scripts that **are** versioned in git. Large third-party binaries are **not**.

| Path | Tracked? | Purpose |
| --- | --- | --- |
| `gen_ui_glyphs.py` | yes | Rebuild `src/app/ui_glyph_ranges.inc` from i18n + sources |
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

## Release performance checks

Run performance evidence from a Release build. These examples exercise each
benchmark independently and save the machine-specific JSON result:

```powershell
.\build-mingw-release\xpbd_perf_benchmark.exe --mode uv --cubes 1000 --warmup 5 --samples 30 --output perf_uv.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode xpbd --particles 512 --xpbd-steps 60 --warmup 5 --samples 30 --output perf_xpbd.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode reconstruct --bones 1000 --warmup 5 --samples 30 --output perf_reconstruct.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode bake --bones 128 --xpbd-steps 60 --warmup 3 --samples 10 --output perf_bake.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode rigid --bones 144 --xpbd-steps 600 --rigid-substeps 4 --diagnostics contacts --warmup 3 --samples 10 --output perf_rigid.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode frame-layout --bones 144 --xpbd-steps 600 --warmup 3 --samples 10 --output perf_frame_layout.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode audit --bones 128 --warmup 5 --samples 30 --output perf_audit.json
.\build-mingw-release\xpbd_perf_benchmark.exe --mode simd --values 262144 --kernel-iterations 16 --warmup 5 --samples 30 --output perf_simd.json
```

The `reconstruct` report separates compile+reconstruct, the compatible
compiled return-by-value wrapper, and caller-owned `reconstructInto` Scratch
timings.

Treat timings as hardware-specific evidence, not CI assertions. A SIMD kernel
may enter production `Auto` selection only after demonstrating at least
**1.25x kernel speedup** and **1.15x end-to-end speedup**. The dense
scaled-add pilot measured only about 1.02-1.11x and remains outside the
solver's production `Auto` path. The exact SAT projection kernel passed both
gates on the reference machine (2.64x scalar microkernel, 1.78x final-audit
end to end), so final rigid-body collision audits use its SSE2 implementation.
At this eight-vertex call granularity SSE2 was faster than AVX2; dispatch is
kernel-specific rather than assuming the widest ISA always wins.

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

## Rebuild UI glyph atlas

After adding Chinese (or other non-Latin) UI strings:

```powershell
python .\tools\gen_ui_glyphs.py
# then rebuild the app so the font atlas is re-baked
```
