// Focused regression smoke tests for viewport mesh + texture helpers.
// Linked as xpbd_viewport_regression_tests (see CMakeLists.txt).

#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/rtxpt_bridge.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/labpbr_authoring.hpp"
#include "xpbd/gfx/labpbr_export.hpp"
#include "xpbd/gfx/labpbr_import.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/rt_scene_records.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
#include "xpbd/gfx/texture_image.hpp"
#include "xpbd/gfx/vulkan_queue_selection.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/gfx/world_environment.hpp"
#include "xpbd/baker/cube_geometry.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool cond, const char *label) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", label);
  }
}

void expectNear(float actual, float expected, float tolerance,
                 const char *label) {
  expect(std::abs(actual - expected) <= tolerance, label);
}

void expectNearDouble(double actual, double expected, double tolerance,
                      const char *label) {
  expect(std::abs(actual - expected) <= tolerance, label);
}

void testLogicalFramebufferViewportContract() {
  using xpbd::gfx::logicalViewportToFramebuffer;
  const auto mixed_dpi = logicalViewportToFramebuffer(
      10.0f, 20.0f, 300.0f, 200.0f, 1.5f, 2.0f, 1920, 1080);
  expect(mixed_dpi.x == 15 && mixed_dpi.y == 40 &&
             mixed_dpi.w == 450 && mixed_dpi.h == 400,
         "logical preview viewport maps exactly across per-axis DPI");

  const auto clipped = logicalViewportToFramebuffer(
      900.0f, 500.0f, 200.0f, 100.0f, 2.0f, 2.0f, 1920, 1080);
  expect(clipped.x == 1800 && clipped.y == 1000 &&
             clipped.w == 120 && clipped.h == 80,
         "framebuffer viewport clips safely after resize");

  const auto minimum = logicalViewportToFramebuffer(
      4.0f, 5.0f, 0.0f, -2.0f,
      std::numeric_limits<float>::quiet_NaN(), 0.0f, 64, 64);
  expect(minimum.x == 4 && minimum.y == 5 &&
             minimum.w == 1 && minimum.h == 1,
         "invalid DPI and collapsed UI viewport retain a one-pixel target");
}

void testVulkanQueueFamilySelection() {
  using xpbd::gfx::VulkanQueueFamilySupport;
  using xpbd::gfx::selectVulkanQueueFamilies;

  constexpr std::array laptop_queue_families{
      VulkanQueueFamilySupport{true, true},
      VulkanQueueFamilySupport{false, false},
      VulkanQueueFamilySupport{false, true},
  };
  constexpr auto shared =
      selectVulkanQueueFamilies(laptop_queue_families);
  static_assert(shared.valid() && shared.shared());
  static_assert(shared.graphics_family == 0u &&
                shared.present_family == 0u);
  expect(shared.shared() && shared.graphics_family == 0u,
         "Vulkan queue selection prefers the first shared graphics/present "
         "family");

  constexpr std::array split_queue_families{
      VulkanQueueFamilySupport{true, false},
      VulkanQueueFamilySupport{false, true},
  };
  constexpr auto split =
      selectVulkanQueueFamilies(split_queue_families);
  static_assert(split.valid() && !split.shared());
  expect(split.graphics_family == 0u && split.present_family == 1u,
         "Vulkan queue selection retains a valid split-family fallback");

  constexpr std::array unusable_queue_families{
      VulkanQueueFamilySupport{false, true},
  };
  constexpr auto unusable =
      selectVulkanQueueFamilies(unusable_queue_families);
  static_assert(!unusable.valid());
  expect(!unusable.valid(),
         "Vulkan queue selection rejects devices without graphics support");
}

// Minimal 1x1 opaque white PNG (generated: RGB 8-bit, single white pixel).
constexpr unsigned char kWhitePng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xFF, 0xFF, 0x3F,
    0x00, 0x05, 0xFE, 0x02, 0xFE, 0x0D, 0xEF, 0x46, 0xB8, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

void testTextureFromMemory() {
  xpbd::gfx::TextureImage img;
  std::string err;
  const bool ok = xpbd::gfx::loadTextureImageFromMemory(
      kWhitePng, static_cast<int>(sizeof(kWhitePng)), img, &err);
  expect(ok, "loadTextureImageFromMemory(1x1 png)");
  expect(img.valid(), "texture valid after load");
  expect(img.width == 1 && img.height == 1, "texture size 1x1");
  expect(img.source_channels == 3, "source RGB channel count retained");
  if (img.valid()) {
    float r = 0, g = 0, b = 0, a = 0;
    img.sample(0.5f, 0.5f, r, g, b, a);
    expect(r > 0.9f && g > 0.9f && b > 0.9f, "sample near white");
    expect(a > 0.9f, "sample alpha opaque");
  }
  expect(!err.empty() || ok, "error string only on failure");
}

void testCc0PreviewSceneAssets() {
  using xpbd::gfx::PreviewSceneId;
  using xpbd::gfx::ViewportRasterScene;
  using xpbd::gfx::canonicalPreviewSceneId;
  using xpbd::gfx::kPreviewSceneChoiceCount;
  using xpbd::gfx::loadPreviewSceneSkyboxAsset;
  using xpbd::gfx::previewSceneAssetFilename;
  using xpbd::gfx::previewSceneChoiceIndex;
  using xpbd::gfx::previewSceneIdFromChoiceIndex;

  expect(canonicalPreviewSceneId(PreviewSceneId::Dawn) ==
             PreviewSceneId::Sunset &&
             canonicalPreviewSceneId(PreviewSceneId::Space) ==
                 PreviewSceneId::Night &&
             canonicalPreviewSceneId(PreviewSceneId::End) ==
                 PreviewSceneId::Night &&
             canonicalPreviewSceneId(PreviewSceneId::Storm) ==
                 PreviewSceneId::Overcast,
         "retired preview presets map to curated stable replacements");
  for (int index = 0; index < kPreviewSceneChoiceCount; ++index) {
    const PreviewSceneId id = previewSceneIdFromChoiceIndex(index);
    expect(previewSceneChoiceIndex(id) == index,
           "curated preview scene index round-trips");
  }

  const std::filesystem::path asset_root =
      std::filesystem::path(XPBD_TEST_SOURCE_DIR) / "assets" /
      "preview_scenes";
  const std::array<PreviewSceneId, 5> asset_ids{
      PreviewSceneId::Studio, PreviewSceneId::Sky, PreviewSceneId::Night,
      PreviewSceneId::Sunset, PreviewSceneId::Overcast};
  for (const PreviewSceneId id : asset_ids) {
    const std::filesystem::path source =
        asset_root / previewSceneAssetFilename(id);
    expect(std::filesystem::is_regular_file(source),
           "bundled CC0 preview HDR exists");
    xpbd::gfx::PreviewSkybox skybox;
    std::string error;
    expect(loadPreviewSceneSkyboxAsset(id, asset_root, skybox, &error),
           "bundled CC0 preview HDR converts to cubemap");
    expect(skybox.valid() && skybox.face_size == 384 && skybox.cc0_asset &&
               skybox.source_identity == source.string(),
           "converted preview cubemap retains CC0 source identity");
    if (skybox.valid()) {
      std::uint8_t minimum = 255u;
      std::uint8_t maximum = 0u;
      std::uint8_t maximum_chroma = 0u;
      for (std::size_t pixel = 0; pixel + 3u < skybox.rgba.size();
           pixel += 4u) {
        const auto [lo, hi] =
            std::minmax({skybox.rgba[pixel + 0], skybox.rgba[pixel + 1],
                         skybox.rgba[pixel + 2]});
        minimum = std::min(minimum, lo);
        maximum = std::max(maximum, hi);
        maximum_chroma =
            std::max(maximum_chroma, static_cast<std::uint8_t>(hi - lo));
      }
      expect(minimum < maximum && maximum_chroma > 8u,
             "converted preview cubemap has finite visible range");
      if (id != PreviewSceneId::Studio) {
        const std::size_t face_pixels =
            static_cast<std::size_t>(skybox.face_size) * skybox.face_size;
        const std::size_t lower_offset = face_pixels * 4u * 3u;
        std::uint64_t lower_sum = 0u;
        for (std::size_t pixel = 0; pixel < face_pixels; ++pixel) {
          const std::size_t sample = lower_offset + pixel * 4u;
          lower_sum += skybox.rgba[sample + 0] +
                       skybox.rgba[sample + 1] +
                       skybox.rgba[sample + 2];
        }
        const double lower_average =
            static_cast<double>(lower_sum) /
            static_cast<double>(face_pixels * 3u);
        expect(lower_average > 12.0,
               "pure-sky cubemap synthesizes a non-black lower hemisphere");
      }
    }
  }

  ViewportRasterScene studio;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Studio, true, true,
                                      false, 0.0f, studio, asset_root);
  expect(studio.id == PreviewSceneId::Studio && studio.skybox.cc0_asset &&
             studio.environment.solid.empty() &&
             studio.environment.transparent.empty() && !studio.solid_ground &&
             !studio.show_environment,
         "CC0 Studio removes the old y=0 room geometry");

  ViewportRasterScene ocean;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, true,
                                      1.0f, ocean, asset_root);
  expect(ocean.skybox.cc0_asset && !ocean.dynamic_sky &&
             ocean.surface_dynamic_baked && ocean.show_environment,
         "dynamic Ocean retains a static CC0 sky and animated surface");

  constexpr std::size_t kDesertVertexCount = 256u * 256u * 6u;
  constexpr std::size_t kOceanVertexCount = 176u * 176u * 6u;
  ViewportRasterScene desert;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Desert, true, true,
                                      false, 0.0f, desert, asset_root);
  expect(desert.environment.solid.size() == kDesertVertexCount &&
             desert.environment.transparent.empty(),
         "FastNoiseLite Desert emits the high-resolution 256x256 topology");
  float desert_min_height = std::numeric_limits<float>::max();
  float desert_max_height = std::numeric_limits<float>::lowest();
  float desert_min_normal_y = 1.0f;
  bool desert_finite = true;
  bool desert_origin_clear = true;
  for (const xpbd::gfx::MeshVertex &vertex : desert.environment.solid) {
    desert_finite =
        desert_finite && std::isfinite(vertex.px) &&
        std::isfinite(vertex.py) && std::isfinite(vertex.pz) &&
        std::isfinite(vertex.nx) && std::isfinite(vertex.ny) &&
        std::isfinite(vertex.nz) && std::isfinite(vertex.r) &&
        std::isfinite(vertex.g) && std::isfinite(vertex.b) &&
        std::isfinite(vertex.a);
    desert_min_height = std::min(desert_min_height, vertex.py);
    desert_max_height = std::max(desert_max_height, vertex.py);
    desert_min_normal_y = std::min(desert_min_normal_y, vertex.ny);
    if (std::abs(vertex.px) <= 10.0f && std::abs(vertex.pz) <= 10.0f) {
      desert_origin_clear =
          desert_origin_clear && std::abs(vertex.py) < 0.35f;
    }
  }
  expect(desert_finite, "FastNoiseLite Desert vertices remain finite");
  expect(desert_max_height - desert_min_height > 12.0f &&
             desert_min_normal_y < 0.98f,
         "FastNoiseLite Desert has non-flat dune relief and normals");
  expect(desert_origin_clear,
         "FastNoiseLite Desert preserves the y=0 inspection area");
  const std::uint64_t desert_generation = desert.geometry_generation;
  const std::vector<xpbd::gfx::MeshVertex> frozen_desert =
      desert.environment.solid;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Desert, true, true,
                                      false, 25.0f, desert, asset_root);
  expect(desert.geometry_generation == desert_generation &&
             desert.environment.solid.size() == frozen_desert.size() &&
             desert.environment.solid.front().py ==
                 frozen_desert.front().py,
         "static Desert does not rebuild when only time advances");

  expect(ocean.environment.solid.size() == 6u &&
             ocean.environment.transparent.size() == kOceanVertexCount,
         "osgw Ocean keeps a six-vertex deep body and high-resolution 176x176 "
         "wave topology");
  float ocean_min_alpha = 1.0f;
  float ocean_max_alpha = 0.0f;
  bool ocean_finite = true;
  bool ocean_normals_unit = true;
  bool ocean_origin_clear = true;
  bool ocean_horizontal_displacement = false;
  constexpr float kOceanHalf = 180.0f;
  constexpr float kOceanStep = 360.0f / 176.0f;
  for (const xpbd::gfx::MeshVertex &vertex : ocean.environment.transparent) {
    ocean_finite =
        ocean_finite && std::isfinite(vertex.px) &&
        std::isfinite(vertex.py) && std::isfinite(vertex.pz) &&
        std::isfinite(vertex.nx) && std::isfinite(vertex.ny) &&
        std::isfinite(vertex.nz) && std::isfinite(vertex.r) &&
        std::isfinite(vertex.g) && std::isfinite(vertex.b) &&
        std::isfinite(vertex.a);
    const float normal_length =
        std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny +
                  vertex.nz * vertex.nz);
    ocean_normals_unit =
        ocean_normals_unit && std::abs(normal_length - 1.0f) < 2.0e-3f;
    ocean_min_alpha = std::min(ocean_min_alpha, vertex.a);
    ocean_max_alpha = std::max(ocean_max_alpha, vertex.a);
    if (vertex.px * vertex.px + vertex.pz * vertex.pz < 100.0f) {
      ocean_origin_clear = ocean_origin_clear && vertex.py < -0.20f;
    }
    const float grid_x = (vertex.px + kOceanHalf) / kOceanStep;
    const float grid_z = (vertex.pz + kOceanHalf) / kOceanStep;
    ocean_horizontal_displacement =
        ocean_horizontal_displacement ||
        (std::abs(grid_x - std::round(grid_x)) > 0.02f &&
         std::abs(grid_z - std::round(grid_z)) > 0.02f);
  }
  expect(ocean_finite && ocean_normals_unit,
         "osgw Ocean positions and analytic normals remain finite/unit");
  expect(ocean_min_alpha >= 0.69f && ocean_max_alpha <= 0.99f &&
             ocean_max_alpha - ocean_min_alpha > 0.01f,
         "osgw Ocean remains on the varied-alpha transparent route");
  expect(ocean_horizontal_displacement,
         "osgw Ocean applies horizontal Gerstner displacement");
  expect(ocean_origin_clear,
         "osgw Ocean keeps the y=0 model inspection area uncovered");

  const std::vector<xpbd::gfx::MeshVertex> ocean_at_one =
      ocean.environment.transparent;
  const std::uint64_t ocean_generation = ocean.geometry_generation;
  const std::vector<std::uint8_t> ocean_sky = ocean.skybox.rgba;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, true,
                                      1.2f, ocean, asset_root);
  std::size_t changed_ocean_vertices = 0u;
  for (std::size_t index = 0; index < ocean_at_one.size(); ++index) {
    const auto &before = ocean_at_one[index];
    const auto &after = ocean.environment.transparent[index];
    if (std::abs(before.px - after.px) > 1.0e-4f ||
        std::abs(before.py - after.py) > 1.0e-4f ||
        std::abs(before.pz - after.pz) > 1.0e-4f) {
      ++changed_ocean_vertices;
    }
  }
  expect(ocean.geometry_generation == ocean_generation + 1u &&
             ocean.environment.transparent.size() == ocean_at_one.size() &&
             changed_ocean_vertices > ocean_at_one.size() / 2u,
         "dynamic osgw Ocean advances positions with stable topology");
  expect(ocean.skybox.rgba == ocean_sky && ocean.skybox.cc0_asset &&
             !ocean.dynamic_sky,
         "dynamic osgw Ocean leaves the packaged CC0 sky unchanged");

  ViewportRasterScene static_ocean;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, false,
                                      0.0f, static_ocean, asset_root);
  const std::uint64_t static_ocean_generation =
      static_ocean.geometry_generation;
  const std::vector<xpbd::gfx::MeshVertex> frozen_ocean =
      static_ocean.environment.transparent;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, false,
                                      30.0f, static_ocean, asset_root);
  expect(static_ocean.geometry_generation == static_ocean_generation &&
             static_ocean.environment.transparent.size() ==
                 frozen_ocean.size() &&
             static_ocean.environment.transparent.front().px ==
                 frozen_ocean.front().px &&
             static_ocean.environment.transparent.front().py ==
                 frozen_ocean.front().py,
         "static osgw Ocean freezes its t=0 surface");
}

