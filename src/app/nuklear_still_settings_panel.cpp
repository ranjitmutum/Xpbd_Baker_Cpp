#include "nuklear_still_settings_panel.hpp"

#include "xpbd/app/app_session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace xpbd::app {
namespace {

constexpr std::size_t integerIndex(StillSettingsIntegerControl control) {
  return static_cast<std::size_t>(control);
}

void unfocusTextEditOnPointerWidget(nk_context *ctx) {
  if (ctx == nullptr) {
    return;
  }
  const struct nk_rect bounds = nk_widget_bounds(ctx);
  if (nk_input_is_mouse_click_down_in_rect(
          &ctx->input, NK_BUTTON_LEFT, bounds, nk_true)) {
    nk_edit_unfocus(ctx);
  }
}

void describePropertyHitRegions(
    nk_context *ctx, const char *label, int value,
    const struct nk_rect &property_bounds, struct nk_rect &edit_bounds,
    struct nk_rect &drag_bounds, struct nk_rect &decrement_bounds,
    struct nk_rect &increment_bounds) {
  const auto &style = ctx->style.property;
  const auto *font = ctx->style.font;
  decrement_bounds.h = font->height;
  decrement_bounds.w = decrement_bounds.h;
  decrement_bounds.x = property_bounds.x + style.border + style.padding.x;
  decrement_bounds.y = property_bounds.y + style.border +
                       property_bounds.h * 0.5f - decrement_bounds.h * 0.5f;

  increment_bounds.y = decrement_bounds.y;
  increment_bounds.w = decrement_bounds.w;
  increment_bounds.h = decrement_bounds.h;
  increment_bounds.x = property_bounds.x + property_bounds.w -
                       (increment_bounds.w + style.padding.x);

  const int label_length = label != nullptr ? nk_strlen(label) : 0;
  struct nk_rect inner_label{};
  inner_label.x = decrement_bounds.x + decrement_bounds.w + style.padding.x;
  inner_label.w = font->width(font->userdata, font->height,
                              label != nullptr ? label : "", label_length) +
                  2.0f * style.padding.x;

  char value_text[NK_MAX_NUMBER_BUFFER]{};
  const int value_length =
      std::snprintf(value_text, sizeof(value_text), "%d", value);
  float edit_width =
      font->width(font->userdata, font->height, value_text,
                  (std::max)(0, value_length)) +
      style.edit.cursor_size + 2.0f * style.padding.x;
  edit_width =
      (std::min)(edit_width,
                 increment_bounds.x - (inner_label.x + inner_label.w));
  edit_bounds.w = (std::max)(0.0f, edit_width);
  edit_bounds.x = increment_bounds.x - (edit_bounds.w + style.padding.x);
  edit_bounds.y = property_bounds.y + style.border;
  edit_bounds.h = property_bounds.h - 2.0f * style.border;

  drag_bounds.x = inner_label.x;
  drag_bounds.y = property_bounds.y;
  drag_bounds.w = (std::max)(0.0f, edit_bounds.x - drag_bounds.x);
  drag_bounds.h = property_bounds.h;
}

bool intProperty(nk_context *ctx, const StillSettingsPanelGeometry &geometry,
                 const char *label, int &value, int lo, int hi, int step,
                 struct nk_rect &label_bounds,
                 struct nk_rect &property_bounds,
                 struct nk_rect &edit_bounds, struct nk_rect &drag_bounds,
                 struct nk_rect &decrement_bounds,
                 struct nk_rect &increment_bounds,
                 struct nk_rect &slider_bounds) {
  int next_value = value;
  const std::string property_id =
      std::string("#") + (label != nullptr ? label : "value");
  const float content_width = nk_window_get_content_region_size(ctx).x;
  const auto &property_style = ctx->style.property;
  const auto *font = ctx->style.font;
  const int label_length = label != nullptr ? nk_strlen(label) : 0;
  const float internal_label_width =
      font->width(font->userdata, font->height,
                  label != nullptr ? label : "", label_length);
  const std::string lo_text = std::to_string(lo);
  const std::string hi_text = std::to_string(hi);
  const float maximum_value_width =
      (std::max)(font->width(font->userdata, font->height, lo_text.c_str(),
                            static_cast<int>(lo_text.size())),
                 font->width(font->userdata, font->height, hi_text.c_str(),
                            static_cast<int>(hi_text.size())));
  // Nuklear draws the property identity as a second label inside the numeric
  // box. Account for that text, both arrow buttons, and a usable edit field;
  // otherwise long English labels collapse the edit rectangle to zero width.
  const float minimum_content_width =
      internal_label_width + maximum_value_width +
      2.0f * font->height + property_style.edit.cursor_size +
      property_style.border + 7.0f * property_style.padding.x;
  const float property_width =
      (std::max)({104.0f, 112.0f * geometry.scale, minimum_content_width});
  const float slider_width =
      (std::max)(72.0f, 90.0f * geometry.scale);
  const float spacing = ctx->style.window.spacing.x * 2.0f;
  const bool wide = content_width >=
                    property_width + slider_width + spacing + 72.0f;
  int changed = 0;
  if (wide) {
    const float label_width =
        (std::max)(72.0f, content_width - property_width - slider_width -
                               spacing);
    nk_layout_row_begin(ctx, NK_STATIC, geometry.button_height, 3);
    nk_layout_row_push(ctx, label_width);
    label_bounds = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, property_width);
    property_bounds = nk_widget_bounds(ctx);
    describePropertyHitRegions(ctx, label, next_value, property_bounds,
                               edit_bounds, drag_bounds, decrement_bounds,
                               increment_bounds);
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_property_int(ctx, property_id.c_str(), lo, &next_value, hi,
                               step, static_cast<float>(step));
    nk_layout_row_push(ctx, slider_width);
    slider_bounds = nk_widget_bounds(ctx);
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_slider_int(ctx, lo, &next_value, hi, step);
    nk_layout_row_end(ctx);
  } else {
    nk_layout_row_begin(ctx, NK_DYNAMIC, geometry.button_height, 2);
    nk_layout_row_push(ctx, 0.35f);
    label_bounds = nk_widget_bounds(ctx);
    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_layout_row_push(ctx, 0.65f);
    property_bounds = nk_widget_bounds(ctx);
    describePropertyHitRegions(ctx, label, next_value, property_bounds,
                               edit_bounds, drag_bounds, decrement_bounds,
                               increment_bounds);
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_property_int(ctx, property_id.c_str(), lo, &next_value, hi,
                               step, static_cast<float>(step));
    nk_layout_row_end(ctx);
    nk_layout_row_dynamic(ctx, geometry.row_height, 1);
    slider_bounds = nk_widget_bounds(ctx);
    unfocusTextEditOnPointerWidget(ctx);
    changed |= nk_slider_int(ctx, lo, &next_value, hi, step);
  }
  if (changed != 0) {
    value = next_value;
    return true;
  }
  return false;
}

