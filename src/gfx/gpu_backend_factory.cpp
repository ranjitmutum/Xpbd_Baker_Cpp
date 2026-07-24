#include "xpbd/gfx/backend_select.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/log.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <string>

namespace xpbd::gfx {
namespace {

struct Attempt {
    BackendPreference pref;
    SDL_WindowFlags flags;
    bool gl_attrs;
    const char* label;
};

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

        if (!SDL_Vulkan_LoadLibrary(nullptr)) {
            log_fail(std::string("SDL_Vulkan_LoadLibrary: ") +
                     (SDL_GetError() ? SDL_GetError() : "failed"));
            return false;
        }
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