void testLabPbrDecode() {
  using xpbd::gfx::LabPbrMetalKind;
  using xpbd::gfx::LabPbrDebugView;
  using xpbd::gfx::decodeLabPbrTexel;
  using xpbd::gfx::labPbrDebugColor;
  using xpbd::gfx::labPbrDebugViewFromName;
  using xpbd::gfx::labPbrDebugViewName;
  using xpbd::gfx::labPbrFeatureFlags;
  using xpbd::gfx::srgb8ToLinear;

  expectNear(srgb8ToLinear(0u), 0.0f, 1.0e-6f, "sRGB black -> linear zero");
  expectNear(srgb8ToLinear(255u), 1.0f, 1.0e-6f,
             "sRGB white -> linear one");
  expectNear(srgb8ToLinear(188u), 0.5029f, 2.0e-3f,
             "sRGB midpoint uses IEC transfer");

  const std::array<std::uint8_t, 4> base{188u, 128u, 0u, 64u};
  auto fallback = decodeLabPbrTexel(base, nullptr, nullptr);
  expectNear(fallback.base_color_linear[0], 0.5029f, 2.0e-3f,
             "base RGB resolves to linear");
  expectNear(fallback.opacity, 64.0f / 255.0f, 1.0e-6f,
             "base alpha remains opacity");
  expectNear(fallback.tangent_normal[2], 1.0f, 1.0e-6f,
             "missing normal uses flat tangent normal");
  expectNear(fallback.ambient_occlusion, 1.0f, 1.0e-6f,
             "missing normal uses unoccluded AO");
  expectNear(fallback.linear_roughness, 1.0f, 1.0e-6f,
             "missing specular uses fully rough fallback");
  expectNear(fallback.dielectric_f0, 0.04f, 1.0e-6f,
             "missing specular uses dielectric F0");
  expectNear(fallback.emission_strength, 0.0f, 1.0e-6f,
             "missing specular has no emission");
  expect(labPbrFeatureFlags(nullptr) == 0u,
         "missing material exposes no GPU sidecar features");

  const std::array<std::uint8_t, 4> normal{255u, 128u, 64u, 0u};
  const std::array<std::uint8_t, 4> specular{128u, 229u, 64u, 254u};
  const auto resolved = decodeLabPbrTexel(base, &normal, &specular);
  expect(resolved.tangent_normal[0] > 0.99f,
         "normal red decodes toward tangent +X");
  expect(resolved.tangent_normal[1] < 0.0f,
         "DirectX normal green decodes Y-minus");
  expectNear(resolved.ambient_occlusion, 64.0f / 255.0f, 1.0e-6f,
             "normal blue decodes linear AO");
  expectNear(resolved.relative_depth, 0.25f, 1.0e-6f,
             "normal alpha zero decodes maximum relative depth");
  expectNear(resolved.perceptual_smoothness, 128.0f / 255.0f, 1.0e-6f,
             "specular red decodes perceptual smoothness");
  const float expected_roughness = 1.0f - 128.0f / 255.0f;
  expectNear(resolved.linear_roughness,
             expected_roughness * expected_roughness, 1.0e-6f,
             "perceptual smoothness converts to linear roughness");
  expectNear(resolved.dielectric_f0, 229.0f / 255.0f, 1.0e-6f,
             "dielectric F0 divides by 255");
  expectNear(resolved.porosity, 1.0f, 1.0e-6f,
             "specular blue 64 decodes full porosity");
  expectNear(resolved.emission_strength, 1.0f, 1.0e-6f,
             "specular alpha 254 decodes full emission");
  expectNear(resolved.opacity, 64.0f / 255.0f, 1.0e-6f,
             "emission does not replace base opacity");
  expect(labPbrDebugViewFromName("roughness") ==
             LabPbrDebugView::LinearRoughness,
         "material debug name selects linear roughness");
  expect(std::string(labPbrDebugViewName(LabPbrDebugView::Emission)) ==
             "emission",
         "material debug view has stable diagnostic name");
  const auto opacity_debug =
      labPbrDebugColor(resolved, LabPbrDebugView::Opacity);
  expectNear(opacity_debug[0], resolved.opacity, 1.0e-6f,
             "CPU opacity debug color matches resolved opacity");
  const auto normal_debug =
      labPbrDebugColor(resolved, LabPbrDebugView::Normal);
  expectNear(normal_debug[0], resolved.tangent_normal[0] * 0.5f + 0.5f,
             1.0e-6f,
             "CPU normal debug color matches Raster/PT convention");

  auto rgb_specular = decodeLabPbrTexel(base, nullptr, &specular, 3);
  expectNear(rgb_specular.emission_strength, 0.0f, 1.0e-6f,
             "RGB-only specular forces emission off before filtering");
  const std::array<std::uint8_t, 4> ignored_emission{0u, 0u, 0u, 255u};
  auto ignored_alpha =
      decodeLabPbrTexel(base, nullptr, &ignored_emission);
  expectNear(ignored_alpha.emission_strength, 0.0f, 1.0e-6f,
             "specular alpha 255 is ignored/no emission");

  const std::array<std::uint8_t, 4> iron_specular{255u, 230u, 255u, 0u};
  const auto iron = decodeLabPbrTexel(base, nullptr, &iron_specular);
  expect(iron.metal_kind == LabPbrMetalKind::Predefined,
         "metal code 230 selects predefined metal");
  expect(iron.f0_color[0] > 0.5f && iron.f0_color[0] < 0.7f,
         "iron F0 is derived from official optical constants");
  expectNear(iron.subsurface_scattering, 1.0f, 1.0e-6f,
             "specular blue 255 decodes full SSS");

  const std::array<std::uint8_t, 4> custom_specular{0u, 255u, 0u, 255u};
  const auto custom = decodeLabPbrTexel(base, nullptr, &custom_specular);
  expect(custom.metal_kind == LabPbrMetalKind::Custom,
         "metal code 255 selects custom metal");
  expectNear(custom.f0_color[0], custom.base_color_linear[0], 1.0e-6f,
             "custom metal uses linear base color as F0");
}

void testLabPbrDiscoveryAndFallback() {
  namespace fs = std::filesystem;
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_regression_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error, "create isolated LabPBR regression directory");
  if (filesystem_error) {
    return;
  }

  const fs::path base_path = directory / "atlas.png";
  const fs::path normal_path = directory / "atlas_n.png";
  const fs::path specular_path = directory / "atlas_s.png";
  const fs::path properties_path = directory / "texture.properties";
  auto writeBytes = [](const fs::path &path, const void *data,
                       std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
    return output.good();
  };
  expect(writeBytes(base_path, kWhitePng, sizeof(kWhitePng)),
         "write temporary base atlas");
  expect(writeBytes(normal_path, kWhitePng, sizeof(kWhitePng)),
         "write temporary normal sidecar");
  expect(writeBytes(specular_path, kWhitePng, sizeof(kWhitePng)),
         "write temporary specular sidecar");
  const std::string properties = "format=lab-pbr/1.3\n";
  expect(writeBytes(properties_path, properties.data(), properties.size()),
         "write temporary LabPBR declaration");

  const auto paths = xpbd::gfx::discoverLabPbrAssets(base_path);
  expect(paths.normal == normal_path && paths.normal_exists,
         "discover sibling _n.png");
  expect(paths.specular == specular_path && paths.specular_exists,
         "discover sibling _s.png");
  expect(paths.properties == properties_path && paths.properties_exists,
         "discover texture.properties");

  xpbd::gfx::TextureImage base;
  std::string error;
  expect(xpbd::gfx::loadTextureImage(base_path, base, &error),
         "load temporary base atlas");
  xpbd::gfx::ResolvedMaterialTable material;
  expect(xpbd::gfx::resolveLabPbrMaterial(base, base_path, material, &error),
         "resolve declared LabPBR material");
  expect(material.valid(), "resolved material table valid");
  expect(material.format == xpbd::gfx::LabPbrFormat::LabPbr13,
         "declared LabPBR 1.3 accepted");
  expect(material.normal_map_active && material.specular_map_active,
         "compatible sidecars activated");
  expect(xpbd::gfx::labPbrFeatureFlags(&material) ==
             (xpbd::gfx::kLabPbrNormalMapActive |
              xpbd::gfx::kLabPbrSpecularMapActive),
         "resolved GPU feature bits match active sidecars");
  expectNear(material.texels[0].emission_strength, 0.0f, 1.0e-6f,
             "RGB sidecar synthesized alpha does not emit");

  const std::string unsupported = "format=lab-pbr/1.2\n";
  expect(writeBytes(properties_path, unsupported.data(), unsupported.size()),
         "replace declaration with unsupported format");
  xpbd::gfx::ResolvedMaterialTable fallback;
  expect(xpbd::gfx::resolveLabPbrMaterial(base, base_path, fallback, &error),
         "unsupported format degrades to base material");
  expect(fallback.format == xpbd::gfx::LabPbrFormat::Unsupported,
         "unsupported format is explicit");
  expect(!fallback.normal_map_active && !fallback.specular_map_active,
         "unsupported sidecars are safely ignored");
  expect(!fallback.warnings.empty(), "unsupported format reports warning");

  fs::remove_all(directory, filesystem_error);
}

void testStrictLabPbrSuiteImport() {
  namespace fs = std::filesystem;
  using xpbd::gfx::LabPbrSuiteImportStatus;

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_strict_import_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated strict LabPBR import directory");
  if (filesystem_error) {
    return;
  }

  const auto writeBytes = [](const fs::path &path,
                             const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
  };
  const auto readBytes = [](const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
  };
  const auto encode = [](int width, int height,
                         const std::vector<std::uint8_t> &rgba) {
    std::vector<std::uint8_t> png;
    std::string error;
    expect(xpbd::gfx::encodePngRgba8(width, height, rgba, png, &error),
           "encode strict LabPBR fixture PNG");
    return png;
  };

  const std::vector<std::uint8_t> base_rgba{
      255u, 128u, 64u, 255u, 32u, 64u, 128u, 127u};
  const std::vector<std::uint8_t> specular_rgba{
      0u, 0u, 0u, 0u, 255u, 230u, 64u, 255u};
  const std::vector<std::uint8_t> normal_rgba{
      128u, 128u, 255u, 255u, 140u, 120u, 200u, 20u};
  const auto base_png = encode(2, 1, base_rgba);
  const auto specular_png = encode(2, 1, specular_rgba);
  const auto normal_png = encode(2, 1, normal_rgba);

  const fs::path base_path = directory / "texture.png";
  const fs::path specular_path = directory / "texture_s.png";
  const fs::path normal_path = directory / "texture_n.png";
  const fs::path properties_path = directory / "texture.properties";
  expect(writeBytes(base_path, base_png), "write strict base PNG");
  expect(writeBytes(specular_path, specular_png),
         "write strict specular PNG");
  expect(writeBytes(normal_path, normal_png), "write strict normal PNG");
  const std::string properties_text = "format=lab-pbr/1.3\n";
  const std::vector<std::uint8_t> properties_bytes(properties_text.begin(),
                                                    properties_text.end());
  expect(writeBytes(properties_path, properties_bytes),
         "write strict LabPBR properties");

  const fs::path second_base = directory / "stone.png";
  const fs::path second_specular = directory / "stone_s.png";
  expect(writeBytes(second_base, base_png), "write second candidate base");
  expect(writeBytes(second_specular, specular_png),
         "write second candidate specular");

  std::string error;
  const auto candidates =
      xpbd::gfx::discoverLabPbrSuiteCandidates(directory, &error);
  expect(error.empty() && candidates.size() == 2u,
         "folder discovery returns only paired base candidates");
  expect(std::find(candidates.begin(), candidates.end(),
                   fs::absolute(base_path).lexically_normal()) !=
             candidates.end() &&
             std::find(candidates.begin(), candidates.end(),
                       fs::absolute(second_base).lexically_normal()) !=
                 candidates.end(),
         "folder discovery retains both stems without guessing");

  xpbd::gfx::LabPbrSuiteImportCache cache;
  auto imported =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(imported.imported() && !imported.suite.cache_hit,
         "strict complete LabPBR suite imports");
  expect(imported.suite.material.specular_map_active &&
             imported.suite.material.normal_map_active &&
             imported.suite.material.format_declared,
         "strict import activates RGBA sidecars and declaration");
  expect(imported.suite.source.base.valid() &&
             imported.suite.source.specular.valid() &&
             imported.suite.source.normal.valid() &&
             imported.suite.source.properties.valid(),
         "strict import retains complete source snapshots");
  expect(*imported.suite.source.base.original_bytes == base_png &&
             *imported.suite.source.specular.original_bytes == specular_png &&
             *imported.suite.source.normal.original_bytes == normal_png &&
             *imported.suite.source.properties.original_bytes ==
                 properties_bytes,
         "strict import preserves exact source bytes");
  expect(readBytes(base_path) == base_png &&
             readBytes(specular_path) == specular_png &&
             readBytes(normal_path) == normal_png &&
             readBytes(properties_path) == properties_bytes,
         "strict import never mutates source files");

  auto cached = xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(cached.imported() && cached.suite.cache_hit && cache.size() == 1u,
         "same path and checksum reimport reuses cache");
  const auto unchanged =
      xpbd::gfx::checkLabPbrSuiteSourceChanges(cached.suite.source);
  expect(!unchanged.reloadRecommended() && !unchanged.metadata_changed &&
             unchanged.error.empty(),
         "unchanged strict source snapshot stays current");

  auto changed_specular_rgba = specular_rgba;
  changed_specular_rgba[0] = 127u;
  const auto changed_specular_png = encode(2, 1, changed_specular_rgba);
  expect(writeBytes(specular_path, changed_specular_png),
         "replace strict specular source");
  const auto changed =
      xpbd::gfx::checkLabPbrSuiteSourceChanges(cached.suite.source);
  expect(changed.content_changed && changed.reloadRecommended() &&
             std::find(changed.changed_paths.begin(),
                       changed.changed_paths.end(),
                       fs::absolute(specular_path).lexically_normal()) !=
                 changed.changed_paths.end(),
         "checksum detects changed source content");
  auto reloaded =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(reloaded.imported() && !reloaded.suite.cache_hit &&
             cache.size() == 2u,
         "changed checksum creates a new cached import");

  fs::remove(specular_path, filesystem_error);
  expect(!filesystem_error, "remove mandatory specular fixture");
  const auto missing =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(missing.status == LabPbrSuiteImportStatus::Failed &&
             !missing.error.empty() && cache.size() == 2u,
         "missing mandatory _s is rejected without damaging cache");
  expect(writeBytes(specular_path, specular_png),
         "restore mandatory specular fixture");

  fs::remove(properties_path, filesystem_error);
  expect(!filesystem_error, "remove optional properties fixture");
  const auto needs_confirmation =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(needs_confirmation.status ==
             LabPbrSuiteImportStatus::NeedsLabPbr13Confirmation,
         "missing properties requires explicit confirmation");
  const auto confirmed =
      xpbd::gfx::importLabPbrSuite(base_path, true, &cache);
  expect(confirmed.imported() &&
             confirmed.suite.source
                 .confirmed_labpbr13_without_properties &&
             !confirmed.suite.material.format_declared,
         "explicit confirmation imports missing-properties suite");

  fs::remove(normal_path, filesystem_error);
  expect(!filesystem_error, "remove optional normal fixture");
  const auto without_normal =
      xpbd::gfx::importLabPbrSuite(base_path, true, nullptr);
  expect(without_normal.imported() &&
             !without_normal.suite.material.normal_map_active,
         "optional _n may be absent");
  expect(writeBytes(normal_path, normal_png),
         "add optional normal after import");
  const auto optional_appeared =
      xpbd::gfx::checkLabPbrSuiteSourceChanges(
          without_normal.suite.source);
  expect(optional_appeared.availability_changed &&
             optional_appeared.reloadRecommended(),
         "new optional sidecar is reported as a source change");

  const std::string wrong_properties = "format=lab-pbr/1.2\n";
  expect(writeBytes(properties_path,
                    std::vector<std::uint8_t>(wrong_properties.begin(),
                                              wrong_properties.end())),
         "write unsupported properties fixture");
  const auto unsupported =
      xpbd::gfx::importLabPbrSuite(base_path, true, nullptr);
  expect(unsupported.status == LabPbrSuiteImportStatus::Failed &&
             unsupported.error.find("unsupported") != std::string::npos,
         "unsupported properties format is rejected");

  expect(writeBytes(properties_path, properties_bytes),
         "restore valid properties fixture");
  const auto one_pixel_specular =
      encode(1, 1, {0u, 0u, 0u, 0u});
  expect(writeBytes(specular_path, one_pixel_specular),
         "write mismatched specular fixture");
  const auto mismatched =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(mismatched.status == LabPbrSuiteImportStatus::Failed &&
             mismatched.error.find("dimensions") != std::string::npos,
         "mismatched _s dimensions are rejected");

  expect(writeBytes(specular_path, {0u, 1u, 2u, 3u}),
         "write corrupt specular fixture");
  const auto corrupt =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(corrupt.status == LabPbrSuiteImportStatus::Failed &&
             corrupt.error.find("decode failed") != std::string::npos,
         "corrupt _s is rejected");

  const auto wrong_selection =
      xpbd::gfx::importLabPbrSuite(specular_path, false, nullptr);
  expect(wrong_selection.status == LabPbrSuiteImportStatus::Failed &&
             wrong_selection.error.find("base") != std::string::npos,
         "selecting _s as base is rejected without stem guessing");

  fs::remove_all(directory, filesystem_error);
}

