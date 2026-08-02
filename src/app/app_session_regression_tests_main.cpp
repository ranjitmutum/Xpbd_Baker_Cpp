// Focused AppSession regression tests for transactional LabPBR suite import.

#include "xpbd/app/app_session.hpp"
#include "xpbd/gfx/labpbr_authoring.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/still_image_export.hpp"
#include "test_support/labpbr_synthetic_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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

bool sameTexture(const xpbd::gfx::SharedTextureImage &lhs,
                 const xpbd::gfx::SharedTextureImage &rhs) {
  if (lhs == rhs) {
    return true;
  }
  return lhs != nullptr && rhs != nullptr && sameTexture(*lhs, *rhs);
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
  xpbd::gfx::SharedTextureImage texture;
  xpbd::gfx::ResolvedUvDomain domain;
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
      session.model_uv_domain,
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
         session.model_uv_domain == before.domain &&
         sameResolvedMaterial(session.resolved_material, before.material) &&
         session.labpbr_uv_coverage.width == before.coverage.width &&
         session.labpbr_uv_coverage.height == before.coverage.height &&
         session.labpbr_uv_coverage.group_runs ==
             before.coverage.group_runs &&
         session.labpbr_group_overrides == before.overrides &&
         session.labpbr_draft == before.draft &&
         sameTexture(session.labpbr_composition.specular,
                     before.composition.specular) &&
         session.labpbr_composition.specular_materialization_deferred ==
             before.composition.specular_materialization_deferred &&
         session.labpbr_composition.deferred_width ==
             before.composition.deferred_width &&
         session.labpbr_composition.deferred_height ==
             before.composition.deferred_height &&
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

void testStillRenderSceneSelectionContentMatrix() {
  using xpbd::app::SceneSelectionKind;
  xpbd::app::StillRenderSnapshot snapshot;

  snapshot.scene_selection.kind = SceneSelectionKind::Empty;
  expect(!snapshot.scene_selection.rendersLoadedContent(true),
         "Still Empty Scene excludes retained model content");

  snapshot.scene_selection.kind = SceneSelectionKind::Loaded;
  expect(snapshot.scene_selection.rendersLoadedContent(true),
         "Still Loaded Scene includes retained model content");

  snapshot.scene_selection.kind = SceneSelectionKind::UserBuilt;
  expect(snapshot.scene_selection.rendersLoadedContent(true),
         "Still User-Built Scene includes retained model content");

  snapshot.scene_selection.kind = SceneSelectionKind::Preset;
  expect(snapshot.scene_selection.rendersLoadedContent(true),
         "Still Preset Scene includes retained model content over its background");

  expect(!snapshot.scene_selection.rendersLoadedContent(false),
         "Still Scene selection cannot synthesize missing model content");
}

void testBoneSubtreeVisibilityGeneration() {
  auto &session = xpbd::app::AppSession::instance();
  const auto original_geometry = session.geometry;
  const auto original_hidden_bones = session.hidden_bone_names;
  const auto original_status = session.status;

  xpbd::loader::Bone root;
  root.name = "visibility_root";
  xpbd::loader::Bone child;
  child.name = "visibility_child";
  child.has_parent = true;
  child.parent = root.name;
  xpbd::loader::Bone sibling;
  sibling.name = "visibility_sibling";
  session.geometry.bones = {root, child, sibling};
  session.hidden_bone_names.clear();

  const std::uint64_t visibility_before =
      session.viewportVisibilityGeneration();
  const std::uint64_t appearance_before =
      session.viewportAppearanceGeneration();
  session.setBoneVisible(root.name, false);
  expect(!session.isBoneVisible(root.name) &&
             !session.isBoneVisible(child.name) &&
             session.isBoneVisible(sibling.name),
         "hiding a bone hides its subtree without affecting siblings");
  expect(session.viewportVisibilityGeneration() > visibility_before &&
             session.viewportAppearanceGeneration() > appearance_before,
         "subtree hide advances visibility and appearance generations");

  const std::uint64_t hidden_generation =
      session.viewportVisibilityGeneration();
  session.setBoneVisible(root.name, false);
  expect(session.viewportVisibilityGeneration() == hidden_generation,
         "idempotent subtree hide does not spuriously advance generation");

  session.setBoneVisible(root.name, true);
  expect(session.isBoneVisible(root.name) &&
             session.isBoneVisible(child.name) &&
             session.viewportVisibilityGeneration() > hidden_generation,
         "restoring a bone restores its subtree and advances generation");
  const std::uint64_t restored_generation =
      session.viewportVisibilityGeneration();
  session.setBoneVisible("visibility_missing", false);
  expect(session.viewportVisibilityGeneration() == restored_generation,
         "missing visibility target leaves session generations unchanged");

  session.geometry = original_geometry;
  session.hidden_bone_names = original_hidden_bones;
  session.status = original_status;
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
  session.path_trace_settings.display_exposure_ev = 2.0f;
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
  session.path_trace_settings.display_exposure_ev = -5.0f;
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
             session.path_trace_settings.pause_accumulation &&
             session.path_trace_settings.display_exposure_ev == 2.0f,
         "path tracing JSON round-trip preserves an explicit +2 EV preference");

  const auto legacy_settings_path =
      std::filesystem::path(settings_path.string() + ".legacy");
  {
    std::ofstream legacy_output(legacy_settings_path,
                                std::ios::binary | std::ios::trunc);
    legacy_output <<
        R"({"schema":"xpbd-path-tracing/1","film":{}})";
  }
  session.path_trace_settings.display_exposure_ev = -5.0f;
  expect(session.loadPathTraceSettings(legacy_settings_path) &&
             session.path_trace_settings.display_exposure_ev ==
                 gfx::kDefaultPathTraceExposureEv &&
             gfx::kDefaultPathTraceExposureEv == 0.0f,
         "legacy path tracing JSON without exposure adopts neutral default");
  std::error_code remove_error;
  std::filesystem::remove(settings_path, remove_error);
  expect(!remove_error,
         "path tracing round-trip removes temporary JSON");
  remove_error.clear();
  std::filesystem::remove(legacy_settings_path, remove_error);
  expect(!remove_error,
         "path tracing legacy fixture removes temporary JSON");
  session.clearPathTraceRenderSnapshot();
}

