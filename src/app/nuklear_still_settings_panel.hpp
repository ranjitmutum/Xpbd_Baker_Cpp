#pragma once

#include "nuklear.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace xpbd::app {

struct StillRenderSettings;

struct StillSettingsPanelLabels {
  const char *filename = "File name";
  const char *width = "Width";
  const char *height = "Height";
  const char *target_samples = "Target samples";
  const char *samples_per_submit = "Samples per submit";
  const char *format = "Format";
  const char *format_png = "PNG (display)";
  const char *format_exr = "EXR (linear HDR)";
  const char *transparent_background = "Transparent background";
};

struct StillSettingsPanelGeometry {
  float scale = 1.0f;
  float button_height = 26.0f;
  float row_height = 22.0f;
};

enum class StillSettingsIntegerControl : std::size_t {
  Width = 0,
  Height,
  TargetSamples,
  SamplesPerSubmit,
  Count,
};

struct StillSettingsPanelLayout {
  struct nk_rect filename_label{};
  struct nk_rect filename_edit{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_labels{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_properties{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_edits{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_drag_regions{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_decrement_buttons{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_increment_buttons{};
  std::array<struct nk_rect,
             static_cast<std::size_t>(StillSettingsIntegerControl::Count)>
      integer_sliders{};
  struct nk_rect format_label{};
  struct nk_rect format_combo{};
  struct nk_rect transparent_background{};
};

enum StillSettingsPanelChange : std::uint32_t {
  StillSettingsChangedNone = 0u,
  StillSettingsChangedFilename = 1u << 0u,
  StillSettingsChangedWidth = 1u << 1u,
  StillSettingsChangedHeight = 1u << 2u,
  StillSettingsChangedTargetSamples = 1u << 3u,
  StillSettingsChangedSamplesPerSubmit = 1u << 4u,
  StillSettingsChangedFormat = 1u << 5u,
  StillSettingsChangedTransparentBackground = 1u << 6u,
};

struct StillSettingsPanelResult {
  nk_flags filename_edit_flags = 0u;
  std::uint32_t changed = StillSettingsChangedNone;
  StillSettingsPanelLayout layout{};
};

// Composes the exact interactive Still-render settings block used by the
// application. The returned bounds are observational only and allow the
// headless regression harness to drive the real widgets without a duplicate
// test-only UI.
StillSettingsPanelResult composeStillRenderSettingsPanel(
    nk_context *ctx, StillRenderSettings &settings,
    const StillSettingsPanelLabels &labels,
    const StillSettingsPanelGeometry &geometry, bool controls_disabled);

} // namespace xpbd::app
