#pragma once

#include "xpbd/core/simd_dispatch.hpp"

#include <cstdint>

namespace xpbd::core::testing {

struct CpuidRegisters {
  std::uint32_t eax = 0;
  std::uint32_t ebx = 0;
  std::uint32_t ecx = 0;
  std::uint32_t edx = 0;
};

using CpuidProbe = bool (*)(void *context, std::uint32_t leaf,
                            std::uint32_t subleaf,
                            CpuidRegisters &registers) noexcept;
using XgetbvProbe = std::uint64_t (*)(void *context,
                                      std::uint32_t index) noexcept;


struct SimdProbe {
  void *context = nullptr;
  CpuidProbe cpuid = nullptr;
  XgetbvProbe xgetbv = nullptr;
};

[[nodiscard]] SimdCapabilities
detectSimdCapabilities(const SimdProbe &probe) noexcept;

}