bool combo(nk_context *ctx, const StillSettingsPanelGeometry &geometry,
           const char *label, const std::vector<const char *> &items,
           int &selected, struct nk_rect &label_bounds,
           struct nk_rect &combo_bounds) {
  if (items.empty()) {
    return false;
  }
  selected = std::clamp(selected, 0, static_cast<int>(items.size()) - 1);
  nk_layout_row_begin(ctx, NK_DYNAMIC, geometry.button_height, 2);
  nk_layout_row_push(ctx, 0.43f);
  label_bounds = nk_widget_bounds(ctx);
  nk_label(ctx, label, NK_TEXT_LEFT);
  nk_layout_row_push(ctx, 0.57f);
  combo_bounds = nk_widget_bounds(ctx);
  unfocusTextEditOnPointerWidget(ctx);
  const int next = nk_combo(
      ctx, items.data(), static_cast<int>(items.size()), selected,
      static_cast<int>(geometry.button_height),
      nk_vec2(260.0f * geometry.scale,
              (std::min)(240.0f * geometry.scale,
                         geometry.button_height * items.size() + 12.0f)));
  nk_layout_row_end(ctx);
  if (next != selected) {
    selected = next;
    return true;
  }
  return false;
}

bool check(nk_context *ctx, const StillSettingsPanelGeometry &geometry,
           const char *label, bool &value, struct nk_rect &bounds) {
  nk_layout_row_dynamic(ctx, geometry.row_height, 1);
  bounds = nk_widget_bounds(ctx);
  nk_bool next_value = value ? nk_true : nk_false;
  unfocusTextEditOnPointerWidget(ctx);
  const int changed = nk_checkbox_label(ctx, label, &next_value);
  if (changed != 0) {
    value = next_value != 0;
    return true;
  }
  return false;
}

} // namespace

