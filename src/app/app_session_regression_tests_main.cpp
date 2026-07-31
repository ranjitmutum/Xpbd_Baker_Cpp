// Focused AppSession regression tests for transactional LabPBR suite import.

#include "xpbd/app/app_session.hpp"
#include "xpbd/gfx/labpbr_authoring.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/still_image_export.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void expect(bool condition, const char *label) {
  if (condition) {
    std::printf("ok: %s\n", label);
    return;
  }
  std::fprintf(stderr, "FAIL: %s\n", label);
  ++g_failures;
}

bool sameTexture(const xpbd::gfx::TextureImage &lhs,
                 const xpbd::gfx::TextureImage &rhs) {
  return lhs.width == rhs.width && lhs.height == rhs.height &&
         lhs.source_channels == rhs.source_channels && lhs.rgba == rhs.rgba;
}

bool sameSourceFile(const xpbd::gfx::LabPbrSourceFile &lhs,
                    const xpbd::gfx::LabPbrSourceFile &rhs) {
  const bool same_bytes =
      (!lhs.original_bytes && !rhs.original_bytes) ||
      (lhs.original_bytes && rhs.original_bytes &&
       *lhs.original_bytes == *rhs.original_bytes);
  return lhs.path == rhs.path && lhs.present == rhs.present &&
         lhs.size == rhs.size && lhs.write_time == rhs.write_time &&
         lhs.sha256 == rhs.sha256 && same_bytes;
}

bool sameSuiteSource(const xpbd::gfx::LabPbrSuiteSource &lhs,
                     const xpbd::gfx::LabPbrSuiteSource &rhs) {
  return sameSourceFile(lhs.base, rhs.base) &&
         sameSourceFile(lhs.specular, rhs.specular) &&
         sameSourceFile(lhs.normal, rhs.normal) &&
         sameSourceFile(lhs.properties, rhs.properties) &&
         lhs.confirmed_labpbr13_without_properties ==
             rhs.confirmed_labpbr13_without_properties &&
         lhs.cache_key == rhs.cache_key;
}

bool sameResolvedMaterial(const xpbd::gfx::ResolvedMaterialTable &lhs,
                          const xpbd::gfx::ResolvedMaterialTable &rhs) {
  return xpbd::gfx::sameResolvedMaterialResource(lhs, rhs) &&
         lhs.format == rhs.format &&
         lhs.format_declared == rhs.format_declared &&
         lhs.declared_format == rhs.declared_format &&
         lhs.warnings == rhs.warnings;
}

bool sameImportedNormal(const xpbd::gfx::ReadOnlyIrisNormalAsset &lhs,
                        const xpbd::gfx::ReadOnlyIrisNormalAsset &rhs) {
  return lhs.source_path == rhs.source_path &&
         lhs.original_file_bytes == rhs.original_file_bytes &&
         lhs.sha256 == rhs.sha256 && sameTexture(lhs.decoded, rhs.decoded);
}

struct MaterialSessionSnapshot {
  xpbd::gfx::TextureImage texture;
  xpbd::gfx::ResolvedMaterialTable material;
  xpbd::gfx::LabPbrUvCoverage coverage;
  std::map<std::string, xpbd::gfx::GroupLabPbrOverride> overrides;
  xpbd::gfx::GroupLabPbrOverride draft;
  xpbd::gfx::LabPbrCompositionResult composition;
  xpbd::gfx::ReadOnlyIrisNormalAsset imported_normal;
  xpbd::gfx::LabPbrSuiteSource source;
  std::string texture_path;
  std::uint64_t generation = 0;
  bool source_change_pending = false;
  bool last_import_cache_hit = false;
};

MaterialSessionSnapshot snapshot(const xpbd::app::AppSession &session) {
  return {
      session.model_texture,
      session.resolved_material,
      session.labpbr_uv_coverage,
      session.labpbr_group_overrides,
      session.labpbr_draft,
      session.labpbr_composition,
      session.labpbr_imported_normal,
      session.labpbr_suite_source,
      session.texture_path,
      session.materialGeneration(),
      session.labpbr_source_change_pending,
      session.labpbr_last_import_cache_hit,
  };
}

bool unchanged(const xpbd::app::AppSession &session,
               const MaterialSessionSnapshot &before) {
  return sameTexture(session.model_texture, before.texture) &&
         sameResolvedMaterial(session.resolved_material, before.material) &&
         session.labpbr_uv_coverage.width == before.coverage.width &&
         session.labpbr_uv_coverage.height == before.coverage.height &&
         session.labpbr_uv_coverage.group_texels ==
             before.coverage.group_texels &&
         session.labpbr_group_overrides == before.overrides &&
         session.labpbr_draft == before.draft &&
         sameTexture(session.labpbr_composition.specular,
                     before.composition.specular) &&
         session.labpbr_composition.conflicts.size() ==
             before.composition.conflicts.size() &&
         session.labpbr_composition.errors == before.composition.errors &&
         session.labpbr_composition.warnings == before.composition.warnings &&
         sameImportedNormal(session.labpbr_imported_normal,
                            before.imported_normal) &&
         sameSuiteSource(session.labpbr_suite_source, before.source) &&
         session.texture_path == before.texture_path &&
         session.materialGeneration() == before.generation &&
         session.labpbr_source_change_pending ==
             before.source_change_pending &&
         session.labpbr_last_import_cache_hit ==
             before.last_import_cache_hit;
}

