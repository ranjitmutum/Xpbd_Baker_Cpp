#pragma once

#include "xpbd/gfx/viewport_mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xpbd::gfx {

// Blockbench-style viewport preview scene (Vulkan raster path).
enum class PreviewSceneId : std::uint8_t {
  None = 0,
  Studio = 1,
  Sky = 2,
  Night = 3,
  Sunset = 4,
  Dawn = 5,
  Space = 6,
  End = 7,
  Desert = 8,
  Ocean = 9,
  Storm = 10,
  Overcast = 11,
};

inline constexpr int kPreviewSceneCount = 12;

// Stable serialized values remain above, while the UI exposes only this
// curated CC0-backed set.
inline constexpr int kPreviewSceneChoiceCount = 8;
inline constexpr std::array<PreviewSceneId, kPreviewSceneChoiceCount>
    kPreviewSceneChoices{
        PreviewSceneId::None,     PreviewSceneId::Studio,
        PreviewSceneId::Sky,      PreviewSceneId::Night,
        PreviewSceneId::Sunset,   PreviewSceneId::Desert,
        PreviewSceneId::Ocean,    PreviewSceneId::Overcast,
    };

// Bump when skybox conversion, face size, mapping, or bundled content changes.
inline constexpr std::uint64_t kPreviewSkyboxContentVersion = 14;

[[nodiscard]] constexpr PreviewSceneId
canonicalPreviewSceneId(PreviewSceneId id) noexcept {
  switch (id) {
  case PreviewSceneId::Dawn:
    return PreviewSceneId::Sunset;
  case PreviewSceneId::Space:
  case PreviewSceneId::End:
    return PreviewSceneId::Night;
  case PreviewSceneId::Storm:
    return PreviewSceneId::Overcast;
  default:
    return id;
  }
}

[[nodiscard]] constexpr const char *previewSceneIdKey(PreviewSceneId id) {
  switch (id) {
  case PreviewSceneId::Studio:
    return "preview_scene_studio";
  case PreviewSceneId::Sky:
    return "preview_scene_sky";
  case PreviewSceneId::Night:
    return "preview_scene_night";
  case PreviewSceneId::Sunset:
    return "preview_scene_sunset";
  case PreviewSceneId::Dawn:
    return "preview_scene_dawn";
  case PreviewSceneId::Space:
    return "preview_scene_space";
  case PreviewSceneId::End:
    return "preview_scene_end";
  case PreviewSceneId::Desert:
    return "preview_scene_desert";
  case PreviewSceneId::Ocean:
    return "preview_scene_ocean";
  case PreviewSceneId::Storm:
    return "preview_scene_storm";
  case PreviewSceneId::Overcast:
    return "preview_scene_overcast";
  case PreviewSceneId::None:
  default:
    return "preview_scene_none";
  }
}

[[nodiscard]] constexpr PreviewSceneId
previewSceneIdFromIndex(int index) noexcept {
  if (index < 0) {
    return PreviewSceneId::None;
  }
  if (index >= kPreviewSceneCount) {
    return PreviewSceneId::Overcast;
  }
  return canonicalPreviewSceneId(static_cast<PreviewSceneId>(index));
}

[[nodiscard]] constexpr int previewSceneIndex(PreviewSceneId id) noexcept {
  return static_cast<int>(id);
}

[[nodiscard]] constexpr PreviewSceneId
previewSceneIdFromChoiceIndex(int index) noexcept {
  if (index < 0) {
    return kPreviewSceneChoices.front();
  }
  if (index >= kPreviewSceneChoiceCount) {
    return kPreviewSceneChoices.back();
  }
  return kPreviewSceneChoices[static_cast<std::size_t>(index)];
}

[[nodiscard]] constexpr int
previewSceneChoiceIndex(PreviewSceneId id) noexcept {
  switch (canonicalPreviewSceneId(id)) {
  case PreviewSceneId::Studio:
    return 1;
  case PreviewSceneId::Sky:
    return 2;
  case PreviewSceneId::Night:
    return 3;
  case PreviewSceneId::Sunset:
    return 4;
  case PreviewSceneId::Desert:
    return 5;
  case PreviewSceneId::Ocean:
    return 6;
  case PreviewSceneId::Overcast:
    return 7;
  case PreviewSceneId::None:
  default:
    return 0;
  }
}

[[nodiscard]] constexpr const char *
previewSceneAssetFilename(PreviewSceneId id) noexcept {
  switch (canonicalPreviewSceneId(id)) {
  case PreviewSceneId::Studio:
    return "studio_small_06_1k.hdr";
  case PreviewSceneId::Night:
    return "qwantani_night_puresky_1k.hdr";
  case PreviewSceneId::Sunset:
    return "qwantani_sunset_puresky_1k.hdr";
  case PreviewSceneId::Overcast:
    return "kloofendal_overcast_puresky_1k.hdr";
  case PreviewSceneId::Sky:
  case PreviewSceneId::Desert:
  case PreviewSceneId::Ocean:
    return "kloppenheim_03_puresky_1k.hdr";
  case PreviewSceneId::None:
  default:
    return "";
  }
}

