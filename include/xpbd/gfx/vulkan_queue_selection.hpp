#pragma once

#include <cstdint>
#include <limits>
#include <span>

namespace xpbd::gfx {

struct VulkanQueueFamilySupport {
  bool graphics = false;
  bool present = false;
};

struct VulkanQueueFamilySelection {
  static constexpr std::uint32_t kInvalidFamily =
      (std::numeric_limits<std::uint32_t>::max)();

  std::uint32_t graphics_family = kInvalidFamily;
  std::uint32_t present_family = kInvalidFamily;

  [[nodiscard]] constexpr bool valid() const noexcept {
    return graphics_family != kInvalidFamily &&
           present_family != kInvalidFamily;
  }

  [[nodiscard]] constexpr bool shared() const noexcept {
    return valid() && graphics_family == present_family;
  }
};

[[nodiscard]] constexpr VulkanQueueFamilySelection
selectVulkanQueueFamilies(
    std::span<const VulkanQueueFamilySupport> families) noexcept {
  VulkanQueueFamilySelection selection;
  for (std::size_t index = 0; index < families.size(); ++index) {
    const auto &support = families[index];
    const auto family = static_cast<std::uint32_t>(index);
    if (support.graphics &&
        selection.graphics_family ==
            VulkanQueueFamilySelection::kInvalidFamily) {
      selection.graphics_family = family;
    }
    if (support.present &&
        selection.present_family ==
            VulkanQueueFamilySelection::kInvalidFamily) {
      selection.present_family = family;
    }
    if (support.graphics && support.present) {
      selection.graphics_family = family;
      selection.present_family = family;
      return selection;
    }
  }
  return selection;
}

} // namespace xpbd::gfx
