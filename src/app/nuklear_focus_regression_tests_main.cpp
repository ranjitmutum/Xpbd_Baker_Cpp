// Headless interaction regression for the application's real Still settings
// Nuklear composition. This intentionally drives the shared product helper;
// there is no test-only facsimile of the settings panel.

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_IMPLEMENTATION
#include "nuklear_still_settings_panel.hpp"

#include "xpbd/app/app_session.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using xpbd::app::StillRenderSettings;
using xpbd::app::StillSettingsIntegerControl;
using xpbd::app::StillSettingsPanelGeometry;
using xpbd::app::StillSettingsPanelLabels;
using xpbd::app::StillSettingsPanelResult;

int g_failures = 0;

void expect(bool condition, std::string_view label) {
  if (condition) {
    std::printf("ok: %.*s\n", static_cast<int>(label.size()), label.data());
    return;
  }
  std::fprintf(stderr, "FAIL: %.*s\n", static_cast<int>(label.size()),
               label.data());
  ++g_failures;
}

std::string caseLabel(std::string_view language, float scale,
                      std::string_view assertion) {
  char prefix[80]{};
  std::snprintf(prefix, sizeof(prefix), "%.*s @ %.1fx: ",
                static_cast<int>(language.size()), language.data(),
                static_cast<double>(scale));
  return std::string(prefix) + std::string(assertion);
}

float fixedTextWidth(nk_handle, float height, const char *text, int length) {
  if (text == nullptr || length <= 0) {
    return 0.0f;
  }
  int glyphs = 0;
  for (int index = 0; index < length; ++index) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if ((byte & 0xc0u) != 0x80u) {
      ++glyphs;
    }
  }
  return static_cast<float>(glyphs) * height * 0.5f;
}

struct KeyEvent {
  enum nk_keys key = NK_KEY_NONE;
  bool down = false;
};

struct InputFrame {
  bool move_mouse = false;
  int mouse_x = 0;
  int mouse_y = 0;
  int mouse_down = -1;
  std::vector<nk_rune> text;
  std::vector<KeyEvent> keys;
  bool unfocus_after_compose = false;
};

struct UiFrame {
  StillSettingsPanelResult panel{};
  nk_window *window = nullptr;
};

constexpr std::size_t controlIndex(StillSettingsIntegerControl control) {
  return static_cast<std::size_t>(control);
}

constexpr std::array<StillSettingsIntegerControl, 4> kIntegerControls{{
    StillSettingsIntegerControl::Width,
    StillSettingsIntegerControl::Height,
    StillSettingsIntegerControl::TargetSamples,
    StillSettingsIntegerControl::SamplesPerSubmit,
}};

constexpr StillSettingsPanelLabels kEnglishLabels{
    "File name",
    "Width",
    "Height",
    "Target samples",
    "Samples per submit",
    "Format",
    "PNG (display)",
    "EXR (linear HDR)",
    "Transparent background",
};

constexpr StillSettingsPanelLabels kChineseLabels{
    "文件名",
    "宽度",
    "高度",
    "目标采样数",
    "每次提交采样数",
    "格式",
    "PNG（显示结果）",
    "EXR（线性 HDR）",
    "透明背景",
};

struct Harness {
  nk_context context{};
  nk_user_font font{};
  StillRenderSettings settings{};
  StillSettingsPanelLabels labels{};
  StillSettingsPanelGeometry geometry{};
  float scale = 1.0f;
  bool has_previous_frame = false;

  Harness(float requested_scale, StillSettingsPanelLabels requested_labels)
      : labels(requested_labels), scale(requested_scale) {
    font.height = 14.0f * scale;
    font.width = fixedTextWidth;
    const nk_bool initialized = nk_init_default(&context, &font);
    expect(initialized == nk_true, "Nuklear test context initializes");
    geometry.scale = scale;
    geometry.button_height = 26.0f * scale;
    geometry.row_height = 22.0f * scale;
    context.style.window.spacing = nk_vec2(4.0f * scale, 4.0f * scale);
    context.style.window.padding = nk_vec2(8.0f * scale, 8.0f * scale);
    context.style.window.combo_padding = nk_vec2(8.0f * scale, 8.0f * scale);
    context.style.property.padding = nk_vec2(4.0f * scale, 2.0f * scale);
  }

  ~Harness() { nk_free(&context); }

