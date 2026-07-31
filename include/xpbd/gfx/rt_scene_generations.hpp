#pragma once

#include <cstdint>

namespace xpbd::gfx {

// Authoritative invalidation domains for the Vulkan RT scene.  Producers
// increment the relevant field at the point where the source data changes;
// consumers must not hash complete vertex buffers in the frame hot path.
struct RtSceneGenerations {
  std::uint64_t topology = 0;
  std::uint64_t positions = 0;
  std::uint64_t transforms = 0;
  std::uint64_t materials = 0;
  std::uint64_t emission = 0;
  std::uint64_t visibility = 0;

  [[nodiscard]] constexpr bool operator==(
      const RtSceneGenerations &) const noexcept = default;
};

[[nodiscard]] constexpr std::uint64_t mixRtGeneration(
    std::uint64_t seed, std::uint64_t value) noexcept {
  // SplitMix-style avalanche; this is a key combiner, not a content hash.
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
  value ^= value >> 31u;
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
  return seed;
}

[[nodiscard]] constexpr std::uint64_t rtSceneGenerationKey(
    const RtSceneGenerations &generations) noexcept {
  std::uint64_t key = 0xcbf29ce484222325ull;
  key = mixRtGeneration(key, generations.topology);
  key = mixRtGeneration(key, generations.positions);
  key = mixRtGeneration(key, generations.transforms);
  key = mixRtGeneration(key, generations.materials);
  key = mixRtGeneration(key, generations.emission);
  key = mixRtGeneration(key, generations.visibility);
  return key;
}

} // namespace xpbd::gfx
