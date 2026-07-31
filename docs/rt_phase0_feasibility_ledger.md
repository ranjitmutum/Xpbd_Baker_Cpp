# RT Phase 0 Feasibility and License Ledger

Frozen on 2026-07-29 for the corrected unattended RT plan. This ledger records
probe inputs; it does not grant permission to accept SDK terms or ship files.
All probe checkouts and binaries live under ignored `.tmp/phase0-deps`.

## Decisions

| Component | Frozen upstream | License boundary | Probe/build path | Result |
|---|---|---|---|---|
| NVIDIA NRD | `v4.17.3`; `9a3fe938a7558fd16b6c91a1c0456305cdcd9f16` | Custom NVIDIA RTX SDK license; use constitutes acceptance | Documentation-only comparison of `NRDIntegration + NRI` and a native Vulkan adapter | **External blocker.** Prefer a native Vulkan adapter for the existing resource/barrier model; use NRI only as a later conformance reference. No NRD/NRI file was downloaded or built. |
| NVIDIA Streamline | `v2.12.0`; `e8aaa6eaac968711fb62473d4ae8256dde20919b` | Repository source is MIT except `sl_nvperf.h/.dll`, which are under the Nsight Perf SDK license; feature binaries have separate package terms | Sparse source/header checkout; MSVC Debug header/no-op executable; manual-hooking review | **Core boundary viable; production features externally gated.** Featureless mode never initializes or proxies Streamline and passes native preview resources through unchanged. |
| Bruneton atmosphere | `34f14e745cff948f4ca3157d1b62a445ffa7286f` | BSD-3-Clause | `definitions.glsl` + `functions.glsl` compiled through a Vulkan 1.2 compute wrapper | **Adaptation required.** Reuse physical GLSL with notice; implement Vulkan LUT images, descriptors, synchronization, cache key/versioning, and dispatch locally. |
| Astronomy Engine C | `v2.1.19`; `865d3da7d8112bbc7911238052c6af4aaf877181` | MIT | MSVC Debug static library from `astronomy.c`; C++20 fixed-UTC observer probe | **Viable.** Vendor only the C source/header/license when Phase 6 starts and expose it through a renderer-owned celestial-state wrapper. |
| Meteoros / NUBIS | `63d1e22a05ea9cc01d95a8d32a0bd48e90eb2368`; NUBIS papers/presentations | Meteoros code is MIT; NUBIS is algorithm guidance, not a vendored library | Original compute shaders compiled for Vulkan 1.2; clean-room fixed-seed noise and independent Sun/Moon lighting shaders compiled | **Adaptation required.** Review modeling, lighting, compute, and temporal concepts only; use the application's own renderer, history rejection, and generated resources. |
| Unity URP Advanced Procedural Skybox | `6ed1c60a4f3efcc646d89f75694233cead71309c` | MIT source; visual assets have separate or unsuitable provenance | Source organization and README review only | **Reference only.** Implement stars, galaxy, moon surface, and cloud visuals in project-owned Vulkan GLSL. |

## Mandatory Exclusions

- Do not ship bundled HDRI files, a stars cubemap, moon photographs, Unity/HDRP
  cloud textures, Meteoros noise/weather textures, or its sample models.
- The Meteoros checkout contains 172 excluded texture/model files totaling
  66,302,081 bytes.
- The night-sky reference contains 8 excluded image/ShaderGraph files totaling
  5,394,599 bytes.
- Do not ship development NVIDIA DLLs, NGX libraries, Streamline feature
  plugins, `sl_nvperf.*`, OTA-downloaded plugins, NRD/NRI files, or RTXPT sample
  dependencies as a result of these probes.
- NUBIS presentation material is a design reference only.

## Streamline Vulkan Boundary

If a signed, authorized runtime package is later supplied, the adapter must:

1. Clear `eAllowOTA` and `eLoadDownloadedPlugins`.
2. Select Vulkan, manual hooking, frame-based resource tagging, and an explicit
   requested-feature list before Vulkan instance/device creation.
