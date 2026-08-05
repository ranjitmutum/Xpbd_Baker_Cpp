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

// Optional path-tracing storage writes. Color and depth are intentionally not
// represented here because the viewport compositor always consumes them.
// Bits 0..9 mirror PathTraceAovLayer so CPU diagnostics and both shaders share
// one compact ABI.
enum class PathTraceOptionalOutput : std::uint32_t {
  RrMotion = static_cast<std::uint32_t>(PathTraceAovLayer::Count),
  RrSpecularHitDistance,
  RrDiffuseAlbedo,
  RrSpecularAlbedo,
  RrNormalRoughness,
  Statistics,
  // Standalone R8_UNORM temporal inputs. They are deliberately not packed
  // into the diagnostic RGBA AOV array because Streamline consumes exact
  // single-channel resources with independent lifetimes.
  TransparencyAndComposition,
  ReactiveMask,
  GuideValidity,
};

[[nodiscard]] constexpr std::uint32_t
pathTraceAovOutputBit(PathTraceAovLayer layer) noexcept {
  return 1u << static_cast<std::uint32_t>(layer);
}

[[nodiscard]] constexpr std::uint32_t
pathTraceOptionalOutputBit(PathTraceOptionalOutput output) noexcept {
  return 1u << static_cast<std::uint32_t>(output);
}

inline constexpr std::uint32_t kPathTraceAllAovOutputMask =
    (1u << static_cast<std::uint32_t>(PathTraceAovLayer::Count)) - 1u;
inline constexpr std::uint32_t kPathTraceRrMotionOutputMask =
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrMotion);
inline constexpr std::uint32_t kPathTraceAllRrGuideOutputMask =
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrMotion) |
    pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::RrSpecularHitDistance) |
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrDiffuseAlbedo) |
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrSpecularAlbedo) |
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrNormalRoughness);
inline constexpr std::uint32_t kPathTraceStatisticsOutputMask =
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::Statistics);
inline constexpr std::uint32_t kPathTraceTransparencyGuideOutputMask =
    pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::TransparencyAndComposition) |
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::ReactiveMask) |
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::GuideValidity);
// Only masks with a validated DLSS SR/RR Vulkan tag belong to the runtime
// contract. GuideValidity remains an internal diagnostics channel and must not
// make temporal reconstruction depend on an untagged image.
inline constexpr std::uint32_t kPathTraceStreamlineMaskOutputMask =
    pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::TransparencyAndComposition) |
    pathTraceOptionalOutputBit(PathTraceOptionalOutput::ReactiveMask);
inline constexpr std::uint32_t kPathTraceSrRequiredOutputMask =
    kPathTraceRrMotionOutputMask | kPathTraceStreamlineMaskOutputMask;
inline constexpr std::uint32_t kPathTraceRrRequiredOutputMask =
    kPathTraceAllRrGuideOutputMask | kPathTraceStreamlineMaskOutputMask;
inline constexpr std::uint32_t kPathTraceAllOptionalOutputMask =
    kPathTraceAllAovOutputMask | kPathTraceAllRrGuideOutputMask |
    kPathTraceStatisticsOutputMask | kPathTraceTransparencyGuideOutputMask;

static_assert(
    static_cast<std::uint32_t>(PathTraceAovLayer::MotionDisocclusion) == 6u &&
        static_cast<std::uint32_t>(
            PathTraceAovLayer::TransparencyOverlay) == 8u &&
        static_cast<std::uint32_t>(
            PathTraceAovLayer::SpecularHitDistance) == 9u &&
        static_cast<std::uint32_t>(PathTraceAovLayer::Count) == 10u &&
        static_cast<std::uint32_t>(
            PathTraceOptionalOutput::RrNormalRoughness) == 14u &&
        static_cast<std::uint32_t>(
            PathTraceOptionalOutput::Statistics) == 15u &&
        static_cast<std::uint32_t>(
            PathTraceOptionalOutput::TransparencyAndComposition) == 16u &&
        static_cast<std::uint32_t>(
            PathTraceOptionalOutput::GuideValidity) == 18u &&
        kPathTraceTransparencyGuideOutputMask == 0x70000u &&
        kPathTraceAllOptionalOutputMask == 0x7ffffu,
    "Path-tracing AOV ABI must stay synchronized with both PT shaders");

} // namespace xpbd::gfx
