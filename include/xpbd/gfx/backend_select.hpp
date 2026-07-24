#pragma once

#include "xpbd/gfx/gpu_backend.hpp"

#include <memory>
#include <string>

struct SDL_Window;

namespace xpbd::gfx {


enum class BackendPreference {
    Auto,
    OpenGL,
    Vulkan,
    Dx11,
    Metal,
};

struct BackendRequest {
    BackendPreference pref = BackendPreference::Auto;

    bool force = false;
    std::string parse_error;
};





BackendRequest parseBackendRequest(int argc, char** argv);

const char* preferenceName(BackendPreference p);


bool createWindowAndBackend(const BackendRequest& req, const char* title, int w, int h,
                            SDL_Window*& window, std::unique_ptr<IGpuBackend>& backend,
                            std::string& err);

}
