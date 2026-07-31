#include "xpbd/gfx/rtxpt_bridge.hpp"

#include <filesystem>

namespace xpbd::gfx {
namespace {

#if defined(XPBD_WITH_RTXPT) && XPBD_WITH_RTXPT
constexpr bool kRtxptBuildEnabled = true;
#if defined(XPBD_RTXPT_VERSION_STR)
constexpr const char *kRtxptVersion = XPBD_RTXPT_VERSION_STR;
#else
constexpr const char *kRtxptVersion = "unknown";
#endif
#if defined(XPBD_RTXPT_ROOT_STR)
constexpr const char *kRtxptRoot = XPBD_RTXPT_ROOT_STR;
#else
constexpr const char *kRtxptRoot = "";
#endif
#else
constexpr bool kRtxptBuildEnabled = false;
constexpr const char *kRtxptVersion = "";
constexpr const char *kRtxptRoot = "";
#endif

bool g_path_tracer_ready = false;

[[nodiscard]] bool probeRtxptTreeOnDisk(std::string &out_root) {
  namespace fs = std::filesystem;
  const char *candidates[] = {
      kRtxptRoot,
      "third_party/RTXPT",
      "../third_party/RTXPT",
      "../../third_party/RTXPT",
  };
  for (const char *c : candidates) {
    if (!c || !c[0]) {
      continue;
    }
    const fs::path root(c);
    if (fs::exists(root / "Rtxpt") &&
        fs::exists(root / "Rtxpt" / "Shaders" / "PathTracer")) {
      out_root = root.string();
      return true;
    }
  }
  return false;
}

} // namespace

bool rtxptBuildEnabled() noexcept { return kRtxptBuildEnabled; }

void setRtxptAlignedPathTracerReady(bool ready) noexcept {
  g_path_tracer_ready = ready;
}

bool rtxptAlignedPathTracerReady() noexcept { return g_path_tracer_ready; }

RtxptStatus queryRtxptStatus() {
  RtxptStatus s;
  s.build_enabled = kRtxptBuildEnabled;
  s.version = kRtxptVersion ? kRtxptVersion : "";
  s.root_path = kRtxptRoot ? kRtxptRoot : "";
  s.tree_available = kRtxptBuildEnabled && kRtxptRoot && kRtxptRoot[0] != '\0';
  if (!s.tree_available) {
    std::string probed;
    if (probeRtxptTreeOnDisk(probed)) {
      s.tree_available = true;
      s.root_path = std::move(probed);
      if (s.version.empty()) {
        s.version = "tree";
      }
    }
  }
  // Stage 3: local RTXPT-aligned path tracer (Vulkan ray-query compute).
  // Full Donut/NVRHI Rtxpt sample remains optional for later interop.
  s.runtime_ready = g_path_tracer_ready;

  if (s.runtime_ready) {
    s.detail =
        "Advanced preview lighting ready. Optional vendor samples " +
        std::string(s.tree_available ? "on disk" : "not installed") +
        " (docs/rtxpt_integration.md).";
  } else if (s.tree_available) {
    s.detail =
        "Vendor sample tree present; advanced lighting needs a compatible GPU.";
  } else if (!s.build_enabled) {
    s.detail =
        "Using built-in preview lighting when the GPU supports it.";
  } else {
    s.detail = "Vendor sample tree not found under third_party.";
  }
  return s;
}

RtImplementation selectRtImplementation(bool user_wants_rt,
                                        const RayTracingCapability &hw,
                                        const RtxptStatus &rtxpt) noexcept {
  if (!user_wants_rt) {
    return RtImplementation::None;
  }
  if (rtxpt.runtime_ready && hw.supported && hw.device_extensions_enabled) {
    return RtImplementation::Rtxpt;
  }
  if (hw.supported && hw.device_extensions_enabled) {
    return RtImplementation::HybridRayQuery;
  }
  return RtImplementation::None;
}

const char *rtImplementationName(RtImplementation impl) noexcept {
  switch (impl) {
  case RtImplementation::Rtxpt:
    return "RTXPT";
  case RtImplementation::HybridRayQuery:
    return "HybridRayQuery";
  case RtImplementation::None:
  default:
    return "None";
  }
}

} // namespace xpbd::gfx