  UiFrame frame(const InputFrame &input = {}, bool controls_disabled = false) {
    if (has_previous_frame) {
      nk_clear(&context);
    }
    has_previous_frame = true;

    nk_input_begin(&context);
    if (input.move_mouse) {
      nk_input_motion(&context, input.mouse_x, input.mouse_y);
    }
    if (input.mouse_down >= 0) {
      nk_input_button(&context, NK_BUTTON_LEFT, input.mouse_x, input.mouse_y,
                      input.mouse_down != 0 ? nk_true : nk_false);
    }
    for (const KeyEvent &event : input.keys) {
      nk_input_key(&context, event.key, event.down ? nk_true : nk_false);
    }
    for (const nk_rune rune : input.text) {
      nk_input_unicode(&context, rune);
    }
    nk_input_end(&context);

    UiFrame result;
    const float window_width = 720.0f * scale;
    const float window_height = 420.0f * scale;
    if (nk_begin(&context, "still-settings-focus-regression",
                 nk_rect(0.0f, 0.0f, window_width, window_height),
                 NK_WINDOW_NO_SCROLLBAR)) {
      result.panel = xpbd::app::composeStillRenderSettingsPanel(
          &context, settings, labels, geometry, controls_disabled);
      if (input.unfocus_after_compose) {
        // SDL Escape is an application-level event. This is the exact Nuklear
        // cancellation primitive that an Escape handler must invoke.
        nk_edit_unfocus(&context);
      }
      result.window = context.current;
    }
    nk_end(&context);
    return result;
  }
};

InputFrame mouseAt(const struct nk_rect &bounds, bool down,
                   float x_fraction = 0.5f, float y_fraction = 0.5f) {
  InputFrame input;
  input.move_mouse = true;
  input.mouse_x = static_cast<int>(bounds.x + bounds.w * x_fraction);
  input.mouse_y = static_cast<int>(bounds.y + bounds.h * y_fraction);
  input.mouse_down = down ? 1 : 0;
  return input;
}

UiFrame click(Harness &harness, const struct nk_rect &bounds,
              float x_fraction = 0.5f, float y_fraction = 0.5f) {
  harness.frame(mouseAt(bounds, true, x_fraction, y_fraction));
  return harness.frame(mouseAt(bounds, false, x_fraction, y_fraction));
}

UiFrame key(Harness &harness, enum nk_keys key_code) {
  InputFrame press;
  press.keys.push_back({key_code, true});
  const UiFrame pressed = harness.frame(press);
  InputFrame release;
  release.keys.push_back({key_code, false});
  harness.frame(release);
  return pressed;
}

UiFrame text(Harness &harness, std::initializer_list<nk_rune> runes) {
  InputFrame input;
  input.text.assign(runes.begin(), runes.end());
  return harness.frame(input);
}

std::uint32_t integerValue(const StillRenderSettings &settings,
                           StillSettingsIntegerControl control) {
  switch (control) {
  case StillSettingsIntegerControl::Width:
    return settings.width;
  case StillSettingsIntegerControl::Height:
    return settings.height;
  case StillSettingsIntegerControl::TargetSamples:
    return settings.target_samples;
  case StillSettingsIntegerControl::SamplesPerSubmit:
    return settings.samples_per_submit;
  case StillSettingsIntegerControl::Count:
    break;
  }
  return 0u;
}

bool validRect(const struct nk_rect &bounds) {
  return std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
         std::isfinite(bounds.w) && std::isfinite(bounds.h) &&
         bounds.w > 0.0f && bounds.h > 0.0f;
}

bool orderedBelow(const struct nk_rect &upper, const struct nk_rect &lower) {
  return lower.y + 0.01f >= upper.y + upper.h;
}

