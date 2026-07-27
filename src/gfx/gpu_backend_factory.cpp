#include "xpbd/gfx/backend_select.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/log.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace xpbd::gfx {
namespace {

struct Attempt {
    BackendPreference pref;
    SDL_WindowFlags flags;
    bool gl_attrs;
    const char* label;
};

bool environmentFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr &&
           (std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
            std::strcmp(value, "TRUE") == 0 ||
            std::strcmp(value, "yes") == 0 ||
            std::strcmp(value, "YES") == 0);
}

void prepareVulkanEnvironment() {
    if (environmentFlagEnabled("XPBD_VULKAN_ALLOW_THIRD_PARTY_LAYERS")) {
        xpbd::log::warn("Vulkan: third-party implicit layers explicitly allowed");
        return;
    }

    // These are the disable controls declared by the GamePP and RTSS implicit
    // layer manifests. Set them before SDL creates the Vulkan window (and thus
    // before SDL loads Vulkan) so the overlays never enter this process.
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

std::unique_ptr<IGpuBackend> makeBackend(BackendPreference p) {
    switch (p) {
        case BackendPreference::OpenGL:
            return createOpenGLBackend();
        case BackendPreference::Vulkan:
            return createVulkanBackend();
        case BackendPreference::Dx11:
            return createDx11Backend();
        case BackendPreference::Metal:
            return createMetalBackend();
        default:
            return nullptr;
    }
}

bool initWithWindow(SDL_Window*& window, const char* title, int w, int h, SDL_WindowFlags flags,
                    bool gl_attrs, BackendPreference pref, std::unique_ptr<IGpuBackend>& out,
                    std::string& err) {
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    if (gl_attrs) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    }
    auto log_fail = [&](const std::string& e) {
        err = e;
        xpbd::log::error(e);
    };

    if (flags & SDL_WINDOW_VULKAN) {
        prepareVulkanEnvironment();
    }
    window = SDL_CreateWindow(title, w, h,
                              flags | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        log_fail(std::string("CreateWindow(") + preferenceName(pref) + "): " +
                 (SDL_GetError() ? SDL_GetError() : "failed"));
        return false;
    }
    auto backend = makeBackend(pref);
    if (!backend) {
        log_fail(std::string(preferenceName(pref)) + " backend not available on this platform");
        return false;
    }
    if (!backend->init(window)) {
        err = std::string(preferenceName(pref)) + " init failed" +
              (SDL_GetError() && SDL_GetError()[0] ? std::string(": ") + SDL_GetError() : "");
        xpbd::log::error(err);
        backend->shutdown();
        return false;
    }
    out = std::move(backend);
    err.clear();
    xpbd::log::infof("OK created window+backend %s", preferenceName(pref));
    return true;
}

}


bool createWindowAndBackend(const BackendRequest& req, const char* title, int w, int h,
                            SDL_Window*& window, std::unique_ptr<IGpuBackend>& backend,
                            std::string& err) {
    window = nullptr;
    backend.reset();

#if defined(_WIN32)
    const Attempt auto_order[] = {
        {BackendPreference::Vulkan, SDL_WINDOW_VULKAN, false, "Vulkan"},
        {BackendPreference::Dx11, 0, false, "DX11"},
        {BackendPreference::OpenGL, SDL_WINDOW_OPENGL, true, "OpenGL"},
    };
#else
    const Attempt auto_order[] = {
        {BackendPreference::Vulkan, SDL_WINDOW_VULKAN, false, "Vulkan"},
        {BackendPreference::OpenGL, SDL_WINDOW_OPENGL, true, "OpenGL"},
    };
#endif

    auto try_one = [&](BackendPreference p) -> bool {
        SDL_WindowFlags flags = 0;
        bool gl_attrs = false;
        if (p == BackendPreference::OpenGL) {
            flags = SDL_WINDOW_OPENGL;
            gl_attrs = true;
        } else if (p == BackendPreference::Vulkan) {
            flags = SDL_WINDOW_VULKAN;
        } else if (p == BackendPreference::Dx11) {
            flags = 0;
        } else if (p == BackendPreference::Metal) {
            flags = 0;
        }
        return initWithWindow(window, title, w, h, flags, gl_attrs, p, backend, err);
    };

    if (req.pref != BackendPreference::Auto) {
        if (try_one(req.pref)) {
            return true;
        }
        if (req.force) {
            return false;
        }

    }

    for (const auto& a : auto_order) {
        if (req.pref != BackendPreference::Auto && a.pref == req.pref) {
            continue;
        }
        if (try_one(a.pref)) {
            return true;
        }
        backend.reset();
    }
    if (err.empty()) {
        err = "No graphics backend available";
    }
    return false;
}

}
