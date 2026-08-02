#include "nuklear_ui_internal.hpp"

#include "nuklear_still_settings_panel.hpp"

#include "xpbd/app/i18n.hpp"
#include "xpbd/baker/output_timeline_resampler.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace xpbd::app::ui_internal {

const char *labPbrChannelLabel(gfx::LabPbrOverrideChannel channel) {
  switch (channel) {
  case gfx::LabPbrOverrideChannel::Roughness:
    return tr("labpbr_channel_roughness");
  case gfx::LabPbrOverrideChannel::Metal:
    return tr("labpbr_channel_metal");
  case gfx::LabPbrOverrideChannel::Porosity:
    return tr("labpbr_channel_volume");
  case gfx::LabPbrOverrideChannel::Emission:
    return tr("labpbr_channel_emission");
  }
  return "?";
}

void drawLabPbrEditor(nk_context *ctx, const Geom &g, AppSession &session) {
  heading(ctx, g, tr("labpbr_material"));
  mutedWrap(ctx, g, tr("labpbr_material_hint"));
  if (!session.model_texture.valid()) {
    muted(ctx, g, tr("labpbr_load_texture"));
    return;
  }

  char atlas_line[160];
  std::snprintf(atlas_line, sizeof(atlas_line), tr("labpbr_atlas_size"),
                session.model_texture.width, session.model_texture.height);
  muted(ctx, g, atlas_line);

  heading(ctx, g, tr("labpbr_specular_image"));
  const bool has_imported_specular =
      session.resolved_material.assets.specular_exists &&
      !session.resolved_material.assets.specular.empty();
  if (has_imported_specular) {
    const std::string file_line =
        std::string(tr("labpbr_specular_file")) +
        session.resolved_material.assets.specular.filename().string();
    mutedWrap(ctx, g, file_line.c_str());
  } else {
    mutedWrap(ctx, g, tr("labpbr_specular_none"));
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("labpbr_import_specular"))) {
    if (const auto path = openFileDialog(
            L"Import PBR Map (LabPBR RGBA PNG)",
            L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0")) {
      session.importLabPbrSpecular(*path);
    }
  }
  if (!has_imported_specular) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("labpbr_remove_specular")) &&
      has_imported_specular) {
    session.removeLabPbrSpecular();
  }
  if (!has_imported_specular) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("labpbr_preview"));
  const std::uint32_t material_flags =
      gfx::labPbrFeatureFlags(session.resolved_material.valid()
                                  ? &session.resolved_material
                                  : nullptr);
  char channel_line[192];
  std::snprintf(channel_line, sizeof(channel_line),
                tr("labpbr_gpu_channels"),
                (material_flags & gfx::kLabPbrNormalMapActive) != 0u
                    ? tr("labpbr_channel_enabled")
                    : tr("labpbr_channel_disabled"),
                (material_flags & gfx::kLabPbrSpecularMapActive) != 0u
                    ? tr("labpbr_channel_enabled")
                    : tr("labpbr_channel_disabled"));
  mutedWrap(ctx, g, channel_line);
  std::vector<const char *> debug_views{
      tr("labpbr_view_shaded"),    tr("labpbr_view_base_color"),
      tr("labpbr_view_normal"),    tr("labpbr_view_ao"),
      tr("labpbr_view_roughness"), tr("labpbr_view_f0"),
      tr("labpbr_view_emission"),  tr("labpbr_view_opacity")};
  int debug_view = static_cast<int>(session.labpbr_debug_view);
  const int previous_debug_view = debug_view;
  if (combo(ctx, g, tr("labpbr_preview_mode"), debug_views, debug_view,
            session.bake_busy.load()) &&
      debug_view != previous_debug_view) {
    session.labpbr_debug_view =
        static_cast<gfx::LabPbrDebugView>(debug_view);
    session.resetPathTraceAccumulation();
  }

  if (session.selected_bone_name.empty()) {
    muted(ctx, g, tr("labpbr_select_group"));
  } else {
    const std::string selected =
        std::string(tr("labpbr_selected_group")) +
        session.selected_bone_name;
    muted(ctx, g, selected.c_str());
    const auto covered_texels =
        session.labpbr_uv_coverage.texelCount(session.selected_bone_name);
    const auto displayed_texels = static_cast<std::size_t>((std::min)(
        covered_texels,
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())));
    char coverage_line[160];
    std::snprintf(coverage_line, sizeof(coverage_line),
                  tr("labpbr_coverage_texels"), displayed_texels);
    muted(ctx, g, coverage_line);
  }

  const bool no_group = session.selected_bone_name.empty();
  if (no_group) {
    nk_widget_disable_begin(ctx);
  }
  const auto draft_toggle = [&](const char *label, bool &enabled) {
    if (check(ctx, g, label, enabled, no_group)) {
      session.markLabPbrDraftDirty();
    }
  };
  const auto draft_slider = [&](const char *label, float &value) {
    if (slider(ctx, g, label, value, 0.0f, 1.0f, no_group, 0.01f)) {
      session.markLabPbrDraftDirty();
    }
  };

  heading(ctx, g, tr("labpbr_group_overrides"));
  draft_toggle(tr("labpbr_override_emission"),
               session.labpbr_draft.emission_enabled);
  if (session.labpbr_draft.emission_enabled) {
    draft_slider(tr("labpbr_emission"), session.labpbr_draft.emission);
  }
  draft_toggle(tr("labpbr_override_roughness"),
               session.labpbr_draft.roughness_enabled);
  if (session.labpbr_draft.roughness_enabled) {
    draft_slider(tr("labpbr_roughness"), session.labpbr_draft.roughness);
  }
  draft_toggle(tr("labpbr_override_metal"),
               session.labpbr_draft.metal_enabled);
  if (session.labpbr_draft.metal_enabled) {
    if (check(ctx, g, tr("labpbr_is_metal"), session.labpbr_draft.metal,
              no_group)) {
      session.markLabPbrDraftDirty();
    }
    int metal_value =
        session.labpbr_draft.metal
            ? static_cast<int>(session.labpbr_draft.metal_code)
            : static_cast<int>(session.labpbr_draft.dielectric_f0);
    if (intProperty(ctx, g,
                    session.labpbr_draft.metal
                        ? tr("labpbr_metal_code")
                        : tr("labpbr_dielectric_f0"),
                    metal_value, session.labpbr_draft.metal ? 230 : 0,
                    session.labpbr_draft.metal ? 255 : 229, 1, no_group)) {
      if (session.labpbr_draft.metal) {
        session.labpbr_draft.metal_code =
            static_cast<std::uint8_t>(metal_value);
      } else {
        session.labpbr_draft.dielectric_f0 =
            static_cast<std::uint8_t>(metal_value);
      }
      session.markLabPbrDraftDirty();
    }
  }
  draft_toggle(tr("labpbr_override_volume"),
               session.labpbr_draft.porosity_enabled);
  if (session.labpbr_draft.porosity_enabled) {
    if (check(ctx, g, tr("labpbr_use_sss"),
              session.labpbr_draft.subsurface_scattering, no_group)) {
      session.markLabPbrDraftDirty();
    }
    if (session.labpbr_draft.subsurface_scattering) {
      draft_slider(tr("labpbr_sss"), session.labpbr_draft.subsurface);
    } else {
      draft_slider(tr("labpbr_porosity"), session.labpbr_draft.porosity);
    }
  }

  char encoded[192];
  const auto &draft = session.labpbr_draft;
  const unsigned encoded_metal =
      draft.metal ? draft.metal_code : draft.dielectric_f0;
  std::snprintf(encoded, sizeof(encoded), tr("labpbr_encoded_argb"),
                static_cast<unsigned>(
                    gfx::encodeLabPbrEmission(draft.emission)),
                static_cast<unsigned>(
                    gfx::encodeLabPbrRoughness(draft.roughness)),
                encoded_metal,
                static_cast<unsigned>(
                    draft.subsurface_scattering
                        ? gfx::encodeLabPbrSubsurface(draft.subsurface)
                        : gfx::encodeLabPbrPorosity(draft.porosity)));
  muted(ctx, g, encoded);
  muted(ctx, g,
        session.labpbr_draft_dirty ? tr("labpbr_draft_unapplied")
                                   : tr("labpbr_draft_applied"));

  nk_layout_row_dynamic(ctx, g.btn, 3);
  if (nk_button_label(ctx, tr("labpbr_apply")) && !no_group) {
    session.applySelectedLabPbrDraft();
  }
  if (nk_button_label(ctx, tr("labpbr_revert")) && !no_group) {
    session.revertSelectedLabPbrDraft();
  }
  if (nk_button_label(ctx, tr("labpbr_restore_texture")) && !no_group) {
    session.restoreSelectedLabPbrFromTexture();
  }
  if (no_group) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("labpbr_uv_conflicts"));
  if (session.labpbr_composition.conflicts.empty()) {
    muted(ctx, g, tr("labpbr_no_conflicts"));
  } else {
    mutedWrap(ctx, g, tr("labpbr_conflicts_block_export"));
    std::map<std::string, std::size_t> conflict_counts;
    for (const auto &conflict : session.labpbr_composition.conflicts) {
      std::string groups;
      for (const auto &group : conflict.groups) {
        if (!groups.empty()) {
          groups += " / ";
        }
        groups += group;
      }
      const std::string key =
          std::string(labPbrChannelLabel(conflict.channel)) + " · " + groups;
      ++conflict_counts[key];
    }
    for (const auto &[description, count] : conflict_counts) {
      char line[320];
      std::snprintf(line, sizeof(line), tr("labpbr_conflict_line"),
                    description.c_str(), count);
      mutedWrap(ctx, g, line);
    }
  }

  heading(ctx, g, tr("labpbr_iris_normal"));
  if (session.labpbr_imported_normal.valid()) {
    const std::string file_line =
        std::string(tr("labpbr_normal_file")) +
        session.labpbr_imported_normal.source_path.filename().string();
    muted(ctx, g, file_line.c_str());
    const std::string checksum_line =
        std::string(tr("labpbr_normal_checksum")) +
        session.labpbr_imported_normal.sha256;
    mutedWrap(ctx, g, checksum_line.c_str());
  } else {
    mutedWrap(ctx, g, tr("labpbr_normal_none"));
  }
  nk_layout_row_dynamic(ctx, g.btn, 2);
  if (nk_button_label(ctx, tr("labpbr_import_normal"))) {
    if (const auto path = openFileDialog(
            L"Import Normal Map (Iris RGBA PNG)",
            L"PNG Image (*.png)\0*.png\0All Files (*.*)\0*.*\0")) {
      session.importLabPbrNormal(*path);
    }
  }
  const bool can_remove_normal = session.labpbr_imported_normal.valid();
  if (!can_remove_normal) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("labpbr_remove_normal")) &&
      can_remove_normal) {
    session.removeLabPbrNormal();
  }
  if (!can_remove_normal) {
    nk_widget_disable_end(ctx);
  }

  heading(ctx, g, tr("labpbr_export"));
  mutedWrap(ctx, g, tr("labpbr_export_hint"));
  const bool can_export = session.labpbr_composition.exportable();
  nk_layout_row_dynamic(ctx, g.btn + 4.0f, 1);
  if (!can_export) {
    nk_widget_disable_begin(ctx);
  }
  if (nk_button_label(ctx, tr("labpbr_export_button")) && can_export) {
    std::wstring default_name = L"material_s.png";
    if (!session.texture_path.empty()) {
      default_name =
          std::filesystem::path(session.texture_path).stem().wstring() +
          L"_s.png";
    }
    if (const auto path =
            savePngFileDialog(L"Export LabPBR Texture Bundle",
                              default_name.c_str())) {
      session.requestLabPbrExport(*path);
    }
  }
  if (!can_export) {
    nk_widget_disable_end(ctx);
  }
}



void drawLabPbrPanel(UiPanelContext &ui) {
  drawLabPbrEditor(ui.nk, ui.geom, ui.session);
}

} // namespace xpbd::app::ui_internal