xpbd::gfx::StaticIndexedModelMesh makeOverlappingLabPbrMesh() {
  xpbd::gfx::StaticIndexedModelMesh mesh;
  mesh.bone_names = {"group_a", "group_b", "untextured"};
  const auto add_quad = [&mesh](std::uint32_t bone_index, bool mirrored,
                                bool textured) {
    const std::uint32_t first_vertex =
        static_cast<std::uint32_t>(mesh.vertices.size());
    const std::uint32_t first_index =
        static_cast<std::uint32_t>(mesh.indices.size());
    mesh.vertices.resize(mesh.vertices.size() + 4u);
    const std::array<std::array<float, 2>, 4> regular{{
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    }};
    const std::array<std::array<float, 2>, 4> flipped{{
        {1.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
    }};
    const auto &uvs = mirrored ? flipped : regular;
    for (std::size_t i = 0; i < uvs.size(); ++i) {
      auto &vertex = mesh.vertices[first_vertex + i];
      vertex.u = uvs[i][0];
      vertex.v = uvs[i][1];
      vertex.bone_index = bone_index;
    }
    mesh.indices.insert(mesh.indices.end(),
                        {first_vertex, first_vertex + 1u,
                         first_vertex + 2u, first_vertex,
                         first_vertex + 2u, first_vertex + 3u});
    xpbd::gfx::StaticModelFace face;
    face.first_vertex = first_vertex;
    face.vertex_count = 4u;
    face.first_index = first_index;
    face.index_count = 6u;
    face.bone_index = bone_index;
    face.textured = textured;
    mesh.faces.push_back(face);
  };
  add_quad(0u, false, true);
  add_quad(1u, true, true);
  add_quad(2u, false, false);
  return mesh;
}

void testLabPbrAuthoringEncodingAndCoverage() {
  using xpbd::gfx::GroupLabPbrOverride;
  using xpbd::gfx::encodeLabPbrEmission;
  using xpbd::gfx::encodeLabPbrPorosity;
  using xpbd::gfx::encodeLabPbrRoughness;
  using xpbd::gfx::encodeLabPbrSubsurface;
  using xpbd::gfx::validGroupLabPbrOverride;
  std::string validation_error;

  expect(encodeLabPbrEmission(0.0f) == 0u,
         "LabPBR emission zero encodes to zero");
  expect(encodeLabPbrEmission(0.5f) == 127u,
         "LabPBR emission midpoint rounds against 254");
  expect(encodeLabPbrEmission(1.0f) == 254u,
         "LabPBR emission maximum never encodes reserved 255");
  expect(encodeLabPbrEmission(2.0f) == 254u,
         "LabPBR emission clamps above one");
  expect(encodeLabPbrRoughness(0.0f) == 255u,
         "zero roughness encodes full smoothness");
  expect(encodeLabPbrRoughness(0.5f) == 128u,
         "roughness midpoint uses inverse rounded smoothness");
  expect(encodeLabPbrRoughness(1.0f) == 0u,
         "full roughness encodes zero smoothness");
  expect(encodeLabPbrRoughness(
             (std::numeric_limits<float>::quiet_NaN)()) == 0u,
         "non-finite roughness uses safe fully rough fallback");
  expect(encodeLabPbrPorosity(0.5f) == 32u,
         "porosity midpoint encodes against 64");
  expect(encodeLabPbrPorosity(1.0f) == 64u,
         "porosity maximum remains below SSS range");
  expect(encodeLabPbrSubsurface(0.0f) == 65u,
         "subsurface starts at the LabPBR SSS range");
  expect(encodeLabPbrSubsurface(0.5f) == 160u,
         "subsurface midpoint encodes against 65..255");
  expect(encodeLabPbrSubsurface(1.0f) == 255u,
         "subsurface maximum reaches 255");

  GroupLabPbrOverride subsurface;
  subsurface.group_name = "sss";
  subsurface.porosity_enabled = true;
  subsurface.subsurface_scattering = true;
  subsurface.subsurface = 0.5f;
  expect(validGroupLabPbrOverride(subsurface, &validation_error),
         "subsurface override validates as a unit interval");
  const auto sss_composition = xpbd::gfx::composeLabPbrSpecular(
      1, 1, nullptr,
      xpbd::gfx::LabPbrUvCoverage{1, 1, {{"sss", {0u}}}},
      {{"sss", subsurface}});
  expect(sss_composition.exportable() &&
             sss_composition.specular.rgba[2] == 160u,
         "subsurface override writes the LabPBR SSS B range");

  GroupLabPbrOverride semantic;
  semantic.group_name = "group_a";
  semantic.metal_enabled = true;
  semantic.metal = false;
  semantic.dielectric_f0 = 229u;
  expect(validGroupLabPbrOverride(semantic, &validation_error),
         "dielectric F0 229 is valid");
  semantic.dielectric_f0 = 230u;
  expect(!validGroupLabPbrOverride(semantic, &validation_error),
         "dielectric F0 230 is rejected");
  semantic.metal = true;
  semantic.metal_code = 229u;
  expect(!validGroupLabPbrOverride(semantic, &validation_error),
         "metal code below 230 is rejected");
  semantic.metal_code = 255u;
  expect(validGroupLabPbrOverride(semantic, &validation_error),
         "custom metal code 255 is valid");

  const auto mesh = makeOverlappingLabPbrMesh();
  const auto coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(mesh, 2, 2);
  expect(coverage.valid(), "LabPBR UV coverage dimensions are valid");
  const auto *group_a = coverage.find("group_a");
  const auto *group_b = coverage.find("group_b");
  expect(group_a != nullptr && group_a->size() == 4u,
         "selected group rasterizes all covered atlas texels");
  expect(group_b != nullptr && group_b->size() == 4u,
         "mirrored group coverage deduplicates triangle texels");
  expect(coverage.find("untextured") == nullptr,
         "untextured faces do not enter LabPBR coverage");
  const auto empty_coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(
          xpbd::gfx::StaticIndexedModelMesh{}, 2, 2);
  expect(empty_coverage.valid() && empty_coverage.group_texels.empty(),
         "empty model produces valid empty LabPBR coverage");
  GroupLabPbrOverride missing_group;
  missing_group.group_name = "missing";
  missing_group.emission_enabled = true;
  missing_group.emission = 1.0f;
  const auto empty_composition = xpbd::gfx::composeLabPbrSpecular(
      2, 2, nullptr, empty_coverage, {{"missing", missing_group}});
  expect(empty_composition.exportable() &&
             empty_composition.warnings.size() == 1u,
         "selected group without textured UVs is a safe warned no-op");

  xpbd::gfx::TextureImage base;
  base.width = 1;
  base.height = 1;
  base.source_channels = 4;
  base.rgba = {255u, 255u, 255u, 255u};
  xpbd::gfx::ResolvedMaterialTable fallback_source;
  fallback_source.width = 1;
  fallback_source.height = 1;
  fallback_source.texels = {
      xpbd::gfx::decodeLabPbrTexel(
          std::array<std::uint8_t, 4>{255u, 255u, 255u, 255u},
          nullptr, nullptr)};
  xpbd::gfx::ResolvedMaterialTable authored;
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             base, fallback_source, nullptr, nullptr, authored,
             &validation_error),
         "rebuild resolved material without authored sidecars");
  expect(!authored.specular_map_active &&
             xpbd::gfx::labPbrFeatureFlags(&authored) == 0u,
         "no override preserves exact missing-specular feature state");
  expectNear(authored.texels[0].dielectric_f0, 0.04f, 1.0e-6f,
             "no override preserves exact dielectric fallback");
  xpbd::gfx::TextureImage authored_specular;
  authored_specular.width = 1;
  authored_specular.height = 1;
  authored_specular.source_channels = 4;
  authored_specular.rgba = {255u, 230u, 64u, 254u};
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             base, fallback_source, nullptr, &authored_specular, authored,
             &validation_error),
         "rebuild resolved material with authored specular");
  expect(authored.specular_map_active &&
             authored.texels[0].metal_kind ==
                 xpbd::gfx::LabPbrMetalKind::Predefined &&
             authored.texels[0].emission_strength > 0.99f,
         "applied authored specular reaches resolved preview semantics");
  xpbd::gfx::TextureImage rgb_normal;
  rgb_normal.width = 1;
  rgb_normal.height = 1;
  rgb_normal.source_channels = 3;
  rgb_normal.rgba = {128u, 128u, 255u, 255u};
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             base, fallback_source, &rgb_normal, nullptr, authored,
             &validation_error),
         "RGB LabPBR normal sidecar remains importable");
  expect(authored.normal_map_active && authored.normal_image.source_channels == 3,
         "RGB LabPBR normal sidecar reaches the resolved material");
}

void testLabPbrCompositionAndConflicts() {
  using xpbd::gfx::GroupLabPbrOverride;
  using xpbd::gfx::LabPbrOverrideChannel;

  const auto coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(
          makeOverlappingLabPbrMesh(), 2, 2);
  xpbd::gfx::TextureImage imported;
  imported.width = 2;
  imported.height = 2;
  imported.source_channels = 4;
  imported.rgba = {
      1u, 2u, 3u, 4u,       5u, 6u, 7u, 8u,
      9u, 10u, 11u, 12u,    13u, 14u, 15u, 16u,
  };

  GroupLabPbrOverride group_a;
  group_a.group_name = "group_a";
  group_a.roughness_enabled = true;
  group_a.roughness = 0.25f;
  std::map<std::string, GroupLabPbrOverride> overrides{
      {"group_a", group_a}};
  auto composed = xpbd::gfx::composeLabPbrSpecular(
      2, 2, &imported, coverage, overrides);
  expect(composed.exportable(),
         "single selected-group override composes without conflict");
  expect(composed.specular.rgba[0] == 191u &&
             composed.specular.rgba[4] == 191u &&
             composed.specular.rgba[8] == 191u &&
             composed.specular.rgba[12] == 191u,
         "roughness override changes only covered R texels");
  expect(composed.specular.rgba[1] == imported.rgba[1] &&
             composed.specular.rgba[2] == imported.rgba[2] &&
             composed.specular.rgba[3] == imported.rgba[3],
         "disabled channels preserve imported texture bytes");

  group_a = {};
  group_a.group_name = "group_a";
  group_a.emission_enabled = true;
  group_a.emission = 0.5f;
  GroupLabPbrOverride group_b = group_a;
  group_b.group_name = "group_b";
  overrides = {{"group_a", group_a}, {"group_b", group_b}};
  composed = xpbd::gfx::composeLabPbrSpecular(
      2, 2, &imported, coverage, overrides);
  expect(composed.exportable() && composed.conflicts.empty(),
         "identical overlapping group values are exportable");

  group_b.emission = 1.0f;
  overrides["group_b"] = group_b;
  composed = xpbd::gfx::composeLabPbrSpecular(
      2, 2, &imported, coverage, overrides);
  expect(!composed.exportable() && composed.conflicts.size() == 4u,
         "different overlapping group values block all conflict texels");
  if (!composed.conflicts.empty()) {
    const auto &conflict = composed.conflicts.front();
    expect(conflict.channel == LabPbrOverrideChannel::Emission,
           "conflict identifies the LabPBR channel");
    expect(conflict.groups.size() == 2u &&
               conflict.groups[0] == "group_a" &&
               conflict.groups[1] == "group_b",
           "conflict reports both groups deterministically");
    expect(conflict.encoded_values.size() == 2u &&
               conflict.encoded_values[0] == 127u &&
               conflict.encoded_values[1] == 254u,
           "conflict reports both encoded channel values");
  }
}

void testLabPbrPngChecksumAndNormalImport() {
  namespace fs = std::filesystem;
  const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  expect(xpbd::gfx::sha256Hex(abc) ==
             "ba7816bf8f01cfea414140de5dae2223"
             "b00361a396177a9cb410ff61f20015ad",
         "SHA-256 matches the abc known vector");

  const std::vector<std::uint8_t> rgba{
      255u, 128u, 64u, 32u, 0u, 1u, 2u, 3u};
  std::vector<std::uint8_t> png;
  std::string error;
  expect(xpbd::gfx::encodePngRgba8(2, 1, rgba, png, &error),
         "encode deterministic RGBA8 PNG");
  xpbd::gfx::TextureImage decoded;
  expect(xpbd::gfx::loadTextureImageFromMemory(
             png.data(), static_cast<int>(png.size()), decoded, &error),
         "decode authored RGBA8 PNG");
  expect(decoded.width == 2 && decoded.height == 1 &&
             decoded.source_channels == 4 && decoded.rgba == rgba,
         "authored PNG round-trips dimensions, channels, and bytes");

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_authoring_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated LabPBR authoring regression directory");
  if (filesystem_error) {
    return;
  }
  const auto write_bytes = [](const fs::path &path,
                              const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
  };

  const fs::path normal_path = directory / "atlas_n.png";
  expect(write_bytes(normal_path, png), "write authored RGBA normal asset");
  xpbd::gfx::ReadOnlyIrisNormalAsset normal;
  expect(xpbd::gfx::importReadOnlyIrisNormal(
             normal_path, 2, 1, normal, &error),
         "import matching RGBA Iris normal asset");
  expect(normal.valid() && normal.original_file_bytes == png &&
             normal.sha256 == xpbd::gfx::sha256Hex(png),
         "Iris normal import preserves exact bytes and checksum");

  const auto preserved_bytes = normal.original_file_bytes;
  const auto preserved_hash = normal.sha256;
  const std::vector<std::uint8_t> rgb_png(
      std::begin(kWhitePng), std::end(kWhitePng));
  expect(write_bytes(normal_path, rgb_png),
         "replace temporary source with RGB PNG");
  expect(!xpbd::gfx::importReadOnlyIrisNormal(
             normal_path, 2, 1, normal, &error),
         "RGB Iris normal import is rejected");
  expect(normal.original_file_bytes == preserved_bytes &&
             normal.sha256 == preserved_hash,
         "failed normal replacement preserves active imported bytes");

  fs::remove_all(directory, filesystem_error);
}

void testLabPbrBundleExport() {
  namespace fs = std::filesystem;
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_export_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated LabPBR export regression directory");
  if (filesystem_error) {
    return;
  }
  const auto read_bytes = [](const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
  };
  const auto write_bytes = [](const fs::path &path,
                              const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
  };

  xpbd::gfx::LabPbrCompositionResult composition;
  composition.specular.width = 2;
  composition.specular.height = 1;
  composition.specular.source_channels = 4;
  composition.specular.rgba = {
      255u, 229u, 64u, 254u, 0u, 255u, 0u, 0u};
  std::vector<std::uint8_t> base_png;
  const std::vector<std::uint8_t> base_rgba{
      240u, 220u, 200u, 255u, 120u, 140u, 160u, 255u};
  std::vector<std::uint8_t> normal_png;
  const std::vector<std::uint8_t> normal_rgba{
      128u, 128u, 255u, 255u, 255u, 0u, 64u, 32u};
  std::string error;
  expect(xpbd::gfx::encodePngRgba8(
             2, 1, base_rgba, base_png, &error),
         "encode bundle base PNG fixture");
  const fs::path base_path = directory / "skin.png";
  expect(write_bytes(base_path, base_png),
         "write bundle base PNG fixture");
  expect(xpbd::gfx::encodePngRgba8(
             2, 1, normal_rgba, normal_png, &error),
         "encode bundle normal PNG fixture");
  const fs::path normal_source = directory / "source_n.png";
  expect(write_bytes(normal_source, normal_png),
         "write bundle normal PNG fixture");
  xpbd::gfx::ReadOnlyIrisNormalAsset normal;
  expect(xpbd::gfx::importReadOnlyIrisNormal(
             normal_source, 2, 1, normal, &error),
         "import bundle normal PNG fixture");

  auto exported = xpbd::gfx::exportLabPbrBundle(
      directory / "skin.png", composition, &normal, false);
  expect(exported.success && !exported.overwrite_required,
         "new LabPBR bundle exports transactionally");
  expect(exported.specular_path.filename() == "skin_s.png" &&
             exported.normal_path.filename() == "skin_n.png" &&
             exported.properties_path.filename() == "texture.properties",
         "LabPBR bundle uses standard output names");
  xpbd::gfx::TextureImage exported_specular;
  expect(xpbd::gfx::loadTextureImage(
             exported.specular_path, exported_specular, &error) &&
             exported_specular.source_channels == 4 &&
             exported_specular.rgba == composition.specular.rgba,
         "exported specular PNG round-trips exact RGBA bytes");
  expect(read_bytes(exported.normal_path) == normal_png,
         "exported Iris normal preserves exact original file bytes");
  const auto properties = read_bytes(exported.properties_path);
  expect(std::string(properties.begin(), properties.end()) ==
             "format=lab-pbr/1.3\n",
         "exported texture.properties declares LabPBR 1.3");

  const auto imported_before_edit =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(imported_before_edit.imported() &&
             imported_before_edit.suite.material.specular_image.rgba ==
                 composition.specular.rgba &&
             imported_before_edit.suite.source.normal.original_bytes &&
             *imported_before_edit.suite.source.normal.original_bytes ==
                 normal_png,
         "exported complete LabPBR suite imports before editing");

  const auto original_specular = exported_specular.rgba;
  composition.specular.rgba[0] = 7u;
  auto overwrite = xpbd::gfx::exportLabPbrBundle(
      directory / "skin_s.png", composition, &normal, false);
  expect(!overwrite.success && overwrite.overwrite_required &&
             overwrite.existing_paths.size() == 3u,
         "existing LabPBR bundle requires explicit overwrite approval");
  expect(xpbd::gfx::loadTextureImage(
             exported.specular_path, exported_specular, &error) &&
             exported_specular.rgba == original_specular,
         "declined overwrite leaves existing bundle unchanged");

  overwrite = xpbd::gfx::exportLabPbrBundle(
      directory / "skin_s.png", composition, &normal, true);
  expect(overwrite.success,
         "approved LabPBR bundle overwrite completes");
  expect(xpbd::gfx::loadTextureImage(
             overwrite.specular_path, exported_specular, &error) &&
             exported_specular.rgba == composition.specular.rgba,
         "approved overwrite installs newly validated specular bytes");
  const auto reimported_after_edit =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(reimported_after_edit.imported() &&
             reimported_after_edit.suite.material.specular_image.rgba ==
                 composition.specular.rgba &&
             reimported_after_edit.suite.source.normal.original_bytes &&
             *reimported_after_edit.suite.source.normal.original_bytes ==
                 normal_png &&
             reimported_after_edit.suite.material.format_declared,
         "import-edit-export-reimport round-trip preserves authored RGBA, exact _n bytes, and properties");

  xpbd::gfx::LabPbrCompositionResult conflicting = composition;
  conflicting.conflicts.push_back({});
  const auto blocked = xpbd::gfx::exportLabPbrBundle(
      directory / "blocked.png", conflicting, nullptr, true);
  expect(!blocked.success &&
             !fs::exists(directory / "blocked_s.png"),
         "UV conflicts block export before any target is created");

  fs::remove_all(directory, filesystem_error);
}

