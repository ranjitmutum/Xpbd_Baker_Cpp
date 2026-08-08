#include "xpbd/gfx/vulkan_window_bootstrap.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdint>
#include <utility>

namespace xpbd::gfx {

bool captureVulkanWindowBootstrap(SDL_Window *window,
                                  VulkanWindowBootstrap &out,
                                  std::string *error) {
  VulkanWindowBootstrap candidate;
  if (window == nullptr) {
    if (error != nullptr) {
      *error = "Cannot capture a Vulkan bootstrap from a null SDL window";
    }
    return false;
  }

  std::uint32_t extension_count = 0u;
  const char *const *extensions =
      SDL_Vulkan_GetInstanceExtensions(&extension_count);
  if (extensions == nullptr || extension_count == 0u) {
    if (error != nullptr) {
      const char *sdl_error = SDL_GetError();
      *error = sdl_error != nullptr && sdl_error[0] != '\0'
                   ? sdl_error
                   : "SDL returned no Vulkan instance extensions";
    }
    return false;
  }
  candidate.required_instance_extensions.reserve(extension_count);
  for (std::uint32_t index = 0u; index < extension_count; ++index) {
    if (extensions[index] == nullptr || extensions[index][0] == '\0') {
      if (error != nullptr) {
        *error = "SDL returned an invalid Vulkan instance extension";
      }
      return false;
    }
    candidate.required_instance_extensions.emplace_back(extensions[index]);
  }

  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  int width = 0;
  int height = 0;
  if (!SDL_GetWindowSizeInPixels(window, &width, &height)) {
    width = 0;
    height = 0;
  }
  candidate.pixel_width = width;
  candidate.pixel_height = height;
  candidate.presentation_available =
      width > 0 && height > 0 &&
      (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) == 0u;

#if defined(_WIN32)
  candidate.surface_kind = VulkanNativeSurfaceKind::Win32;
  candidate.native_window_handle = SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
      nullptr);
  if (candidate.native_window_handle == nullptr) {
    if (error != nullptr) {
      *error = "SDL did not expose the Win32 HWND for Vulkan surface creation";
    }
    return false;
  }
#else
  // The current V5 formal target is Win32/NVIDIA. Preserve synchronous Vulkan
  // compatibility elsewhere without claiming the SDL pointer is thread-safe.
  candidate.surface_kind = VulkanNativeSurfaceKind::SdlMainThreadOnly;
  candidate.legacy_sdl_window = window;
#endif

  if (!candidate.valid()) {
    if (error != nullptr) {
      *error = "Captured Vulkan window bootstrap is incomplete";
    }
    return false;
  }
  out = std::move(candidate);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace xpbd::gfx
