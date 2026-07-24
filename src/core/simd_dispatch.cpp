#include "xpbd/core/simd_dispatch.hpp"

#include "xpbd/core/testing/simd_probe.hpp"

#include <cstdint>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) ||                \
    defined(__x86_64__)
#define XPBD_SIMD_X86 1
#else
#define XPBD_SIMD_X86 0
#endif

#if XPBD_SIMD_X86 && defined(_MSC_VER)
#include <intrin.h>
#elif XPBD_SIMD_X86 && (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#endif

namespace xpbd::core {
namespace {

constexpr std::uint32_t kCpuidSse2 = 1u << 26u;
constexpr std::uint32_t kCpuidOsxsave = 1u << 27u;
constexpr std::uint32_t kCpuidAvx = 1u << 28u;
constexpr std::uint32_t kCpuidAvx2 = 1u << 5u;
constexpr std::uint64_t kXcr0Xmm = 1ull << 1u;
constexpr std::uint64_t kXcr0Ymm = 1ull << 2u;

#if XPBD_SIMD_X86
bool nativeCpuid(void *, std::uint32_t leaf, std::uint32_t subleaf,
                 testing::CpuidRegisters &registers) noexcept {
#if defined(_MSC_VER)
  int values[4]{};
  __cpuidex(values, static_cast<int>(leaf), static_cast<int>(subleaf));
  registers.eax = static_cast<std::uint32_t>(values[0]);
  registers.ebx = static_cast<std::uint32_t>(values[1]);
  registers.ecx = static_cast<std::uint32_t>(values[2]);
  registers.edx = static_cast<std::uint32_t>(values[3]);
#else
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  __cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
  registers.eax = eax;
  registers.ebx = ebx;
  registers.ecx = ecx;
  registers.edx = edx;
#endif
  return true;
}

std::uint64_t nativeXgetbv(void *, std::uint32_t index) noexcept {
#if defined(_MSC_VER)
  return _xgetbv(index);
#else
  std::uint32_t eax = 0;
  std::uint32_t edx = 0;
  __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
  return (static_cast<std::uint64_t>(edx) << 32u) | eax;
#endif
}
#endif

testing::SimdProbe nativeProbe() noexcept {
#if XPBD_SIMD_X86
  return testing::SimdProbe{nullptr, nativeCpuid, nativeXgetbv};
#else
  return {};
#endif
}

SimdSelectionReason
unavailableReason(const SimdCapabilities &capabilities) noexcept {
  if (!capabilities.cpuid_available) {
    return SimdSelectionReason::CpuidUnavailable;
  }
  if (!capabilities.avx) {
    return SimdSelectionReason::AvxUnavailable;
  }
  if (!capabilities.osxsave) {
    return SimdSelectionReason::OsxsaveUnavailable;
  }
  if (!capabilities.xgetbv_available) {
    return SimdSelectionReason::XgetbvUnavailable;
  }
  if (!capabilities.xmm_state_enabled || !capabilities.ymm_state_enabled) {
    return SimdSelectionReason::XmmYmmStateUnavailable;
  }
  return SimdSelectionReason::Avx2Unavailable;
}

}

namespace testing {

SimdCapabilities detectSimdCapabilities(const SimdProbe &probe) noexcept {
  SimdCapabilities capabilities;
  capabilities.xgetbv_available = probe.xgetbv != nullptr;
  if (probe.cpuid == nullptr) {
    return capabilities;
  }

  CpuidRegisters leaf0;
  if (!probe.cpuid(probe.context, 0, 0, leaf0)) {
    return capabilities;
  }
  capabilities.cpuid_available = true;
  capabilities.max_basic_leaf = leaf0.eax;
  if (capabilities.max_basic_leaf < 1) {
    return capabilities;
  }

  CpuidRegisters leaf1;
  if (!probe.cpuid(probe.context, 1, 0, leaf1)) {
    return capabilities;
  }
  capabilities.sse2 = (leaf1.edx & kCpuidSse2) != 0;
  capabilities.avx = (leaf1.ecx & kCpuidAvx) != 0;
  capabilities.osxsave = (leaf1.ecx & kCpuidOsxsave) != 0;


  if (!capabilities.avx || !capabilities.osxsave || probe.xgetbv == nullptr) {
    return capabilities;
  }
  capabilities.xcr0 = probe.xgetbv(probe.context, 0);
  capabilities.xmm_state_enabled = (capabilities.xcr0 & kXcr0Xmm) != 0;
  capabilities.ymm_state_enabled = (capabilities.xcr0 & kXcr0Ymm) != 0;


  if (!capabilities.xmm_state_enabled || !capabilities.ymm_state_enabled ||
      capabilities.max_basic_leaf < 7) {
    return capabilities;
  }

  CpuidRegisters leaf7;
  if (probe.cpuid(probe.context, 7, 0, leaf7)) {
    capabilities.avx2 = (leaf7.ebx & kCpuidAvx2) != 0;
  }
  return capabilities;
}

}

SimdCapabilities detectSimdCapabilities() noexcept {
  return testing::detectSimdCapabilities(nativeProbe());
}

SimdSelectionDiagnostics
selectSimdMode(SimdMode requested,
               const SimdCapabilities &capabilities) noexcept {
  SimdSelectionDiagnostics diagnostics;
  diagnostics.requested = requested;
  diagnostics.capabilities = capabilities;

  if (requested == SimdMode::SSE2) {
    diagnostics.selected = SimdMode::SSE2;
    diagnostics.reason = SimdSelectionReason::ForcedSse2;
    return diagnostics;
  }

  if (requested != SimdMode::Auto && requested != SimdMode::AVX2) {
    diagnostics.selected = SimdMode::SSE2;
    diagnostics.reason = SimdSelectionReason::InvalidMode;
    return diagnostics;
  }

  if (capabilities.avx2Usable()) {
    diagnostics.selected = SimdMode::AVX2;
    diagnostics.reason = SimdSelectionReason::Avx2Available;
    return diagnostics;
  }

  diagnostics.selected = SimdMode::SSE2;
  diagnostics.reason = unavailableReason(capabilities);
  return diagnostics;
}

SimdSelectionDiagnostics simdSelectionDiagnostics(SimdMode requested) noexcept {
  return selectSimdMode(requested, detectSimdCapabilities());
}

SimdMode selectedSimdMode(SimdMode requested) noexcept {
  return simdSelectionDiagnostics(requested).selected;
}

std::string_view simdModeName(SimdMode mode) noexcept {
  switch (mode) {
  case SimdMode::Auto:
    return "Auto";
  case SimdMode::AVX2:
    return "AVX2";
  case SimdMode::SSE2:
    return "SSE2";
  }
  return "Unknown";
}

std::string_view simdSelectionReasonText(SimdSelectionReason reason) noexcept {
  switch (reason) {
  case SimdSelectionReason::Avx2Available:
    return "AVX2 is supported by the CPU and enabled by the OS";
  case SimdSelectionReason::ForcedSse2:
    return "SSE2 was explicitly requested";
  case SimdSelectionReason::CpuidUnavailable:
    return "CPUID is unavailable; using the SSE2 baseline";
  case SimdSelectionReason::AvxUnavailable:
    return "the CPU does not report AVX support; using SSE2";
  case SimdSelectionReason::OsxsaveUnavailable:
    return "OSXSAVE is unavailable; using SSE2";
  case SimdSelectionReason::XgetbvUnavailable:
    return "XGETBV probing is unavailable; using SSE2";
  case SimdSelectionReason::XmmYmmStateUnavailable:
    return "the OS has not enabled both XMM and YMM state; using SSE2";
  case SimdSelectionReason::Avx2Unavailable:
    return "the CPU does not report AVX2 support; using SSE2";
  case SimdSelectionReason::InvalidMode:
    return "the requested SIMD mode is invalid; using SSE2";
  }
  return "unknown SIMD selection reason";
}

}

#undef XPBD_SIMD_X86