void testTangentFrames() {
  const std::array<float, 3> p0{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> p1{1.0f, 0.0f, 0.0f};
  const std::array<float, 3> p2{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
  const std::array<float, 2> uv0{0.0f, 0.0f};
  const std::array<float, 2> uv1{1.0f, 0.0f};
  const std::array<float, 2> uv2{0.0f, 1.0f};

  auto regular =
      xpbd::gfx::computeTangentFrame(p0, p1, p2, normal, uv0, uv1, uv2);
  expectNear(regular.tangent[0], 1.0f, 1.0e-6f,
             "regular UV tangent follows +X");
  expectNear(regular.handedness, 1.0f, 1.0e-6f,
             "regular UV has positive handedness");
  expect(!regular.used_fallback, "regular UV does not use tangent fallback");

  auto mirrored =
      xpbd::gfx::computeTangentFrame(p0, p1, p2, normal, uv0, uv2, uv1);
  expectNear(mirrored.handedness, -1.0f, 1.0e-6f,
             "mirrored UV flips tangent handedness");

  auto degenerate =
      xpbd::gfx::computeTangentFrame(p0, p1, p2, normal, uv0, uv0, uv0);
  expect(degenerate.used_fallback,
         "degenerate UV uses deterministic tangent fallback");
  expectNear(degenerate.tangent[0], 1.0f, 1.0e-6f,
             "degenerate +Z normal falls back to +X tangent");
}

void testRtNormalTransformAndUpdatePolicy() {
  using xpbd::gfx::RtBlasPolicy;
  using xpbd::gfx::RtGeometryUpdateKind;
  using xpbd::gfx::classifyRtGeometryUpdate;
  using xpbd::gfx::transformRtNormalInverseTranspose;

  const std::array<float, 16> non_uniform{
      2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const float inv_sqrt2 = 1.0f / std::sqrt(2.0f);
  const auto transformed = transformRtNormalInverseTranspose(
      non_uniform, {inv_sqrt2, inv_sqrt2, 0.0f});
  expect(!transformed.used_fallback,
         "non-uniform normal uses inverse-transpose path");
  expectNear(transformed.value[0], 0.4472136f, 1.0e-5f,
             "non-uniform normal inverse-transpose X");
  expectNear(transformed.value[1], 0.8944272f, 1.0e-5f,
             "non-uniform normal inverse-transpose Y");
  const std::array<float, 3> transformed_tangent{
      2.0f * inv_sqrt2, -inv_sqrt2, 0.0f};
  const float normal_tangent_dot =
      transformed.value[0] * transformed_tangent[0] +
      transformed.value[1] * transformed_tangent[1] +
      transformed.value[2] * transformed_tangent[2];
  expectNear(normal_tangent_dot, 0.0f, 1.0e-5f,
             "inverse-transpose normal stays orthogonal to transformed tangent");

  const std::array<float, 16> mirrored{
      -2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const auto mirrored_normal =
      transformRtNormalInverseTranspose(mirrored, {1.0f, 0.0f, 0.0f});
  expectNear(mirrored_normal.value[0], -1.0f, 1.0e-6f,
             "mirrored transform preserves inverse determinant sign");

  const std::array<float, 16> singular{
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const auto singular_normal =
      transformRtNormalInverseTranspose(singular, {1.0f, 0.0f, 0.0f});
  expect(singular_normal.used_fallback,
         "singular normal transform reports deterministic fallback");
  expectNear(singular_normal.value[0], 1.0f, 1.0e-6f,
             "singular normal transform remains finite and normalized");

  expect(classifyRtGeometryUpdate(7u, 7u, RtBlasPolicy::RigidLocalSpace,
                                  true) == RtGeometryUpdateKind::None,
         "stable rigid-local content requires no BLAS work");
  expect(classifyRtGeometryUpdate(7u, 8u, RtBlasPolicy::RigidLocalSpace,
                                  true) == RtGeometryUpdateKind::FullBuild,
         "changed rigid-local content fails safe to full BLAS build");
  expect(classifyRtGeometryUpdate(7u, 8u, RtBlasPolicy::DynamicRefit,
                                  false) == RtGeometryUpdateKind::FullBuild,
         "first dynamic geometry update requires full BLAS build");
  expect(classifyRtGeometryUpdate(7u, 8u, RtBlasPolicy::DynamicRefit,
                                  true) == RtGeometryUpdateKind::Refit,
         "built dynamic geometry position change selects BLAS refit");
}

void testRtNearestHitReference() {
  using xpbd::gfx::RtAlphaMode;
  using xpbd::gfx::RtHitCandidate;
  using xpbd::gfx::intersectRtTriangleTwoSided;
  using xpbd::gfx::selectRtNearestValidHit;

  expect(xpbd::gfx::rtDebugViewFromName("instance") ==
                 xpbd::gfx::RtDebugView::Instance &&
             std::string(xpbd::gfx::rtDebugViewName(
                 xpbd::gfx::RtDebugView::Normal)) == "normal" &&
             xpbd::gfx::rtDebugViewFromName("unknown") ==
             xpbd::gfx::RtDebugView::Off,
         "RT pipeline debug-view names are stable and unknown-safe");

  xpbd::gfx::RtSbtLayoutRequest sbt_request;
  sbt_request.shader_group_handle_size = 32u;
  sbt_request.shader_group_handle_alignment = 32u;
  sbt_request.shader_group_base_alignment = 64u;
  sbt_request.max_shader_group_stride = 4096u;
  sbt_request.miss_group_count = 2u;
  sbt_request.hit_group_count = 1u;
  sbt_request.buffer_device_address = 0x1003u;
  sbt_request.buffer_bytes = 224u;
  const auto sbt_layout = xpbd::gfx::computeRtSbtLayout(sbt_request);
  expect(sbt_layout && sbt_layout->shader_group_stride == 32u &&
             sbt_layout->base_offset == 61u &&
             sbt_layout->miss_offset == 64u &&
             sbt_layout->hit_offset == 128u &&
             sbt_layout->layout_bytes == 160u,
         "RT SBT layout aligns stride, base, miss, and hit records");
  sbt_request.shader_group_handle_alignment = 24u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects non-power-of-two handle alignment");
  sbt_request.shader_group_handle_alignment = 32u;
  sbt_request.max_shader_group_stride = 16u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects stride above device maximum");
  sbt_request.max_shader_group_stride = 4096u;
  sbt_request.buffer_bytes = 220u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects undersized allocation after base alignment");
  sbt_request.buffer_bytes = 224u;
  sbt_request.buffer_device_address =
      (std::numeric_limits<std::uint64_t>::max)() - 31u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects device-address alignment overflow");

  xpbd::gfx::RtDispatchBufferBounds dispatch_bounds{
      24u, 12u, 2u, 24u * 16u, 24u * 16u, 12u * 3u * 4u,
      24u * 8u, 24u * 16u, 12u * 4u, 12u * 16u, 2u * 16u};
  expect(xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch accepts complete vertex/primitive/instance buffers");
  dispatch_bounds.normal_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized normal buffer");
  dispatch_bounds.normal_bytes += 1u;
  dispatch_bounds.tangent_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized tangent buffer");
  dispatch_bounds.tangent_bytes += 1u;
  dispatch_bounds.index_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized index buffer");
  dispatch_bounds.index_bytes += 1u;
  dispatch_bounds.primitive_metadata_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized primitive identity buffer");
  dispatch_bounds.primitive_metadata_bytes += 1u;
  dispatch_bounds.instance_metadata_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized instance identity buffer");

  const std::array<float, 3> origin{0.25f, 0.25f, 1.0f};
  const std::array<float, 3> grazing_direction{
      0.9999995f, 0.0f, -0.001f};
  const std::array<float, 3> vertex0{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> vertex1{2000.0f, 0.0f, 0.0f};
  const std::array<float, 3> vertex2{0.0f, 2.0f, 0.0f};
  const auto grazing = intersectRtTriangleTwoSided(
      origin, grazing_direction, vertex0, vertex1, vertex2, 0.001f,
      2000.0f);
  expect(grazing.has_value() && std::isfinite(grazing->distance) &&
             grazing->distance > 999.0f && grazing->distance < 1001.0f,
         "grazing-angle two-sided triangle hit remains finite");
  expect(grazing.has_value() &&
             grazing->barycentrics[0] >= 0.0f &&
             grazing->barycentrics[1] >= 0.0f &&
             grazing->barycentrics[0] +
                     grazing->barycentrics[1] <=
                 1.0f,
         "grazing-angle hit retains valid barycentrics");
  expect(intersectRtTriangleTwoSided(
             origin, grazing_direction, vertex0, vertex2, vertex1,
             0.001f, 2000.0f)
             .has_value(),
         "grazing-angle reference matches two-sided Vulkan instance policy");
  expect(!intersectRtTriangleTwoSided(
              origin, {1.0f, 0.0f, 0.0f}, vertex0, vertex1, vertex2,
              0.001f, 2000.0f)
              .has_value(),
         "parallel ray does not fabricate a triangle hit");

  const float grazing_distance =
      grazing ? grazing->distance : 1000.0f;
  const std::array<RtHitCandidate, 4> candidates{{
      {grazing_distance + 2.0f, 202u, RtAlphaMode::Opaque, 1.0f},
      {grazing_distance - 1.0f, 101u, RtAlphaMode::Cutout, 0.0f},
      {grazing_distance, 303u, RtAlphaMode::Opaque, 1.0f},
      {std::numeric_limits<float>::quiet_NaN(), 404u,
       RtAlphaMode::Opaque, 1.0f},
  }};
  const auto nearest =
      selectRtNearestValidHit(candidates, 0.001f, 2000.0f);
  expect(nearest.has_value() &&
             nearest->primitive_identity == 303u &&
             nearest->distance == grazing_distance,
         "nearest valid hit skips cutout and non-finite candidates");

  const std::array<RtHitCandidate, 2> blend_candidates{{
      {5.0f, 11u, RtAlphaMode::Opaque, 1.0f},
      {4.0f, 12u, RtAlphaMode::Blend, 0.25f},
  }};
  const auto nearest_blend =
      selectRtNearestValidHit(blend_candidates, 0.001f, 100.0f);
  expect(nearest_blend.has_value() &&
             nearest_blend->primitive_identity == 12u &&
             std::abs(nearest_blend->accepted_opacity - 0.25f) <
                 1.0e-6f,
         "nearest valid blended layer preserves fractional opacity");
}

void testPathTraceSamplingAndAccumulation() {
  using xpbd::gfx::PathTraceAccumulationRequest;
  using xpbd::gfx::PathTraceFrameGeneration;
  using xpbd::gfx::PathTraceSettings;
  using xpbd::gfx::PathTraceUpscale;
  using xpbd::gfx::advancePathTraceAccumulation;
  using xpbd::gfx::normalizePathTraceSettings;
  using xpbd::gfx::pathTraceRandom01;
  using xpbd::gfx::pathTraceRandomBits;
  using xpbd::gfx::pathTraceTemporalJitter;
  using xpbd::gfx::samplePathTraceCosineHemisphere;
  using xpbd::gfx::shouldResetTemporalReconstructionHistory;

  PathTraceSettings invalid;
  invalid.samples_per_frame = 0u;
  invalid.maximum_samples = (std::numeric_limits<std::uint32_t>::max)();
  invalid.max_bounces = 99u;
  invalid.analytic_environment_strength =
      (std::numeric_limits<float>::quiet_NaN)();
  invalid.display_exposure_ev =
      (std::numeric_limits<float>::quiet_NaN)();
  const auto normalized = normalizePathTraceSettings(invalid);
  expect(normalized.samples_per_frame == 1u &&
             normalized.maximum_samples == 65'536u &&
             normalized.max_bounces == 64u &&
             normalized.analytic_environment_strength == 0.0f &&
             normalized.display_exposure_ev == 0.0f,
         "path settings clamp invalid and non-finite values");
  invalid.max_diffuse_bounces = 99u;
  invalid.max_glossy_bounces = 99u;
  invalid.max_transmission_bounces = 99u;
  invalid.max_transparent_bounces = 99u;
  invalid.russian_roulette_start = 99u;
  const auto normalized_depth = normalizePathTraceSettings(invalid);
  expect(normalized_depth.max_diffuse_bounces == 16u &&
             normalized_depth.max_glossy_bounces == 16u &&
             normalized_depth.max_transmission_bounces == 32u &&
             normalized_depth.max_transparent_bounces == 64u &&
             normalized_depth.russian_roulette_start == 64u,
         "path settings clamp Phase 5 per-lobe and RR depth ranges");
  PathTraceSettings low_bounces;
  low_bounces.max_bounces = 0u;
  low_bounces.analytic_environment_strength = 99.0f;
  low_bounces.display_exposure_ev = 99.0f;
  const auto normalized_low = normalizePathTraceSettings(low_bounces);
  expect(normalized_low.max_bounces == 1u &&
             normalized_low.analytic_environment_strength == 16.0f &&
             normalized_low.display_exposure_ev == 16.0f,
         "path settings preserve Phase 5 total/environment limits");
  expect(PathTraceSettings{}.display_exposure_ev == 2.0f,
         "default PT exposure is the daylight inspection baseline");
  expect(PathTraceSettings{}.requested_frame_generation ==
             PathTraceFrameGeneration::Off,
         "DLSS Frame Generation is opt-in and defaults to Off");
  PathTraceSettings legacy_upscale;
  legacy_upscale.requested_upscale = PathTraceUpscale::Auto;
  expect(normalizePathTraceSettings(legacy_upscale).requested_upscale ==
             PathTraceUpscale::Quality,
         "legacy Auto DLSS selection migrates to Quality");
  legacy_upscale.requested_upscale = PathTraceUpscale::UltraQuality;
  expect(normalizePathTraceSettings(legacy_upscale).requested_upscale ==
             PathTraceUpscale::Quality,
         "legacy Ultra Quality DLSS selection migrates to Quality");

  const std::uint32_t random_a =
      pathTraceRandomBits(17u, 29u, 5u, 3u, 12345u);
  const std::uint32_t random_repeat =
      pathTraceRandomBits(17u, 29u, 5u, 3u, 12345u);
  const std::uint32_t random_next_dimension =
      pathTraceRandomBits(17u, 29u, 5u, 4u, 12345u);
  const float random_unit =
      pathTraceRandom01(17u, 29u, 5u, 3u, 12345u);
  expect(random_a == random_repeat && random_a != random_next_dimension,
         "fixed path seed is exact and dimensions decorrelate");
  expect(std::isfinite(random_unit) && random_unit >= 0.0f &&
             random_unit < 1.0f,
         "path random float remains finite in [0,1)");
  const auto temporal_jitter_0 =
      pathTraceTemporalJitter(0u, 393u, 590u);
  const auto temporal_jitter_1 =
      pathTraceTemporalJitter(1u, 393u, 590u);
  expect(temporal_jitter_0[0] >= -0.5f &&
             temporal_jitter_0[0] <= 0.5f &&
             temporal_jitter_0[1] >= -0.5f &&
             temporal_jitter_0[1] <= 0.5f,
         "temporal reconstruction jitter stays in pixel bounds");
  expect(temporal_jitter_0 != temporal_jitter_1 &&
             temporal_jitter_0 ==
                 pathTraceTemporalJitter(0u, 393u, 590u),
         "temporal reconstruction jitter is stable and advances");
  expect(temporal_jitter_0 ==
             pathTraceTemporalJitter(18u, 393u, 590u),
         "Quality jitter repeats at NVIDIA's scale-dependent phase count");
  expect(pathTraceTemporalJitter(0u, 590u, 590u) ==
             pathTraceTemporalJitter(8u, 590u, 590u),
         "DLAA jitter repeats after eight phases");
  expect(shouldResetTemporalReconstructionHistory(
             false, 0u, 7u, false) &&
             !shouldResetTemporalReconstructionHistory(
                 true, 7u, 7u, true) &&
             shouldResetTemporalReconstructionHistory(
                 true, 7u, 8u, true) &&
             shouldResetTemporalReconstructionHistory(
                 true, 7u, 7u, false),
         "Streamline history resets only for first/incompatible/invalid-motion "
         "frames, not ordinary dense-motion camera frames");

  const auto hemisphere = samplePathTraceCosineHemisphere(
      {0.0f, 1.0f, 0.0f}, random_unit,
      pathTraceRandom01(17u, 29u, 5u, 4u, 12345u));
  const float hemisphere_length = std::sqrt(
      hemisphere.direction[0] * hemisphere.direction[0] +
      hemisphere.direction[1] * hemisphere.direction[1] +
      hemisphere.direction[2] * hemisphere.direction[2]);
  expect(!hemisphere.used_fallback &&
             std::isfinite(hemisphere_length) &&
             std::abs(hemisphere_length - 1.0f) < 1.0e-5f &&
             hemisphere.direction[1] >= 0.0f,
         "cosine sample is finite, normalized, and above the hemisphere");
  const auto invalid_hemisphere = samplePathTraceCosineHemisphere(
      {0.0f, 0.0f, 0.0f}, 0.5f, 0.5f);
  expect(invalid_hemisphere.used_fallback &&
             invalid_hemisphere.direction ==
                 std::array<float, 3>{0.0f, 1.0f, 0.0f},
         "invalid cosine-sampling normal uses finite deterministic fallback");

  PathTraceAccumulationRequest request;
  request.history_key = 101u;
  request.settings.samples_per_frame = 3u;
  request.settings.maximum_samples = 40u;
  auto first = advancePathTraceAccumulation(request);
  expect(first.history_reset && first.sample_base == 0u &&
             first.dispatch_samples == 3u &&
             first.accumulated_samples_after_dispatch == 3u,
         "new history starts at sample zero and dispatches spp");

  request.history_valid = true;
  request.previous_history_key = 101u;
  request.accumulated_samples = 29u;
  request.settings.samples_per_frame = 4u;
  request.settings.maximum_samples = 32u;
  const auto continued = advancePathTraceAccumulation(request);
  expect(!continued.history_reset && continued.sample_base == 29u &&
             continued.dispatch_samples == 3u &&
             continued.accumulated_samples_after_dispatch == 32u &&
             continued.maximum_reached,
         "spp/max changes preserve history and stop exactly at maximum");

  request.accumulated_samples =
      continued.accumulated_samples_after_dispatch;
  request.settings.maximum_samples = 32u;
  const auto stopped = advancePathTraceAccumulation(request);
  expect(!stopped.history_reset && stopped.sample_base == 32u &&
             stopped.dispatch_samples == 0u &&
             stopped.accumulated_samples_after_dispatch == 32u &&
             stopped.maximum_reached,
         "lowered maximum stops without discarding compatible samples");

  request.history_key = 202u;
  request.settings.maximum_samples = 40u;
  const auto reset = advancePathTraceAccumulation(request);
  expect(reset.history_reset && reset.sample_base == 0u &&
             reset.dispatch_samples == 4u &&
             reset.accumulated_samples_after_dispatch == 4u,
         "radiance-affecting history-key change resets slot accumulation");
}

void testPathTraceAdjustableSettingsContract() {
  using namespace xpbd::gfx;

  const auto realtime =
      pathTraceSettingsForPreset(PathTracePreset::Realtime);
  const auto reference =
      pathTraceSettingsForPreset(PathTracePreset::Reference);
  expect(realtime.preset == PathTracePreset::Realtime &&
             reference.preset == PathTracePreset::Reference &&
             reference.samples_per_frame > realtime.samples_per_frame &&
             reference.max_bounces > realtime.max_bounces,
         "path tracing presets provide distinct normalized quality tiers");

  auto custom = applyPathTracePreset(realtime,
                                     PathTracePreset::HighQuality);
  custom.preset = PathTracePreset::Custom;
  custom.samples_per_frame = 3u;
  const auto restored = restorePathTraceSourcePreset(custom);
  expect(restored.preset == PathTracePreset::HighQuality &&
             restored.source_preset == PathTracePreset::HighQuality &&
             restored.samples_per_frame == 4u,
         "Custom retains and restores its source preset");

  auto schedule = realtime;
  schedule.samples_per_frame = 8u;
  schedule.maximum_samples = 2048u;
  const auto schedule_change =
      classifyPathTraceSettingsChange(realtime, schedule);
  expect(hasPathTraceChange(
             schedule_change,
             PathTraceChangeClass::SamplingSchedule) &&
             !hasPathTraceChange(
                 schedule_change,
                 PathTraceChangeClass::ResetAccumulation),
         "SPP and maximum samples preserve compatible accumulation");

  auto physical = schedule;
  physical.multiple_importance_sampling = false;
  physical.direct_clamp = 2.0f;
  const auto physical_change =
      classifyPathTraceSettingsChange(schedule, physical);
  expect(hasPathTraceChange(
             physical_change,
             PathTraceChangeClass::ResetAccumulation),
         "integrator edits reset accumulated radiance");

  auto film = physical;
  film.display_exposure_ev = -1.0f;
  film.tone_mapping = PathTraceToneMapping::Reinhard;
  const auto film_change =
      classifyPathTraceSettingsChange(physical, film);
  expect(hasPathTraceChange(
             film_change, PathTraceChangeClass::DisplayOnly) &&
             !hasPathTraceChange(
                 film_change,
                 PathTraceChangeClass::ResetAccumulation),
         "film edits are classified as display-only");

  auto target = film;
  target.preview_resolution_scale = 0.5f;
  expect(hasPathTraceChange(
             classifyPathTraceSettingsChange(film, target),
             PathTraceChangeClass::RecreateTarget),
         "preview scale is classified as target recreation");
  PathTraceSettings automatic_seed;
  automatic_seed.reset_generation = 11u;
  const std::uint32_t generated_seed =
      resolvedPathTraceSeed(automatic_seed);
  expect(generated_seed == resolvedPathTraceSeed(automatic_seed) &&
             generated_seed != automatic_seed.seed,
         "automatic seed is stable for one accumulation generation");
  automatic_seed.automatic_seed = false;
  automatic_seed.seed = 42u;
  expect(resolvedPathTraceSeed(automatic_seed) == 42u,
         "fixed seed is passed through exactly");

  PathTraceSettings post;
  post.requested_denoiser =
      PathTraceDenoiser::DlssRayReconstruction;
  post.requested_upscale = PathTraceUpscale::Quality;
  PathTracePostProcessCapabilities capabilities;
  auto unresolved = resolvePathTracePostProcess(post, capabilities);
  expect(!unresolved.denoiser_supported &&
             unresolved.active_denoiser ==
                 PathTraceDenoiser::Raw &&
             !unresolved.upscale_supported &&
             unresolved.active_upscale == PathTraceUpscale::Off,
         "unsupported denoise/upscale requests resolve visibly to raw/off");
  capabilities.dlss_ray_reconstruction = true;
  capabilities.dlss_super_resolution = true;
  const auto rr = resolvePathTracePostProcess(post, capabilities);
  expect(rr.active_denoiser ==
             PathTraceDenoiser::DlssRayReconstruction &&
             rr.active_upscale == PathTraceUpscale::Off &&
             rr.reconstruction_mode == PathTraceUpscale::Quality &&
             rr.conflict_resolved,
         "Ray Reconstruction owns reconstruction and excludes separate SR");
  post.requested_upscale = PathTraceUpscale::Off;
  const auto rr_without_quality =
      resolvePathTracePostProcess(post, capabilities);
  expect(rr_without_quality.active_denoiser ==
                 PathTraceDenoiser::Raw &&
             rr_without_quality.reconstruction_mode ==
                 PathTraceUpscale::Off &&
             rr_without_quality.rr_mode_required,
         "Ray Reconstruction requires a low-resolution quality tier");
  auto retired_reblur = post;
  retired_reblur.requested_denoiser =
      static_cast<PathTraceDenoiser>(2);
  retired_reblur.requested_upscale = PathTraceUpscale::Off;
  expect(normalizePathTraceSettings(retired_reblur).requested_denoiser ==
             PathTraceDenoiser::Raw,
         "retired NRD REBLUR setting migrates safely to Raw");
  auto retired_relax = post;
  retired_relax.requested_denoiser =
      static_cast<PathTraceDenoiser>(3);
  retired_relax.requested_upscale = PathTraceUpscale::Off;
  expect(normalizePathTraceSettings(retired_relax).requested_denoiser ==
             PathTraceDenoiser::Raw,
         "retired NRD RELAX setting migrates safely to Raw");

  const auto snapshot = makePathTraceRenderSnapshot(physical);
  physical.max_bounces = 1u;
  expect(snapshot.settings.max_bounces != physical.max_bounces &&
             snapshot.source_generation ==
                 snapshot.settings.reset_generation,
         "still render snapshot freezes normalized path settings");
}

void testPathTraceBsdfAndDepth() {
  using xpbd::gfx::PathTraceDepthState;
  using xpbd::gfx::PathTraceLobe;
  using xpbd::gfx::PathTraceSettings;
  using xpbd::gfx::RtBsdfMaterial;
  using xpbd::gfx::advancePathTraceDepth;
  using xpbd::gfx::evaluatePathTraceRussianRoulette;
  using xpbd::gfx::evaluateRtBsdf;
  using xpbd::gfx::pathTraceBounceAllowed;
  using xpbd::gfx::pathTraceRandom01;
  using xpbd::gfx::rtBsdfLobeProbabilities;
  using xpbd::gfx::rtDielectricF0FromIor;
  using xpbd::gfx::rtDielectricIorFromF0;
  using xpbd::gfx::rtFresnelDielectric;
  using xpbd::gfx::rtFresnelSchlick;
  using xpbd::gfx::rtGgxDistribution;
  using xpbd::gfx::rtShadingNormalCorrection;
  using xpbd::gfx::rtSmithGgxG1;
  using xpbd::gfx::samplePathTraceCosineHemisphere;
  using xpbd::gfx::sampleRtBsdf;

  const float glass_f0 = rtDielectricF0FromIor(1.5f);
  const float glass_ior = rtDielectricIorFromF0(glass_f0);
  expect(std::abs(glass_f0 - 0.04f) < 1.0e-5f &&
             std::abs(glass_ior - 1.5f) < 1.0e-4f,
         "dielectric F0 and IOR round-trip at glass reference");
  expect(std::abs(rtFresnelDielectric(1.0f, 1.0f, 1.5f) -
                  glass_f0) < 1.0e-5f &&
             rtFresnelDielectric(0.2f, 1.5f, 1.0f) == 1.0f,
         "exact dielectric Fresnel matches normal incidence and TIR");
  const auto schlick_normal =
      rtFresnelSchlick({0.04f, 0.2f, 0.8f}, 1.0f);
  const auto schlick_grazing =
      rtFresnelSchlick({0.04f, 0.2f, 0.8f}, 0.0f);
  expect(schlick_normal ==
             std::array<float, 3>{0.04f, 0.2f, 0.8f} &&
             schlick_grazing ==
                 std::array<float, 3>{1.0f, 1.0f, 1.0f},
         "Schlick Fresnel preserves F0 and reaches one at grazing");
  expect(std::isfinite(rtGgxDistribution(1.0f, 0.02f)) &&
             rtGgxDistribution(1.0f, 0.02f) >
                 rtGgxDistribution(1.0f, 0.8f) &&
             rtSmithGgxG1(1.0f, 0.5f) == 1.0f &&
             rtSmithGgxG1(0.0f, 0.5f) == 0.0f,
         "GGX distribution and Smith masking retain finite limits");

  RtBsdfMaterial dielectric;
  dielectric.base_color = {0.8f, 0.6f, 0.3f};
  dielectric.f0 = {glass_f0, glass_f0, glass_f0};
  dielectric.roughness = 0.3f;
  dielectric.ior = 1.5f;
  const auto dielectric_probabilities =
      rtBsdfLobeProbabilities(dielectric);
  expect(dielectric_probabilities.diffuse > 0.0f &&
             dielectric_probabilities.glossy > 0.0f &&
             dielectric_probabilities.transmission == 0.0f,
         "opaque dielectric selects diffuse and glossy only");

  RtBsdfMaterial metal = dielectric;
  metal.base_color = {0.1f, 0.9f, 0.25f};
  metal.f0 = {0.9f, 0.55f, 0.15f};
  metal.transmission = 1.0f;
  metal.metal = true;
  const auto metal_probabilities = rtBsdfLobeProbabilities(metal);
  expect(metal_probabilities.diffuse == 0.0f &&
             metal_probabilities.glossy == 1.0f &&
             metal_probabilities.transmission == 0.0f,
         "metal suppresses diffuse and transmission lobes");

  const std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> view{0.0f, 1.0f, 0.0f};
  std::uint32_t valid_samples = 0u;
  bool sample_eval_consistent = true;
  for (std::uint32_t sample_index = 0u; sample_index < 4096u;
       ++sample_index) {
    const auto sampled = sampleRtBsdf(
        dielectric, normal, view, true,
        pathTraceRandom01(3u, 7u, sample_index, 0u, 91u),
        pathTraceRandom01(3u, 7u, sample_index, 1u, 91u),
        pathTraceRandom01(3u, 7u, sample_index, 2u, 91u));
    if (!sampled.valid) {
      continue;
    }
    ++valid_samples;
    const auto evaluated =
        evaluateRtBsdf(dielectric, normal, view, sampled.direction);
    sample_eval_consistent =
        sample_eval_consistent && evaluated.valid &&
        std::abs(evaluated.pdf - sampled.pdf) < 1.0e-6f;
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      sample_eval_consistent =
          sample_eval_consistent &&
          std::abs(evaluated.value[channel] -
                   sampled.value[channel]) < 1.0e-6f;
    }
  }
  expect(sample_eval_consistent,
         "BSDF sample/eval values and mixture PDFs are consistent");
  expect(valid_samples > 3500u,
         "deterministic BSDF sampling yields sufficient valid reflections");

  const std::array<RtBsdfMaterial, 4> furnace_materials{{
      {{1.0f, 1.0f, 1.0f}, {0.04f, 0.04f, 0.04f},
       0.05f, 0.0f, 1.5f, false},
      {{1.0f, 1.0f, 1.0f}, {0.04f, 0.04f, 0.04f},
       0.8f, 0.0f, 1.5f, false},
      {{1.0f, 1.0f, 1.0f}, {0.95f, 0.7f, 0.2f},
       0.08f, 0.0f, 1.5f, true},
      {{1.0f, 1.0f, 1.0f}, {0.95f, 0.7f, 0.2f},
       0.7f, 0.0f, 1.5f, true},
  }};
  for (std::size_t material_index = 0u;
       material_index < furnace_materials.size(); ++material_index) {
    std::array<double, 3> furnace{};
    constexpr std::uint32_t kFurnaceSamples = 32768u;
    for (std::uint32_t sample_index = 0u;
         sample_index < kFurnaceSamples; ++sample_index) {
      const auto direction = samplePathTraceCosineHemisphere(
          normal,
          pathTraceRandom01(11u, 19u, sample_index, 0u,
                            501u + static_cast<std::uint32_t>(material_index)),
          pathTraceRandom01(11u, 19u, sample_index, 1u,
                            501u + static_cast<std::uint32_t>(material_index)));
      const auto evaluated = evaluateRtBsdf(
          furnace_materials[material_index], normal, view,
          direction.direction);
      if (!evaluated.valid) {
        continue;
      }
      constexpr double kPi = 3.14159265358979323846;
      for (std::size_t channel = 0u; channel < 3u; ++channel) {
        furnace[channel] += evaluated.value[channel] * kPi;
      }
    }
    for (double &channel : furnace) {
      channel /= kFurnaceSamples;
      expect(std::isfinite(channel) && channel >= 0.0 &&
                 channel <= 1.02,
             "white-furnace reflection remains finite and energy bounded");
    }
  }

  RtBsdfMaterial glass;
  glass.base_color = {1.0f, 0.95f, 0.9f};
  glass.f0 = {glass_f0, glass_f0, glass_f0};
  glass.roughness = 0.05f;
  glass.transmission = 1.0f;
  glass.ior = 1.5f;
  const auto refraction =
      sampleRtBsdf(glass, normal, view, true, 0.99f, 0.4f, 0.7f);
  expect(refraction.valid && refraction.delta &&
             refraction.lobe == PathTraceLobe::Transmission &&
             !refraction.total_internal_reflection &&
             refraction.direction[1] < 0.0f,
         "glass sample refracts through the opposite hemisphere");
  const std::array<float, 3> inside_grazing{
      0.9f, 0.4358899f, 0.0f};
  const auto tir = sampleRtBsdf(
      glass, normal, inside_grazing, false, 0.99f, 0.4f, 0.7f);
  expect(tir.valid && tir.delta &&
             tir.total_internal_reflection &&
             tir.direction[1] > 0.0f,
         "glass sample reflects under total internal reflection");
  const std::array<std::array<float, 3>, 2> glass_views{{
      view, inside_grazing}};
  const std::array<bool, 2> glass_front_faces{{true, false}};
  for (std::size_t view_index = 0u;
       view_index < glass_views.size(); ++view_index) {
    std::array<double, 3> energy{};
    constexpr std::uint32_t kGlassEnergySamples = 65536u;
    for (std::uint32_t sample_index = 0u;
         sample_index < kGlassEnergySamples; ++sample_index) {
      const auto sampled = sampleRtBsdf(
          glass, normal, glass_views[view_index],
          glass_front_faces[view_index],
          pathTraceRandom01(23u, 31u, sample_index, 0u,
                            701u + static_cast<std::uint32_t>(view_index)),
          pathTraceRandom01(23u, 31u, sample_index, 1u,
                            701u + static_cast<std::uint32_t>(view_index)),
          pathTraceRandom01(23u, 31u, sample_index, 2u,
                            701u + static_cast<std::uint32_t>(view_index)));
      if (!sampled.valid) {
        continue;
      }
      for (std::size_t channel = 0u; channel < 3u; ++channel) {
        energy[channel] += sampled.weight[channel];
      }
    }
    const double energy_limit = view_index == 0u ? 1.02 : 1.05;
    for (double &channel : energy) {
      channel /= kGlassEnergySamples;
      expect(std::isfinite(channel) && channel >= 0.0 &&
                 channel <= energy_limit,
             "glass/TIR white-furnace sampling remains energy bounded");
    }
  }

  expect(rtShadingNormalCorrection(normal, normal, view, normal) == 1.0f,
         "identical normals have unit shading-normal correction");
  const float tilted_correction = rtShadingNormalCorrection(
      normal, {0.0f, 0.8f, 0.6f}, view, {0.3f, 0.9f, 0.1f});
  expect(std::isfinite(tilted_correction) &&
             tilted_correction > 0.0f && tilted_correction <= 16.0f,
         "tilted shading-normal correction stays finite and bounded");
  expect(rtShadingNormalCorrection(
             normal, {0.0f, 0.8f, 0.6f}, view,
             {0.0f, -0.2f, 1.0f}) == 0.0f,
         "shading-normal correction rejects hemisphere disagreement");

  PathTraceSettings depth_settings;
  depth_settings.max_bounces = 3u;
  depth_settings.max_diffuse_bounces = 1u;
  depth_settings.max_glossy_bounces = 2u;
  depth_settings.max_transmission_bounces = 1u;
  depth_settings.russian_roulette_start = 2u;
  PathTraceDepthState depth;
  const auto first_diffuse =
      advancePathTraceDepth(depth_settings, depth, PathTraceLobe::Diffuse);
  expect(first_diffuse.has_value() &&
             !pathTraceBounceAllowed(depth_settings, *first_diffuse,
                                     PathTraceLobe::Diffuse),
         "per-lobe diffuse boundary stops exactly at the configured count");
  const auto second_glossy = advancePathTraceDepth(
      depth_settings, *first_diffuse, PathTraceLobe::Glossy);
  const auto third_transmission = advancePathTraceDepth(
      depth_settings, *second_glossy, PathTraceLobe::Transmission);
  expect(third_transmission.has_value() &&
             !pathTraceBounceAllowed(depth_settings, *third_transmission,
                                     PathTraceLobe::Glossy),
         "total bounce boundary stops every lobe exactly");

  const auto rr_before = evaluatePathTraceRussianRoulette(
      depth_settings, *first_diffuse, 0.2f, 0.9f);
  const auto rr_kill = evaluatePathTraceRussianRoulette(
      depth_settings, *second_glossy, 0.2f, 0.9f);
  const auto rr_survive = evaluatePathTraceRussianRoulette(
      depth_settings, *second_glossy, 0.2f, 0.1f);
  expect(!rr_before.applied && rr_before.survives &&
             rr_kill.applied && !rr_kill.survives &&
             rr_survive.applied && rr_survive.survives &&
             std::abs(rr_survive.throughput_scale - 5.0f) < 1.0e-6f,
         "Russian roulette starts at its boundary and reweights survivors");
}

void testEmptyTextureSample() {
  xpbd::gfx::TextureImage empty;
  float r = 0, g = 0, b = 0, a = 0;
  empty.sample(0.0f, 0.0f, r, g, b, a);
  expect(r == 1.0f && g == 1.0f && b == 1.0f && a == 1.0f,
         "empty texture samples white");
}

void testViewportMeshEmptyGeometry() {
  xpbd::loader::Geometry geo;
  xpbd::baker::BoneMapper mapper;
  xpbd::gfx::ViewportMeshBuilder builder;
  builder.setGeometry(&geo);
  builder.setBoneMapper(&mapper);
  builder.setShowBones(false);
  builder.setShowGround(true);

  xpbd::gfx::ViewportGpuScene scene;
  builder.buildRest(scene);
  // Ground grid alone should produce some line geometry when enabled.
  expect(scene.line_segment_count >= 0, "buildRest completes");
  expect(scene.cube_count == 0, "empty geometry has zero cubes");

  xpbd::gfx::StaticIndexedModelMesh static_mesh;
  builder.buildStaticIndexedModel(static_mesh);
  expect(static_mesh.cube_count == 0, "static indexed model empty");
  expect(static_mesh.vertices.empty(), "static model has no vertices");
}

void testCanonicalCubeAndRtSceneRecords() {
  xpbd::loader::Geometry geometry;
  geometry.description.has_texture_size = true;
  geometry.description.texture_width = 16;
  geometry.description.texture_height = 16;
  xpbd::loader::Bone root;
  root.name = "root";
  xpbd::loader::Cube root_cube;
  root_cube.uv_mode = xpbd::loader::CubeUVMode::Box;
  root_cube.uv_box[0] = 0.0;
  root_cube.uv_box[1] = 0.0;
  root.cubes.push_back(root_cube);
  geometry.bones.push_back(root);

  xpbd::baker::BoneMapper mapper;
  mapper.replaceModelBones(geometry.bones);
  xpbd::gfx::ViewportMeshBuilder builder;
  builder.setGeometry(&geometry);
  builder.setBoneMapper(&mapper);
  xpbd::gfx::StaticIndexedModelMesh mesh;
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.cube_count == 1u && mesh.faces.size() == 6u,
         "canonical non-degenerate cube emits six faces");
  expect(mesh.vertices.size() == 24u && mesh.indices.size() == 36u,
         "canonical cube emits 24 face-split vertices and 12 triangles");
  bool flat_normals = true;
  for (const auto &face : mesh.faces) {
    for (std::uint32_t vertex = 1u; vertex < face.vertex_count; ++vertex) {
      const auto &first = mesh.vertices[face.first_vertex];
      const auto &next = mesh.vertices[face.first_vertex + vertex];
      flat_normals =
          flat_normals &&
          std::abs(first.nx - next.nx) < 1.0e-6f &&
          std::abs(first.ny - next.ny) < 1.0e-6f &&
          std::abs(first.nz - next.nz) < 1.0e-6f;
    }
  }
  expect(flat_normals, "canonical cube preserves one flat normal per face");

  xpbd::loader::Geometry transformed_geometry;
  transformed_geometry.description.has_texture_size = true;
  transformed_geometry.description.texture_width = 16;
  transformed_geometry.description.texture_height = 16;
  xpbd::loader::Bone transformed_bone;
  transformed_bone.name = "transformed";
  xpbd::loader::Cube transformed_cube;
  transformed_cube.origin[0] = 1.0;
  transformed_cube.origin[1] = 2.0;
  transformed_cube.origin[2] = 3.0;
  transformed_cube.size[0] = 2.0;
  transformed_cube.size[1] = 4.0;
  transformed_cube.size[2] = 6.0;
  transformed_cube.inflate = 0.5;
  transformed_cube.has_pivot = true;
  transformed_cube.pivot[0] = 2.0;
  transformed_cube.pivot[1] = 4.0;
  transformed_cube.pivot[2] = 6.0;
  transformed_cube.has_rotation = true;
  transformed_cube.rotation[0] = 15.0;
  transformed_cube.rotation[1] = 30.0;
  transformed_cube.rotation[2] = 45.0;
  transformed_cube.uv_mode = xpbd::loader::CubeUVMode::Box;
  transformed_cube.uv_box[0] = 1.0;
  transformed_cube.uv_box[1] = 2.0;
  transformed_cube.mirror = true;
  transformed_bone.cubes.push_back(transformed_cube);
  transformed_geometry.bones.push_back(transformed_bone);
  xpbd::baker::BoneMapper transformed_mapper;
  transformed_mapper.replaceModelBones(transformed_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder transformed_builder;
  transformed_builder.setGeometry(&transformed_geometry);
  transformed_builder.setBoneMapper(&transformed_mapper);
  xpbd::gfx::StaticIndexedModelMesh transformed_mesh;
  transformed_builder.buildStaticIndexedModel(transformed_mesh);
  const auto transformed_bind =
      xpbd::baker::CubeGeometry::bindVertices(transformed_cube);
  bool vertices_match_bind = transformed_mesh.faces.size() == 6u;
  for (const auto &vertex : transformed_mesh.vertices) {
    bool matched = false;
    for (std::size_t source = 0; source < 8u; ++source) {
      matched = matched ||
                (std::abs(vertex.px -
                          transformed_bind[source * 3u + 0u]) < 1.0e-5 &&
                 std::abs(vertex.py -
                          transformed_bind[source * 3u + 1u]) < 1.0e-5 &&
                 std::abs(vertex.pz -
                          transformed_bind[source * 3u + 2u]) < 1.0e-5);
    }
    vertices_match_bind = vertices_match_bind && matched;
  }
  expect(vertices_match_bind,
         "canonical tessellator preserves pivot/rotation/inflate vertices");
  expect(transformed_mesh.vertices[0].u >
             transformed_mesh.vertices[1].u &&
             transformed_mesh.vertices[0].tangent_handedness < 0.0f,
         "mirrored Box UV reverses U and tangent handedness");

  xpbd::loader::Geometry per_face_geometry;
  per_face_geometry.description.has_texture_size = true;
  per_face_geometry.description.texture_width = 16;
  per_face_geometry.description.texture_height = 16;
  xpbd::loader::Bone per_face_bone;
  per_face_bone.name = "per_face";
  xpbd::loader::Cube per_face_cube;
  per_face_cube.uv_mode = xpbd::loader::CubeUVMode::PerFace;
  per_face_cube.uv_north = {4.0, 5.0, 2.0, 3.0, true};
  per_face_cube.uv_north.rotation_degrees = 90;
  per_face_bone.cubes.push_back(per_face_cube);
  per_face_geometry.bones.push_back(per_face_bone);
  xpbd::baker::BoneMapper per_face_mapper;
  per_face_mapper.replaceModelBones(per_face_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder per_face_builder;
  per_face_builder.setGeometry(&per_face_geometry);
  per_face_builder.setBoneMapper(&per_face_mapper);
  xpbd::gfx::StaticIndexedModelMesh per_face_mesh;
  per_face_builder.buildStaticIndexedModel(per_face_mesh);
  std::size_t textured_face_count = 0u;
  for (const auto &face : per_face_mesh.faces) {
    textured_face_count += face.textured ? 1u : 0u;
  }
  const auto north_it = std::find_if(
      per_face_mesh.faces.begin(), per_face_mesh.faces.end(),
      [](const xpbd::gfx::StaticModelFace &face) {
        return face.direction ==
               xpbd::gfx::StaticModelFaceDirection::North;
      });
  const xpbd::gfx::StaticModelFace *north_face =
      north_it != per_face_mesh.faces.end() ? &*north_it : nullptr;
  expect(per_face_mesh.faces.size() == 1u &&
             textured_face_count == 1u &&
             north_face != nullptr && north_face->textured,
         "per-face UV omits unauthored faces and keeps authored face");
  if (north_face != nullptr) {
    expectNear(per_face_mesh.vertices[north_face->first_vertex].u,
               4.0f / 16.0f, 1.0e-6f,
               "per-face UV rotation preserves authored U edge");
    expectNear(per_face_mesh.vertices[north_face->first_vertex].v,
               8.0f / 16.0f, 1.0e-6f,
               "per-face UV rotation maps the clockwise source corner");
  }

  xpbd::loader::Geometry flat_geometry = per_face_geometry;
  flat_geometry.bones[0].name = "flat_face";
  flat_geometry.bones[0].cubes[0].size[1] = 0.0;
  flat_geometry.bones[0].cubes[0].uv_north = {};
  flat_geometry.bones[0].cubes[0].uv_south = {};
  flat_geometry.bones[0].cubes[0].uv_up = {4.0, 5.0, 2.0, 3.0, true};
  flat_geometry.bones[0].cubes[0].uv_down = {4.0, 5.0, 2.0, 3.0, true};
  xpbd::baker::BoneMapper flat_mapper;
  flat_mapper.replaceModelBones(flat_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder flat_builder;
  flat_builder.setGeometry(&flat_geometry);
  flat_builder.setBoneMapper(&flat_mapper);
  xpbd::gfx::StaticIndexedModelMesh flat_mesh;
  flat_builder.buildStaticIndexedModel(flat_mesh);
  expect(flat_mesh.faces.size() == 1u &&
             flat_mesh.faces.front().direction ==
                 xpbd::gfx::StaticModelFaceDirection::Up,
         "zero-thickness UV cube removes coincident opposite face");
  if (!flat_mesh.faces.empty()) {
    const auto first_vertex = flat_mesh.faces.front().first_vertex;
    expectNear(flat_mesh.vertices[first_vertex].u, 6.0f / 16.0f, 1.0e-6f,
               "Bedrock Up UV starts at the opposite U corner");
    expectNear(flat_mesh.vertices[first_vertex].v, 8.0f / 16.0f, 1.0e-6f,
               "Bedrock Up UV starts at the opposite V corner");
  }

  xpbd::gfx::TextureImage base_atlas;
  base_atlas.width = 1;
  base_atlas.height = 1;
  base_atlas.source_channels = 4;
  base_atlas.rgba = {255u, 255u, 255u, 255u};
  const auto draw_plan =
      xpbd::gfx::makeStaticModelDrawPlan(mesh, &base_atlas);
  expect(draw_plan.primitive_materials.size() == 12u,
         "draw plan keeps metadata for every canonical cube triangle");
  bool source_identity = true;
  for (std::size_t primitive = 0;
       primitive < draw_plan.primitive_materials.size(); ++primitive) {
    const auto &source = draw_plan.primitive_materials[primitive];
    source_identity =
        source_identity && source.source_face_index == primitive / 2u &&
        source.source_triangle_index == primitive % 2u &&
        source.bone_index == 0u && source.cube_index == 0u &&
        source.face_direction ==
            static_cast<xpbd::gfx::StaticModelFaceDirection>(
                primitive / 2u);
  }
  expect(source_identity,
         "draw-plan reordering metadata preserves primitive source identity");

  xpbd::gfx::ResolvedMaterialTable emissive_material;
  emissive_material.specular_map_active = true;
  auto records = xpbd::gfx::buildRigidModelRtSceneRecords(
      mesh, draw_plan, &emissive_material);
  expect(records.valid() && records.geometries.size() == 1u &&
             records.instances.size() == 1u &&
             records.primitives.size() == 12u,
         "one rigid bone produces one geometry, instance, and 12 records");
  expect(records.geometries[0].blas_policy ==
             xpbd::gfx::RtBlasPolicy::RigidLocalSpace &&
             records.geometries[0].local_space &&
             !records.geometries[0].dynamic_vertices,
         "rigid cube group selects immutable local-space BLAS policy");
  expect(records.instances[0].instance_custom_index == 0u &&
             records.primitives[0].uses_emission_texture,
         "stable instance id and read-only emission metadata are retained");
  expect(records.materials.size() == 1u &&
             records.materials[0].read_only &&
             records.materials[0].feature_flags ==
                 xpbd::gfx::kLabPbrSpecularMapActive,
         "RT scene exposes one read-only resolved-material record");
  const auto packed_layout =
      xpbd::gfx::buildRtPackedPrimitiveLayout(draw_plan, records);
  expect(packed_layout.valid() &&
             packed_layout.geometry_ranges.size() == 1u &&
             packed_layout.geometry_ranges[0].first_index == 0u &&
             packed_layout.geometry_ranges[0].index_count == 36u &&
             packed_layout.source_primitive_indices.size() == 12u,
         "single-bone RT primitive packing is dense and complete");

  std::vector<xpbd::gfx::StaticModelBoneState> transforms(1);
  transforms[0].transform[12] = 2.0f;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             records, transforms, true),
         "rigid instance transform initializes with reset history");
  expectNear(records.instances[0].current_transform[12], 2.0f, 1.0e-6f,
             "rigid instance current transform uses bone transform");
  expectNear(records.instances[0].previous_transform[12], 2.0f, 1.0e-6f,
             "reset rigid history copies current into previous");
  transforms[0].transform[12] = 4.0f;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             records, transforms, false),
         "rigid instance transform advances without BLAS mutation");
  expectNear(records.instances[0].previous_transform[12], 2.0f, 1.0e-6f,
             "rigid instance keeps previous transform for motion");
  expectNear(records.instances[0].current_transform[12], 4.0f, 1.0e-6f,
             "rigid instance installs next current transform");

  xpbd::loader::Bone child;
  child.name = "child";
  xpbd::loader::Cube child_cube;
  child_cube.uv_mode = xpbd::loader::CubeUVMode::Box;
  child_cube.uv_box[0] = 0.0;
  child_cube.uv_box[1] = 0.0;
  child.cubes.push_back(child_cube);
  auto two_bone_geometry = geometry;
  two_bone_geometry.bones[0].rotation[1] = 90.0;
  child.has_parent = true;
  child.parent = "root";
  two_bone_geometry.bones.push_back(child);
  xpbd::baker::BoneMapper two_bone_mapper;
  two_bone_mapper.replaceModelBones(two_bone_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder two_bone_builder;
  two_bone_builder.setGeometry(&two_bone_geometry);
  two_bone_builder.setBoneMapper(&two_bone_mapper);
  xpbd::gfx::StaticIndexedModelMesh two_bone_mesh;
  two_bone_builder.buildStaticIndexedModel(two_bone_mesh);
  xpbd::gfx::StaticModelFrameData hierarchy_frame;
  two_bone_builder.buildStaticRestFrame(hierarchy_frame);
  bool inherited_parent_rotation = hierarchy_frame.bones.size() == 2u;
  for (const std::size_t element :
       {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u}) {
    inherited_parent_rotation =
        inherited_parent_rotation &&
        std::abs(hierarchy_frame.bones[0].transform[element] -
                 hierarchy_frame.bones[1].transform[element]) < 1.0e-5f;
  }
  inherited_parent_rotation =
      inherited_parent_rotation &&
      std::abs(hierarchy_frame.bones[1].transform[0] - 1.0f) > 0.5f;
  expect(inherited_parent_rotation,
         "child TLAS transform inherits parent rest rotation");
  const auto two_bone_plan =
      xpbd::gfx::makeStaticModelDrawPlan(two_bone_mesh, &base_atlas);
  auto two_bone_records =
      xpbd::gfx::buildRigidModelRtSceneRecords(
          two_bone_mesh, two_bone_plan, &emissive_material);
  const auto two_bone_packed =
      xpbd::gfx::buildRtPackedPrimitiveLayout(two_bone_plan,
                                               two_bone_records);
  expect(two_bone_records.valid() &&
             two_bone_records.geometries.size() == 2u &&
             two_bone_records.instances[0].instance_custom_index == 0u &&
             two_bone_records.instances[1].instance_custom_index == 1u,
         "two rigid bones produce stable dense TLAS instance ids");
  expect(two_bone_packed.valid() &&
             two_bone_packed.geometry_ranges.size() == 2u &&
             two_bone_packed.geometry_ranges[0].first_index == 0u &&
             two_bone_packed.geometry_ranges[0].index_count == 36u &&
             two_bone_packed.geometry_ranges[1].first_index == 36u &&
             two_bone_packed.geometry_ranges[1].index_count == 36u &&
             two_bone_packed.indices.size() == 72u,
         "per-bone BLAS ranges repack all primitives without omission");
  const auto child_first_identity =
      xpbd::gfx::resolveRtPackedPrimitiveIdentity(
          two_bone_packed, two_bone_records, 1u, 0u);
  const auto child_last_identity =
      xpbd::gfx::resolveRtPackedPrimitiveIdentity(
          two_bone_packed, two_bone_records, 1u, 11u);
  expect(child_first_identity.has_value() &&
             child_first_identity->packed_primitive_index == 12u &&
             child_first_identity->source_primitive_index == 12u &&
             child_first_identity->geometry_index == 1u &&
             child_first_identity->bone_index == 1u &&
             child_first_identity->cube_index == 0u,
         "GPU-style second-instance first hit resolves source cube identity");
  expect(child_last_identity.has_value() &&
             child_last_identity->packed_primitive_index == 23u &&
             child_last_identity->source_primitive_index == 23u &&
             child_last_identity->geometry_index == 1u &&
             child_last_identity->bone_index == 1u,
         "GPU-style second-instance last hit stays within packed range");
  expect(!xpbd::gfx::resolveRtPackedPrimitiveIdentity(
              two_bone_packed, two_bone_records, 2u, 0u)
              .has_value() &&
             !xpbd::gfx::resolveRtPackedPrimitiveIdentity(
                 two_bone_packed, two_bone_records, 1u, 12u)
                 .has_value(),
         "GPU-style identity resolver rejects invalid instance/local indices");

  xpbd::loader::Animation rigid_animation;
  rigid_animation.loop = true;
  rigid_animation.loop_behavior =
      xpbd::loader::Animation::LoopBehavior::Loop;
  rigid_animation.animation_length = 2.0;
  xpbd::loader::BoneAnimation root_motion;
  root_motion.has_rotation = true;
  root_motion.rotation.put(0.0, {0.0, 0.0, 0.0});
  root_motion.rotation.put(1.0, {0.0, 45.0, 0.0});
  root_motion.rotation.put(2.0, {0.0, 0.0, 0.0});
  root_motion.setLooping(true);
  rigid_animation.bones.emplace("root", std::move(root_motion));

  xpbd::gfx::StaticModelFrameData rigid_frame_zero;
  xpbd::gfx::StaticModelFrameData rigid_frame_one;
  two_bone_builder.buildStaticAnimationFrame(&rigid_animation, 0.0,
                                             rigid_frame_zero);
  two_bone_builder.buildStaticAnimationFrame(&rigid_animation, 1.0,
                                             rigid_frame_one);
  expect(rigid_frame_zero.bones.size() == 2u &&
             rigid_frame_one.bones.size() == 2u &&
             rigid_frame_zero.bones[0].transform !=
                 rigid_frame_one.bones[0].transform &&
             rigid_frame_zero.bones[1].transform !=
                 rigid_frame_one.bones[1].transform,
         "numeric live animation changes parent and inherited child transforms");
  const auto rigid_local_vertices = two_bone_mesh.vertices;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             two_bone_records, rigid_frame_zero.bones, true) &&
             xpbd::gfx::updateRigidRtInstanceTransforms(
                 two_bone_records, rigid_frame_one.bones, false),
         "live rigid animation advances TLAS transform history");
  expect(two_bone_records.instances[0].previous_transform !=
                 two_bone_records.instances[0].current_transform &&
             two_bone_records.instances[1].previous_transform !=
                 two_bone_records.instances[1].current_transform,
         "live rigid animation keeps distinct current and previous transforms");
  bool rigid_local_positions_unchanged =
      two_bone_mesh.vertices.size() == rigid_local_vertices.size();
  for (std::size_t vertex = 0;
       rigid_local_positions_unchanged &&
       vertex < two_bone_mesh.vertices.size();
       ++vertex) {
    rigid_local_positions_unchanged =
        two_bone_mesh.vertices[vertex].px ==
            rigid_local_vertices[vertex].px &&
        two_bone_mesh.vertices[vertex].py ==
            rigid_local_vertices[vertex].py &&
        two_bone_mesh.vertices[vertex].pz ==
            rigid_local_vertices[vertex].pz;
  }
  expect(rigid_local_positions_unchanged,
         "live rigid animation leaves canonical BLAS-local vertices unchanged");

  geometry.bones[0].cubes.push_back(root_cube);
  mapper.replaceModelBones(geometry.bones);
  builder.setGeometry(&geometry);
  builder.setBoneMapper(&mapper);
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.faces.size() == 12u && mesh.indices.size() == 72u,
         "overlapping opaque source cubes retain all internal faces");

  xpbd::loader::Geometry degenerate_geometry;
  xpbd::loader::Bone degenerate_bone;
  degenerate_bone.name = "flat";
  xpbd::loader::Cube degenerate_cube;
  degenerate_cube.size[0] = 0.0;
  degenerate_bone.cubes.push_back(degenerate_cube);
  degenerate_geometry.bones.push_back(degenerate_bone);
  mapper.replaceModelBones(degenerate_geometry.bones);
  builder.setGeometry(&degenerate_geometry);
  builder.setBoneMapper(&mapper);
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.cube_count == 1u && mesh.faces.size() == 1u &&
             mesh.indices.size() == 6u,
         "degenerate flat cube counts source but emits one two-sided face");
}

void testRayTracingCapability() {
  using xpbd::gfx::evaluateRayTracingCapability;
  using xpbd::gfx::isNvidiaVendorId;
  using xpbd::gfx::kVendorIdNvidia;
  using xpbd::gfx::RenderPath;
  using xpbd::gfx::resolveRenderPath;
  using xpbd::gfx::clampRayTracingPreference;

  expect(isNvidiaVendorId(kVendorIdNvidia), "NVIDIA vendor id match");
  expect(!isNvidiaVendorId(0x1002u), "AMD vendor is not NVIDIA");
  expect(!isNvidiaVendorId(0x8086u), "Intel vendor is not NVIDIA");

  auto amd = evaluateRayTracingCapability(0x1002u, 1, "Radeon", true, true, 1);
  expect(!amd.supported, "AMD with RT exts still unsupported (NVIDIA-only)");
  expect(!amd.unsupported_reason.empty(), "AMD has reason string");

  auto nv_no_ext =
      evaluateRayTracingCapability(kVendorIdNvidia, 1, "RTX", false, true, 1);
  expect(!nv_no_ext.supported, "NVIDIA without RT extensions unsupported");

  auto nv_no_feat =
      evaluateRayTracingCapability(kVendorIdNvidia, 1, "RTX", true, false, 1);
  expect(!nv_no_feat.supported, "NVIDIA without RT features unsupported");

  auto nv_ok =
      evaluateRayTracingCapability(kVendorIdNvidia, 1, "RTX 3080", true, true,
                                   31, 0, 0);
  expect(nv_ok.supported, "NVIDIA with RT extensions+features supported");
  expect(nv_ok.is_nvidia, "flag is_nvidia");
  expect(nv_ok.max_ray_recursion_depth == 31u, "recursion depth stored");

  expect(resolveRenderPath(false, nv_ok) == RenderPath::Raster,
         "default path is raster");
  expect(resolveRenderPath(true, nv_ok) == RenderPath::Raster,
         "RT preference without device_extensions_enabled stays raster");
  nv_ok.device_extensions_enabled = true;
  expect(resolveRenderPath(true, nv_ok) == RenderPath::RayTracing,
         "RT preference with armed device uses RT path");
  expect(resolveRenderPath(true, amd) == RenderPath::Raster,
         "unsupported GPU falls back to raster");

  expect(!clampRayTracingPreference(true, false),
         "clamp clears preference when HW unsupported");
  expect(clampRayTracingPreference(true, true),
         "clamp keeps preference when HW supported");
  expect(!clampRayTracingPreference(false, true),
         "clamp keeps default-off preference");
}

void testRtAlphaSemantics() {
  using xpbd::gfx::RtAlphaMode;
  using xpbd::gfx::RtFrontToBackAccumulator;
  using xpbd::gfx::rtAcceptedOpacity;
  using xpbd::gfx::rtShadowVisibilityAfter;

  expectNear(rtAcceptedOpacity(RtAlphaMode::Cutout, 0.0f), 0.0f, 1.0e-6f,
             "RT cutout rejects transparent texel");
  expectNear(rtAcceptedOpacity(RtAlphaMode::Cutout, 0.5f), 1.0f, 1.0e-6f,
             "RT cutout accepts surviving texel as opaque");
  expectNear(rtAcceptedOpacity(RtAlphaMode::Blend, 0.25f), 0.25f, 1.0e-6f,
             "RT blend preserves fractional alpha");

  RtFrontToBackAccumulator accumulated;
  expect(!accumulated.add(1.0f, 0.0f, 0.0f, 0.25f,
                          RtAlphaMode::Blend),
         "front blend keeps traversal open");
  expectNear(accumulated.premultiplied_r, 0.25f, 1.0e-6f,
             "front blend stores premultiplied red");
  expectNear(accumulated.alpha(), 0.25f, 1.0e-6f,
             "front blend output alpha");
  expect(accumulated.add(0.0f, 0.0f, 1.0f, 1.0f,
                         RtAlphaMode::Opaque),
         "opaque surface terminates traversal");
  expectNear(accumulated.premultiplied_b, 0.75f, 1.0e-6f,
             "opaque surface fills remaining transmittance");
  expectNear(accumulated.alpha(), 1.0f, 1.0e-6f,
             "front-to-back stack becomes opaque");

  float shadow = rtShadowVisibilityAfter(1.0f, 0.5f, RtAlphaMode::Blend);
  shadow = rtShadowVisibilityAfter(shadow, 0.5f, RtAlphaMode::Blend);
  expectNear(shadow, 0.25f, 1.0e-6f,
             "two half-alpha layers transmit one quarter shadow light");
}

void testRtMotionProjection() {
  using xpbd::gfx::evaluateRtMotionProjection;
  using xpbd::gfx::RtMotionProjectionInput;

  RtMotionProjectionInput input;
  input.current_uv = {0.5f, 0.5f};
  input.previous_clip = {0.0f, 0.0f, 0.5f, 1.0f};
  input.viewport_width = 1920u;
  input.viewport_height = 1080u;

  auto projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion rejects absent camera/geometry history");

  input.camera_history_valid = true;
  input.geometry_history_valid = true;
  projected = evaluateRtMotionProjection(input);
  expect(projected.valid && projected.disocclusion == 0.0f,
         "motion accepts finite in-viewport history");
  expectNear(projected.current_to_previous_pixels[0], 0.0f, 1.0e-6f,
             "static motion x is zero");
  expectNear(projected.current_to_previous_pixels[1], 0.0f, 1.0e-6f,
             "static motion y is zero");

  input.previous_clip = {0.25f, -0.5f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(projected.valid, "translated motion remains valid");
  expectNear(projected.current_to_previous_pixels[0], 240.0f, 1.0e-4f,
             "motion x uses current-to-previous pixel convention");
  expectNear(projected.current_to_previous_pixels[1], -270.0f, 1.0e-4f,
             "motion y uses current-to-previous pixel convention");

  input.previous_clip = {0.0f, 0.0f, 0.5f, -1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion marks behind-camera history disoccluded");

  input.previous_clip = {3.0f, 0.0f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion marks previous sample outside viewport disoccluded");

  input.previous_clip = {
      std::numeric_limits<float>::infinity(), 0.0f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion rejects non-finite previous clip values");
}

void testStaticMaterialClassification() {
  xpbd::gfx::StaticIndexedModelMesh mesh;
  mesh.bone_names.emplace_back("root");
  mesh.vertices.resize(3);
  mesh.vertices[0].u = 0.0f;
  mesh.vertices[0].v = 0.0f;
  mesh.vertices[1].u = 1.0f;
  mesh.vertices[1].v = 0.0f;
  mesh.vertices[2].u = 0.0f;
  mesh.vertices[2].v = 1.0f;
  mesh.indices = {0u, 1u, 2u};
  xpbd::gfx::StaticModelFace face;
  face.first_vertex = 0;
  face.vertex_count = 3;
  face.first_index = 0;
  face.index_count = 3;
  face.bone_index = 0;
  face.textured = true;
  mesh.faces.push_back(face);

  xpbd::gfx::TextureImage texture;
  texture.width = 1;
  texture.height = 1;
  texture.rgba = {255u, 255u, 255u, 128u};
  expect(xpbd::gfx::staticModelFaceMaterial(mesh, mesh.faces[0], &texture) ==
             xpbd::gfx::StaticModelMaterialClass::Blend,
         "fractional texture alpha classifies as RT/raster blend");
  auto blend_plan = xpbd::gfx::makeStaticModelDrawPlan(mesh, &texture);
  expect(blend_plan.blend.index_count == 3u,
         "blend face reaches blend index range");
  expect(blend_plan.primitive_materials.size() == 1u &&
             blend_plan.primitive_materials[0].textured &&
             blend_plan.primitive_materials[0].material ==
                 xpbd::gfx::StaticModelMaterialClass::Blend,
         "ordered primitive keeps exact textured blend metadata");

  texture.rgba[3] = 0u;
  expect(xpbd::gfx::staticModelFaceMaterial(mesh, mesh.faces[0], &texture) ==
             xpbd::gfx::StaticModelMaterialClass::Cutout,
         "zero-alpha texture classifies as cutout");
  texture.rgba[3] = 255u;
  expect(xpbd::gfx::staticModelFaceMaterial(mesh, mesh.faces[0], &texture) ==
             xpbd::gfx::StaticModelMaterialClass::Opaque,
         "opaque texture classifies as opaque");
}

std::vector<std::uint8_t> makeRadianceHdr(
    int width, int height,
    const std::vector<std::array<std::uint8_t, 4>> &rgbe) {
  const std::string header =
      "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
      std::to_string(height) + " +X " + std::to_string(width) + "\n";
  std::vector<std::uint8_t> encoded(header.begin(), header.end());
  for (const auto &pixel : rgbe) {
    encoded.insert(encoded.end(), pixel.begin(), pixel.end());
  }
  return encoded;
}

void testWorldEnvironmentFoundation() {
  using namespace xpbd::gfx;
  constexpr double pi = 3.14159265358979323846;

  const UtcDateTime utc{2024, 1, 1, 0, 0, 0.0};
  const ObserverLocation shanghai{31.2304, 121.4737, 5.0, 0.0};
  CelestialState celestial;
  std::string error;
  expect(computeCelestialState(utc, shanghai, celestial, &error),
         "fixed UTC/observer produces a celestial state");
  expect(celestial.valid && error.empty(),
         "valid celestial state has no error");
  expectNearDouble(celestial.sun.azimuth_degrees, 126.176, 0.002,
                   "Sun azimuth matches frozen Astronomy reference");
  expectNearDouble(celestial.sun.apparent_altitude_degrees, 11.5352, 0.002,
                   "Sun altitude matches frozen Astronomy reference");
  expectNearDouble(celestial.moon.azimuth_degrees, 266.919, 0.002,
                   "Moon azimuth matches frozen Astronomy reference");
  expectNearDouble(celestial.moon.apparent_altitude_degrees, 29.1462, 0.002,
                   "Moon altitude matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_illuminated_fraction, 0.780373, 0.000002,
                   "Moon fraction matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_magnitude, -11.1495, 0.0002,
                   "Moon magnitude matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_distance_km, 404656.0, 1.0,
                   "Moon distance matches frozen Astronomy reference");
  expectNearDouble(celestial.moon.angular_diameter_degrees, 0.492003,
                   0.000002,
                   "Moon diameter matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_libration_latitude_degrees, -4.65849,
                   0.00002,
                   "Moon libration latitude matches frozen reference");
  expectNearDouble(celestial.moon_libration_longitude_degrees, 0.039338,
                   0.00002,
                   "Moon libration longitude matches frozen reference");
  const auto length = [](const std::array<double, 3> &direction) {
    return std::sqrt(direction[0] * direction[0] +
                     direction[1] * direction[1] +
                     direction[2] * direction[2]);
  };
  expectNearDouble(length(celestial.sun.direction), 1.0, 1e-12,
                   "Sun world direction is normalized");
  expectNearDouble(length(celestial.moon.direction), 1.0, 1e-12,
                   "Moon world direction is normalized");
  expect(celestial.sun.angular_diameter_degrees > 0.45 &&
             celestial.sun.angular_diameter_degrees < 0.56,
         "Sun apparent diameter is physically bounded");
  expect(celestial.twilight == TwilightPhase::Day,
         "positive geometric Sun altitude classifies as day");

  CelestialState rotated;
  ObserverLocation rotated_north = shanghai;
  rotated_north.north_offset_degrees = 90.0;
  expect(computeCelestialState(utc, rotated_north, rotated, &error),
         "north-offset celestial state computes");
  expectNearDouble(rotated.sun.direction[0], celestial.sun.direction[2],
                   1e-12, "north offset rotates Sun world X");
  expectNearDouble(rotated.sun.direction[2], -celestial.sun.direction[0],
                   1e-12, "north offset rotates Sun world Z");

  CelestialState one_minute;
  const UtcDateTime minute_later{2024, 1, 1, 0, 1, 0.0};
  expect(computeCelestialState(minute_later, shanghai, one_minute, &error),
         "one-minute-later celestial state computes");
  const double continuity_dot =
      celestial.sun.direction[0] * one_minute.sun.direction[0] +
      celestial.sun.direction[1] * one_minute.sun.direction[1] +
      celestial.sun.direction[2] * one_minute.sun.direction[2];
  expect(continuity_dot > 0.9999 && continuity_dot < 1.0,
         "celestial direction changes continuously over one minute");

  CelestialState night;
  const UtcDateTime night_utc{2024, 1, 1, 12, 0, 0.0};
  expect(computeCelestialState(night_utc, shanghai, night, &error) &&
             night.twilight == TwilightPhase::Night,
         "deep negative Sun altitude classifies as night");
  expect(std::string(twilightPhaseName(TwilightPhase::Civil)) == "civil",
         "twilight phase names are stable");

  CelestialState preserved;
  preserved.valid = true;
  preserved.moon_distance_km = 42.0;
  ObserverLocation invalid_observer = shanghai;
  invalid_observer.latitude_degrees = 91.0;
  expect(!computeCelestialState(utc, invalid_observer, preserved, &error),
         "invalid observer is rejected");
  expect(preserved.valid && preserved.moon_distance_km == 42.0,
         "failed celestial update is transactional");

  const BrunetonAtmosphereConfig earth_atmosphere =
      defaultEarthAtmosphereConfig();
  expect(earth_atmosphere.valid(),
         "frozen Earth Bruneton configuration is valid");
  expect(earth_atmosphere.dimensions.transmittance_width == 256u &&
             earth_atmosphere.dimensions.transmittance_height == 64u &&
             earth_atmosphere.dimensions.scatteringWidth() == 256u &&
             earth_atmosphere.dimensions.scattering_view_cosine == 128u &&
             earth_atmosphere.dimensions.scattering_radial == 32u &&
             earth_atmosphere.dimensions.irradiance_width == 64u &&
             earth_atmosphere.dimensions.irradiance_height == 16u,
         "Bruneton LUT dimensions match the frozen upstream contract");
  expectNearDouble(earth_atmosphere.physical.bottom_radius_km, 6360.0, 0.0,
                   "Earth atmosphere bottom radius is frozen");
  expectNearDouble(earth_atmosphere.physical.top_radius_km, 6420.0, 0.0,
                   "Earth atmosphere top radius is frozen");
  expectNearDouble(
      earth_atmosphere.physical.rayleigh_scattering_per_km[1],
      0.013557762447920219, 1e-16,
      "green Rayleigh coefficient matches the upstream Earth model");
  expectNearDouble(
      earth_atmosphere.physical.absorption_extinction_per_km[1],
      0.0018809, 1e-12,
      "green ozone coefficient matches the upstream Earth model");
  const std::string atmosphere_key =
      brunetonAtmosphereCacheKey(earth_atmosphere);
  expect(!atmosphere_key.empty() &&
             atmosphere_key ==
                 brunetonAtmosphereCacheKey(defaultEarthAtmosphereConfig()),
         "Bruneton cache identity is deterministic");
  BrunetonAtmosphereConfig modified_atmosphere = earth_atmosphere;
  modified_atmosphere.physical.ground_albedo[0] = 0.11;
  expect(modified_atmosphere.valid() &&
             brunetonAtmosphereCacheKey(modified_atmosphere) != atmosphere_key,
         "physical parameter changes invalidate the Bruneton cache identity");
  BrunetonAtmosphereConfig invalid_atmosphere = earth_atmosphere;
  invalid_atmosphere.physical.top_radius_km =
      invalid_atmosphere.physical.bottom_radius_km;
  expect(!invalid_atmosphere.valid() &&
             brunetonAtmosphereCacheKey(invalid_atmosphere).empty(),
         "invalid Bruneton configurations cannot acquire cache identities");

  std::vector<std::array<std::uint8_t, 4>> pixels(
      8u, std::array<std::uint8_t, 4>{64u, 64u, 64u, 129u});
  pixels[2] = {128u, 128u, 128u, 132u};
  const std::vector<std::uint8_t> hdr = makeRadianceHdr(4, 2, pixels);
  FloatEnvironmentImage image;
  expect(decodeRadianceHdr(hdr, image, &error),
         "strict 2:1 Radiance HDR decodes to float");
  expect(image.valid() && image.width == 4u && image.height == 2u,
         "decoded HDR retains dimensions");
  expect(image.rgba[2u * 4u] > image.rgba[0],
         "decoded HDR retains a bright importance texel");
  expect(image.rgba[3] == 1.0f,
         "decoded HDR synthesizes unit alpha");

  FloatEnvironmentImage preserved_image;
  preserved_image.width = 1u;
  preserved_image.height = 1u;
  preserved_image.rgba = {1.0f, 2.0f, 3.0f, 1.0f};
  const std::vector<std::uint8_t> wrong_ratio =
      makeRadianceHdr(4, 3,
                      std::vector<std::array<std::uint8_t, 4>>(
                          12u, {64u, 64u, 64u, 129u}));
  expect(!decodeRadianceHdr(wrong_ratio, preserved_image, &error),
         "non-2:1 HDR is rejected");
  expect(preserved_image.width == 1u && preserved_image.rgba[1] == 2.0f,
         "failed HDR decode is transactional");
  const std::vector<std::uint8_t> black_hdr =
      makeRadianceHdr(4, 2,
                      std::vector<std::array<std::uint8_t, 4>>(
                          8u, {0u, 0u, 0u, 0u}));
  expect(!decodeRadianceHdr(black_hdr, preserved_image, &error),
         "empty-radiance HDR is rejected");
  HdrDecodeLimits tiny_budget;
  tiny_budget.maximum_decoded_bytes = 64u;
  expect(!decodeRadianceHdr(hdr, preserved_image, &error, tiny_budget),
         "HDR decoded-memory budget is enforced");

  AliasTable alias;
  const std::array<double, 3> alias_weights{1.0, 3.0, 0.0};
  expect(alias.build(alias_weights), "alias table accepts finite weights");
  expectNearDouble(alias.probability(0), 0.25, 1e-12,
                   "alias probability preserves first weight");
  expectNearDouble(alias.probability(1), 0.75, 1e-12,
                   "alias probability preserves second weight");
  expect(alias.probability(2) == 0.0,
         "zero-weight alias entry remains unsampled");
  std::array<double, 3> reconstructed_alias_pmf{};
  for (std::size_t column = 0; column < alias.size(); ++column) {
    const double acceptance = alias.acceptance(column);
    const std::uint32_t alternate = alias.aliasIndex(column);
    expect(acceptance >= 0.0 && acceptance <= 1.0 &&
               alternate < alias.size(),
           "exported alias entry is GPU-safe");
    reconstructed_alias_pmf[column] +=
        acceptance / static_cast<double>(alias.size());
    reconstructed_alias_pmf[alternate] +=
        (1.0 - acceptance) / static_cast<double>(alias.size());
  }
  for (std::size_t index = 0; index < alias.size(); ++index) {
    expectNearDouble(reconstructed_alias_pmf[index],
                     alias.probability(index), 1e-12,
                     "exported alias entry reconstructs PMF");
  }
  expect(!AliasTable{}.valid(), "empty alias table is invalid");

  EnvironmentDistribution environment;
  expect(environment.build(image),
         "HDR radiance builds an environment distribution");
  double probability_sum = 0.0;
  double pdf_integral = 0.0;
  for (std::uint32_t y = 0; y < environment.height(); ++y) {
    const double theta =
        pi * (static_cast<double>(y) + 0.5) /
        static_cast<double>(environment.height());
    for (std::uint32_t x = 0; x < environment.width(); ++x) {
      const double phi =
          2.0 * pi * (static_cast<double>(x) + 0.5) /
          static_cast<double>(environment.width());
      const std::array<double, 3> direction{
          std::sin(theta) * std::sin(phi), std::cos(theta),
          std::sin(theta) * std::cos(phi)};
      probability_sum += environment.texelProbability(x, y);
      pdf_integral += environment.solidAnglePdf(direction) *
                      environment.texelSolidAngle(y);
    }
  }
  expectNearDouble(probability_sum, 1.0, 1e-12,
                   "environment texel probabilities normalize");
  expectNearDouble(pdf_integral, 1.0, 1e-12,
                   "environment solid-angle PDF integrates to one");
  expect(environment.texelProbability(2u, 0u) >
             environment.texelProbability(0u, 0u),
         "environment importance favors the bright texel");
  expect(environment.aliasAcceptance(2u, 0u) >= 0.0 &&
             environment.aliasAcceptance(2u, 0u) <= 1.0 &&
             environment.aliasIndex(2u, 0u) <
                 environment.width() * environment.height(),
         "environment exposes a GPU-safe alias entry");
  const EnvironmentDirectionSample environment_sample =
      environment.sample(0.37, 0.61, 0.25, 0.75, 0.7);
  expect(environment_sample.valid,
         "environment alias sampling produces a valid direction");
  expectNearDouble(environment.solidAnglePdf(environment_sample.direction, 0.7),
                   environment_sample.solid_angle_pdf, 1e-12,
                   "environment sample and evaluated PDF agree after rotation");

  HdrEnvironmentAsset hdr_asset;
  expect(buildHdrEnvironmentAsset(hdr, "fixture.hdr", "fixture-sha", 7u,
                                  hdr_asset, &error),
         "complete HDR asset commits radiance and distribution");
  expect(hdr_asset.valid() && hdr_asset.generation == 7u,
         "committed HDR asset retains identity and generation");
  HdrEnvironmentAsset preserved_asset = std::move(hdr_asset);
  const std::uint64_t preserved_generation = preserved_asset.generation;
  expect(!buildHdrEnvironmentAsset(wrong_ratio, "broken.hdr", "broken-sha",
                                   8u, preserved_asset, &error),
         "invalid replacement HDR asset is rejected");
  expect(preserved_asset.valid() &&
             preserved_asset.generation == preserved_generation &&
             preserved_asset.source_identity == "fixture.hdr",
         "failed HDR asset replacement preserves committed state");

  WorldEnvironmentState world;
  ResolvedWorldEnvironment resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.background_visible &&
             !resolved.environment_lighting,
         "default World resolves to Sky Off without IBL");
  world.sky_rendering = SkyRendering::UserHdri;
  world.selected_hdr_identity = "missing.hdr";
  world.hdr = std::move(preserved_asset);
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.warning.empty() &&
             world.sky_rendering == SkyRendering::UserHdri &&
             world.selected_hdr_identity == "missing.hdr",
         "missing selected HDR resolves Off without changing requested state");
  world.selected_hdr_identity = "fixture.hdr";
  world.global_lighting_strength_ev = std::log2(2.5f);
  world.background_exposure = 3.0f;
  world.rotation_radians = static_cast<float>(-0.5 * pi);
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::UserHdri &&
             resolved.hdr != nullptr && resolved.environment_lighting,
         "available selected HDR resolves as User HDRI");
  expectNear(resolved.environment_strength, 2.5f, 1e-6f,
             "HDR physical strength is independent");
  expectNear(resolved.global_lighting_strength, 2.5f, 1e-6f,
             "global sky EV resolves to one authoritative multiplier");
  expectNear(resolved.background_multiplier, 20.0f, 1e-5f,
             "background EV composes with physical global sky energy");
  expectNear(resolved.background_exposure, 3.0f, 1e-6f,
             "HDR background exposure is retained separately");
  expectNear(resolved.rotation_radians, static_cast<float>(1.5 * pi), 1e-6f,
             "HDR rotation is normalized");
  UtcDateTime shifted_local;
  expect(shiftUtcDateTime(UtcDateTime{2024, 1, 1, 0, 30, 0.0},
                          -3600.0, shifted_local, &error) &&
             shifted_local.year == 2023 && shifted_local.month == 12 &&
             shifted_local.day == 31 && shifted_local.hour == 23 &&
             shifted_local.minute == 30,
         "calendar-safe local/UTC conversion crosses year boundaries");
  world.environment_lighting = false;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::UserHdri &&
             resolved.environment_strength == 0.0f &&
             resolved.background_visible,
         "background visibility does not force environment lighting");
  world.sky_rendering = SkyRendering::ProceduralDayNight;
  world.procedural_resources_ready = false;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off,
         "unavailable procedural resources resolve to Off");
  world.procedural_resources_ready = true;
  world.celestial = celestial;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::ProceduralDayNight &&
             resolved.celestial != nullptr && resolved.atmosphere != nullptr &&
             resolved.clouds == nullptr,
         "ready procedural resources resolve with celestial and atmosphere");
  const VolumetricCloudState default_clouds;
  const std::string default_cloud_key =
      volumetricCloudCacheKey(default_clouds);
  expect(default_clouds.valid() && !default_clouds.enabled &&
             !default_cloud_key.empty() &&
             default_cloud_key ==
                 volumetricCloudCacheKey(VolumetricCloudState{}),
         "disabled volumetric-cloud defaults have deterministic identity");
  VolumetricCloudState active_clouds = default_clouds;
  active_clouds.enabled = true;
  const std::string active_cloud_key =
      volumetricCloudCacheKey(active_clouds);
  expect(active_clouds.valid() && !active_cloud_key.empty() &&
             active_cloud_key != default_cloud_key,
         "enabling volumetric clouds invalidates their cache identity");
  VolumetricCloudState advected_clouds = active_clouds;
  advected_clouds.weather_offset_km[0] += 2.0f;
  advected_clouds.time_seconds += 1.0f;
  ++advected_clouds.generation;
  expect(volumetricCloudCacheKey(advected_clouds) != active_cloud_key,
         "cloud weather, time, and generation affect cache identity");
  world.clouds = active_clouds;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::ProceduralDayNight &&
             resolved.clouds == &world.clouds,
         "enabled valid clouds are exposed to the procedural renderer");
  world.clouds.ray_steps = 0u;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.warning.empty() &&
             volumetricCloudCacheKey(world.clouds).empty(),
         "invalid cloud quality resolves procedural sky to Off");
  world.clouds = default_clouds;
  world.atmosphere.physical.top_radius_km =
      world.atmosphere.physical.bottom_radius_km;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.warning.empty(),
         "invalid atmosphere resources resolve procedural sky to Off");

  const std::array<EmissivePatch, 3> patches{{
      {0u, 0u, 0u, 1.0, 1.0},
      {1u, 0u, 0u, 2.0, 2.0},
      {2u, 0u, 0u, 3.0, 0.0},
  }};
  EmissivePatchDistribution emitters;
  expect(emitters.build(patches),
         "emissive patches build area-times-luminance weights");
  expectNearDouble(emitters.probability(0), 0.2, 1e-12,
                   "first emissive patch probability is exact");
  expectNearDouble(emitters.probability(1), 0.8, 1e-12,
                   "second emissive patch probability is exact");
  expect(emitters.probability(2) == 0.0,
         "zero-radiance patch has zero probability");

  expectNearDouble(powerHeuristic(1.0, 1.0), 0.5, 1e-12,
                   "power heuristic splits equal PDFs");
  expectNearDouble(powerHeuristic(1.0, 3.0), 0.1, 1e-12,
                   "power heuristic exponent two is exact");
  expectNearDouble(powerHeuristic(1e300, 3e300), 0.1, 1e-12,
                   "power heuristic is overflow safe");
  expect(powerHeuristic(0.0, 1.0) == 0.0 &&
             powerHeuristic(1.0, 0.0) == 1.0,
         "power heuristic handles zero PDF boundaries");
  expectNearDouble(areaPdfToSolidAngle(0.25, 4.0, 0.5), 2.0, 1e-12,
                   "area PDF converts to solid-angle PDF");
  expect(areaPdfToSolidAngle(1.0, 1.0, 0.0) == 0.0,
         "solid-angle conversion rejects a grazing light");
}

