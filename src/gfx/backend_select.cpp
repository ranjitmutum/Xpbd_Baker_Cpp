#include "xpbd/gfx/backend_select.hpp"

#include <cstdlib>
#include <cstring>

namespace xpbd::gfx {
namespace {

bool eq(const char* a, const char* b) {
    return a && b && std::strcmp(a, b) == 0;
}

bool matchPref(const char* s, BackendPreference& out) {
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
    if (eq(s, "d3d") || eq(s, "dx11") || eq(s, "d3d11") || eq(s, "DX11")) {
        out = BackendPreference::Dx11;
        return true;
    }
    if (eq(s, "ml") || eq(s, "metal") || eq(s, "Metal")) {
        out = BackendPreference::Metal;
        return true;
    }
    if (eq(s, "auto")) {
        out = BackendPreference::Auto;
        return true;
    }
    return false;
}

}

const char* preferenceName(BackendPreference p) {
    switch (p) {
        case BackendPreference::OpenGL:
            return "OpenGL";
        case BackendPreference::Vulkan:
            return "Vulkan";
        case BackendPreference::Dx11:
            return "DX11";
        case BackendPreference::Metal:
            return "Metal";
        default:
            return "Auto";
    }
}

BackendRequest parseBackendRequest(int argc, char** argv) {
    BackendRequest req;

    if (const char* env = std::getenv("XPBD_GFX")) {
        BackendPreference p = BackendPreference::Auto;
        if (matchPref(env, p)) {
            req.pref = p;
            req.force = (p != BackendPreference::Auto);
        }
    }

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
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

        if (a != argv[i] && (eq(a, "d3d12") || eq(a, "gles"))) {
            req.parse_error = std::string("Unsupported backend flag: ") + argv[i];
        }
    }
    return req;
}

}
