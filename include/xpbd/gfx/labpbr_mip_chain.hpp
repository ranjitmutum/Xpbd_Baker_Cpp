#pragma once

#include "xpbd/gfx/texture_image.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xpbd::gfx {

enum class LabPbrMipSemantic : std::uint8_t {
  BaseColorCoverage = 0,
  IrisNormalAoHeight = 1,
  SpecularPacked = 2,
};

struct LabPbrAtlasIsland {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t source_face = 0;
  bool used_by_cutout = false;
  bool used_by_blend = false;
};

struct LabPbrMipLevel {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;

  [[nodiscard]] bool valid() const noexcept;
};

struct LabPbrMipChain {
  LabPbrMipSemantic semantic = LabPbrMipSemantic::BaseColorCoverage;
  std::vector<LabPbrMipLevel> levels;
  std::uint32_t safe_max_lod = 0;
  bool atlas_isolation_proven = false;
  std::string fallback_reason;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] const LabPbrMipLevel *baseLevel() const noexcept;
};

// Derive safe, axis-aligned pixel rectangles from the raw Bedrock face UVs
// retained by StaticModelVertex. The output is transactional: on failure it
// is cleared and error contains a stable diagnostic reason.
[[nodiscard]] bool buildLabPbrAtlasIslands(
    const StaticIndexedModelMesh &mesh, const TextureImage &texture,
    std::vector<LabPbrAtlasIsland> &out, std::string *error = nullptr);

// Build a deterministic semantic mip chain. The source base level is always
// retained when source is valid. Unsafe or unprovable island input returns a
// base-only chain with safe_max_lod == 0 and a structured fallback_reason.
[[nodiscard]] LabPbrMipChain buildLabPbrMipChain(
    const TextureImage &source, std::span<const LabPbrAtlasIsland> islands,
    LabPbrMipSemantic semantic,
    const LabPbrMipChain *base_color_coverage = nullptr);

[[nodiscard]] std::size_t
labPbrMipChainByteSize(const LabPbrMipChain &chain) noexcept;

} // namespace xpbd::gfx