void testRtxptBridgeSelection() {
  using xpbd::gfx::queryRtxptStatus;
  using xpbd::gfx::RayTracingCapability;
  using xpbd::gfx::RtImplementation;
  using xpbd::gfx::selectRtImplementation;
  using xpbd::gfx::rtImplementationName;

  const auto st = queryRtxptStatus();
  expect(rtImplementationName(RtImplementation::Rtxpt) != nullptr,
         "RTXPT name non-null");

  RayTracingCapability hw{};
  hw.supported = true;
  hw.device_extensions_enabled = true;

  expect(selectRtImplementation(false, hw, st) == RtImplementation::None,
         "user off -> None");

  // Without path-tracer-ready flag: hybrid shadows.
  xpbd::gfx::setRtxptAlignedPathTracerReady(false);
  auto st2 = queryRtxptStatus();
  expect(selectRtImplementation(true, hw, st2) ==
             RtImplementation::HybridRayQuery,
         "user on + HW, no path tracer -> hybrid");

  xpbd::gfx::setRtxptAlignedPathTracerReady(true);
  auto st3 = queryRtxptStatus();
  expect(st3.runtime_ready, "path tracer ready flag visible in status");
  expect(selectRtImplementation(true, hw, st3) == RtImplementation::Rtxpt,
         "user on + HW + path tracer -> Rtxpt");

  RayTracingCapability no_hw{};
  expect(selectRtImplementation(true, no_hw, st3) == RtImplementation::None,
         "path tracer without HW extensions still None");
  xpbd::gfx::setRtxptAlignedPathTracerReady(false);
}

} // namespace

int main() {
  testLogicalFramebufferViewportContract();
  testVulkanQueueFamilySelection();
  testTextureFromMemory();
  testCc0PreviewSceneAssets();
  testEmptyTextureSample();
  testLabPbrDecode();
  testLabPbrDiscoveryAndFallback();
  testStrictLabPbrSuiteImport();
  testLabPbrAuthoringEncodingAndCoverage();
  testLabPbrCompositionAndConflicts();
  testLabPbrPngChecksumAndNormalImport();
  testLabPbrBundleExport();
  testTangentFrames();
  testRtNormalTransformAndUpdatePolicy();
  testRtNearestHitReference();
  testPathTraceSamplingAndAccumulation();
  testPathTraceAdjustableSettingsContract();
  testPathTraceBsdfAndDepth();
  testViewportMeshEmptyGeometry();
  testCanonicalCubeAndRtSceneRecords();
  testRayTracingCapability();
  testRtAlphaSemantics();
  testRtMotionProjection();
  testStaticMaterialClassification();
  testWorldEnvironmentFoundation();
  testRtxptBridgeSelection();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("All viewport regression smoke tests passed.\n");
  return 0;
}