void testDefaultEmptyScene() {
  const auto &session = xpbd::app::AppSession::instance();
  expect(session.scene_selection.kind ==
             xpbd::app::SceneSelectionKind::Empty &&
             !session.sceneRendersLoadedContent(),
         "new session defaults to an explicit non-rendering Empty Scene");
  expect(session.preview_scene_id == xpbd::gfx::PreviewSceneId::None,
         "new session defaults to the empty preview scene");
  expect(!session.show_ground && !session.show_preview_grid &&
             !session.show_preview_axes,
         "new empty scene has no ground, grid, or axes");
  expect(session.world_environment.sky_rendering ==
             xpbd::gfx::SkyRendering::Off,
         "new session defaults to Sky Rendering Off");
}

void testSceneSelectionTransactionsAndPersistence() {
  using namespace xpbd;
  auto &session = app::AppSession::instance();
  const auto original_geometry = session.geometry;
  const auto original_model_path = session.model_path;
  const auto original_selection = session.scene_selection;
  const auto original_preview = session.preview_scene_id;
  const bool original_grid = session.show_preview_grid;
  const bool original_axes = session.show_preview_axes;
  const bool original_dynamic = session.dynamic_preview_scene;

  const auto sky_mode = session.world_environment.sky_rendering;
  const auto sky_generation = session.world_environment.generation;
  const auto sky_lighting_generation =
      session.world_environment.lighting_generation;
  const auto sky_celestial_generation =
      session.world_environment.celestial_generation;
  const auto sky_cloud_generation =
      session.world_environment.cloud_generation;
  const auto reset_before =
      session.path_trace_settings.reset_generation;
  expect(session.selectPresetScene(gfx::PreviewSceneId::Desert) &&
             session.scene_selection.kind ==
                 app::SceneSelectionKind::Preset &&
             session.scene_selection.preset ==
                 gfx::PreviewSceneId::Desert &&
             session.preview_scene_id ==
                 gfx::PreviewSceneId::Desert &&
             session.path_trace_settings.reset_generation >
                 reset_before,
         "curated preset selection commits one coherent Scene transaction");
  expect(session.world_environment.sky_rendering == sky_mode &&
             session.world_environment.generation == sky_generation &&
             session.world_environment.lighting_generation ==
                 sky_lighting_generation &&
             session.world_environment.celestial_generation ==
                 sky_celestial_generation &&
             session.world_environment.cloud_generation ==
                 sky_cloud_generation,
         "Scene preset selection never mutates Sky Rendering state");

  const auto committed_selection = session.scene_selection;
  const auto committed_preview = session.preview_scene_id;
  expect(!session.selectPresetScene(
             static_cast<gfx::PreviewSceneId>(255)) &&
             session.scene_selection.kind ==
                 committed_selection.kind &&
             session.scene_selection.preset ==
                 committed_selection.preset &&
             session.scene_selection.generation ==
                 committed_selection.generation &&
             session.preview_scene_id == committed_preview,
         "non-curated preset selection is rejected transactionally");

  session.show_preview_grid = true;
  session.show_preview_axes = true;
  session.dynamic_preview_scene = true;
  const auto settings_path =
      std::filesystem::temp_directory_path() /
      ("xpbd-scene-selection-" +
       std::to_string(
           std::chrono::steady_clock::now()
               .time_since_epoch()
               .count()) +
       ".json");
  expect(session.saveSceneSelectionSettings(settings_path),
         "Scene selection saves a versioned JSON snapshot");
  session.selectScene(app::SceneSelectionKind::Empty);
  session.show_preview_grid = false;
  session.show_preview_axes = false;
  session.dynamic_preview_scene = false;
  expect(session.loadSceneSelectionSettings(settings_path) &&
             session.scene_selection.kind ==
                 app::SceneSelectionKind::Preset &&
             session.scene_selection.preset ==
                 gfx::PreviewSceneId::Desert &&
             session.show_preview_grid &&
             session.show_preview_axes &&
             session.dynamic_preview_scene,
         "Preset and custom Scene options round-trip transactionally");

  session.geometry.bones.emplace_back();
  session.geometry.bones.back().name = "scene_contract_probe";
  session.model_path = "scene-contract-probe.geo.json";
  expect(session.selectScene(app::SceneSelectionKind::Loaded) &&
             session.sceneRendersLoadedContent(),
         "Loaded Scene becomes available when model content exists");
  const auto retained_bones = session.geometry.bones.size();
  const auto retained_source = session.model_path;
  expect(session.selectScene(app::SceneSelectionKind::Empty) &&
             !session.sceneRendersLoadedContent() &&
             session.geometry.bones.size() == retained_bones &&
             session.model_path == retained_source,
         "Empty Scene hides loaded content without deleting it");
  expect(session.selectScene(app::SceneSelectionKind::UserBuilt) &&
             session.sceneRendersLoadedContent(),
         "User-Built Scene restores retained in-memory content");

  const auto invalid_path =
      settings_path.parent_path() /
      (settings_path.stem().string() + "-invalid.json");
  {
    std::ofstream invalid(invalid_path, std::ios::binary | std::ios::trunc);
    invalid << "{\"schema\":\"xpbd-scene-selection/1\","
               "\"kind\":1,\"preset\":99}\n";
  }
  const auto before_invalid = session.scene_selection;
  const auto preview_before_invalid = session.preview_scene_id;
  expect(!session.loadSceneSelectionSettings(invalid_path) &&
             session.scene_selection.kind == before_invalid.kind &&
             session.scene_selection.preset == before_invalid.preset &&
             session.scene_selection.generation ==
                 before_invalid.generation &&
             session.preview_scene_id == preview_before_invalid,
         "invalid Scene settings preserve the committed selection");

  std::error_code remove_error;
  std::filesystem::remove(settings_path, remove_error);
  remove_error.clear();
  std::filesystem::remove(invalid_path, remove_error);

  session.geometry = original_geometry;
  session.model_path = original_model_path;
  session.scene_selection = original_selection;
  session.preview_scene_id = original_preview;
  session.show_preview_grid = original_grid;
  session.show_preview_axes = original_axes;
  session.dynamic_preview_scene = original_dynamic;
  session.last_error.clear();
}