3. Query every enabled feature's instance extensions, device extensions,
   Vulkan 1.2/1.3 features, and queue requirements before device creation.
4. Securely verify/load the interposer and proxy only the required Vulkan
   instance/device/swapchain/present hooks.
5. Route the common present hook exactly once per frame, including resize paths,
   and shut Streamline down before Vulkan destruction.
6. Keep native-resolution UI and still rendering outside Streamline.

With no authorized package or no supported requested feature, the adapter does
none of the above and retains the native Vulkan path.

## Frozen Hashes

| File | SHA-256 |
|---|---|
| Bruneton `LICENSE` | `5EC3448A08DB57C46D0C7B24286D30A2941C0CB235BA6FB6640FA6B2BD698270` |
| Bruneton `definitions.glsl` | `6682DE618A277143BBA2643830AD9AFD6B915F19E7821AB418AFD7B6F8DD6C92` |
| Bruneton `functions.glsl` | `BDFBE3BA3D60A4879AE34392A28D8917F10338B462F5ADDA46625D48A6C47597` |
| Astronomy Engine `LICENSE` | `F74B2D1397AA155FE3774ADE1B55F3F1440AC3F5D885CB159B91EEBE087695C6` |
| Astronomy Engine `astronomy.c` | `DA392C2DE301503752E8C413CAFD91F3AE2F335E011D9950C17EABB76D2D8839` |
| Astronomy Engine `astronomy.h` | `3DF408D920266D9C3EB5B5ED1561D03CF1DE1CFF6FF23B5B5D6AEA19D2FB28D1` |
| Meteoros `LICENSE` | `268A46102EAB1F0A8321457A84ED8A2B50A25AFF601CEBF88BC0AD205A4A42B8` |
| Meteoros `cloudRayMarch.comp` | `6E8D7642683A014F4900D6A7D9AD0E6CCA676382F519AF4198375800A8C6D280` |
| Meteoros `reprojection.comp` | `2E657C4BA928B6902E0EE0CCAAB3D6993730FABF6D3017CBA05698AD19C195A7` |
| Night-sky `LICENSE.md` | `549FC83596910704E276AE58897162EAB47E218EC429044E2FF1C46D4165CB62` |
| Night-sky `ProcedualSkybox.hlsl` | `6BC0352B0365F14F15275A86F79FB60D41C1B38A6D1121FEA65315AF6AA2031D` |
| Streamline `license.txt` | `7B6F23E7D6F3AD6292F9308D2B42CDC3D82AE4E9B2ABB55F230279D83BEDD43D` |
| Streamline manual-hooking guide | `065CA2A99A7ED387A53E6DE6B9C01769922D5EDE61434458F5E9ECE6A8894B2D` |
| Streamline `sl.h` | `E1E81A7428D15B30DB37587E9469BD68A56D630D820A4ACCEC0AEE3B17E157DD` |
| Streamline `sl_core_types.h` | `D0A4BC9D2EFAC5AA2D6171EB93F10E59A856C334A786536C0A871608EE8A877F` |

NRD is identified by its frozen tag/commit and upstream license URL because
downloading or using its SDK is outside the current authorization.

## Notice and Packaging Rule

The probe-only components are not shipped and therefore do not add installed
license files yet. When an approved phase vendors source, copy its exact frozen
license into `notices/`, add it to `notices/README.txt`, preserve source
attribution, and re-audit the packaged file list. NVIDIA packages require a
separate authorization and license review before any download or packaging.

Upstreams:

- <https://github.com/NVIDIA-RTX/NRD>
- <https://github.com/NVIDIA-RTX/Streamline>
- <https://github.com/ebruneton/precomputed_atmospheric_scattering>
- <https://github.com/cosinekitty/astronomy>
- <https://github.com/AmanSachan1/Meteoros>
- <https://github.com/BxKangKi/Unity-URP-Advanced-Procedural-Skybox>
