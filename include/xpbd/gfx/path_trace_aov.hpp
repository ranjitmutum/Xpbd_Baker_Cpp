#pragma once

#include <cstdint>
#include <string_view>

namespace xpbd::gfx {

// Developer-only display modes for inspecting the exact path-tracing and
// Streamline inputs. They do not change radiance calculations or RR tags;
// requesting an unallocated diagnostic can trigger one target rebuild.
enum class RrAovDebugView : std::uint8_t {
  Off = 0,
  RawColor = 1,
  ReconstructedColor = 2,
  DeviceDepth = 3,
  LinearDepth = 4,
  Motion = 5,
  MotionMagnitude = 6,
  PreviousUvOutside = 7,
  DiffuseAlbedo = 8,
  SpecularAlbedo = 9,
  Normal = 10,
  Roughness = 11,
  SpecularHitDistance = 12,
  ReactiveMask = 13,
  TransparencyAndComposition = 14,
  GuideValidity = 15,
  TemporalBoundaryOverlay = 16,
};

[[nodiscard]] constexpr const char *
rrAovDebugViewName(RrAovDebugView view) noexcept {
  switch (view) {
  case RrAovDebugView::RawColor:
    return "raw-color";
  case RrAovDebugView::ReconstructedColor:
    return "rr-output";
  case RrAovDebugView::DeviceDepth:
    return "device-depth";
  case RrAovDebugView::LinearDepth:
    return "linear-depth";
  case RrAovDebugView::Motion:
    return "motion";
  case RrAovDebugView::MotionMagnitude:
    return "motion-magnitude";
  case RrAovDebugView::PreviousUvOutside:
    return "previous-uv-outside";
  case RrAovDebugView::DiffuseAlbedo:
    return "diffuse-albedo";
  case RrAovDebugView::SpecularAlbedo:
    return "specular-albedo";
  case RrAovDebugView::Normal:
    return "normal";
  case RrAovDebugView::Roughness:
    return "roughness";
  case RrAovDebugView::SpecularHitDistance:
    return "specular-hit-distance";
  case RrAovDebugView::ReactiveMask:
    return "reactive-mask";
  case RrAovDebugView::TransparencyAndComposition:
    return "transparency-composition";
  case RrAovDebugView::GuideValidity:
    return "guide-validity";
  case RrAovDebugView::TemporalBoundaryOverlay:
    return "temporal-boundary-overlay";
  case RrAovDebugView::Off:
  default:
    return "off";
  }
}

[[nodiscard]] constexpr RrAovDebugView
rrAovDebugViewFromName(std::string_view name) noexcept {
  if (name == "raw" || name == "raw-color") {
    return RrAovDebugView::RawColor;
  }
  if (name == "rr" || name == "rr-output" ||
      name == "reconstructed" || name == "reconstructed-color") {
    return RrAovDebugView::ReconstructedColor;
  }
  if (name == "depth" || name == "device-depth") {
    return RrAovDebugView::DeviceDepth;
  }
  if (name == "linear-depth") {
    return RrAovDebugView::LinearDepth;
  }
  if (name == "motion") {
    return RrAovDebugView::Motion;
  }
  if (name == "motion-magnitude") {
    return RrAovDebugView::MotionMagnitude;
  }
  if (name == "previous-uv-outside" || name == "previous-outside") {
    return RrAovDebugView::PreviousUvOutside;
  }
  if (name == "diffuse-albedo") {
    return RrAovDebugView::DiffuseAlbedo;
  }
  if (name == "specular-albedo") {
    return RrAovDebugView::SpecularAlbedo;
  }
  if (name == "normal") {
    return RrAovDebugView::Normal;
  }
  if (name == "roughness") {
    return RrAovDebugView::Roughness;
  }
  if (name == "specular-hit-distance" || name == "hit-distance") {
    return RrAovDebugView::SpecularHitDistance;
  }
  if (name == "reactive" || name == "reactive-mask") {
    return RrAovDebugView::ReactiveMask;
  }
  if (name == "transparency" ||
      name == "transparency-composition") {
    return RrAovDebugView::TransparencyAndComposition;
  }
  if (name == "guide-validity" || name == "validity") {
    return RrAovDebugView::GuideValidity;
  }
  if (name == "temporal-boundary-overlay" ||
      name == "boundary-overlay" || name == "edge-overlay") {
    return RrAovDebugView::TemporalBoundaryOverlay;
  }
  return RrAovDebugView::Off;
}

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

[[nodiscard]] constexpr std::uint32_t
rrAovDebugRequiredOutputMask(RrAovDebugView view) noexcept {
  switch (view) {
  case RrAovDebugView::LinearDepth:
    return pathTraceAovOutputBit(
        PathTraceAovLayer::GeometryNormalLinearDepth);
  case RrAovDebugView::Motion:
  case RrAovDebugView::MotionMagnitude:
    return pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrMotion);
  case RrAovDebugView::PreviousUvOutside:
    return pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrMotion) |
           pathTraceAovOutputBit(PathTraceAovLayer::MotionDisocclusion);
  case RrAovDebugView::TemporalBoundaryOverlay:
    return pathTraceOptionalOutputBit(PathTraceOptionalOutput::RrMotion) |
           pathTraceOptionalOutputBit(
               PathTraceOptionalOutput::GuideValidity);
  case RrAovDebugView::DiffuseAlbedo:
    return pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::RrDiffuseAlbedo);
  case RrAovDebugView::SpecularAlbedo:
    return pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::RrSpecularAlbedo);
  case RrAovDebugView::Normal:
  case RrAovDebugView::Roughness:
    return pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::RrNormalRoughness);
  case RrAovDebugView::SpecularHitDistance:
    return pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::RrSpecularHitDistance);
  case RrAovDebugView::ReactiveMask:
    return pathTraceOptionalOutputBit(PathTraceOptionalOutput::ReactiveMask);
  case RrAovDebugView::TransparencyAndComposition:
    return pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::TransparencyAndComposition);
  case RrAovDebugView::GuideValidity:
    return pathTraceOptionalOutputBit(
        PathTraceOptionalOutput::GuideValidity);
  case RrAovDebugView::Off:
  case RrAovDebugView::RawColor:
  case RrAovDebugView::ReconstructedColor:
  case RrAovDebugView::DeviceDepth:
  default:
    return 0u;
  }
}

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