bool writeBytes(const std::filesystem::path &path,
                const std::vector<std::uint8_t> &bytes) {
  std::ofstream output(xpbd::gfx::pathForFilesystemIo(path),
                       std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  return output.good();
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path &path) {
  std::ifstream input(xpbd::gfx::pathForFilesystemIo(path), std::ios::binary);
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
  expect(session.labpbr_suite_source.metadataValid() &&
             !session.labpbr_suite_source.base.original_bytes &&
             !session.labpbr_suite_source.specular.original_bytes &&
             !session.labpbr_suite_source.normal.original_bytes &&
             !session.labpbr_suite_source.properties.original_bytes &&
             session.resolved_material.valid() &&
             session.labpbr_imported_normal.valid() &&
             session.model_texture ==
                 session.resolved_material.baseImageAsset() &&
             session.labpbr_imported_normal.decoded ==
                 session.resolved_material.normalImageAsset() &&
             session.labpbr_composition.specular ==
                 session.resolved_material.specularImageAsset(),
         "AppSession retains metadata-only source and shared material assets");

  const auto cached_base = session.model_texture;
  const auto cached_normal =
      session.resolved_material.normalImageAsset();
  const auto cached_specular =
      session.resolved_material.specularImageAsset();
  const auto generation_before_cache_hit = session.materialGeneration();
  expect(session.reloadLabPbrSuite() &&
             session.labpbr_last_import_cache_hit &&
             session.model_texture == cached_base &&
             session.resolved_material.normalImageAsset() == cached_normal &&
             session.resolved_material.specularImageAsset() ==
                 cached_specular &&
             session.materialGeneration() == generation_before_cache_hit,
         "AppSession checksum reload hits the shared LRU without changing material generation");

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
  expect(session.labpbr_imported_normal.original_file_bytes != nullptr &&
             *session.labpbr_imported_normal.original_file_bytes == normal_png,
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
  expect(!session.labpbr_uv_coverage.valid() &&
             session.labpbr_uv_coverage.group_runs.empty() &&
             session.labpbr_composition.exportable() &&
             !session.labpbr_composition.specular_materialization_deferred &&
             session.labpbr_composition.specular ==
                 session.resolved_material.specularImageAsset(),
         "no Override keeps Coverage nonresident and shares Source Specular");
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
             session.resolved_material.sample(0.5f, 0.5f).emission_strength ==
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

void testUnicodeLongPathTextureTransactions() {
  namespace fs = std::filesystem;

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path root =
      fs::temp_directory_path() / fs::path(L"xpbd_Unicode路径_事务") /
      std::to_wstring(nonce);
  fs::path directory = root;
  while (directory.native().size() < 330u) {
    directory /= fs::path(L"层级_长路径_甲乙丙丁_0123456789");
  }

  std::error_code filesystem_error;
  fs::create_directories(xpbd::gfx::pathForFilesystemIo(directory),
                         filesystem_error);
  expect(!filesystem_error && directory.native().size() > 260u,
         "create runtime Unicode path longer than the legacy Windows limit");
  if (filesystem_error) {
    return;
  }

  const std::vector<std::uint8_t> base_rgba{
      240u, 120u, 60u, 255u, 20u, 70u, 150u, 80u};
  const std::vector<std::uint8_t> specular_rgba{
      0u, 10u, 32u, 0u, 255u, 230u, 64u, 254u};
  const std::vector<std::uint8_t> normal_rgba{
      128u, 128u, 255u, 255u, 140u, 120u, 210u, 24u};
  const auto base_png = encodePng(2, 1, base_rgba);
  const auto specular_png = encodePng(2, 1, specular_rgba);
  const auto normal_png = encodePng(2, 1, normal_rgba);
  const std::string properties_text = "format=lab-pbr/1.3\n";
  const std::vector<std::uint8_t> properties_bytes(properties_text.begin(),
                                                     properties_text.end());

  const fs::path base_path = directory / fs::path(L"角色_基础贴图.png");
  const fs::path specular_path =
      directory / fs::path(L"角色_基础贴图_s.png");
  const fs::path normal_path =
      directory / fs::path(L"角色_基础贴图_n.png");
  const fs::path properties_path = directory / "texture.properties";
  expect(writeBytes(base_path, base_png) &&
             writeBytes(specular_path, specular_png) &&
             writeBytes(normal_path, normal_png) &&
             writeBytes(properties_path, properties_bytes),
         "write runtime Unicode Base/Normal/Specular/properties fixtures");

  xpbd::gfx::TextureImage decoded_base;
  std::string error;
  expect(xpbd::gfx::loadTextureImage(base_path, decoded_base, &error) &&
             decoded_base.rgba == base_rgba &&
             decoded_base.path == xpbd::gfx::pathUtf8String(base_path),
         "memory-only Base import accepts a Unicode long path");

  xpbd::gfx::TextureImage preserved = decoded_base;
  preserved.path = "preserved-candidate";
  const auto preserved_before = preserved;
  xpbd::gfx::TextureDecodeLimits tight_limits;
  tight_limits.maximum_peak_bytes = 1u;
  error.clear();
  expect(!xpbd::gfx::loadTextureImage(base_path, preserved, &error,
                                      tight_limits) &&
             sameTexture(preserved, preserved_before) &&
             preserved.path == preserved_before.path &&
             error.find("budget stage") != std::string::npos,
         "Base snapshot budget rejection preserves the output candidate");

  xpbd::gfx::TextureImage resident_preserved = decoded_base;
  resident_preserved.path = "resident-preserved-candidate";
  const auto resident_before = resident_preserved;
  xpbd::gfx::TextureDecodeLimits resident_limits;
  // 77 encoded + 8 decoder + 8 candidate + 8 resident caller bytes = 101.
  resident_limits.maximum_peak_bytes = 100u;
  error.clear();
  expect(!xpbd::gfx::loadTextureImage(base_path, resident_preserved, &error,
                                      resident_limits) &&
             sameTexture(resident_preserved, resident_before) &&
             resident_preserved.path == resident_before.path &&
             error.find("required_peak=101 bytes") != std::string::npos,
         "Base path decode counts resident output and preserves it on budget failure");

  const auto strict = xpbd::gfx::importLabPbrSuite(base_path, false);
  expect(strict.imported() && strict.suite.base_image != nullptr &&
             strict.suite.base_image->rgba == base_rgba &&
             strict.suite.base_image ==
                 strict.suite.material.baseImageAsset() &&
             *strict.suite.source.base.original_bytes == base_png &&
             *strict.suite.source.specular.original_bytes == specular_png &&
             *strict.suite.source.normal.original_bytes == normal_png &&
             *strict.suite.source.properties.original_bytes ==
                 properties_bytes,
         "strict Unicode suite import retains exact immutable snapshots");

  xpbd::gfx::ReadOnlyIrisNormalAsset iris;
  expect(xpbd::gfx::importReadOnlyIrisNormal(normal_path, 2, 1, iris,
                                             &error) &&
             iris.original_file_bytes != nullptr &&
             *iris.original_file_bytes == normal_png &&
             iris.decoded != nullptr && iris.decoded->rgba == normal_rgba &&
             iris.decoded->path == xpbd::gfx::pathUtf8String(normal_path),
         "read-only Iris import accepts Unicode long paths and exact bytes");

  auto &session = xpbd::app::AppSession::instance();
  session.labpbr_draft_dirty = false;
  session.clearTexture();
  expect(session.loadTexture(base_path) &&
             session.texture_path == xpbd::gfx::pathUtf8String(base_path),
         "AppSession commits a Base texture from a Unicode long path");
  expect(session.requestLabPbrSuiteImport(base_path) &&
             session.labpbr_suite_source.metadataValid() &&
             !session.labpbr_suite_source.base.original_bytes &&
             !session.labpbr_suite_source.specular.original_bytes &&
             session.texture_path == xpbd::gfx::pathUtf8String(base_path),
         "AppSession commits a strict suite from a Unicode long path");
  expect(session.importLabPbrNormal(normal_path) &&
             session.labpbr_imported_normal.original_file_bytes != nullptr &&
             *session.labpbr_imported_normal.original_file_bytes == normal_png,
         "AppSession commits a read-only Iris normal from a Unicode long path");
  const auto committed = snapshot(session);

  const auto saved_peak_budget = session.labpbr_peak_memory_budget_bytes;
  session.labpbr_peak_memory_budget_bytes = 1u;
  const bool budget_rejected = !session.loadTexture(base_path);
  session.labpbr_peak_memory_budget_bytes = saved_peak_budget;
  expect(budget_rejected && unchanged(session, committed) &&
             session.last_error.find("budget") != std::string::npos,
         "product LabPBR budget failure preserves Session Generation and material state");

  const fs::path missing_base = directory / fs::path(L"不存在_基础.png");
  expect(!session.loadTexture(missing_base) && unchanged(session, committed) &&
             session.last_error.find("file-check stage") != std::string::npos,
         "missing Unicode Base import preserves the complete material session");

  const fs::path corrupt_base = directory / fs::path(L"损坏_基础.png");
  const std::vector<std::uint8_t> corrupt_header{0u, 1u, 2u, 3u};
  expect(writeBytes(corrupt_base, corrupt_header) &&
             !session.loadTexture(corrupt_base) &&
             unchanged(session, committed) &&
             session.last_error.find("Header stage") != std::string::npos,
         "corrupt Unicode Base header preserves the complete material session");

  auto corrupt_decode_png = base_png;
  const std::array<std::uint8_t, 4> idat{'I', 'D', 'A', 'T'};
  const auto idat_position =
      std::search(corrupt_decode_png.begin(), corrupt_decode_png.end(),
                  idat.begin(), idat.end());
  if (idat_position != corrupt_decode_png.end() &&
      std::distance(idat_position, corrupt_decode_png.end()) > 4) {
    *(idat_position + 4) = 0u;
  }
  const fs::path decode_failure = directory / fs::path(L"损坏_IDAT.png");
  expect(idat_position != corrupt_decode_png.end() &&
             writeBytes(decode_failure, corrupt_decode_png) &&
             !session.loadTexture(decode_failure) &&
             unchanged(session, committed) &&
             session.last_error.find("Decode stage") != std::string::npos,
         "post-Header Base decode failure preserves the complete session");

  const fs::path missing_suite_directory = directory / fs::path(L"缺少边车");
  fs::create_directories(
      xpbd::gfx::pathForFilesystemIo(missing_suite_directory),
      filesystem_error);
  const fs::path missing_suite_base =
      missing_suite_directory / fs::path(L"缺少组合.png");
  expect(!filesystem_error && writeBytes(missing_suite_base, base_png) &&
             !session.requestLabPbrSuiteImport(missing_suite_base) &&
             unchanged(session, committed) &&
             session.last_error.find("Specular Sidecar") != std::string::npos,
         "missing Unicode Sidecar preserves the complete material session");

  const fs::path corrupt_suite_directory = directory / fs::path(L"损坏边车");
  fs::create_directories(
      xpbd::gfx::pathForFilesystemIo(corrupt_suite_directory),
      filesystem_error);
  const fs::path corrupt_suite_base =
      corrupt_suite_directory / fs::path(L"组合.png");
  const fs::path corrupt_suite_specular =
      corrupt_suite_directory / fs::path(L"组合_s.png");
  expect(!filesystem_error && writeBytes(corrupt_suite_base, base_png) &&
             writeBytes(corrupt_suite_specular, corrupt_header) &&
             !session.requestLabPbrSuiteImport(corrupt_suite_base) &&
             unchanged(session, committed) &&
             session.last_error.find("Specular Sidecar") != std::string::npos &&
             session.last_error.find("Header stage") != std::string::npos,
         "corrupt Unicode Sidecar preserves the complete material session");

  const fs::path missing_iris = directory / fs::path(L"不存在_法线.png");
  expect(!session.importLabPbrNormal(missing_iris) &&
             unchanged(session, committed) &&
             session.last_error.find("file-check stage") != std::string::npos,
         "missing Unicode Iris normal preserves the complete material session");
  const fs::path corrupt_iris = directory / fs::path(L"损坏_法线.png");
  expect(writeBytes(corrupt_iris, corrupt_header) &&
             !session.importLabPbrNormal(corrupt_iris) &&
             unchanged(session, committed) &&
             session.last_error.find("Header stage") != std::string::npos,
         "corrupt Unicode Iris normal preserves the complete material session");
  const fs::path mismatched_iris = directory / fs::path(L"错尺寸_法线.png");
  expect(writeBytes(mismatched_iris,
                    encodePng(1, 1, {128u, 128u, 255u, 255u})) &&
             !session.importLabPbrNormal(mismatched_iris) &&
             unchanged(session, committed) &&
             session.last_error.find("Domain stage") != std::string::npos,
         "mismatched Unicode Iris Domain preserves the complete session");

  const fs::path app_source_path =
      fs::absolute(fs::path(__FILE__)).parent_path() / "app_session.cpp";
  const fs::path texture_source_path =
      fs::absolute(fs::path(__FILE__)).parent_path().parent_path() / "gfx" /
      "texture_image.cpp";
  const auto app_source_bytes = readBytes(app_source_path);
  const auto texture_source_bytes = readBytes(texture_source_path);
  const std::string app_source(app_source_bytes.begin(), app_source_bytes.end());
  const std::string texture_source(texture_source_bytes.begin(),
                                   texture_source_bytes.end());
  expect(app_source.find("IFileOpenDialog") != std::string::npos &&
             app_source.find("IFileSaveDialog") != std::string::npos &&
             app_source.find("GetOpenFileName") == std::string::npos &&
             app_source.find("GetSaveFileName") == std::string::npos &&
             app_source.find("OPENFILENAME") == std::string::npos &&
             app_source.find("MAX_PATH") == std::string::npos,
         "file-dialog source contract has no fixed legacy path buffer");
  expect(texture_source.find("stbi_info_from_memory") != std::string::npos &&
             texture_source.find("stbi_load_from_memory") !=
                 std::string::npos &&
             texture_source.find("stbi_info(") == std::string::npos &&
             texture_source.find("stbi_load(") == std::string::npos,
         "texture source contract uses memory-only stb decode");

  expect(readBytes(base_path) == base_png &&
             readBytes(specular_path) == specular_png &&
             readBytes(normal_path) == normal_png &&
             readBytes(properties_path) == properties_bytes,
         "Unicode success and failure paths never mutate source fixtures");
  session.clearTexture();
  filesystem_error.clear();
  fs::remove_all(xpbd::gfx::pathForFilesystemIo(root), filesystem_error);
  expect(!filesystem_error, "remove runtime Unicode long-path fixtures");
}

void testLargeUvModelMaterialTransactions() {
  namespace fs = std::filesystem;
  using xpbd::gfx::UvDomainKind;
  using xpbd::test_support::SyntheticLabPbrSuitePaths;
  using xpbd::test_support::SyntheticLargeUvFixture;

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() / fs::path(L"xpbd会话_大UV_事务") /
      std::to_string(nonce);
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create Unicode large-UV AppSession fixture directory");
  if (filesystem_error) {
    return;
  }

  SyntheticLargeUvFixture fixture;
  std::string error;
  expect(xpbd::test_support::buildSyntheticLargeUvFixture(fixture, &error),
         "build AppSession large-UV runtime fixture");
  SyntheticLabPbrSuitePaths suite_paths;
  expect(xpbd::test_support::writeSyntheticLabPbrSuite(
             fixture, directory, fs::path(L"角色材质_眼睛.png"),
             suite_paths, &error),
         "write Unicode Base Normal Specular suite for AppSession");

  static constexpr std::string_view kLargeModel = R"json(
{"format_version":"1.12.0","minecraft:geometry":[{"description":{"identifier":"geometry.session_large_uv","texture_width":16,"texture_height":16},"bones":[
{"name":"body","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[16,16,1],"uv":{"north":{"uv":[0,0],"uv_size":[16,16]}}}]},
{"name":"eye_left","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[16,16,1],"uv":{"north":{"uv":[192,0],"uv_size":[16,16]}}}]},
{"name":"eye_right","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[16,16,1],"uv":{"north":{"uv":[208,0],"uv_size":[16,16]}}}]}
]}]})json";
  static constexpr std::string_view kOutOfBoundsModel = R"json(
{"format_version":"1.12.0","minecraft:geometry":[{"description":{"identifier":"geometry.session_out_of_bounds","texture_width":16,"texture_height":16},"bones":[
{"name":"outside","pivot":[0,0,0],"cubes":[{"origin":[0,0,0],"size":[16,16,1],"uv":{"north":{"uv":[300,0],"uv_size":[16,16]}}}]}
]}]})json";
  const fs::path large_model_path = directory / fs::path(L"角色_大UV.geo.json");
  const fs::path out_of_bounds_model_path =
      directory / fs::path(L"角色_真正越界.geo.json");
  expect(writeBytes(
             large_model_path,
             std::vector<std::uint8_t>(kLargeModel.begin(),
                                       kLargeModel.end())) &&
             writeBytes(
                 out_of_bounds_model_path,
                 std::vector<std::uint8_t>(kOutOfBoundsModel.begin(),
                                           kOutOfBoundsModel.end())),
         "write Unicode runtime model fixtures");

  auto &session = xpbd::app::AppSession::instance();
  session.labpbr_draft_dirty = false;
  session.clearTexture();
  session.loadModel(large_model_path);
  expect(session.last_error.empty() &&
             session.geometry.description.identifier ==
                 "geometry.session_large_uv" &&
             session.geometry.bones.size() == 3u,
         "real AppSession model entry commits valid large-UV geometry");
  expect(session.requestLabPbrSuiteImport(suite_paths.base) &&
             session.model_uv_domain.kind == UvDomainKind::Recovered &&
             session.model_uv_domain.width == 256.0 &&
             session.resolved_material.normal_map_active &&
             session.resolved_material.specular_map_active,
         "real suite entry commits one Recovered Domain for all channels");
  expect(!session.labpbr_uv_coverage.valid() &&
             session.labpbr_uv_coverage.group_runs.empty() &&
             !session.labpbr_composition.specular_materialization_deferred &&
             session.labpbr_composition.specular ==
                 session.resolved_material.specularImageAsset(),
         "large-UV suite without Overrides shares Source Specular");
  const auto source_specular = session.labpbr_composition.specular;
  session.selected_bone_name = "eye_left";
  session.loadSelectedLabPbrDraft();
  session.labpbr_draft.roughness_enabled = true;
  session.labpbr_draft.roughness = 0.25f;
  session.markLabPbrDraftDirty();
  expect(session.applySelectedLabPbrDraft(),
         "real AppSession Override materializes large-UV authoring state");
  const auto *left_coverage = session.labpbr_uv_coverage.find("eye_left");
  const auto *right_coverage = session.labpbr_uv_coverage.find("eye_right");
  expect(session.labpbr_uv_coverage.valid() &&
             left_coverage != nullptr && !left_coverage->empty() &&
             right_coverage != nullptr && !right_coverage->empty() &&
             session.labpbr_composition.specular != nullptr &&
             session.labpbr_composition.specular->valid() &&
             session.labpbr_composition.specular != source_specular &&
             session.labpbr_composition.specular ==
                 session.resolved_material.specularImageAsset() &&
             !session.labpbr_composition.specular_materialization_deferred,
         "first Override materializes run Coverage for both large-UV eyes");
  std::weak_ptr<const xpbd::gfx::TextureImage> authored_specular =
      session.labpbr_composition.specular;
  expect(session.restoreSelectedLabPbrFromTexture() &&
             session.labpbr_composition.specular == source_specular &&
             authored_specular.expired(),
         "clearing the last Override releases the authored COW image");
  session.labpbr_draft.roughness_enabled = true;
  session.labpbr_draft.roughness = 0.25f;
  session.markLabPbrDraftDirty();
  expect(session.applySelectedLabPbrDraft(),
         "large-UV Override can be reapplied after COW release");

  std::vector<std::uint8_t> small_rgba(16u * 16u * 4u, 255u);
  std::vector<std::uint8_t> small_png;
  expect(xpbd::gfx::encodePngRgba8(16, 16, small_rgba, small_png, &error),
         "encode runtime smaller-atlas rejection fixture");
  const fs::path small_texture_path =
      directory / fs::path(L"错误的小贴图_16x16.png");
  expect(writeBytes(small_texture_path, small_png),
         "write runtime smaller-atlas rejection fixture");
  const auto material_before_small_texture = snapshot(session);
  expect(!session.loadTexture(small_texture_path) &&
             session.last_error.find("exceed") != std::string::npos &&
             unchanged(session, material_before_small_texture),
         "smaller atlas Domain failure preserves Session and GPU material");

  const auto material_before_bad_model = snapshot(session);
  const std::string model_path_before = session.model_path;
  const std::string identifier_before =
      session.geometry.description.identifier;
  const std::vector<std::string> bone_names_before{
      session.geometry.bones[0].name,
      session.geometry.bones[1].name,
      session.geometry.bones[2].name,
  };
  const double left_uv_before =
      session.geometry.bones[1].cubes[0].uv_north.u;
  const auto model_generation_before = session.modelGeneration();
  const auto physics_generation_before = session.physicsGeneration();
  const auto material_generation_before = session.materialGeneration();
  const auto appearance_generation_before =
      session.viewportAppearanceGeneration();
  const auto visibility_generation_before =
      session.viewportVisibilityGeneration();
  const auto scene_generation_before = session.scene_selection.generation;
  const auto pt_reset_before = session.path_trace_settings.reset_generation;
  session.loadModel(out_of_bounds_model_path);
  const bool same_bones =
      session.geometry.bones.size() == bone_names_before.size() &&
      session.geometry.bones[0].name == bone_names_before[0] &&
      session.geometry.bones[1].name == bone_names_before[1] &&
      session.geometry.bones[2].name == bone_names_before[2];
  expect(session.last_error.find("exceed") != std::string::npos &&
             session.model_path == model_path_before &&
             session.geometry.description.identifier == identifier_before &&
             same_bones &&
             session.geometry.bones[1].cubes[0].uv_north.u == left_uv_before &&
             unchanged(session, material_before_bad_model),
         "out-of-domain model load preserves committed model and material");
  expect(session.modelGeneration() == model_generation_before &&
             session.physicsGeneration() == physics_generation_before &&
             session.materialGeneration() == material_generation_before &&
             session.viewportAppearanceGeneration() ==
                 appearance_generation_before &&
             session.viewportVisibilityGeneration() ==
                 visibility_generation_before &&
             session.scene_selection.generation == scene_generation_before &&
             session.path_trace_settings.reset_generation == pt_reset_before,
         "failed model preflight preserves every renderer/history generation");

  session.labpbr_draft_dirty = false;
  session.clearTexture();
  fs::remove_all(directory, filesystem_error);
  expect(!filesystem_error,
         "remove Unicode large-UV AppSession fixture directory");
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

  expect(session.labpbr_suite_source.metadataValid() &&
             session.labpbr_suite_source.base.path == normalized_base,
         "external suite commits through the real AppSession path");
  expect(session.labpbr_suite_source
                 .confirmed_labpbr13_without_properties &&
             session.resolved_material.specular_map_active &&
             session.resolved_material.normal_map_active,
         "external suite activates confirmed LabPBR specular and normal maps");
  expect(session.model_texture != nullptr &&
             session.model_texture->width == 1024 &&
             session.model_texture->height == 1024,
         "external suite preserves expected 1024x1024 dimensions");
  expect(session.materialGeneration() > generation_before,
         "external suite advances material generation on first commit");
  expect(!session.labpbr_suite_source.base.original_bytes &&
             !session.labpbr_suite_source.specular.original_bytes &&
             !session.labpbr_suite_source.normal.original_bytes &&
             session.labpbr_imported_normal.original_file_bytes != nullptr &&
             *session.labpbr_imported_normal.original_file_bytes ==
                 normal_bytes,
         "external Session retains metadata and only the Iris encoded asset");

  auto committed = snapshot(session);
  expect(session.reloadLabPbrSuite() && session.labpbr_last_import_cache_hit,
         "external suite reload reuses the shared byte-bounded LRU cache");
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

  session.playback_state = xpbd::app::PlaybackState::Playing;
  expect(session.queueStillRender(),
         "queue another still render after completion");
  session.still_render_job.status.state =
      xpbd::gfx::StillRenderJobState::Failed;
  session.still_render_job.status.error =
      "injected target allocation failure";
  session.synchronizeStillRenderState();
  expect(session.playback_state == xpbd::app::PlaybackState::Playing &&
             session.last_error == "injected target allocation failure" &&
             !session.stillRenderActive(),
         "failed still render restores playback and exposes backend error");

  session.playback_state = xpbd::app::PlaybackState::Playing;
  expect(session.queueStillRender(),
         "queue another still render after failure");
  session.still_render_job.status.state =
      xpbd::gfx::StillRenderJobState::Cancelled;
  session.synchronizeStillRenderState();
  expect(session.playback_state == xpbd::app::PlaybackState::Playing &&
             session.last_error.empty() && !session.stillRenderActive(),
         "cancelled still render restores playback and clears stale errors");

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
  auto altered_display = display;
  altered_display.exposure = 8.0f;
  altered_display.white_balance_kelvin = 3200.0f;
  altered_display.bloom_strength = 3.0f;
  altered_display.tone_mapping = 2u;
  const fs::path invariant_exr_path =
      directory / "output" / "encoder-display-variant.exr";
  expect(xpbd::gfx::writeStillImageRgba16f(
             invariant_exr_path, xpbd::gfx::StillImageFormat::Exr, 2u, 1u,
             rgba16f.data(), rgba16f.size(), altered_display, false, nullptr,
             0u, &error) &&
             readBytes(invariant_exr_path) == readBytes(exr_path),
         "EXR bytes ignore exposure, white balance, bloom, and tone mapping");

  const std::array<std::uint16_t, 4> bloom_rgba16f{
      0x4400u, 0x4400u, 0x4400u, 0x3800u};
  auto bloom_display = display;
  bloom_display.bloom_strength = 1.0f;
  bloom_display.tone_mapping = 1u;
  const fs::path bloom_png_path =
      directory / "output" / "coverage-weighted-bloom.png";
  xpbd::gfx::TextureImage decoded_bloom_png;
  expect(xpbd::gfx::writeStillImageRgba16f(
             bloom_png_path, xpbd::gfx::StillImageFormat::Png, 1u, 1u,
             bloom_rgba16f.data(), bloom_rgba16f.size(), bloom_display, true,
             nullptr, 0u, &error) &&
             xpbd::gfx::loadTextureImage(bloom_png_path, decoded_bloom_png,
                                         &error) &&
             decoded_bloom_png.rgba.size() == 4u &&
             decoded_bloom_png.rgba[0] >= 234u &&
             decoded_bloom_png.rgba[0] <= 236u &&
             decoded_bloom_png.rgba[0] == decoded_bloom_png.rgba[1] &&
             decoded_bloom_png.rgba[1] == decoded_bloom_png.rgba[2] &&
             decoded_bloom_png.rgba[3] == 128u,
         "PNG bloom matches preview coverage weighting before tone mapping");
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

  const std::array<std::uint8_t, 24> cubemap_rgba{
      255u, 0u,   0u,   255u, // +X red
      0u,   255u, 0u,   255u, // -X green
      0u,   0u,   255u, 255u, // +Y blue
      255u, 255u, 0u,   255u, // -Y yellow
      255u, 0u,   255u, 255u, // +Z magenta
      0u,   255u, 255u, 255u  // -Z cyan
  };
  xpbd::gfx::StillImageCubemapBackground cubemap_background;
  cubemap_background.face_size = 1u;
  cubemap_background.rgba8 = cubemap_rgba.data();
  cubemap_background.rgba8_size = cubemap_rgba.size();
  cubemap_background.inverse_view_projection = {
      1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  cubemap_background.camera_position = {0.0f, 0.0f, 0.0f};
  const std::array<std::uint16_t, 4> empty_sky_rgba16f{};
  const std::array<float, 1> far_depth{1.0f};

  const fs::path cubemap_png_path = directory / "output" / "cubemap.png";
  xpbd::gfx::TextureImage decoded_cubemap_png;
  expect(xpbd::gfx::writeStillImageRgba16f(
             cubemap_png_path, xpbd::gfx::StillImageFormat::Png, 1u, 1u,
             empty_sky_rgba16f.data(), empty_sky_rgba16f.size(), display,
             false, far_depth.data(), far_depth.size(), &error,
             &cubemap_background) &&
             xpbd::gfx::loadTextureImage(cubemap_png_path,
                                         decoded_cubemap_png, &error) &&
             decoded_cubemap_png.rgba ==
                 std::vector<std::uint8_t>({255u, 0u, 255u, 255u}),
         "opaque Still sky samples the frozen +Z preview cubemap face exactly");

  const std::array<std::uint16_t, 4> half_covered_red_rgba16f{
      0x3c00u, 0x0000u, 0x0000u, 0x3800u};
  const fs::path half_covered_png_path =
      directory / "output" / "cubemap-half-covered.png";
  xpbd::gfx::TextureImage decoded_half_covered_png;
  expect(xpbd::gfx::writeStillImageRgba16f(
             half_covered_png_path, xpbd::gfx::StillImageFormat::Png, 1u, 1u,
             half_covered_red_rgba16f.data(),
             half_covered_red_rgba16f.size(), display, false,
             far_depth.data(), far_depth.size(), &error,
             &cubemap_background) &&
             xpbd::gfx::loadTextureImage(half_covered_png_path,
                                         decoded_half_covered_png, &error) &&
             decoded_half_covered_png.rgba ==
                 std::vector<std::uint8_t>({255u, 0u, 128u, 255u}),
         "opaque Still preserves a half-covered foreground over preview sky");

  const fs::path half_covered_transparent_png_path =
      directory / "output" / "cubemap-half-covered-transparent.png";
  xpbd::gfx::TextureImage decoded_half_covered_transparent_png;
  expect(xpbd::gfx::writeStillImageRgba16f(
             half_covered_transparent_png_path,
             xpbd::gfx::StillImageFormat::Png, 1u, 1u,
             half_covered_red_rgba16f.data(),
             half_covered_red_rgba16f.size(), display, true,
             far_depth.data(), far_depth.size(), &error,
             &cubemap_background) &&
             xpbd::gfx::loadTextureImage(
                 half_covered_transparent_png_path,
                 decoded_half_covered_transparent_png, &error) &&
             decoded_half_covered_transparent_png.rgba ==
                 std::vector<std::uint8_t>({255u, 0u, 0u, 128u}),
         "transparent Still preserves far-depth foreground coverage");

  const fs::path transparent_cubemap_png_path =
      directory / "output" / "cubemap-transparent.png";
  xpbd::gfx::TextureImage decoded_transparent_cubemap_png;
  expect(xpbd::gfx::writeStillImageRgba16f(
             transparent_cubemap_png_path, xpbd::gfx::StillImageFormat::Png,
             1u, 1u, empty_sky_rgba16f.data(), empty_sky_rgba16f.size(),
             display, true, far_depth.data(), far_depth.size(), &error,
             &cubemap_background) &&
             xpbd::gfx::loadTextureImage(transparent_cubemap_png_path,
                                         decoded_transparent_cubemap_png,
                                         &error) &&
             decoded_transparent_cubemap_png.rgba ==
                 std::vector<std::uint8_t>({0u, 0u, 0u, 0u}),
         "transparent Still output clears sky instead of compositing cubemap");

  const fs::path cubemap_exr_path = directory / "output" / "cubemap.exr";
  expect(xpbd::gfx::writeStillImageRgba16f(
             cubemap_exr_path, xpbd::gfx::StillImageFormat::Exr, 1u, 1u,
             empty_sky_rgba16f.data(), empty_sky_rgba16f.size(), display,
             false, far_depth.data(), far_depth.size(), &error,
             &cubemap_background),
         "opaque EXR composites the frozen preview cubemap");
  std::ifstream cubemap_exr_stream(cubemap_exr_path, std::ios::binary);
  const std::vector<std::uint8_t> cubemap_exr_bytes{
      std::istreambuf_iterator<char>(cubemap_exr_stream),
      std::istreambuf_iterator<char>()};
  const auto trailing_half = [&](std::size_t channel) {
    const std::size_t offset = cubemap_exr_bytes.size() - 8u + channel * 2u;
    return static_cast<std::uint16_t>(cubemap_exr_bytes[offset]) |
           static_cast<std::uint16_t>(cubemap_exr_bytes[offset + 1u]) << 8u;
  };
  expect(cubemap_exr_bytes.size() > 16u && trailing_half(0u) == 0x3c00u &&
             trailing_half(1u) == 0x3c00u && trailing_half(2u) == 0x0000u &&
             trailing_half(3u) == 0x3c00u,
         "EXR sky stores display-linear A/B/G/R half-float channels");
  cubemap_exr_stream.close();

  const fs::path half_covered_exr_path =
      directory / "output" / "cubemap-half-covered.exr";
  expect(xpbd::gfx::writeStillImageRgba16f(
             half_covered_exr_path, xpbd::gfx::StillImageFormat::Exr, 1u, 1u,
             half_covered_red_rgba16f.data(),
             half_covered_red_rgba16f.size(), display, false,
             far_depth.data(), far_depth.size(), &error,
             &cubemap_background),
         "opaque EXR composites a half-covered foreground over preview sky");
  std::ifstream half_covered_exr_stream(half_covered_exr_path,
                                        std::ios::binary);
  const std::vector<std::uint8_t> half_covered_exr_bytes{
      std::istreambuf_iterator<char>(half_covered_exr_stream),
      std::istreambuf_iterator<char>()};
  const auto half_covered_trailing_half = [&](std::size_t channel) {
    const std::size_t offset =
        half_covered_exr_bytes.size() - 8u + channel * 2u;
    return static_cast<std::uint16_t>(half_covered_exr_bytes[offset]) |
           static_cast<std::uint16_t>(half_covered_exr_bytes[offset + 1u])
               << 8u;
  };
  expect(half_covered_exr_bytes.size() > 16u &&
             half_covered_trailing_half(0u) == 0x3c00u &&
             half_covered_trailing_half(1u) == 0x3800u &&
             half_covered_trailing_half(2u) == 0x0000u &&
             half_covered_trailing_half(3u) == 0x3c00u,
         "EXR half coverage blends foreground and display-linear sky");
  half_covered_exr_stream.close();

  auto invalid_cubemap_background = cubemap_background;
  invalid_cubemap_background.rgba8_size -= 1u;
  error.clear();
  expect(!xpbd::gfx::writeStillImageRgba16f(
             directory / "output" / "invalid-cubemap.png",
             xpbd::gfx::StillImageFormat::Png, 1u, 1u,
             empty_sky_rgba16f.data(), empty_sky_rgba16f.size(), display,
             false, far_depth.data(), far_depth.size(), &error,
             &invalid_cubemap_background) &&
             error == "still image cubemap background is invalid",
         "Still exporter rejects malformed frozen cubemap ownership");
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
  testStillRenderSceneSelectionContentMatrix();
  testBoneSubtreeVisibilityGeneration();
  testSceneSelectionTransactionsAndPersistence();
  testIndependentSkyRenderingState();
  testPathTraceSettingsPersistenceAndClassification();
  testStillRenderSnapshotAndOutput();
  testTransactionalLabPbrSuiteImport();
  testUnicodeLongPathTextureTransactions();
  testLargeUvModelMaterialTransactions();
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
