#pragma once

#include <cstdint>
#include <string_view>

namespace xpbd::core {



enum class SimdMode : std::uint8_t {
  Auto,
  AVX2,
  SSE2,
};







struct SimdCapabilities {
  bool cpuid_available = false;
  std::uint32_t max_basic_leaf = 0;
  bool sse2 = false;
  bool avx = false;
  bool osxsave = false;
  bool xgetbv_available = false;
  std::uint64_t xcr0 = 0;
  bool xmm_state_enabled = false;
  bool ymm_state_enabled = false;
  bool avx2 = false;

  [[nodiscard]] constexpr bool avx2Usable() const noexcept {
    return cpuid_available && avx && osxsave && xgetbv_available &&
           xmm_state_enabled && ymm_state_enabled && avx2;
  }
};

enum class SimdSelectionReason : std::uint8_t {
  Avx2Available,
  ForcedSse2,
  CpuidUnavailable,
  AvxUnavailable,
  OsxsaveUnavailable,
  XgetbvUnavailable,
  XmmYmmStateUnavailable,
  Avx2Unavailable,
  InvalidMode,
};

struct SimdSelectionDiagnostics {
  SimdMode requested = SimdMode::Auto;
  SimdMode selected = SimdMode::SSE2;
  SimdSelectionReason reason = SimdSelectionReason::CpuidUnavailable;
  SimdCapabilities capabilities{};


  [[nodiscard]] constexpr bool fellBack() const noexcept {
    return requested == SimdMode::AVX2 && selected == SimdMode::SSE2;
  }
};


[[nodiscard]] SimdCapabilities detectSimdCapabilities() noexcept;



[[nodiscard]] SimdSelectionDiagnostics
selectSimdMode(SimdMode requested,
               const SimdCapabilities &capabilities) noexcept;



[[nodiscard]] SimdSelectionDiagnostics
simdSelectionDiagnostics(SimdMode requested = SimdMode::Auto) noexcept;


[[nodiscard]] SimdMode
selectedSimdMode(SimdMode requested = SimdMode::Auto) noexcept;

[[nodiscard]] std::string_view simdModeName(SimdMode mode) noexcept;
[[nodiscard]] std::string_view
simdSelectionReasonText(SimdSelectionReason reason) noexcept;

}