void testIndependentSkyRenderingState() {
  auto &session = xpbd::app::AppSession::instance();
  const auto generation_before = session.world_environment.generation;
  const auto reset_before = session.path_trace_settings.reset_generation;
  expect(session.setSkyRendering(xpbd::gfx::SkyRendering::ProceduralDayNight),
         "procedural Sky Rendering initializes from a stable celestial state");
  expect(session.world_environment.sky_rendering ==
             xpbd::gfx::SkyRendering::ProceduralDayNight &&
             session.world_environment.procedural_resources_ready &&
             session.world_environment.celestial.valid,
         "procedural Sky Rendering keeps its own world state");
  expect(session.world_environment.generation > generation_before &&
             session.path_trace_settings.reset_generation > reset_before,
         "Sky Rendering changes invalidate only the world/PT history");
  const auto physical_reset_before_display =
      session.path_trace_settings.reset_generation;
  const auto display_generation =
      session.world_environment.display_generation;
  session.world_environment.background_exposure = 1.0f;
  session.touchWorldEnvironmentDisplay();
  expect(session.world_environment.display_generation >
                 display_generation &&
             session.path_trace_settings.reset_generation ==
                 physical_reset_before_display,
         "background-only edits do not reset physical PT accumulation");
  const auto sky_before = session.world_environment.celestial;
  auto edited_utc = sky_before.utc;
  edited_utc.hour = (edited_utc.hour + 1) % 24;
  const auto edited_observer = sky_before.observer;
  const auto edited_generation = session.world_environment.generation;
  expect(session.setProceduralSkyControls(edited_utc, edited_observer, 25.0,
                                          12.0),
         "procedural Sky accepts UTC and bounded Sun angle controls");
  expect(session.world_environment.celestial.utc.hour == edited_utc.hour &&
             session.world_environment.sun_azimuth_offset_degrees == 25.0 &&
             session.world_environment.sun_altitude_offset_degrees == 12.0 &&
             session.world_environment.generation > edited_generation,
         "procedural Sky commits celestial edits and invalidates history");
  const auto committed_sky = session.world_environment.celestial;
  expect(!session.setProceduralSkyControls(
              xpbd::gfx::UtcDateTime{2024, 2, 31, 0, 0, 0.0},
              edited_observer,
              0.0, 0.0) &&
             session.world_environment.celestial.utc.year ==
                     committed_sky.utc.year &&
             session.world_environment.celestial.utc.month ==
                     committed_sky.utc.month &&
             session.world_environment.celestial.utc.day ==
                     committed_sky.utc.day &&
             session.world_environment.celestial.utc.hour ==
                     committed_sky.utc.hour,
         "invalid procedural date is rejected transactionally");
  const auto cloud_generation = session.world_environment.clouds.generation;
  const auto classified_cloud_generation =
      session.world_environment.cloud_generation;
  session.world_environment.clouds.enabled = true;
  session.touchWorldEnvironment(true);
  expect(session.world_environment.clouds.generation > cloud_generation &&
             session.world_environment.cloud_generation >
                 classified_cloud_generation,
         "cloud edits advance classified cloud generations without scene changes");
  session.world_environment.time.playing = true;
  session.world_environment.time.time_speed = 3600.0f;
  const auto playback_utc = session.world_environment.celestial.utc;
  const float playback_cloud_time =
      session.world_environment.clouds.time_seconds;
  const std::uint32_t playback_cloud_frame =
      session.world_environment.clouds.temporal_frame;
  expect(session.advanceWorldSkyTime(0.25) &&
             (session.world_environment.celestial.utc.second !=
                  playback_utc.second ||
              session.world_environment.celestial.utc.minute !=
                  playback_utc.minute ||
              session.world_environment.celestial.utc.hour !=
                  playback_utc.hour),
         "bounded sky playback advances astronomical UTC transactionally");
  expect(session.world_environment.clouds.time_seconds >
                 playback_cloud_time &&
             session.world_environment.clouds.temporal_frame ==
                 playback_cloud_frame + 1u,
         "sky playback advances cloud advection and temporal history");
  session.world_environment.time.playing = false;
  session.world_environment.global_lighting_strength_ev = 2.0f;
  session.world_environment.background_exposure = -1.0f;
  session.world_environment.sun.direction_mode =
      xpbd::gfx::SkyDirectionMode::ArtisticOffset;
  session.world_environment.moon.direction_mode =
      xpbd::gfx::SkyDirectionMode::ArtisticOffset;
  session.world_environment.moon.azimuth_offset_degrees = -15.0f;
  session.world_environment.moon.altitude_offset_degrees = 7.0f;
  session.world_environment.moon.phase_mode =
      xpbd::gfx::MoonPhaseMode::Manual;
  session.world_environment.moon.manual_illuminated_fraction = 0.75f;
  session.world_environment.night.light_pollution = 0.2f;
  session.world_environment.clouds.lighting_strength = 1.5f;
  expect(session.setProceduralSkyControls(
             session.world_environment.celestial.utc,
             session.world_environment.celestial.observer, 25.0, 12.0),
         "artistic Sun and Moon directions recompute transactionally");
  const auto settings_path =
      std::filesystem::temp_directory_path() /
      ("xpbd-world-sky-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".json");
  expect(session.saveWorldSkySettings(settings_path),
         "World Sky settings save to a versioned JSON snapshot");
  session.world_environment.global_lighting_strength_ev = -3.0f;
  session.world_environment.background_exposure = 4.0f;
  session.world_environment.moon.azimuth_offset_degrees = 0.0f;
  session.world_environment.moon.manual_illuminated_fraction = 0.1f;
  session.world_environment.clouds.lighting_strength = 0.2f;
  expect(session.loadWorldSkySettings(settings_path),
         "World Sky settings load transactionally");
  const auto restored =
      xpbd::gfx::resolveWorldEnvironment(session.world_environment);
  expect(std::abs(restored.global_lighting_strength - 4.0f) < 1.0e-6f &&
             std::abs(restored.background_multiplier - 2.0f) < 1.0e-6f &&
             std::abs(session.world_environment.moon.azimuth_offset_degrees +
                      15.0f) < 1.0e-6f &&
             std::abs(
                 session.world_environment.moon.manual_illuminated_fraction -
                 0.75f) < 1.0e-6f &&
             std::abs(session.world_environment.clouds.lighting_strength -
                      1.5f) < 1.0e-6f,
         "World Sky round-trip restores physical, display, celestial, and cloud controls");
  std::error_code settings_remove_error;
  std::filesystem::remove(settings_path, settings_remove_error);
  expect(!settings_remove_error,
         "World Sky round-trip removes its temporary JSON snapshot");
  expect(session.setSkyRendering(xpbd::gfx::SkyRendering::Off) &&
             session.world_environment.sky_rendering ==
                 xpbd::gfx::SkyRendering::Off,
         "Sky Rendering can be switched off independently");
}

void testPathTraceSettingsPersistenceAndClassification() {
  using namespace xpbd;
  auto &session = app::AppSession::instance();
  session.path_trace_settings =
      gfx::pathTraceSettingsForPreset(
          gfx::PathTracePreset::Balanced);

  auto schedule = session.path_trace_settings;
  schedule.samples_per_frame = 8u;
  schedule.maximum_samples = 8192u;
  const auto reset_before =
      session.path_trace_settings.reset_generation;
  const auto schedule_changes =
      session.applyPathTraceSettings(schedule);
  expect(gfx::hasPathTraceChange(
             schedule_changes,
             gfx::PathTraceChangeClass::SamplingSchedule) &&
             session.path_trace_settings.reset_generation ==
                 reset_before,
         "AppSession keeps accumulation for SPP/maximum edits");

  auto physical = session.path_trace_settings;
  physical.preset = gfx::PathTracePreset::Custom;
  physical.direct_clamp = 4.0f;
  physical.emissive_multiplier = 2.5f;
  const auto physical_changes =
      session.applyPathTraceSettings(physical);
  expect(gfx::hasPathTraceChange(
             physical_changes,
             gfx::PathTraceChangeClass::ResetAccumulation) &&
             session.path_trace_settings.reset_generation >
                 reset_before,
         "AppSession advances accumulation generation for integrator edits");

  const auto physical_reset =
      session.path_trace_settings.reset_generation;
  auto film = session.path_trace_settings;
  film.display_exposure_ev = -2.0f;
  film.white_balance_kelvin = 4200.0f;
  const auto film_changes = session.applyPathTraceSettings(film);
  expect(gfx::hasPathTraceChange(
             film_changes,
             gfx::PathTraceChangeClass::DisplayOnly) &&
             session.path_trace_settings.reset_generation ==
                 physical_reset,
         "AppSession applies film controls without resetting raw history");

  session.freezePathTraceRenderSnapshot();
  const float frozen_exposure =
      session.path_trace_render_snapshot->settings.display_exposure_ev;
  session.path_trace_settings.display_exposure_ev = 3.0f;
  expect(session.path_trace_render_snapshot.has_value() &&
             session.path_trace_render_snapshot->settings
                     .display_exposure_ev == frozen_exposure,
         "AppSession render snapshot is immutable after live edits");

  const auto settings_path =
      std::filesystem::temp_directory_path() /
      ("xpbd-path-tracing-" +
       std::to_string(
           std::chrono::steady_clock::now()
               .time_since_epoch()
               .count()) +
       ".json");
  session.path_trace_settings.requested_denoiser =
      gfx::PathTraceDenoiser::DlssRayReconstruction;
  session.path_trace_settings.requested_upscale =
      gfx::PathTraceUpscale::Quality;
  session.path_trace_settings.requested_frame_generation =
      gfx::PathTraceFrameGeneration::On;
  session.path_trace_settings.requested_reflex_mode =
      gfx::PathTraceReflexMode::OnBoost;
  session.path_trace_settings.preview_resolution_scale = 0.5f;
  session.path_trace_settings.pause_accumulation = true;
  expect(session.savePathTraceSettings(settings_path),
         "path tracing settings save to versioned JSON");
  session.path_trace_settings.requested_denoiser =
      gfx::PathTraceDenoiser::Raw;
  session.path_trace_settings.requested_upscale =
      gfx::PathTraceUpscale::Off;
  session.path_trace_settings.requested_frame_generation =
      gfx::PathTraceFrameGeneration::Off;
  session.path_trace_settings.requested_reflex_mode =
      gfx::PathTraceReflexMode::Off;
  session.path_trace_settings.preview_resolution_scale = 1.0f;
  session.path_trace_settings.pause_accumulation = false;
  expect(session.loadPathTraceSettings(settings_path) &&
             session.path_trace_settings.requested_denoiser ==
                 gfx::PathTraceDenoiser::DlssRayReconstruction &&
             session.path_trace_settings.requested_upscale ==
                 gfx::PathTraceUpscale::Quality &&
             session.path_trace_settings.requested_frame_generation ==
                 gfx::PathTraceFrameGeneration::On &&
             session.path_trace_settings.requested_reflex_mode ==
                 gfx::PathTraceReflexMode::OnBoost &&
             std::abs(session.path_trace_settings
                          .preview_resolution_scale -
                      0.5f) < 1.0e-6f &&
             session.path_trace_settings.pause_accumulation,
         "path tracing JSON round-trip restores all parameter groups");
  std::error_code remove_error;
  std::filesystem::remove(settings_path, remove_error);
  expect(!remove_error,
         "path tracing round-trip removes temporary JSON");
  session.clearPathTraceRenderSnapshot();
}

bool writeBytes(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<std::uint8_t>
encodePng(int width, int height, const std::vector<std::uint8_t> &rgba) {
  std::vector<std::uint8_t> png;
  std::string error;
  expect(xpbd::gfx::encodePngRgba8(width, height, rgba, png, &error),
         "encode AppSession LabPBR fixture PNG");
  return png;
}

void testTransactionalLabPbrSuiteImport() {
  namespace fs = std::filesystem;

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_app_session_labpbr_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated AppSession LabPBR import directory");
  if (filesystem_error) {
    return;
  }

  const std::vector<std::uint8_t> base_rgba{
      255u, 128u, 64u, 255u, 32u, 64u, 128u, 127u};
  const std::vector<std::uint8_t> specular_rgba{
      0u, 0u, 0u, 0u, 255u, 230u, 64u, 254u};
  const std::vector<std::uint8_t> normal_rgba{
      128u, 128u, 255u, 255u, 140u, 120u, 200u, 20u};
  const auto base_png = encodePng(2, 1, base_rgba);
  const auto specular_png = encodePng(2, 1, specular_rgba);
  const auto normal_png = encodePng(2, 1, normal_rgba);
  const std::string properties_text = "format=lab-pbr/1.3\n";
  const std::vector<std::uint8_t> properties_bytes(properties_text.begin(),
                                                    properties_text.end());

  const fs::path base_path = directory / "active.png";
  const fs::path specular_path = directory / "active_s.png";
  const fs::path normal_path = directory / "active_n.png";
  const fs::path properties_path = directory / "texture.properties";
  expect(writeBytes(base_path, base_png), "write AppSession base fixture");
  expect(writeBytes(specular_path, specular_png),
         "write AppSession specular fixture");
  expect(writeBytes(normal_path, normal_png),
         "write AppSession normal fixture");
  expect(writeBytes(properties_path, properties_bytes),
         "write AppSession properties fixture");

  auto &session = xpbd::app::AppSession::instance();
  session.labpbr_draft_dirty = false;
  session.clearTexture();
  expect(session.loadTexture(base_path) &&
             !session.resolved_material.normal_map_active &&
             !session.resolved_material.specular_map_active,
         "base texture import does not auto-attach sibling LabPBR images");
  session.clearTexture();
  expect(session.requestLabPbrSuiteImport(base_path),
         "AppSession commits a complete strict LabPBR suite");
  expect(session.labpbr_suite_source.valid() &&
             session.resolved_material.valid() &&
             session.labpbr_imported_normal.valid(),
         "AppSession retains committed source/material/normal state");

  auto committed = snapshot(session);
  expect(readBytes(base_path) == base_png &&
             readBytes(specular_path) == specular_png &&
             readBytes(normal_path) == normal_png &&
             readBytes(properties_path) == properties_bytes,
         "AppSession import never mutates source files");

  auto generation_before_normal = session.materialGeneration();
  session.removeLabPbrNormal();
  expect(!session.labpbr_imported_normal.valid() &&
             !session.resolved_material.normal_map_active &&
             session.materialGeneration() == generation_before_normal + 1u,
         "dedicated Iris normal removal updates resolved GPU material state");
  generation_before_normal = session.materialGeneration();
  expect(session.importLabPbrNormal(normal_path) &&
             session.labpbr_imported_normal.valid() &&
             session.resolved_material.normal_map_active &&
             session.materialGeneration() == generation_before_normal + 1u,
         "dedicated Iris normal import updates resolved GPU material state");
  expect(session.labpbr_imported_normal.original_file_bytes == normal_png,
         "dedicated Iris normal import retains exact source bytes");

  const std::vector<std::uint8_t> direct_specular_rgba{
      255u, 10u, 0u, 32u, 64u, 230u, 255u, 200u};
  const auto direct_specular_png =
      encodePng(2, 1, direct_specular_rgba);
  const fs::path direct_specular_path = directory / "selected_specular.png";
  expect(writeBytes(direct_specular_path, direct_specular_png),
         "write direct LabPBR specular fixture");
  auto generation_before_specular = session.materialGeneration();
  expect(session.importLabPbrSpecular(direct_specular_path) &&
             session.resolved_material.specular_map_active &&
             session.resolved_material.assets.specular ==
                 direct_specular_path &&
             session.materialGeneration() ==
                 generation_before_specular + 1u,
         "direct RGBA specular image import updates resolved GPU material");
  const auto generation_before_base_reload = session.materialGeneration();
  expect(session.loadTexture(base_path) &&
             session.labpbr_imported_normal.valid() &&
             session.resolved_material.normal_map_active &&
             session.resolved_material.specular_map_active &&
             session.resolved_material.assets.specular ==
                 direct_specular_path &&
             session.materialGeneration() ==
                 generation_before_base_reload,
         "same-size base import preserves independently selected PBR slots "
         "without discovering sibling images");
  const auto direct_committed = snapshot(session);
  const fs::path mismatched_specular_path =
      directory / "mismatched_specular.png";
  expect(writeBytes(mismatched_specular_path,
                    encodePng(
                        1, 1,
                        std::vector<std::uint8_t>{0u, 10u, 0u, 0u})),
         "write mismatched direct specular fixture");
  expect(!session.importLabPbrSpecular(mismatched_specular_path) &&
             unchanged(session, direct_committed),
         "failed direct specular import is transactional");

  const fs::path export_directory = directory / "export";
  fs::create_directories(export_directory, filesystem_error);
  expect(!filesystem_error, "create isolated LabPBR export directory");
  const fs::path export_destination = export_directory / "exported.png";
  auto generation_before_export = session.materialGeneration();
  expect(session.requestLabPbrExport(export_destination),
         "successful LabPBR export completes without confirmation");
  expect(session.materialGeneration() == generation_before_export + 1u,
         "successful LabPBR export advances material generation once");
  expect(readBytes(export_directory / "exported_n.png") == normal_png,
         "successful LabPBR export preserves exact imported normal bytes");

  generation_before_export = session.materialGeneration();
  expect(!session.requestLabPbrExport(export_destination) &&
             session.labpbr_export_confirmation_pending,
         "existing LabPBR export requests overwrite confirmation");
  expect(session.materialGeneration() == generation_before_export,
         "overwrite prompt does not advance material generation");
  session.confirmLabPbrExport(false);
  expect(session.materialGeneration() == generation_before_export,
         "cancelled LabPBR export does not advance material generation");
  expect(!session.requestLabPbrExport(export_destination) &&
             session.labpbr_export_confirmation_pending,
         "LabPBR overwrite can be requested again after cancellation");
  session.confirmLabPbrExport(true);
  expect(session.materialGeneration() == generation_before_export + 1u,
         "confirmed LabPBR overwrite advances material generation once");
  committed = snapshot(session);

  const fs::path missing_specular_base = directory / "missing.png";
  expect(writeBytes(missing_specular_base, base_png),
         "write missing-specular replacement base");
  expect(!session.requestLabPbrSuiteImport(missing_specular_base),
         "AppSession rejects replacement without mandatory _s");
  expect(unchanged(session, committed),
         "missing _s replacement rolls back all committed material state");

  expect(!session.requestLabPbrSuiteRelink(missing_specular_base),
         "AppSession rejects invalid suite relink");
  expect(unchanged(session, committed),
         "failed relink rolls back all committed material state");

  const fs::path confirmation_directory = directory / "no_properties";
  fs::create_directories(confirmation_directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated missing-properties fixture directory");
  const fs::path confirmation_base =
      confirmation_directory / "confirmation.png";
  const fs::path confirmation_specular =
      confirmation_directory / "confirmation_s.png";
  expect(writeBytes(confirmation_base, base_png),
         "write confirmation replacement base");
  expect(writeBytes(confirmation_specular, specular_png),
         "write confirmation replacement specular");
  expect(!session.requestLabPbrSuiteImport(confirmation_base) &&
             session.labpbr_import_confirmation_pending,
         "missing properties pauses for explicit LabPBR 1.3 confirmation");
  expect(unchanged(session, committed),
         "pending confirmation preserves committed material state");
  session.confirmLabPbrSuiteImport(false);
  expect(!session.labpbr_import_confirmation_pending &&
             unchanged(session, committed),
         "cancelled confirmation preserves committed material state");

  session.labpbr_draft_dirty = true;
  expect(!session.requestLabPbrSuiteImport(base_path),
         "dirty LabPBR draft blocks suite replacement");
  session.labpbr_draft_dirty = false;
  expect(unchanged(session, committed),
         "dirty-draft refusal preserves committed material state");

  expect(readBytes(base_path) == base_png &&
             readBytes(specular_path) == specular_png &&
             readBytes(normal_path) == normal_png &&
             readBytes(properties_path) == properties_bytes &&
             readBytes(missing_specular_base) == base_png &&
             readBytes(confirmation_base) == base_png &&
             readBytes(confirmation_specular) == specular_png,
         "failed session operations never mutate any source fixture");

  // Minimal 1x1 RGB PNG. Direct PBR import accepts RGB and treats the absent
  // LabPBR alpha channel as the reserved no-emission value.
  const std::vector<std::uint8_t> rgb_png{
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
      0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xFF, 0xFF, 0x3F,
      0x00, 0x05, 0xFE, 0x02, 0xFE, 0x0D, 0xEF, 0x46, 0xB8, 0x00, 0x00, 0x00,
      0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
  const fs::path rgb_base_path = directory / "rgb_base.png";
  const fs::path rgb_pbr_path = directory / "rgb_pbr.png";
  expect(writeBytes(rgb_base_path, rgb_png) &&
             writeBytes(rgb_pbr_path, rgb_png),
         "write direct RGB PBR fixtures");
  session.clearTexture();
  expect(session.loadTexture(rgb_base_path) &&
             session.importLabPbrSpecular(rgb_pbr_path) &&
             session.resolved_material.specular_map_active &&
             session.resolved_material.specular_image.source_channels == 3 &&
             !session.resolved_material.texels.empty() &&
             session.resolved_material.texels.front().emission_strength ==
                 0.0f,
         "direct RGB PBR image imports as a no-emission LabPBR map");
  expect(session.loadTexture(rgb_base_path) &&
             session.resolved_material.specular_map_active &&
             session.resolved_material.specular_image.source_channels == 3,
         "same-size base reload preserves a directly imported RGB PBR map");
  session.clearTexture();
  fs::remove_all(directory, filesystem_error);
  expect(!filesystem_error, "remove isolated AppSession fixture directory");
}

void testExternalLabPbrSuite(const std::filesystem::path &base_path) {
  namespace fs = std::filesystem;

  const fs::path normalized_base =
      fs::absolute(base_path).lexically_normal();
  const fs::path parent = normalized_base.parent_path();
  const std::wstring stem = normalized_base.stem().wstring();
  const fs::path specular_path = parent / fs::path(stem + L"_s.png");
  const fs::path normal_path = parent / fs::path(stem + L"_n.png");
  const fs::path properties_path = parent / "texture.properties";
  const auto base_bytes = readBytes(normalized_base);
  const auto specular_bytes = readBytes(specular_path);
  const auto normal_bytes = readBytes(normal_path);

  expect(!base_bytes.empty() && !specular_bytes.empty(),
         "external suite has readable base and mandatory _s bytes");
  expect(!normal_bytes.empty(), "external suite has readable optional _n bytes");
  expect(!fs::exists(properties_path),
         "external validation suite intentionally lacks texture.properties");
  if (base_bytes.empty() || specular_bytes.empty()) {
    return;
  }

  auto &session = xpbd::app::AppSession::instance();
  session.labpbr_draft_dirty = false;
  session.clearTexture();
  const auto generation_before = session.materialGeneration();
  const bool imported_immediately =
      session.requestLabPbrSuiteImport(normalized_base);
  expect(!imported_immediately &&
             session.labpbr_import_confirmation_pending,
         "external suite pauses for missing-properties confirmation");
  session.confirmLabPbrSuiteImport(true);

  expect(session.labpbr_suite_source.valid() &&
             session.labpbr_suite_source.base.path == normalized_base,
         "external suite commits through the real AppSession path");
  expect(session.labpbr_suite_source
                 .confirmed_labpbr13_without_properties &&
             session.resolved_material.specular_map_active &&
             session.resolved_material.normal_map_active,
         "external suite activates confirmed LabPBR specular and normal maps");
  expect(session.model_texture.width == 1024 &&
             session.model_texture.height == 1024,
         "external suite preserves expected 1024x1024 dimensions");
  expect(session.materialGeneration() > generation_before,
         "external suite advances material generation on first commit");
  expect(session.labpbr_suite_source.base.original_bytes &&
             *session.labpbr_suite_source.base.original_bytes == base_bytes &&
             session.labpbr_suite_source.specular.original_bytes &&
             *session.labpbr_suite_source.specular.original_bytes ==
                 specular_bytes &&
             session.labpbr_suite_source.normal.original_bytes &&
             *session.labpbr_suite_source.normal.original_bytes == normal_bytes,
         "external suite retains exact original source bytes");

  auto committed = snapshot(session);
  expect(session.reloadLabPbrSuite() && session.labpbr_last_import_cache_hit,
         "external suite reload reuses the checksum cache");
  committed.last_import_cache_hit = true;
  expect(unchanged(session, committed),
         "cached external reload leaves committed material generation stable");
  expect(readBytes(normalized_base) == base_bytes &&
             readBytes(specular_path) == specular_bytes &&
             readBytes(normal_path) == normal_bytes,
         "external validation never mutates normalized source files");
  session.clearTexture();
}

void testStillRenderSnapshotAndOutput() {
  namespace fs = std::filesystem;
  auto &session = xpbd::app::AppSession::instance();
  std::error_code filesystem_error;
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd-still-render-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error, "create isolated still-render directory");
  if (filesystem_error) {
    return;
  }

  session.setApplicationDirectory(directory);
  session.clearPathTraceRenderSnapshot();
  session.still_render_job = {};
  session.still_render_job.settings.filename = "../bad:name.exr";
  session.still_render_job.settings.width = 1u;
  session.still_render_job.settings.height = 20'000u;
  session.still_render_job.settings.target_samples = 0u;
  session.still_render_job.settings.samples_per_submit = 100u;
  session.still_render_job.settings.format =
      xpbd::gfx::StillImageFormat::Exr;
  session.still_render_job.settings.transparent_background = true;
  session.path_trace_settings.requested_denoiser =
      xpbd::gfx::PathTraceDenoiser::DlssRayReconstruction;
  session.path_trace_settings.requested_upscale =
      xpbd::gfx::PathTraceUpscale::Quality;
  session.path_trace_settings.requested_frame_generation =
      xpbd::gfx::PathTraceFrameGeneration::On;
  session.path_trace_settings.requested_reflex_mode =
      xpbd::gfx::PathTraceReflexMode::OnBoost;
  session.path_trace_settings.adaptive_sampling = true;
  session.path_trace_settings.preview_resolution_scale = 0.5f;
  session.playback_state = xpbd::app::PlaybackState::Playing;
  session.world_environment.generation = 41u;
  expect(session.queueStillRender(),
         "queue still render into the application output folder");
  const auto &job = session.still_render_job;
  const fs::path output_path(job.status.output_path);
  expect(job.settings.width == 64u && job.settings.height == 4'096u &&
             job.settings.target_samples == 32u &&
             job.settings.samples_per_submit == 32u,
         "still-render numeric settings clamp to bounded ranges");
  expect(output_path.parent_path() == directory / "output" &&
             output_path.extension() == ".exr" &&
             output_path.filename().string().find(':') == std::string::npos,
         "still-render filename is sanitized below app/output");
  expect(session.stillRenderActive() &&
             session.playback_state == xpbd::app::PlaybackState::Paused &&
             job.snapshot.has_value() &&
             job.snapshot->world_environment.generation == 41u,
         "queued still render freezes world state and pauses playback");
  expect(job.snapshot->path_trace_settings.requested_denoiser ==
                 xpbd::gfx::PathTraceDenoiser::Raw &&
             job.snapshot->path_trace_settings.requested_upscale ==
                 xpbd::gfx::PathTraceUpscale::Off &&
             job.snapshot->path_trace_settings.requested_frame_generation ==
                 xpbd::gfx::PathTraceFrameGeneration::Off &&
             job.snapshot->path_trace_settings.requested_reflex_mode ==
                 xpbd::gfx::PathTraceReflexMode::Off &&
             !job.snapshot->path_trace_settings.adaptive_sampling &&
             job.snapshot->path_trace_settings.preview_resolution_scale ==
                 1.0f &&
             job.snapshot->path_trace_settings.interactive_quality ==
                 xpbd::gfx::PathTraceInteractiveQuality::Full,
         "still render forces raw full-resolution sample accumulation");

  std::array<float, 16> view{};
  std::array<float, 16> proj{};
  view[0] = view[5] = view[10] = view[15] = 1.0f;
  proj[0] = proj[5] = proj[10] = proj[15] = 2.0f;
  expect(session.freezeQueuedStillRenderCamera(view.data(), proj.data(),
                                               12.5f) &&
             session.still_render_job.snapshot->camera_frozen &&
             session.still_render_job.snapshot->view_matrix == view &&
             session.still_render_job.snapshot->proj_matrix == proj,
         "queued still render freezes exact camera matrices");
  session.world_environment.generation = 99u;
  expect(session.still_render_job.snapshot->world_environment.generation ==
             41u,
         "still-render world snapshot is a deep independent value");

  session.still_render_job.status.state =
      xpbd::gfx::StillRenderJobState::Completed;
  session.synchronizeStillRenderState();
  expect(session.playback_state == xpbd::app::PlaybackState::Playing &&
             !session.stillRenderActive(),
         "terminal still render restores prior playback state");

  const std::array<std::uint16_t, 8> rgba16f{
      0x3c00u, 0x3800u, 0x0000u, 0x0000u,
      0x0000u, 0x3c00u, 0x3800u, 0x3c00u};
  const std::array<float, 2> device_depth{1.0f, 0.5f};
  xpbd::gfx::StillImageDisplayTransform display;
  std::vector<std::uint8_t> exr;
  std::string error;
  expect(xpbd::gfx::encodeOpenExrRgba16f(
             2u, 1u, rgba16f.data(), rgba16f.size(), true, exr, &error) &&
             exr.size() > 64u && exr[0] == 0x76u && exr[1] == 0x2fu &&
             exr[2] == 0x31u && exr[3] == 0x01u,
         "linear RGBA16F encoder writes an OpenEXR scanline stream");
  const fs::path exr_path = directory / "output" / "encoder.exr";
  expect(xpbd::gfx::writeStillImageRgba16f(
             exr_path, xpbd::gfx::StillImageFormat::Exr, 2u, 1u,
             rgba16f.data(), rgba16f.size(), display, false, nullptr, 0u,
             &error) &&
             fs::file_size(exr_path, filesystem_error) == exr.size(),
         "still-image writer emits the encoded EXR file");
  const fs::path png_path = directory / "output" / "transparent.png";
  xpbd::gfx::TextureImage decoded_png;
  expect(xpbd::gfx::writeStillImageRgba16f(
             png_path, xpbd::gfx::StillImageFormat::Png, 2u, 1u,
             rgba16f.data(), rgba16f.size(), display, true,
             device_depth.data(), device_depth.size(), &error) &&
             xpbd::gfx::loadTextureImage(png_path, decoded_png, &error) &&
             decoded_png.rgba.size() == 8u &&
             decoded_png.rgba[0] == 0u && decoded_png.rgba[1] == 0u &&
             decoded_png.rgba[2] == 0u && decoded_png.rgba[3] == 0u &&
             decoded_png.rgba[7] == 255u,
         "depth-aware PNG clears a visible sky behind transparent output");
  if (const char *evidence_directory =
          std::getenv("XPBD_STILL_TEST_EVIDENCE");
      evidence_directory != nullptr && evidence_directory[0] != '\0') {
    const fs::path evidence_root(evidence_directory);
    expect(xpbd::gfx::writeStillImageRgba16f(
               evidence_root / "still-export.exr",
               xpbd::gfx::StillImageFormat::Exr, 2u, 1u, rgba16f.data(),
               rgba16f.size(), display, true, device_depth.data(),
               device_depth.size(), &error) &&
               xpbd::gfx::writeStillImageRgba16f(
                   evidence_root / "still-export.png",
                   xpbd::gfx::StillImageFormat::Png, 2u, 1u,
                   rgba16f.data(), rgba16f.size(), display, true,
                   device_depth.data(), device_depth.size(), &error),
           "write optional still-image decoder evidence");
  }

  session.still_render_job = {};
  session.setApplicationDirectory({});
  fs::remove_all(directory, filesystem_error);
  expect(!filesystem_error, "remove isolated still-render directory");
}

} // namespace

int main(int argc, char **argv) {
  testDefaultEmptyScene();
  testSceneSelectionTransactionsAndPersistence();
  testIndependentSkyRenderingState();
  testPathTraceSettingsPersistenceAndClassification();
  testStillRenderSnapshotAndOutput();
  testTransactionalLabPbrSuiteImport();
  if (argc >= 2) {
    testExternalLabPbrSuite(std::filesystem::path(argv[1]));
  }
  if (g_failures != 0) {
    std::fprintf(stderr, "%d AppSession regression(s) failed\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("All AppSession regressions passed");
  return EXIT_SUCCESS;
}