StillSettingsPanelResult composeStillRenderSettingsPanel(
    nk_context *ctx, StillRenderSettings &settings,
    const StillSettingsPanelLabels &labels,
    const StillSettingsPanelGeometry &geometry, bool controls_disabled) {
  StillSettingsPanelResult result;
  if (ctx == nullptr) {
    return result;
  }

  std::array<char, 128> filename_buffer{};
  const std::size_t filename_bytes =
      (std::min)(settings.filename.size(), filename_buffer.size() - 1u);
  std::copy_n(settings.filename.data(), filename_bytes, filename_buffer.data());
  int filename_length = static_cast<int>(filename_bytes);

  if (controls_disabled) {
    nk_widget_disable_begin(ctx);
  }

  nk_layout_row_dynamic(ctx, geometry.button_height, 2);
  result.layout.filename_label = nk_widget_bounds(ctx);
  nk_label(ctx, labels.filename, NK_TEXT_LEFT);
  result.layout.filename_edit = nk_widget_bounds(ctx);
  result.filename_edit_flags = nk_edit_string(
      ctx, NK_EDIT_FIELD | NK_EDIT_SIG_ENTER, filename_buffer.data(),
      &filename_length, static_cast<int>(filename_buffer.size() - 1u),
      nk_filter_default);
  if (!controls_disabled &&
      (result.filename_edit_flags & (NK_EDIT_ACTIVE | NK_EDIT_COMMITED))) {
    settings.filename.assign(
        filename_buffer.data(),
        static_cast<std::size_t>((std::max)(0, filename_length)));
    result.changed |= StillSettingsChangedFilename;
  }

  int width = static_cast<int>(settings.width);
  if (intProperty(
          ctx, geometry, labels.width, width, 64, 4'096, 1,
          result.layout.integer_labels[integerIndex(
              StillSettingsIntegerControl::Width)],
          result.layout.integer_properties[integerIndex(
              StillSettingsIntegerControl::Width)],
          result.layout.integer_edits[integerIndex(
              StillSettingsIntegerControl::Width)],
          result.layout.integer_drag_regions[integerIndex(
              StillSettingsIntegerControl::Width)],
          result.layout.integer_decrement_buttons[integerIndex(
              StillSettingsIntegerControl::Width)],
          result.layout.integer_increment_buttons[integerIndex(
              StillSettingsIntegerControl::Width)],
          result.layout.integer_sliders[integerIndex(
              StillSettingsIntegerControl::Width)])) {
    settings.width = static_cast<std::uint32_t>(width);
    result.changed |= StillSettingsChangedWidth;
  }

  int height = static_cast<int>(settings.height);
  if (intProperty(
          ctx, geometry, labels.height, height, 64, 4'096, 1,
          result.layout.integer_labels[integerIndex(
              StillSettingsIntegerControl::Height)],
          result.layout.integer_properties[integerIndex(
              StillSettingsIntegerControl::Height)],
          result.layout.integer_edits[integerIndex(
              StillSettingsIntegerControl::Height)],
          result.layout.integer_drag_regions[integerIndex(
              StillSettingsIntegerControl::Height)],
          result.layout.integer_decrement_buttons[integerIndex(
              StillSettingsIntegerControl::Height)],
          result.layout.integer_increment_buttons[integerIndex(
              StillSettingsIntegerControl::Height)],
          result.layout.integer_sliders[integerIndex(
              StillSettingsIntegerControl::Height)])) {
    settings.height = static_cast<std::uint32_t>(height);
    result.changed |= StillSettingsChangedHeight;
  }

  int target_samples = static_cast<int>(settings.target_samples);
  if (intProperty(
          ctx, geometry, labels.target_samples, target_samples, 32, 65'536, 1,
          result.layout.integer_labels[integerIndex(
              StillSettingsIntegerControl::TargetSamples)],
          result.layout.integer_properties[integerIndex(
              StillSettingsIntegerControl::TargetSamples)],
          result.layout.integer_edits[integerIndex(
              StillSettingsIntegerControl::TargetSamples)],
          result.layout.integer_drag_regions[integerIndex(
              StillSettingsIntegerControl::TargetSamples)],
          result.layout.integer_decrement_buttons[integerIndex(
              StillSettingsIntegerControl::TargetSamples)],
          result.layout.integer_increment_buttons[integerIndex(
              StillSettingsIntegerControl::TargetSamples)],
          result.layout.integer_sliders[integerIndex(
              StillSettingsIntegerControl::TargetSamples)])) {
    settings.target_samples = static_cast<std::uint32_t>(target_samples);
    result.changed |= StillSettingsChangedTargetSamples;
  }

  int samples_per_submit = static_cast<int>(settings.samples_per_submit);
  if (intProperty(
          ctx, geometry, labels.samples_per_submit, samples_per_submit, 1, 32,
          1,
          result.layout.integer_labels[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)],
          result.layout.integer_properties[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)],
          result.layout.integer_edits[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)],
          result.layout.integer_drag_regions[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)],
          result.layout.integer_decrement_buttons[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)],
          result.layout.integer_increment_buttons[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)],
          result.layout.integer_sliders[integerIndex(
              StillSettingsIntegerControl::SamplesPerSubmit)])) {
    settings.samples_per_submit =
        static_cast<std::uint32_t>(samples_per_submit);
    result.changed |= StillSettingsChangedSamplesPerSubmit;
  }

  std::vector<const char *> formats{labels.format_png, labels.format_exr};
  int format = static_cast<int>(settings.format);
  if (combo(ctx, geometry, labels.format, formats, format,
            result.layout.format_label, result.layout.format_combo)) {
    settings.format = static_cast<gfx::StillImageFormat>(format);
    result.changed |= StillSettingsChangedFormat;
  }

  if (check(ctx, geometry, labels.transparent_background,
            settings.transparent_background,
            result.layout.transparent_background)) {
    result.changed |= StillSettingsChangedTransparentBackground;
  }

  if (controls_disabled) {
    nk_widget_disable_end(ctx);
    result.changed = StillSettingsChangedNone;
  }
  return result;
}

} // namespace xpbd::app
