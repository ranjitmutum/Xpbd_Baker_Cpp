XPBD Bone Baker (C++) — third-party notices
==========================================

This folder is copied next to the application binary at build time.
Keep it when redistributing the app.

Application license: Apache License 2.0 (see APACHE-2.0.txt).
Original author: ranjitmutum
C++ port: 卡门线
Project: https://github.com/ranjitmutum/xpbd_baker

Third-party components (summaries; see files / upstream for full text):
  - Nuklear ............... MIT / public domain style  (third_party/nuklear)
  - stb_image ............. MIT / public domain         (third_party/stb)
  - SDL3 .................. zlib                        (vcpkg sdl3)
  - nlohmann-json ......... MIT                         (vcpkg)
  - Bullet Physics ........ zlib                        (vcpkg bullet3; BULLET-ZLIB.txt)
  - spdlog / fmt .......... MIT                         (vcpkg)
  - Vulkan-Headers/Loader . Apache-2.0                  (vcpkg)
  - Astronomy Engine C ..... MIT                         (third_party/astronomy; ASTRONOMY_ENGINE.txt)
  - Bruneton Atmosphere .... BSD-3-Clause                (third_party/bruneton; BRUNETON_ATMOSPHERE.txt)
  - Poly Haven HDRIs ....... CC0                         (assets/preview_scenes; POLY_HAVEN_CC0.txt)
RT Phase 0 feasibility references are not compiled, linked, or shipped.
Their frozen revisions, license boundaries, excluded assets, and future notice
requirements are recorded in docs/rt_phase0_feasibility_ledger.md.

EUI-NEO under third_party/EUI-NEO is NOT linked into the shipping GUI.
