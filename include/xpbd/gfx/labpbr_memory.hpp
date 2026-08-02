#pragma once

#include <cstdint>
#include <limits>
#include <string>

namespace xpbd::gfx {

struct LabPbrMemoryEstimate {
  std::uint64_t resident_bytes = 0;
  std::uint64_t peak_bytes = 0;
  std::uint64_t coverage_peak_bytes = 0;
  std::uint64_t cache_bytes = 0;

  bool operator==(const LabPbrMemoryEstimate &) const = default;
};

// The request deliberately exposes the legacy resolved-texel stride. S00 uses
// sizeof(ResolvedMaterialTexel) to record the old baseline; S04 must pass zero
// after the persistent per-pixel table is removed.
struct LabPbrMemoryEstimateRequest {
  std::uint64_t width = 0;
  std::uint64_t height = 0;
  std::uint64_t resident_rgba_image_count = 0;
  std::uint64_t candidate_rgba_image_count = 0;
  std::uint64_t resolved_texel_bytes_per_pixel = 0;
  std::uint64_t resident_fixed_bytes = 0;
  std::uint64_t candidate_fixed_bytes = 0;
  std::uint64_t encoded_snapshot_bytes = 0;
  std::uint64_t decoder_peak_bytes = 0;
  std::uint64_t coverage_peak_bytes = 0;
  std::uint64_t cache_bytes = 0;
};

namespace detail {

[[nodiscard]] inline bool checkedLabPbrAdd(std::uint64_t lhs,
                                            std::uint64_t rhs,
                                            std::uint64_t &out) noexcept {
  if (rhs > (std::numeric_limits<std::uint64_t>::max)() - lhs) {
    return false;
  }
  out = lhs + rhs;
  return true;
}

[[nodiscard]] inline bool checkedLabPbrMultiply(
    std::uint64_t lhs, std::uint64_t rhs, std::uint64_t &out) noexcept {
  if (lhs != 0u &&
      rhs > (std::numeric_limits<std::uint64_t>::max)() / lhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

} // namespace detail

// resident_bytes excludes cache ownership so cache_bytes remains separately
// auditable. peak_bytes includes direct residency, transient candidates,
// snapshots, decoder scratch, Coverage peak, and cache ownership.
[[nodiscard]] inline bool estimateLabPbrMemory(
    const LabPbrMemoryEstimateRequest &request,
    LabPbrMemoryEstimate &out, std::string *error = nullptr) {
  const auto fail = [&](const char *message) {
    if (error != nullptr) {
      *error = message;
    }
    return false;
  };
  if (request.width == 0u || request.height == 0u) {
    return fail("LabPBR memory estimate dimensions must be positive");
  }

  std::uint64_t pixels = 0;
  std::uint64_t rgba_bytes = 0;
  std::uint64_t resident_images = 0;
  std::uint64_t candidate_images = 0;
  std::uint64_t resolved_texels = 0;
  if (!detail::checkedLabPbrMultiply(request.width, request.height, pixels) ||
      !detail::checkedLabPbrMultiply(pixels, 4u, rgba_bytes) ||
      !detail::checkedLabPbrMultiply(
          rgba_bytes, request.resident_rgba_image_count, resident_images) ||
      !detail::checkedLabPbrMultiply(
          rgba_bytes, request.candidate_rgba_image_count, candidate_images) ||
      !detail::checkedLabPbrMultiply(
          pixels, request.resolved_texel_bytes_per_pixel,
          resolved_texels)) {
    return fail("LabPBR memory estimate multiplication overflow");
  }

  LabPbrMemoryEstimate candidate;
  candidate.coverage_peak_bytes = request.coverage_peak_bytes;
  candidate.cache_bytes = request.cache_bytes;
  if (!detail::checkedLabPbrAdd(request.resident_fixed_bytes,
                                resident_images,
                                candidate.resident_bytes) ||
      !detail::checkedLabPbrAdd(candidate.resident_bytes, resolved_texels,
                                candidate.resident_bytes)) {
    return fail("LabPBR resident memory estimate overflow");
  }

  candidate.peak_bytes = candidate.resident_bytes;
  const std::uint64_t peak_components[] = {
      candidate_images,
      request.candidate_fixed_bytes,
      request.encoded_snapshot_bytes,
      request.decoder_peak_bytes,
      request.coverage_peak_bytes,
      request.cache_bytes,
  };
  for (const std::uint64_t component : peak_components) {
    if (!detail::checkedLabPbrAdd(candidate.peak_bytes, component,
                                  candidate.peak_bytes)) {
      return fail("LabPBR peak memory estimate overflow");
    }
  }

  out = candidate;
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

} // namespace xpbd::gfx
