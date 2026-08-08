#pragma once

#include <string>
#include <vector>

struct SDL_Window;

namespace xpbd::gfx {

enum class VulkanNativeSurfaceKind {
  Unsupported = 0,
  Win32,
  SdlMainThreadOnly,
};

// Captured exclusively on the SDL/Main thread. All strings and scalar window
// state are owned; the SDL pointer is retained only for the non-threaded,
// non-Win32 compatibility path and is rejected by the dedicated RenderThread.
struct VulkanWindowBootstrap {
  std::vector<std::string> required_instance_extensions;
  VulkanNativeSurfaceKind surface_kind =
      VulkanNativeSurfaceKind::Unsupported;
  void *native_window_handle = nullptr;
  SDL_Window *legacy_sdl_window = nullptr;
  int pixel_width = 0;
  int pixel_height = 0;
  bool presentation_available = false;

  [[nodiscard]] bool valid() const noexcept {
    if (required_instance_extensions.empty()) {
      return false;
    }
    switch (surface_kind) {
    case VulkanNativeSurfaceKind::Win32:
      return native_window_handle != nullptr;
    case VulkanNativeSurfaceKind::SdlMainThreadOnly:
      return legacy_sdl_window != nullptr;
    case VulkanNativeSurfaceKind::Unsupported:
      break;
    }
    return false;
  }

  [[nodiscard]] bool renderThreadCompatible() const noexcept {
    return valid() && surface_kind == VulkanNativeSurfaceKind::Win32 &&
           native_window_handle != nullptr;
  }
};

// SDL 3.4 marks window properties/flags/pixel-size queries main-thread-only.
// Call this before dispatching Vulkan initialization to another thread.
[[nodiscard]] bool captureVulkanWindowBootstrap(
    SDL_Window *window, VulkanWindowBootstrap &out,
    std::string *error = nullptr);

} // namespace xpbd::gfx
