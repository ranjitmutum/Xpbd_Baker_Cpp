#pragma once

#include <cstdint>

namespace xpbd::gfx {

// Shared ABI between the path-tracing shaders, Vulkan image array,
// Streamline/DLSS adapters, diagnostics, and CPU regression references.
enum class PathTraceAovLayer : std::uint32_t {
  GeometryNormalLinearDepth = 0,
  ShadingNormalRoughness = 1,
  DiffuseAlbedo = 2,
  SpecularAlbedoHitDistance = 3,
  DiffuseRadianceHitDistance = 4,
  SpecularRadianceHitDistance = 5,
  MotionDisocclusion = 6,
  Emission = 7,
  TransparencyOverlay = 8,
  // World-space distance from the primary surface to the first reflected
  // specular hit. Stored in R for DLSS Ray Reconstruction.
  SpecularHitDistance = 9,
  Count = 10,
};

static_assert(
    static_cast<std::uint32_t>(PathTraceAovLayer::MotionDisocclusion) == 6u &&
        static_cast<std::uint32_t>(
            PathTraceAovLayer::TransparencyOverlay) == 8u &&
        static_cast<std::uint32_t>(
            PathTraceAovLayer::SpecularHitDistance) == 9u &&
        static_cast<std::uint32_t>(PathTraceAovLayer::Count) == 10u,
    "Path-tracing AOV ABI must stay synchronized with both PT shaders");

} // namespace xpbd::gfx
