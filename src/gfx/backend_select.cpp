#include "xpbd/gfx/backend_select.hpp"

#include <cstdlib>
#include <cstring>

namespace xpbd::gfx {
namespace {

bool eq(const char *a, const char *b) {
  return a && b && std::strcmp(a, b) == 0;
}

bool matchPref(const char *s, BackendPreference &out) {
  if (!s || !*s) {
    return false;
  }
  if (eq(s, "gl") || eq(s, "opengl") || eq(s, "OpenGL")) {
    out = BackendPreference::OpenGL;
    return true;
  }
  if (eq(s, "vk") || eq(s, "vulkan") || eq(s, "Vulkan")) {
    out = BackendPreference::Vulkan;
    return true;
  }
  if (eq(s, "auto")) {
    out = BackendPreference::Auto;
    return true;
  }
  return false;
}

} // namespace

const char *preferenceName(BackendPreference p) {
  switch (p) {
  case BackendPreference::OpenGL:
    return "OpenGL";
  case BackendPreference::Vulkan:
    return "Vulkan";
  default:
    return "Auto";
  }
}

BackendRequest parseBackendRequest(int argc, char **argv) {
  BackendRequest req;

  if (const char *env = std::getenv("XPBD_GFX")) {
    BackendPreference p = BackendPreference::Auto;
    if (matchPref(env, p)) {
      req.pref = p;
      req.force = (p != BackendPreference::Auto);
    } else if (env[0] != '\0') {
      if (eq(env, "ml") || eq(env, "metal") || eq(env, "Metal") ||
          eq(env, "d3d") ||
          eq(env, "dx11") || eq(env, "d3d11")) {
        req.parse_error =
            std::string("Graphics backend '") + env +
            "' was removed; Vulkan is the supported renderer";
        req.force = true;
      }
    }
  }

  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!a) {
      continue;
    }

    while (*a == '-') {
      ++a;
    }
    BackendPreference p = BackendPreference::Auto;
    if (matchPref(a, p)) {
      req.pref = p;
      req.force = (p != BackendPreference::Auto);
      req.parse_error.clear();
      continue;
    }

    if (a != argv[i] &&
        (eq(a, "ml") || eq(a, "metal") || eq(a, "d3d") ||
         eq(a, "d3d12") || eq(a, "d3d11") || eq(a, "dx11") ||
         eq(a, "gles"))) {
      req.parse_error =
          std::string("Graphics backend flag '") + argv[i] +
          "' was removed; use -vk";
      req.force = true;
    }
  }
  return req;
}

} // namespace xpbd::gfx