void testScaledBilingualLayoutAndFocusMatrix() {
  const std::array<std::pair<std::string_view, StillSettingsPanelLabels>, 2>
      languages{{{"en-US", kEnglishLabels}, {"zh-CN", kChineseLabels}}};
  const std::array<float, 3> scales{{1.0f, 1.5f, 2.0f}};
  constexpr std::array<std::uint32_t, 4> committed_values{{
      640u,
      720u,
      256u,
      16u,
  }};

  for (const auto &[language, labels] : languages) {
    for (const float scale : scales) {
      Harness harness(scale, labels);
      UiFrame frame = harness.frame();
      const auto &layout = frame.panel.layout;
      expect(validRect(layout.filename_edit),
             caseLabel(language, scale, "filename edit has a real hit box"));
      bool integer_layout_valid = true;
      bool vertical_order_valid = true;
      struct nk_rect previous = layout.filename_edit;
      for (const StillSettingsIntegerControl control : kIntegerControls) {
        const std::size_t index = controlIndex(control);
        integer_layout_valid &= validRect(layout.integer_properties[index]) &&
                                validRect(layout.integer_edits[index]) &&
                                validRect(layout.integer_sliders[index]);
        vertical_order_valid &=
            orderedBelow(previous, layout.integer_properties[index]);
        previous = layout.integer_sliders[index].y >
                           layout.integer_properties[index].y
                       ? layout.integer_sliders[index]
                       : layout.integer_properties[index];
      }
      expect(integer_layout_valid,
             caseLabel(language, scale,
                       "all four real numeric controls remain clickable"));
      expect(vertical_order_valid,
             caseLabel(language, scale,
                       "numeric rows never overlap the filename field"));
      expect(validRect(layout.format_combo) &&
                 validRect(layout.transparent_background) &&
                 orderedBelow(previous, layout.format_combo) &&
                 orderedBelow(layout.format_combo,
                              layout.transparent_background),
             caseLabel(language, scale,
                       "format and transparency rows remain ordered"));

      const std::string original_filename = harness.settings.filename;
      for (std::size_t index = 0; index < kIntegerControls.size(); ++index) {
        const StillSettingsIntegerControl control = kIntegerControls[index];
        frame = harness.frame();
        const struct nk_rect edit_bounds =
            frame.panel.layout.integer_edits[index];
        const UiFrame activated = click(harness, edit_bounds);
        expect(activated.window != nullptr &&
                   activated.window->property.active &&
                   activated.window->edit.active &&
                   activated.window->edit.name == NK_PROPERTY_EDIT_IDENTITY,
               caseLabel(language, scale,
                         "numeric click owns the reserved property focus"));

        // CJK input models an IME commit reaching a numeric field. Decimal
        // filtering must reject it without forwarding it to the filename.
        text(harness, {0x9759u}); // 静
        expect(harness.settings.filename == original_filename,
               caseLabel(language, scale,
                         "IME-like Unicode cannot leak into filename"));

        const std::string digits = std::to_string(committed_values[index]);
        InputFrame type_digits;
        for (const char digit : digits) {
          type_digits.text.push_back(static_cast<nk_rune>(digit));
        }
        const UiFrame typed = harness.frame(type_digits);
        expect(harness.settings.filename == original_filename &&
                   (typed.panel.filename_edit_flags & NK_EDIT_ACTIVE) == 0u,
               caseLabel(language, scale,
                         "numeric keyboard text stays out of filename"));

        key(harness, NK_KEY_TAB);
        key(harness, NK_KEY_LEFT);
        key(harness, NK_KEY_RIGHT);
        expect(harness.settings.filename == original_filename,
               caseLabel(language, scale,
                         "Tab and keyboard arrows preserve focus isolation"));

        const UiFrame committed = key(harness, NK_KEY_ENTER);
        (void)committed;
        frame = harness.frame();
        expect(integerValue(harness.settings, control) ==
                   committed_values[index],
               caseLabel(language, scale,
                         "Enter commits the intended numeric control"));
        expect(frame.window != nullptr && !frame.window->property.active &&
                   !frame.window->edit.active && frame.window->edit.name == 0u,
               caseLabel(language, scale,
                         "numeric completion clears property ownership"));
      }
    }
  }
}

void testFilenameUnicodeTabArrowAndEscapeStyleUnfocus() {
  Harness harness(1.5f, kChineseLabels);
  UiFrame frame = harness.frame();
  click(harness, frame.panel.layout.filename_edit);
  text(harness, {0x9759u, 0x5e27u, 0x6d4bu, 0x8bd5u}); // 静帧测试
  key(harness, NK_KEY_TAB);
  key(harness, NK_KEY_LEFT);
  key(harness, NK_KEY_RIGHT);
  text(harness, {'X'});
  const std::string edited_filename = harness.settings.filename;
  expect(edited_filename.find("静帧测试") != std::string::npos,
         "filename accepts IME-like UTF-8 input through the real panel");
  expect(edited_filename.find('X') != std::string::npos,
         "Tab and arrow input do not redirect filename text");
  expect(harness.settings.width == 1920u && harness.settings.height == 1080u &&
             harness.settings.target_samples == 1024u &&
             harness.settings.samples_per_submit == 8u,
         "filename keyboard editing cannot mutate numeric settings");

  InputFrame cancel;
  cancel.unfocus_after_compose = true;
  const UiFrame cancelled = harness.frame(cancel);
  expect(cancelled.window != nullptr && !cancelled.window->edit.active &&
             cancelled.window->edit.name == 0u,
         "Escape-style explicit Nuklear unfocus clears filename ownership");
  text(harness, {0x9003u}); // 逃
  expect(harness.settings.filename == edited_filename,
         "text after Escape-style unfocus is not accepted by filename");
}