// Only Ocean retains time-animated scene geometry. CC0 skybox assets are
// intentionally static and independent from physical World/Sky rendering.
[[nodiscard]] constexpr bool
previewSceneSupportsDynamic(PreviewSceneId id) noexcept {
  return canonicalPreviewSceneId(id) == PreviewSceneId::Ocean;
}

struct PreviewSceneLighting {
  std::array<float, 3> direction{0.35f, 0.85f, 0.40f};
  float ambient = 0.38f;
  std::array<float, 3> color{1.0f, 1.0f, 1.0f};
  float intensity = 0.85f;
  float clear_r = 26.0f / 255.0f;
  float clear_g = 28.0f / 255.0f;
  float clear_b = 34.0f / 255.0f;
};

// Matches mesh / static_mesh push-constant layout (vertex + fragment).
struct alignas(16) MeshScenePushConstants {
  float mvp[16]{};
  float light_dir[3]{0.35f, 0.85f, 0.40f};
  float ambient = 0.38f;
  float light_color[3]{1.0f, 1.0f, 1.0f};
  float intensity = 0.85f;
  std::uint32_t material_debug[4]{};
};
static_assert(sizeof(MeshScenePushConstants) == 112);
static_assert(alignof(MeshScenePushConstants) == 16);

// Static cubemap skybox transform (vertex + fragment pipeline layout).
struct alignas(16) SkyboxPushConstants {
  float mvp[16]{};
};
static_assert(sizeof(SkyboxPushConstants) == 64);
static_assert(alignof(SkyboxPushConstants) == 16);

// Procedural cubemap faces: order +X, -X, +Y, -Y, +Z, -Z.
struct PreviewSkybox {
  int face_size = 0;
  std::vector<std::uint8_t> rgba;
  std::uint64_t generation = 0;
  std::string source_identity;
  bool cc0_asset = false;

  [[nodiscard]] bool valid() const noexcept {
    if (face_size <= 0) {
      return false;
    }
    const std::size_t expected =
        static_cast<std::size_t>(face_size) *
        static_cast<std::size_t>(face_size) * 4u * 6u;
    return rgba.size() == expected;
  }

  void clear() {
    face_size = 0;
    rgba.clear();
    generation = 0;
    source_identity.clear();
    cc0_asset = false;
  }
};

struct ViewportRasterScene {
  PreviewSceneId id = PreviewSceneId::Studio;
  PreviewSceneLighting lighting{};
  ViewportGpuScene environment;
  ViewportGpuScene grid;
  PreviewSkybox skybox;
  bool show_grid = true;
  bool show_axes = true;
  bool show_environment = true;
  bool solid_ground = false;
  bool environment_unlit = true;
  // Incremented whenever environment triangle positions/colors/topology change.
  std::uint64_t geometry_generation = 0;
  // Randomised per scene switch (static + dynamic); drives noise offsets.
  float scene_seed = 0.0f;
  // Last surface mesh bake time (ocean/desert throttle).
  float surface_time_baked = -1.0e9f;
  bool surface_dynamic_baked = false;

  void clear() {
    environment.clear();
    grid.clear();
    skybox.clear();
    ++geometry_generation;
    scene_seed = 0.0f;
    surface_time_baked = -1.0e9f;
    surface_dynamic_baked = false;
  }
};

[[nodiscard]] PreviewSceneLighting
makePreviewSceneLighting(PreviewSceneId id, float time_sec = 0.0f,
                         bool dynamic = false,
                         float scene_seed = 0.0f) noexcept;

// Loads one of the fixed CC0 Radiance HDR assets and converts its 2:1
// equirectangular image to the six RGBA8 cubemap faces used by the preview.
// Transactional: `out` is unchanged on failure.
[[nodiscard]] bool
loadPreviewSceneSkyboxAsset(PreviewSceneId id,
                            const std::filesystem::path &asset_root,
                            PreviewSkybox &out,
                            std::string *error = nullptr);

// dynamic + time_sec animate supported local scene geometry (currently Ocean).
// The bundled CC0 skybox remains static. Reseeds scene_seed on scene/mode
// changes. An empty asset_root uses the procedural fallback for tests.
void buildViewportRasterScene(PreviewSceneId id, bool show_grid, bool show_axes,
                              bool dynamic, float time_sec,
                              ViewportRasterScene &out,
                              const std::filesystem::path &asset_root = {});

[[nodiscard]] std::array<float, 3>
previewLightDirectionFromSide(int light_side) noexcept;

[[nodiscard]] constexpr bool previewSceneUsesSkybox(PreviewSceneId id) noexcept {
  switch (id) {
  case PreviewSceneId::None:
    return false;
  default:
    return true;
  }
}

// Scenes that also build local ground / water geometry under the sky.
[[nodiscard]] constexpr bool
previewSceneHasSurfaceMesh(PreviewSceneId id) noexcept {
  return id == PreviewSceneId::Desert || id == PreviewSceneId::Ocean;
}

} // namespace xpbd::gfx
