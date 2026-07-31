#pragma once

// NVIDIA RTX Path Tracing (RTXPT) integration bridge.
// Upstream: https://github.com/NVIDIA-RTX/RTXPT
//
// RTXPT is the preferred RT backend for this project (modern path tracer sample
// built on Donut/NVRHI). It is not a drop-in library; see docs/rtxpt_integration.md.

#include "xpbd/gfx/ray_tracing.hpp"

#include <string>

namespace xpbd::gfx {

// Which RT implementation is active / preferred for a frame.
enum class RtImplementation : std::uint8_t {
  None = 0,
  // Interim: local Vulkan ray-query directional shadows (BLAS/TLAS).
  HybridRayQuery = 1,
  // Target: NVIDIA RTX Path Tracing sample stack (Donut/NVRHI path tracer).
  Rtxpt = 2,
};

struct RtxptStatus {
  // Compile-time: XPBD_WITH_RTXPT and tree discovered by CMake.
  bool build_enabled = false;
  // Tree present at XPBD_RTXPT_ROOT (headers / sources available on disk).
  bool tree_available = false;
  // The built-in RTXPT-aligned Vulkan path-trace pass is live. This does not
  // imply that the optional upstream Donut/NVRHI sample is runtime-linked.
  bool runtime_ready = false;
  std::string version; // e.g. "1.8.1" when known
  std::string root_path;
  std::string detail; // human-readable status for UI / logs
};

// Compile-time: built with -DXPBD_WITH_RTXPT=ON and a discovered tree.
[[nodiscard]] bool rtxptBuildEnabled() noexcept;

// Runtime snapshot (safe when RTXPT is not present).
[[nodiscard]] RtxptStatus queryRtxptStatus();

// Set by the Vulkan backend when the RTXPT-aligned path-tracer pass is ready.
void setRtxptAlignedPathTracerReady(bool ready) noexcept;
[[nodiscard]] bool rtxptAlignedPathTracerReady() noexcept;

// Prefer RTXPT when runtime-ready; else hybrid ray-query when HW supports it.
[[nodiscard]] RtImplementation
selectRtImplementation(bool user_wants_rt,
                       const RayTracingCapability &hw,
                       const RtxptStatus &rtxpt) noexcept;

[[nodiscard]] const char *rtImplementationName(RtImplementation impl) noexcept;

} // namespace xpbd::gfx