void testPointerArrowsAndPropertyDragStayIsolated() {
  Harness harness(1.5f, kChineseLabels);
  UiFrame frame = harness.frame();
  click(harness, frame.panel.layout.filename_edit);
  text(harness, {'x'});
  const std::string filename = harness.settings.filename;

  for (std::size_t index = 0; index < kIntegerControls.size(); ++index) {
    frame = harness.frame();
    const std::uint32_t before =
        integerValue(harness.settings, kIntegerControls[index]);
    click(harness, frame.panel.layout.integer_increment_buttons[index]);
    expect(integerValue(harness.settings, kIntegerControls[index]) ==
               before + 1u,
           "numeric increment arrow changes only its intended value");
    expect(harness.settings.filename == filename,
           "numeric increment arrow cannot write into filename");

    frame = harness.frame();
    click(harness, frame.panel.layout.integer_decrement_buttons[index]);
    expect(integerValue(harness.settings, kIntegerControls[index]) == before,
           "numeric decrement arrow restores only its intended value");
    expect(harness.settings.filename == filename,
           "numeric decrement arrow cannot write into filename");
  }

  frame = harness.frame();
  const struct nk_rect drag =
      frame.panel.layout
          .integer_drag_regions[controlIndex(StillSettingsIntegerControl::Width)];
  const std::uint32_t width_before = harness.settings.width;
  InputFrame press = mouseAt(drag, true, 0.5f, 0.5f);
  harness.frame(press);
  InputFrame move = press;
  move.mouse_x += static_cast<int>(24.0f * harness.scale);
  harness.frame(move);
  InputFrame release = move;
  release.mouse_down = 0;
  harness.frame(release);
  expect(harness.settings.width > width_before,
         "property drag changes the intended numeric value");
  expect(harness.settings.filename == filename,
         "property drag cannot mutate the filename edit");
}

void testFormatAndTransparentControlsTransferFocus() {
  Harness harness(1.0f, kEnglishLabels);
  UiFrame frame = harness.frame();
  click(harness, frame.panel.layout.filename_edit);
  text(harness, {'q'});
  const std::string filename = harness.settings.filename;

  frame = harness.frame();
  click(harness, frame.panel.layout.format_combo);
  frame = harness.frame();
  const struct nk_rect combo = frame.panel.layout.format_combo;
  const float popup_y = combo.y + combo.h -
                        harness.context.style.window.combo_border;
  const float item_height = harness.geometry.button_height;
  const float item_spacing = harness.context.style.window.spacing.y;
  const float padding_y = harness.context.style.window.combo_padding.y;
  struct nk_rect second_item{
      combo.x + harness.context.style.window.combo_padding.x,
      popup_y + padding_y + item_height + item_spacing,
      120.0f * harness.scale,
      item_height,
  };
  click(harness, second_item);
  expect(harness.settings.format == xpbd::gfx::StillImageFormat::Exr,
         "format combo selects EXR through the real popup");
  expect(harness.settings.filename == filename,
         "format popup interaction cannot mutate filename");

  frame = harness.frame();
  click(harness, frame.panel.layout.transparent_background, 0.08f, 0.5f);
  expect(harness.settings.transparent_background,
         "transparent-background checkbox toggles its setting");
  expect(harness.settings.filename == filename,
         "checkbox interaction cannot mutate filename");
}

void testDisabledPanelIsInert() {
  Harness harness(2.0f, kChineseLabels);
  const StillRenderSettings baseline = harness.settings;
  UiFrame frame = harness.frame({}, true);
  InputFrame filename_press =
      mouseAt(frame.panel.layout.filename_edit, true);
  harness.frame(filename_press, true);
  InputFrame filename_release =
      mouseAt(frame.panel.layout.filename_edit, false);
  harness.frame(filename_release, true);
  InputFrame disabled_text;
  disabled_text.text = {0x7981u, '9'}; // 禁9
  harness.frame(disabled_text, true);
  frame = harness.frame({}, true);
  const struct nk_rect increment =
      frame.panel.layout.integer_increment_buttons[controlIndex(
          StillSettingsIntegerControl::TargetSamples)];
  harness.frame(mouseAt(increment, true), true);
  harness.frame(mouseAt(increment, false), true);
  frame = harness.frame({}, true);
  const struct nk_rect transparent =
      frame.panel.layout.transparent_background;
  harness.frame(mouseAt(transparent, true, 0.08f, 0.5f), true);
  harness.frame(mouseAt(transparent, false, 0.08f, 0.5f), true);
  expect(harness.settings.filename == baseline.filename &&
             harness.settings.width == baseline.width &&
             harness.settings.height == baseline.height &&
             harness.settings.target_samples == baseline.target_samples &&
             harness.settings.samples_per_submit ==
                 baseline.samples_per_submit &&
             harness.settings.format == baseline.format &&
             harness.settings.transparent_background ==
                 baseline.transparent_background,
         "disabled Still settings panel is fully inert");
}

} // namespace

int main() {
  testScaledBilingualLayoutAndFocusMatrix();
  testFilenameUnicodeTabArrowAndEscapeStyleUnfocus();
  testPointerArrowsAndPropertyDragStayIsolated();
  testFormatAndTransparentControlsTransferFocus();
  testDisabledPanelIsInert();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d Nuklear focus regression assertion(s) failed\n",
                 g_failures);
    return 1;
  }
  std::printf("all Nuklear Still settings focus regressions passed\n");
  return 0;
}
