#include "xpbd/gfx/backend_select.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/vulkan_window_bootstrap.hpp"
#include "xpbd/log.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace xpbd::gfx {
namespace {

struct Attempt {
  BackendPreference pref;
  SDL_WindowFlags flags;
  bool gl_attrs;
};

bool environmentFlagEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr &&
         (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
          std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "yes") == 0 ||
          std::strcmp(value, "YES") == 0);
}

void prepareVulkanEnvironment() {
  if (environmentFlagEnabled("XPBD_VULKAN_ALLOW_THIRD_PARTY_LAYERS")) {
    xpbd::log::warn("Vulkan: third-party implicit layers explicitly allowed");
    return;
  }

  const bool gamepp_disabled =
      SDL_setenv_unsafe("DISABLE_GAMEPP_LAYER", "1", 1) == 0;
  const bool rtss_disabled =
      SDL_setenv_unsafe("DISABLE_RTSS_LAYER", "1", 1) == 0;
  if (gamepp_disabled && rtss_disabled) {
    xpbd::log::info(
        "Vulkan: isolated from GamePP and RTSS implicit overlay layers");
  } else {
    xpbd::log::warnf(
        "Vulkan: failed to isolate one or more implicit overlay layers: %s",
        SDL_GetError());
  }
}

std::unique_ptr<IGpuBackend> makeBackend(BackendPreference pref) {
  switch (pref) {
  case BackendPreference::OpenGL:
#if defined(XPBD_GFX_OPENGL)
    return createOpenGLBackend();
#else
    return nullptr;
#endif
  case BackendPreference::Vulkan:
#if defined(XPBD_GFX_VULKAN)
    return createVulkanBackend();
#else
    return nullptr;
#endif
  default:
    return nullptr;
  }
}

void destroyWindow(SDL_Window *&window) {
  if (window != nullptr) {
    SDL_DestroyWindow(window);
    window = nullptr;
  }
}

bool initWithWindow(SDL_Window *&window, const char *title, int w, int h,
                    SDL_WindowFlags flags, bool gl_attrs,
                    BackendPreference pref,
                    std::unique_ptr<IGpuBackend> &out, std::string &err) {
  destroyWindow(window);

  if (gl_attrs) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  }

  auto logFailure = [&](const std::string &message) {
    err = message;
    xpbd::log::error(message);
  };

  if ((flags & SDL_WINDOW_VULKAN) != 0) {
    prepareVulkanEnvironment();
  }

  SDL_WindowFlags window_flags =
      flags | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (environmentFlagEnabled("XPBD_STRICT_RT_GATE_PROBE")) {
    window_flags |= SDL_WINDOW_HIDDEN;
  }
  window = SDL_CreateWindow(title, w, h, window_flags);
  if (window == nullptr) {
    logFailure(std::string("CreateWindow(") + preferenceName(pref) + "): " +
               (SDL_GetError() ? SDL_GetError() : "failed"));
    return false;
  }

  auto candidate = makeBackend(pref);
  if (!candidate) {
    logFailure(std::string(preferenceName(pref)) +
               " backend is not enabled in this build");
    destroyWindow(window);
    return false;
  }

  bool initialized = false;
  if (pref == BackendPreference::Vulkan) {
    VulkanWindowBootstrap bootstrap;
    std::string bootstrap_error;
    if (!captureVulkanWindowBootstrap(window, bootstrap, &bootstrap_error)) {
      logFailure(std::string("Vulkan window bootstrap failed: ") +
                 bootstrap_error);
      candidate->shutdown();
      destroyWindow(window);
      return false;
    }
    initialized = candidate->init(bootstrap);
  } else {
    initialized = candidate->init(window);
  }

  if (!initialized) {
    logFailure(std::string(preferenceName(pref)) + " init failed" +
               (SDL_GetError() && SDL_GetError()[0]
                    ? std::string(": ") + SDL_GetError()
                    : ""));
    candidate->shutdown();
    destroyWindow(window);
    return false;
  }

  out = std::move(candidate);
  err.clear();
  xpbd::log::infof("OK created window+backend %s", preferenceName(pref));
  return true;
}

} // namespace

bool createWindowAndBackend(const BackendRequest &req, const char *title, int w,
                            int h, SDL_Window *&window,
                            std::unique_ptr<IGpuBackend> &backend,
                            std::string &err) {
  window = nullptr;
  backend.reset();

  if (!req.parse_error.empty()) {
    err = req.parse_error;
    xpbd::log::error(err);
    return false;
  }
  if (req.pref == BackendPreference::OpenGL) {
    err = "OpenGL renderer is deprecated; Vulkan RenderThread is required";
    xpbd::log::error(err);
    return false;
  }

  constexpr Attempt auto_order[] = {
      {BackendPreference::Vulkan, SDL_WINDOW_VULKAN, false},
  };

  auto tryOne = [&](BackendPreference pref) {
    if (pref == BackendPreference::OpenGL) {
      return initWithWindow(window, title, w, h, SDL_WINDOW_OPENGL, true, pref,
                            backend, err);
    }
    return initWithWindow(window, title, w, h, SDL_WINDOW_VULKAN, false, pref,
                          backend, err);
  };

  if (req.pref != BackendPreference::Auto) {
    if (tryOne(req.pref)) {
      return true;
    }
    if (req.force) {
      return false;
    }
  }

  for (const Attempt &attempt : auto_order) {
    if (req.pref != BackendPreference::Auto && attempt.pref == req.pref) {
      continue;
    }
    if (initWithWindow(window, title, w, h, attempt.flags, attempt.gl_attrs,
                       attempt.pref, backend, err)) {
      return true;
    }
    backend.reset();
  }

  destroyWindow(window);
  if (err.empty()) {
    err = "Vulkan RenderThread backend is unavailable";
  }
  return false;
}

} // namespace xpbd::gfx
