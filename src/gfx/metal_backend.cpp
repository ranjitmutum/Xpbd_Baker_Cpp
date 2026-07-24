#include "xpbd/gfx/gpu_backend.hpp"

#include <SDL3/SDL.h>

namespace xpbd::gfx {
namespace {

class MetalBackendStub final : public IGpuBackend {
public:
    bool init(SDL_Window*) override {
        SDL_Log("Metal backend is only available on Apple platforms.");
        return false;
    }
    void shutdown() override {}
    void resize(int, int) override {}
    bool uploadFontAtlas(const void*, int, int, bool) override { return false; }
    unsigned int fontTextureId() const override { return 0; }
    void render(const FrameInput&) override {}
    BackendKind kind() const override { return BackendKind::Metal; }
    const char* name() const override { return "Metal"; }
    const char* deviceName() const override { return "unavailable"; }
    FrameStats stats() const override { return {}; }
};

}

std::unique_ptr<IGpuBackend> createMetalBackend() {
#if defined(__APPLE__)

    return std::make_unique<MetalBackendStub>();
#else
    return std::make_unique<MetalBackendStub>();
#endif
}

}
