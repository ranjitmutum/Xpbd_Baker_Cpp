#include "xpbd/gfx/streamline_vulkan_runtime.hpp"

#include "xpbd/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <thread>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef XPBD_STREAMLINE_HAS_DLSS_SR
#define XPBD_STREAMLINE_HAS_DLSS_SR 0
#endif
#ifndef XPBD_STREAMLINE_HAS_DLSS_RR
#define XPBD_STREAMLINE_HAS_DLSS_RR 0
#endif
#ifndef XPBD_STREAMLINE_HAS_DLSS_G
#define XPBD_STREAMLINE_HAS_DLSS_G 0
#endif
#ifndef XPBD_STREAMLINE_HAS_REFLEX
#define XPBD_STREAMLINE_HAS_REFLEX 0
#endif
#ifndef XPBD_STREAMLINE_HAS_PCL
#define XPBD_STREAMLINE_HAS_PCL 0
#endif

#if XPBD_WITH_STREAMLINE && defined(_WIN32)
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_helpers.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
// This header provides its implementation and must be included in one
// translation unit only.
#include <sl_security.h>
#endif

namespace xpbd::gfx {

const char *frameGenerationRuntimeStateName(
    FrameGenerationRuntimeState state) noexcept {
  switch (state) {
  case FrameGenerationRuntimeState::Unsupported:
    return "Unsupported";
  case FrameGenerationRuntimeState::NativeOff:
    return "NativeOff";
  case FrameGenerationRuntimeState::EnablingDrain:
    return "EnablingDrain";
  case FrameGenerationRuntimeState::EnablingLoadPlugin:
    return "EnablingLoadPlugin";
  case FrameGenerationRuntimeState::EnablingCreateProxySwapchain:
    return "EnablingCreateProxySwapchain";
  case FrameGenerationRuntimeState::ProxyArmed:
    return "ProxyArmed";
  case FrameGenerationRuntimeState::Active:
    return "Active";
  case FrameGenerationRuntimeState::DisablingOptions:
    return "DisablingOptions";
  case FrameGenerationRuntimeState::DisablingDrain:
    return "DisablingDrain";
  case FrameGenerationRuntimeState::DisablingDestroyProxySwapchain:
    return "DisablingDestroyProxySwapchain";
  case FrameGenerationRuntimeState::DisablingUnloadPlugin:
    return "DisablingUnloadPlugin";
  case FrameGenerationRuntimeState::FaultedRecoveringNative:
    return "FaultedRecoveringNative";
  case FrameGenerationRuntimeState::ShuttingDown:
    return "ShuttingDown";
  }
  return "Unknown";
}

const char *swapchainOwnershipName(
    SwapchainOwnership ownership) noexcept {
  return ownership == SwapchainOwnership::Native ? "native" : "proxy";
}

namespace {

bool environmentDisabled() {
  const char *value = std::getenv("XPBD_STREAMLINE");
  if (value == nullptr) {
    return false;
  }
  return value[0] == '0' || value[0] == 'n' || value[0] == 'N' ||
         value[0] == 'f' || value[0] == 'F';
}

#if XPBD_WITH_STREAMLINE && defined(_WIN32)
bool environmentEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0' &&
         value[0] != 'n' && value[0] != 'N' && value[0] != 'f' &&
         value[0] != 'F';
}

std::uint32_t environmentFailureCount(const char *name,
                                      std::uint32_t maximum) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return 0u;
  }
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || end == nullptr || end[0] != '\0') {
    return 0u;
  }
  return static_cast<std::uint32_t>((std::min)(
      parsed, static_cast<unsigned long>(maximum)));
}

bool verifyAuthenticodeSignature(const wchar_t *path) {
  if (path == nullptr || path[0] == L'\0') {
    return false;
  }
  HMODULE wintrust = LoadLibraryExW(
      L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (wintrust == nullptr) {
    return false;
  }
  using WinVerifyTrustFn = LONG(WINAPI *)(HWND, GUID *, LPVOID);
  const auto verify = reinterpret_cast<WinVerifyTrustFn>(
      GetProcAddress(wintrust, "WinVerifyTrust"));
  if (verify == nullptr) {
    FreeLibrary(wintrust);
    return false;
  }

  WINTRUST_FILE_INFO file{};
  file.cbStruct = sizeof(file);
  file.pcwszFilePath = path;
  WINTRUST_DATA trust{};
  trust.cbStruct = sizeof(trust);
  trust.dwUIChoice = WTD_UI_NONE;
  trust.fdwRevocationChecks = WTD_REVOKE_NONE;
  trust.dwUnionChoice = WTD_CHOICE_FILE;
  trust.pFile = &file;
  trust.dwStateAction = WTD_STATEACTION_VERIFY;
  trust.dwProvFlags =
      WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = verify(nullptr, &policy, &trust);
  trust.dwStateAction = WTD_STATEACTION_CLOSE;
  (void)verify(nullptr, &policy, &trust);
  FreeLibrary(wintrust);
  return result == ERROR_SUCCESS;
}

LONG CALLBACK streamlineExceptionDiagnostic(
    EXCEPTION_POINTERS *exception) {
  if (exception == nullptr || exception->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const DWORD code = exception->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION &&
      code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_STACK_OVERFLOW) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const void *address = exception->ExceptionRecord->ExceptionAddress;
  MEMORY_BASIC_INFORMATION memory{};
  std::wstring module_path(32768, L'\0');
  std::uintptr_t module_base = 0;
  if (VirtualQuery(address, &memory, sizeof(memory)) == sizeof(memory) &&
      memory.AllocationBase != nullptr) {
    module_base =
        reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    const DWORD length = GetModuleFileNameW(
        static_cast<HMODULE>(memory.AllocationBase), module_path.data(),
        static_cast<DWORD>(module_path.size()));
    module_path.resize(length);
  } else {
    module_path = L"<unknown>";
  }
  const std::uintptr_t instruction =
      reinterpret_cast<std::uintptr_t>(address);
  xpbd::log::errorf(
      "Streamline crash diagnostic: code=0x%08lx address=0x%llx "
      "module_base=0x%llx offset=0x%llx module=%ls",
      static_cast<unsigned long>(code),
      static_cast<unsigned long long>(instruction),
      static_cast<unsigned long long>(module_base),
      static_cast<unsigned long long>(
          module_base != 0 ? instruction - module_base : 0),
      module_path.c_str());
#if defined(_M_X64)
  if (exception->ContextRecord != nullptr) {
    const std::uintptr_t stack_pointer =
        static_cast<std::uintptr_t>(exception->ContextRecord->Rsp);
    std::uintptr_t stack_words[96]{};
    SIZE_T bytes_read = 0;
    if (ReadProcessMemory(GetCurrentProcess(),
                          reinterpret_cast<const void *>(stack_pointer),
                          stack_words, sizeof(stack_words),
                          &bytes_read)) {
      const std::size_t word_count =
          static_cast<std::size_t>(bytes_read) / sizeof(stack_words[0]);
      std::size_t image_candidates = 0;
      for (std::size_t index = 0;
           index < word_count && image_candidates < 16; ++index) {
        const std::uintptr_t candidate = stack_words[index];
        MEMORY_BASIC_INFORMATION candidate_memory{};
        if (candidate < 0x10000 ||
            VirtualQuery(reinterpret_cast<const void *>(candidate),
                         &candidate_memory,
                         sizeof(candidate_memory)) !=
                sizeof(candidate_memory) ||
            candidate_memory.Type != MEM_IMAGE ||
            candidate_memory.AllocationBase == nullptr) {
          continue;
        }
        std::wstring candidate_module(32768, L'\0');
        const DWORD candidate_length = GetModuleFileNameW(
            static_cast<HMODULE>(candidate_memory.AllocationBase),
            candidate_module.data(),
            static_cast<DWORD>(candidate_module.size()));
        if (candidate_length == 0) {
          continue;
        }
        candidate_module.resize(candidate_length);
        const std::uintptr_t candidate_base =
            reinterpret_cast<std::uintptr_t>(
                candidate_memory.AllocationBase);
        xpbd::log::errorf(
            "Streamline crash stack candidate[%zu] "
            "stack_word=%zu address=0x%llx offset=0x%llx module=%ls",
            image_candidates, index,
            static_cast<unsigned long long>(candidate),
            static_cast<unsigned long long>(candidate - candidate_base),
            candidate_module.c_str());
        ++image_candidates;
      }
    }
  }
#endif
  xpbd::log::flush();
  return EXCEPTION_CONTINUE_SEARCH;
}

std::filesystem::path executableDirectory() {
  std::wstring path(32768, L'\0');
  const DWORD length =
      GetModuleFileNameW(nullptr, path.data(),
                         static_cast<DWORD>(path.size()));
  if (length == 0 || length >= path.size()) {
    return {};
  }
  path.resize(length);
  return std::filesystem::path(path).parent_path();
}

void streamlineLog(sl::LogType type, const char *message) {
  if (message == nullptr) {
    return;
  }
  switch (type) {
  case sl::LogType::eError:
    xpbd::log::errorf("Streamline: %s", message);
    break;
  case sl::LogType::eWarn:
    xpbd::log::warnf("Streamline: %s", message);
    break;
  default:
    xpbd::log::infof("Streamline: %s", message);
    break;
  }
}

template <class Function>
Function loadExport(HMODULE module, const char *name) {
  return reinterpret_cast<Function>(GetProcAddress(module, name));
}
#endif

} // namespace

struct StreamlineVulkanRuntime::Impl {
  std::string status =
#if XPBD_WITH_STREAMLINE
      "Streamline not initialized";
#else
      "Streamline SDK was not available at build time";
#endif
  bool initialized = false;
  bool shutdown = false;
  bool dlss_plugin_available = false;
  bool dlss_rr_plugin_available = false;
  bool frame_generation_plugin_available = false;
  bool reflex_plugin_available = false;
  bool pcl_plugin_available = false;
  bool dlss_feature_loaded = false;
  bool dlss_rr_feature_loaded = false;
  bool reflex_feature_loaded = false;
  bool pcl_feature_loaded = false;
  // Last request/submission result, deliberately separate from packaged,
  // loaded, and adapter-supported state so a transient fault cannot poison
  // an independent feature's capability.
  bool dlss_runtime_faulted = false;
  bool dlss_rr_runtime_faulted = false;
  bool dlss_supported = false;
  bool dlss_rr_supported = false;
  bool dlss_g_vulkan_supported = false;
  bool reflex_supported = false;
  bool pcl_supported = false;
  bool reflex_options_valid = false;
  PathTraceReflexMode configured_reflex_mode =
      PathTraceReflexMode::On;
  FrameGenerationDiagnostic frame_generation{};
  // Compatibility mirrors used by the existing SR/RR/UI call sites while
  // the transition boundary is migrated. They are written only through the
  // state helpers below and are not used to decide swapchain ownership.
  bool frame_generation_supported = false;
  bool frame_generation_feature_loaded = false;
  bool frame_generation_options_enabled = false;
  bool frame_generation_active = false;
  // Result of the most recent DLSS-G GetState call.  This is deliberately
  // independent from the lifecycle enum: ProxyArmed/Active describe our
  // transaction, while state_ok records the SDK's runtime validation.
  bool frame_generation_state_ok = false;
  std::uint32_t frames_actually_presented = 1u;
  std::string frame_generation_status =
      "DLSS Frame Generation not initialized";
  std::atomic_bool requested_frame_generation{false};
  bool frame_generation_failure_latched = false;
  bool frame_generation_context_available = false;
  bool frame_generation_recovery_required = false;
  FrameGenerationDisableProgress frame_generation_disable{};
  std::int32_t frame_generation_disable_result = 0;
  bool frame_generation_shutdown_requested = false;
  std::uint32_t frame_generation_injected_disable_failures_remaining = 0u;
  std::uint64_t frame_generation_present_thread_id = 0u;
  std::uint32_t frame_generation_present_frame_index = 0u;
  bool frame_generation_state_poll_pending = false;
  bool frame_generation_state_poll_inputs_ready = false;
  std::uint32_t frame_generation_state_poll_frame_index = 0u;
  std::uint32_t frame_generation_tag_frame_index = 0u;
  std::uint64_t frame_generation_generation = 0u;
  bool force_history_reset = true;
  // A transient invalid guide frame pauses DLSS-G without destroying its
  // proxy swapchain. The first valid frame after the pause must carry a
  // reset=true common-constants update. If SR/RR already submitted constants
  // for that token, reuse them: Streamline permits only one update per frame.
  bool frame_generation_resume_reset_pending = false;
  // These are lifecycle state, not Streamline SDK objects. Keep them present
  // in optional-SDK builds so diagnostics and native swapchain fallbacks share
  // the same state contract.
  std::uint32_t current_frame_index =
      (std::numeric_limits<std::uint32_t>::max)();
  std::uint32_t constants_frame_index =
      (std::numeric_limits<std::uint32_t>::max)();
  SwapchainOwnership swapchain_ownership = SwapchainOwnership::Native;
  bool swapchain_present = false;

#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };

  HMODULE module = nullptr;
  PFun_slInit *sl_init = nullptr;
  PFun_slShutdown *sl_shutdown = nullptr;
  PFun_slIsFeatureSupported *sl_is_feature_supported = nullptr;
  PFun_slIsFeatureLoaded *sl_is_feature_loaded = nullptr;
  PFun_slSetFeatureLoaded *sl_set_feature_loaded = nullptr;
  PFun_slGetFeatureRequirements *sl_get_feature_requirements = nullptr;
  PFun_slSetTagForFrame *sl_set_tag_for_frame = nullptr;
  PFun_slSetConstants *sl_set_constants = nullptr;
  PFun_slEvaluateFeature *sl_evaluate_feature = nullptr;
  PFun_slGetFeatureFunction *sl_get_feature_function = nullptr;
  PFun_slGetNewFrameToken *sl_get_new_frame_token = nullptr;
  PFun_slFreeResources *sl_free_resources = nullptr;
  PFun_slDLSSGetOptimalSettings *sl_dlss_get_optimal_settings = nullptr;
  PFun_slDLSSSetOptions *sl_dlss_set_options = nullptr;
  PFun_slDLSSDGetOptimalSettings *sl_dlssd_get_optimal_settings = nullptr;
  PFun_slDLSSDSetOptions *sl_dlssd_set_options = nullptr;
  PFun_slDLSSGGetState *sl_dlss_g_get_state = nullptr;
  PFun_slDLSSGSetOptions *sl_dlss_g_set_options = nullptr;
  PFun_slReflexGetState *sl_reflex_get_state = nullptr;
  PFun_slReflexSleep *sl_reflex_sleep = nullptr;
  PFun_slReflexSetOptions *sl_reflex_set_options = nullptr;
  PFun_slPCLGetState *sl_pcl_get_state = nullptr;
  PFun_slPCLSetOptions *sl_pcl_set_options = nullptr;
  PFun_slPCLSetMarker *sl_pcl_set_marker = nullptr;
  std::uint32_t pcl_stats_window_message = 0u;
  sl::FrameToken *current_frame_token = nullptr;
  bool simulation_marker_open = false;
  sl::DLSSGOptions frame_generation_options{};
  struct FrameGenerationOptionsKey {
    bool valid = false;
    sl::DLSSGMode mode = sl::DLSSGMode::eOff;
    std::uint32_t num_frames_to_generate = 0u;
    std::uint32_t num_back_buffers = 0u;
    std::uint32_t mvec_depth_width = 0u;
    std::uint32_t mvec_depth_height = 0u;
    std::uint32_t color_width = 0u;
    std::uint32_t color_height = 0u;
    std::uint32_t color_format = 0u;
    std::uint32_t mvec_format = 0u;
    std::uint32_t depth_format = 0u;
    std::uint32_t hudless_format = 0u;
    std::uint32_t ui_format = 0u;
    sl::DLSSGQueueParallelismMode queue_mode =
        sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    sl::Boolean ui_recomposition = sl::Boolean::eFalse;
    bool operator==(const FrameGenerationOptionsKey &other) const noexcept {
      return valid == other.valid && mode == other.mode &&
             num_frames_to_generate == other.num_frames_to_generate &&
             num_back_buffers == other.num_back_buffers &&
             mvec_depth_width == other.mvec_depth_width &&
             mvec_depth_height == other.mvec_depth_height &&
             color_width == other.color_width &&
             color_height == other.color_height &&
             color_format == other.color_format &&
             mvec_format == other.mvec_format &&
             depth_format == other.depth_format &&
             hudless_format == other.hudless_format &&
             ui_format == other.ui_format && queue_mode == other.queue_mode &&
             ui_recomposition == other.ui_recomposition;
    }
  } frame_generation_options_key{};
  std::atomic<std::int32_t> frame_generation_api_error{VK_SUCCESS};
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  std::array<Image, 2> dlss_outputs{};
  std::uint32_t last_output_slot = 0;
  PathTraceUpscale configured_mode = PathTraceUpscale::Off;
  std::uint32_t configured_output_width = 0;
  std::uint32_t configured_output_height = 0;
  bool configured_ray_reconstruction = false;
  std::string last_tag_validation_failure;

  PFN_vkCreateInstance create_instance = nullptr;
  PFN_vkEnumeratePhysicalDevices enumerate_physical_devices = nullptr;
  PFN_vkCreateDevice create_device = nullptr;
  PFN_vkCreateWin32SurfaceKHR create_win32_surface = nullptr;
  PFN_vkDestroySurfaceKHR destroy_surface = nullptr;
  PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
  PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
  PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
  PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
  PFN_vkQueuePresentKHR queue_present = nullptr;
  PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
  void *exception_diagnostic = nullptr;
#endif
};

void setFrameGenerationState(
    StreamlineVulkanRuntime::Impl &impl,
    FrameGenerationRuntimeState state, std::uint64_t frame_index,
    const char *reason) noexcept {
  const FrameGenerationRuntimeState previous = impl.frame_generation.state;
  impl.frame_generation.state = state;
  impl.frame_generation.requested =
      impl.requested_frame_generation.load(std::memory_order_acquire);
  impl.frame_generation.supported = impl.frame_generation_supported;
  impl.frame_generation.plugin_loaded =
      impl.frame_generation_feature_loaded;
  impl.frame_generation.proxy_swapchain =
      impl.swapchain_ownership ==
      SwapchainOwnership::StreamlineFrameGenerationProxy;
  impl.frame_generation.ownership = impl.swapchain_ownership;
  impl.frame_generation.options_on =
      impl.frame_generation_options_enabled;
  impl.frame_generation.valid_inputs_tagged =
      impl.frame_generation.tag_generation != 0u;
  impl.frame_generation.state_ok = impl.frame_generation_state_ok;
  impl.frame_generation.frames_actually_presented =
      impl.frames_actually_presented;
  impl.frame_generation.disable_attempts =
      impl.frame_generation_disable.attempts;
  impl.frame_generation.disable_completed_drains =
      impl.frame_generation_disable.completed_drains;
  impl.frame_generation.disable_result =
      impl.frame_generation_disable_result;
  impl.frame_generation.disable_confirmed_off =
      impl.frame_generation_disable.confirmed_off;
  impl.frame_generation.disable_exhausted =
      impl.frame_generation_disable.exhausted;
  impl.frame_generation.failure_latched =
      impl.frame_generation_failure_latched;
  impl.frame_generation.recovery_required =
      impl.frame_generation_recovery_required;
  impl.frame_generation.present_thread_id =
      impl.frame_generation_present_thread_id;
  impl.frame_generation.present_thread_bound =
      impl.frame_generation_present_thread_id != 0u;
  impl.frame_generation.swapchain_generation =
      impl.frame_generation_generation;
  impl.frame_generation.constants_frame_index =
      impl.constants_frame_index;
  impl.frame_generation.tag_frame_index =
      impl.frame_generation_tag_frame_index;
  impl.frame_generation.present_frame_index =
      impl.frame_generation_present_frame_index;
  impl.frame_generation.status = impl.frame_generation_status;
  const bool legal = frameGenerationRuntimeCombinationIsLegal(
      state, impl.frame_generation.plugin_loaded,
      impl.frame_generation.ownership, impl.frame_generation.options_on,
      impl.frame_generation.valid_inputs_tagged);
#ifndef NDEBUG
  assert(legal && "illegal DLSS-G runtime state combination");
#else
  if (!legal) {
    xpbd::log::errorf(
        "DLSSG invariant violation: state=%s plugin=%d swapchain=%s "
        "options=%d tags=%d",
        frameGenerationRuntimeStateName(state),
        impl.frame_generation.plugin_loaded ? 1 : 0,
        swapchainOwnershipName(impl.frame_generation.ownership),
        impl.frame_generation.options_on ? 1 : 0,
        impl.frame_generation.valid_inputs_tagged ? 1 : 0);
  }
#endif
  if (previous != state) {
    xpbd::log::infof(
        "DLSSG transition: from=%s to=%s requested=%d plugin=%d "
        "swapchain=%s options=%s tags=%s frame=%llu generation=%llu "
        "reason=%s",
        frameGenerationRuntimeStateName(previous),
        frameGenerationRuntimeStateName(state),
        impl.frame_generation.requested ? 1 : 0,
        impl.frame_generation.plugin_loaded ? 1 : 0,
        swapchainOwnershipName(impl.frame_generation.ownership),
        impl.frame_generation.options_on ? "on" : "off",
        impl.frame_generation.valid_inputs_tagged ? "valid" : "null",
        static_cast<unsigned long long>(frame_index),
        static_cast<unsigned long long>(impl.frame_generation_generation),
        reason != nullptr ? reason : "unspecified");
  }
}

[[nodiscard]] bool frameGenerationOptionsKeyIsValid(
    const StreamlineVulkanRuntime::Impl &impl) noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  return impl.frame_generation_options_key.valid;
#else
  (void)impl;
  return false;
#endif
}

[[nodiscard]] bool frameGenerationRuntimeMayDestroy(
    const StreamlineVulkanRuntime::Impl &impl) noexcept {
  const bool options_key_valid = frameGenerationOptionsKeyIsValid(impl);
  const bool valid_inputs_tagged =
      impl.frame_generation.tag_generation != 0u;
  const bool already_native_and_clear =
      impl.swapchain_ownership == SwapchainOwnership::Native &&
      !impl.frame_generation_options_enabled && !options_key_valid &&
      !valid_inputs_tagged;
  return already_native_and_clear || frameGenerationDisableMayDestroy(
                                         impl.frame_generation_disable,
                                         impl.frame_generation_options_enabled,
                                         options_key_valid,
                                         valid_inputs_tagged);
}

void synchronizeFrameGenerationDisableFailure(
    StreamlineVulkanRuntime::Impl &impl) noexcept {
  impl.frame_generation_failure_latched |=
      impl.frame_generation_disable.failure_latched;
  impl.frame_generation_recovery_required |=
      impl.frame_generation_disable.recovery_required;
  impl.frame_generation_state_ok = false;
}

#if XPBD_WITH_STREAMLINE && defined(_WIN32)
namespace {

std::atomic<std::int32_t> g_dlss_g_api_error{VK_SUCCESS};

void onDlssGApiError(const sl::APIError &error) {
  g_dlss_g_api_error.store(
      static_cast<std::int32_t>(error.vkRes), std::memory_order_release);
}

sl::DLSSMode dlssMode(PathTraceUpscale mode) {
  switch (mode) {
  case PathTraceUpscale::Dlaa:
    return sl::DLSSMode::eDLAA;
  case PathTraceUpscale::UltraQuality:
    return sl::DLSSMode::eMaxQuality;
  case PathTraceUpscale::Quality:
    return sl::DLSSMode::eMaxQuality;
  case PathTraceUpscale::Balanced:
    return sl::DLSSMode::eBalanced;
  case PathTraceUpscale::Performance:
    return sl::DLSSMode::eMaxPerformance;
  case PathTraceUpscale::UltraPerformance:
    return sl::DLSSMode::eUltraPerformance;
  case PathTraceUpscale::Auto:
    return sl::DLSSMode::eMaxQuality;
  case PathTraceUpscale::Off:
  default:
    return sl::DLSSMode::eOff;
  }
}

sl::DLSSOptions makeDlssOptions(
    PathTraceUpscale mode, std::uint32_t width,
    std::uint32_t height) {
  sl::DLSSOptions options{};
  options.mode = dlssMode(mode);
  options.outputWidth = width;
  options.outputHeight = height;
  options.preExposure = 1.0f;
  options.exposureScale = 1.0f;
  options.colorBuffersHDR = sl::Boolean::eTrue;
  options.useAutoExposure = sl::Boolean::eTrue;
  // Alpha upscaling is experimental and temporal. XPBD composites DLSS RGB
  // with a current-frame, spatially upsampled PT coverage mask instead, so a
  // rotating camera cannot drag stale foreground alpha over the raster sky.
  options.alphaUpscalingEnabled = sl::Boolean::eFalse;
  options.dlaaPreset = sl::DLSSPreset::ePresetK;
  options.qualityPreset = sl::DLSSPreset::ePresetK;
  options.balancedPreset = sl::DLSSPreset::ePresetK;
  options.performancePreset = sl::DLSSPreset::ePresetM;
  options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
  options.ultraQualityPreset = sl::DLSSPreset::ePresetK;
  return options;
}

std::uint32_t findMemoryType(
    VkPhysicalDevice physical_device, std::uint32_t bits,
    VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memory_properties{};
  vkGetPhysicalDeviceMemoryProperties(
      physical_device, &memory_properties);
  for (std::uint32_t index = 0;
       index < memory_properties.memoryTypeCount; ++index) {
    if ((bits & (1u << index)) != 0u &&
        (memory_properties.memoryTypes[index].propertyFlags &
         properties) == properties) {
      return index;
    }
  }
  return (std::numeric_limits<std::uint32_t>::max)();
}

void destroyDlssImage(
    VkDevice device, StreamlineVulkanRuntime::Impl::Image &image) {
  if (image.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device, image.view, nullptr);
  }
  if (image.image != VK_NULL_HANDLE) {
    vkDestroyImage(device, image.image, nullptr);
  }
  if (image.memory != VK_NULL_HANDLE) {
    vkFreeMemory(device, image.memory, nullptr);
  }
  image = {};
}

bool createDlssImage(
    VkPhysicalDevice physical_device, VkDevice device,
    std::uint32_t width, std::uint32_t height,
    StreamlineVulkanRuntime::Impl::Image &image) {
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image_info.extent = {width, height, 1u};
  image_info.mipLevels = 1u;
  image_info.arrayLayers = 1u;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device, &image_info, nullptr, &image.image) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, image.image, &requirements);
  const std::uint32_t memory_type = findMemoryType(
      physical_device, requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memory_type ==
      (std::numeric_limits<std::uint32_t>::max)()) {
    destroyDlssImage(device, image);
    return false;
  }
  VkMemoryAllocateInfo allocation{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device, &allocation, nullptr, &image.memory) !=
          VK_SUCCESS ||
      vkBindImageMemory(device, image.image, image.memory, 0u) !=
          VK_SUCCESS) {
    destroyDlssImage(device, image);
    return false;
  }
  VkImageViewCreateInfo view_info{
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image.image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  view_info.subresourceRange = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  if (vkCreateImageView(device, &view_info, nullptr, &image.view) !=
      VK_SUCCESS) {
    destroyDlssImage(device, image);
    return false;
  }
  image.width = width;
  image.height = height;
  image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void copyColumnMajorToRowVector(
    const float *source, sl::float4x4 &destination) {
  // A column-major column-vector matrix has the same contiguous order as its
  // row-vector transpose, which is what Streamline's row-major constants use.
  std::memcpy(&destination[0].x, source, sizeof(float) * 16u);
}

void glProjectionToVulkan(const float *source, float *destination) {
  for (std::uint32_t column = 0; column < 4u; ++column) {
    const float x = source[column * 4u + 0u];
    const float y = source[column * 4u + 1u];
    const float z = source[column * 4u + 2u];
    const float w = source[column * 4u + 3u];
    destination[column * 4u + 0u] = x;
    destination[column * 4u + 1u] = -y;
    destination[column * 4u + 2u] = 0.5f * z + 0.5f * w;
    destination[column * 4u + 3u] = w;
  }
}

sl::float4x4 identityMatrix() {
  return {
      sl::float4{1.0f, 0.0f, 0.0f, 0.0f},
      sl::float4{0.0f, 1.0f, 0.0f, 0.0f},
      sl::float4{0.0f, 0.0f, 1.0f, 0.0f},
      sl::float4{0.0f, 0.0f, 0.0f, 1.0f},
  };
}

sl::DLSSDOptions makeDlssdOptions(
    PathTraceUpscale mode, std::uint32_t width,
    std::uint32_t height, const float *world_to_view = nullptr) {
  sl::DLSSDOptions options{};
  options.mode = dlssMode(mode);
  options.outputWidth = width;
  options.outputHeight = height;
  options.preExposure = 1.0f;
  options.exposureScale = 1.0f;
  options.colorBuffersHDR = sl::Boolean::eTrue;
  options.normalRoughnessMode =
      sl::DLSSDNormalRoughnessMode::ePacked;
  // RR reconstructs RGB/radiance only. Current-frame PT coverage owns the
  // later foreground/sky composite and is intentionally not temporal.
  options.alphaUpscalingEnabled = sl::Boolean::eFalse;
  options.dlaaPreset = sl::DLSSDPreset::ePresetD;
  options.qualityPreset = sl::DLSSDPreset::ePresetD;
  options.balancedPreset = sl::DLSSDPreset::ePresetD;
  options.performancePreset = sl::DLSSDPreset::ePresetD;
  options.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;
  options.ultraQualityPreset = sl::DLSSDPreset::ePresetD;
  if (world_to_view != nullptr) {
    copyColumnMajorToRowVector(
        world_to_view, options.worldToCameraView);
    sl::matrixFullInvert(
        options.cameraViewToWorld, options.worldToCameraView);
  } else {
    options.worldToCameraView = identityMatrix();
    options.cameraViewToWorld = identityMatrix();
  }
  return options;
}

sl::Resource textureResource(
    VkImage image, VkDeviceMemory memory, VkImageView view,
    VkImageLayout layout, VkFormat format, VkImageUsageFlags usage,
    std::uint32_t width, std::uint32_t height,
    std::uint32_t array_layers = 1u) {
  sl::Resource resource{
      sl::ResourceType::eTex2d, reinterpret_cast<void *>(image),
      reinterpret_cast<void *>(memory), reinterpret_cast<void *>(view),
      static_cast<std::uint32_t>(layout)};
  resource.width = width;
  resource.height = height;
  resource.nativeFormat = static_cast<std::uint32_t>(format);
  resource.mipLevels = 1u;
  resource.arrayLayers = array_layers;
  resource.flags = 0u;
  resource.usage = usage;
  return resource;
}

struct TaggedTextureExpectation {
  sl::BufferType type = sl::kBufferTypeDepth;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageUsageFlags required_usage = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
};

const char *taggedTextureValidationFailure(
    const sl::ResourceTag &tag,
    const TaggedTextureExpectation &expected) noexcept {
  if (tag.type != expected.type) {
    return "buffer-type-mismatch";
  }
  if (tag.lifecycle != sl::ResourceLifecycle::eOnlyValidNow) {
    return "unexpected-lifecycle";
  }
  if (tag.resource == nullptr) {
    return "null-resource-description";
  }
  const sl::Resource &resource = *tag.resource;
  if (resource.type != sl::ResourceType::eTex2d) {
    return "resource-is-not-tex2d";
  }
  if (resource.native == nullptr) {
    return "null-image";
  }
  if (resource.memory == nullptr) {
    return "null-memory";
  }
  if (resource.view == nullptr) {
    return "null-view";
  }
  if (resource.nativeFormat ==
      static_cast<std::uint32_t>(VK_FORMAT_UNDEFINED)) {
    return "undefined-format";
  }
  if (resource.nativeFormat !=
      static_cast<std::uint32_t>(expected.format)) {
    return "format-mismatch";
  }
  if (resource.state == (std::numeric_limits<std::uint32_t>::max)() ||
      resource.state ==
          static_cast<std::uint32_t>(VK_IMAGE_LAYOUT_UNDEFINED)) {
    return "undefined-layout";
  }
  if (resource.state != static_cast<std::uint32_t>(expected.layout)) {
    return "layout-mismatch";
  }
  if (resource.width != expected.width ||
      resource.height != expected.height || resource.width == 0u ||
      resource.height == 0u) {
    return "resource-extent-mismatch";
  }
  if (resource.mipLevels != 1u) {
    return "invalid-mip-count";
  }
  if (resource.arrayLayers != 1u) {
    return "invalid-array-layer-count";
  }
  if ((resource.usage & expected.required_usage) !=
      expected.required_usage) {
    return "missing-required-usage";
  }
  if (tag.extent.width == 0u || tag.extent.height == 0u) {
    return "empty-tag-extent";
  }
  if (tag.extent.left > resource.width ||
      tag.extent.width > resource.width - tag.extent.left ||
      tag.extent.top > resource.height ||
      tag.extent.height > resource.height - tag.extent.top) {
    return "tag-extent-out-of-bounds";
  }
  if (tag.extent.left != 0u || tag.extent.top != 0u ||
      tag.extent.width != expected.width ||
      tag.extent.height != expected.height) {
    return "tag-extent-mismatch";
  }
  return nullptr;
}

template <std::size_t Count>
bool validateTaggedTextureSet(
    const char *feature, std::uint32_t frame_index,
    const std::array<sl::ResourceTag, Count> &tags,
    const std::array<TaggedTextureExpectation, Count> &expectations,
    std::string &last_failure, std::string &status) {
  for (std::size_t index = 0; index < Count; ++index) {
    const char *reason =
        taggedTextureValidationFailure(tags[index], expectations[index]);
    if (reason == nullptr) {
      continue;
    }

    const char *tag_name = sl::getBufferTypeAsStr(expectations[index].type);
    const std::string signature =
        std::string(feature) + "/" + tag_name + "/" + reason;
    status = std::string(feature) +
             " resource validation failed: tag=" + tag_name +
             " reason=" + reason;
    if (last_failure != signature) {
      const sl::Resource *resource = tags[index].resource;
      xpbd::log::errorf(
          "SL_TAG_VALIDATION_FAILED feature=%s frame=%u tag=%s "
          "reason=%s image=%p memory=%p view=%p format=%u "
          "expected_format=%u layout=%u expected_layout=%u "
          "size=%ux%u expected_size=%ux%u mips=%u layers=%u "
          "usage=0x%08x required_usage=0x%08x "
          "extent={top:%u,left:%u,width:%u,height:%u}",
          feature, frame_index, tag_name, reason,
          resource != nullptr ? resource->native : nullptr,
          resource != nullptr ? resource->memory : nullptr,
          resource != nullptr ? resource->view : nullptr,
          resource != nullptr ? resource->nativeFormat : 0u,
          static_cast<unsigned>(expectations[index].format),
          resource != nullptr ? resource->state : 0u,
          static_cast<unsigned>(expectations[index].layout),
          resource != nullptr ? resource->width : 0u,
          resource != nullptr ? resource->height : 0u,
          expectations[index].width, expectations[index].height,
          resource != nullptr ? resource->mipLevels : 0u,
          resource != nullptr ? resource->arrayLayers : 0u,
          resource != nullptr ? resource->usage : 0u,
          static_cast<unsigned>(expectations[index].required_usage),
          tags[index].extent.top, tags[index].extent.left,
          tags[index].extent.width, tags[index].extent.height);
    }
    last_failure = signature;
    return false;
  }
  last_failure.clear();
  return true;
}

void transitionDlssOutput(
    VkCommandBuffer command_buffer,
    StreamlineVulkanRuntime::Impl::Image &image,
    VkImageLayout new_layout) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = image.layout;
  barrier.newLayout = new_layout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image.image;
  barrier.subresourceRange = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
  VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  VkPipelineStageFlags destination_stage =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  if (image.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    source_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  } else if (image.layout == VK_IMAGE_LAYOUT_GENERAL) {
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    source_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  }
  if (new_layout == VK_IMAGE_LAYOUT_GENERAL) {
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  } else {
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  }
  vkCmdPipelineBarrier(
      command_buffer, source_stage, destination_stage, 0u, 0u, nullptr, 0u,
      nullptr, 1u, &barrier);
  image.layout = new_layout;
}

sl::Constants makeDlssConstants(const StreamlineDlssFrame &frame) {
  float current_projection[16]{};
  float previous_projection[16]{};
  glProjectionToVulkan(frame.projection, current_projection);
  glProjectionToVulkan(
      frame.previous_projection != nullptr ? frame.previous_projection
                                           : frame.projection,
      previous_projection);

  sl::float4x4 current_world_to_view{};
  sl::float4x4 previous_world_to_view{};
  sl::float4x4 previous_view_to_clip{};
  sl::Constants constants{};
  copyColumnMajorToRowVector(
      current_projection, constants.cameraViewToClip);
  copyColumnMajorToRowVector(
      frame.view, current_world_to_view);
  copyColumnMajorToRowVector(
      frame.previous_view != nullptr ? frame.previous_view : frame.view,
      previous_world_to_view);
  copyColumnMajorToRowVector(
      previous_projection, previous_view_to_clip);

  sl::matrixFullInvert(
      constants.clipToCameraView, constants.cameraViewToClip);
  sl::float4x4 current_view_to_world{};
  sl::matrixFullInvert(current_view_to_world, current_world_to_view);
  sl::float4x4 current_view_to_previous_view{};
  sl::matrixMul(
      current_view_to_previous_view, current_view_to_world,
      previous_world_to_view);
  sl::float4x4 clip_to_previous_view{};
  sl::matrixMul(
      clip_to_previous_view, constants.clipToCameraView,
      current_view_to_previous_view);
  sl::matrixMul(
      constants.clipToPrevClip, clip_to_previous_view,
      previous_view_to_clip);
  sl::matrixFullInvert(
      constants.prevClipToClip, constants.clipToPrevClip);
  constants.clipToLensClip = identityMatrix();

  constants.jitterOffset = {frame.jitter_x, frame.jitter_y};
  constants.mvecScale = {
      1.0f / static_cast<float>(frame.render_width),
      1.0f / static_cast<float>(frame.render_height)};
  constants.cameraPinholeOffset = {0.0f, 0.0f};
  constants.cameraPos = {
      current_view_to_world[3].x, current_view_to_world[3].y,
      current_view_to_world[3].z};
  constants.cameraRight = {
      current_view_to_world[0].x, current_view_to_world[0].y,
      current_view_to_world[0].z};
  constants.cameraUp = {
      current_view_to_world[1].x, current_view_to_world[1].y,
      current_view_to_world[1].z};
  constants.cameraFwd = {
      -current_view_to_world[2].x, -current_view_to_world[2].y,
      -current_view_to_world[2].z};

  const float projection_z = frame.projection[10];
  const float projection_w = frame.projection[14];
  const float near_plane = projection_w / (projection_z - 1.0f);
  const float far_plane = projection_w / (projection_z + 1.0f);
  constants.cameraNear =
      std::isfinite(near_plane) && near_plane > 0.0f ? near_plane : 0.01f;
  constants.cameraFar =
      std::isfinite(far_plane) && far_plane > constants.cameraNear
          ? far_plane
          : 10000.0f;
  constants.cameraFOV =
      2.0f * std::atan(
                 1.0f / (std::max)(std::abs(frame.projection[5]),
                                   1.0e-6f));
  constants.cameraAspectRatio =
      std::abs(frame.projection[5]) /
      (std::max)(std::abs(frame.projection[0]), 1.0e-6f);
  constants.motionVectorsInvalidValue = 0.0f;
  constants.depthInverted = sl::Boolean::eFalse;
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset =
      frame.reset_history ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  // Some Streamline documentation revisions expose
  // Constants::renderingGameFrames while the official 2.12.0 production
  // headers omit it from the v2 ABI. Set it whenever the selected SDK header
  // provides the member without changing the ABI used by 2.12.0.
  [&]<class Constants>(Constants &value) {
    if constexpr (requires { value.renderingGameFrames; }) {
      value.renderingGameFrames = sl::Boolean::eTrue;
    }
  }(constants);
  constants.orthographicProjection = sl::Boolean::eFalse;
  constants.motionVectorsDilated = sl::Boolean::eFalse;
  constants.motionVectorsJittered = sl::Boolean::eFalse;
  return constants;
}

sl::FrameToken *frameTokenFor(
    StreamlineVulkanRuntime::Impl &impl,
    std::uint32_t frame_index) {
  if (impl.current_frame_token != nullptr &&
      impl.current_frame_index == frame_index) {
    return impl.current_frame_token;
  }
  if (impl.sl_get_new_frame_token == nullptr) {
    return nullptr;
  }
  sl::FrameToken *token = nullptr;
  const sl::Result result =
      impl.sl_get_new_frame_token(token, &frame_index);
  if (result != sl::Result::eOk || token == nullptr) {
    impl.status =
        std::string("Streamline frame-token acquisition failed: ") +
        sl::getResultAsStr(result);
    return nullptr;
  }
  impl.current_frame_token = token;
  impl.current_frame_index = frame_index;
  return token;
}

sl::ReflexMode reflexMode(
    PathTraceReflexMode requested,
    bool frame_generation_requested) {
  if (frame_generation_requested &&
      requested == PathTraceReflexMode::Off) {
    return sl::ReflexMode::eLowLatency;
  }
  switch (requested) {
  case PathTraceReflexMode::Off:
    return sl::ReflexMode::eOff;
  case PathTraceReflexMode::OnBoost:
    return sl::ReflexMode::eLowLatencyWithBoost;
  case PathTraceReflexMode::On:
  default:
    return sl::ReflexMode::eLowLatency;
  }
}

void setPclMarker(StreamlineVulkanRuntime::Impl &impl,
                  sl::PCLMarker marker) {
  if (!impl.pcl_supported || impl.sl_pcl_set_marker == nullptr ||
      impl.current_frame_token == nullptr) {
    return;
  }
  const sl::Result result =
      impl.sl_pcl_set_marker(marker, *impl.current_frame_token);
  if (result != sl::Result::eOk) {
    xpbd::log::warnf("Streamline PCL marker %u failed: %s",
                     static_cast<unsigned>(marker),
                     sl::getResultAsStr(result));
  }
}

} // namespace
#endif

StreamlineVulkanRuntime::StreamlineVulkanRuntime()
    : impl_(std::make_unique<Impl>()) {
  impl_->frame_generation.state =
      FrameGenerationRuntimeState::Unsupported;
  impl_->frame_generation.status = impl_->frame_generation_status;
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  // Developer-only deterministic G03 fixture. It is read once, defaults to
  // zero, and can inject only real options-enabled eOff attempts.
  impl_->frame_generation_injected_disable_failures_remaining =
      environmentFailureCount("XPBD_R0F_FG_DISABLE_FAIL_COUNT",
                              kFrameGenerationDisableMaxAttempts);
  if (impl_->frame_generation_injected_disable_failures_remaining != 0u) {
    xpbd::log::warnf(
        "R0F_G03_DISABLE_FAILURE_INJECTION armed count=%u",
        impl_->frame_generation_injected_disable_failures_remaining);
  }
#endif
}

StreamlineVulkanRuntime::~StreamlineVulkanRuntime() {
  shutdownBeforeVulkan();
  releaseAfterVulkan();
}

bool StreamlineVulkanRuntime::initializeBeforeVulkan() {
  if (impl_->initialized) {
    return true;
  }
  if (environmentDisabled()) {
    impl_->status = "Streamline disabled by XPBD_STREAMLINE";
    xpbd::log::info(impl_->status);
    return false;
  }
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (environmentEnabled("XPBD_STREAMLINE_CRASH_DIAGNOSTICS") &&
      impl_->exception_diagnostic == nullptr) {
    impl_->exception_diagnostic = AddVectoredExceptionHandler(
        1, streamlineExceptionDiagnostic);
  }
  const std::filesystem::path directory = executableDirectory();
  const std::filesystem::path interposer = directory / "sl.interposer.dll";
  if (directory.empty() || !std::filesystem::is_regular_file(interposer)) {
    impl_->status =
        "Signed Streamline runtime is missing beside the executable";
    xpbd::log::warn(impl_->status);
    return false;
  }
  auto signed_streamline_file =
      [&](const wchar_t *name, const char *feature) {
    const std::filesystem::path path = directory / name;
    const std::wstring native_path = path.native();
    if (!std::filesystem::is_regular_file(path) ||
        !sl::security::verifyEmbeddedSignature(native_path.c_str())) {
      xpbd::log::errorf(
          "Streamline %s runtime is missing or unsigned: %s", feature,
          path.filename().string().c_str());
      return false;
    }
    return true;
  };
  auto signed_ngx_file = [&](const wchar_t *name, const char *feature) {
    const std::filesystem::path path = directory / name;
    const std::wstring native_path = path.native();
    if (!std::filesystem::is_regular_file(path) ||
        !verifyAuthenticodeSignature(native_path.c_str())) {
      xpbd::log::errorf(
          "NVIDIA NGX %s runtime is missing or has an invalid "
          "Authenticode signature: %s",
          feature, path.filename().string().c_str());
      return false;
    }
    return true;
  };

  const bool interposer_signed =
      signed_streamline_file(L"sl.interposer.dll", "core");
  const bool common_signed =
      signed_streamline_file(L"sl.common.dll", "core");
  const bool core_signed = interposer_signed && common_signed;
  if (!core_signed) {
    impl_->status = "Signed Streamline core runtime is incomplete";
    return false;
  }
#if XPBD_STREAMLINE_HAS_DLSS_SR
  {
    const bool plugin =
        signed_streamline_file(L"sl.dlss.dll", "DLSS SR");
    const bool ngx = signed_ngx_file(L"nvngx_dlss.dll", "DLSS SR");
    impl_->dlss_plugin_available = plugin && ngx;
  }
#endif
#if XPBD_STREAMLINE_HAS_DLSS_RR
  {
    const bool plugin =
        signed_streamline_file(L"sl.dlss_d.dll", "DLSS RR");
    const bool ngx = signed_ngx_file(L"nvngx_dlssd.dll", "DLSS RR");
    impl_->dlss_rr_plugin_available =
        impl_->dlss_plugin_available && plugin && ngx;
  }
#endif
#if XPBD_STREAMLINE_HAS_REFLEX
  {
    const bool plugin =
        signed_streamline_file(L"sl.reflex.dll", "Reflex");
    const bool low_latency =
        signed_streamline_file(L"NvLowLatencyVk.dll", "Reflex Vulkan");
    impl_->reflex_plugin_available = plugin && low_latency;
  }
#endif
  bool frame_generation_files_available = false;
#if XPBD_STREAMLINE_HAS_DLSS_G
  {
    const bool plugin =
        signed_streamline_file(L"sl.dlss_g.dll", "DLSS FG");
    const bool ngx = signed_ngx_file(L"nvngx_dlssg.dll", "DLSS FG");
    frame_generation_files_available = plugin && ngx;
  }
#endif
  impl_->frame_generation_plugin_available =
      frame_generation_files_available && impl_->reflex_plugin_available;
  if (frame_generation_files_available &&
      !impl_->frame_generation_plugin_available) {
    xpbd::log::warn(
        "DLSS Frame Generation runtime is present but Reflex is unavailable; "
        "only Frame Generation is disabled");
  }
#if XPBD_STREAMLINE_HAS_PCL
  {
    impl_->pcl_plugin_available =
        signed_streamline_file(L"sl.pcl.dll", "PCL");
  }
#endif

  std::array<sl::Feature, 5> requested_features{};
  std::uint32_t requested_feature_count = 0u;
  auto request_feature = [&](bool available, sl::Feature feature) {
    if (available) {
      requested_features[requested_feature_count++] = feature;
    }
  };
  request_feature(impl_->dlss_plugin_available, sl::kFeatureDLSS);
  request_feature(impl_->dlss_rr_plugin_available, sl::kFeatureDLSS_RR);
  request_feature(impl_->frame_generation_plugin_available,
                  sl::kFeatureDLSS_G);
  request_feature(impl_->reflex_plugin_available, sl::kFeatureReflex);
  request_feature(impl_->pcl_plugin_available, sl::kFeaturePCL);
  if (requested_feature_count == 0u) {
    impl_->status =
        "No signed Streamline feature runtime is available; using native "
        "Vulkan";
    xpbd::log::warn(impl_->status);
    return false;
  }
  const std::wstring interposer_path = interposer.native();
  impl_->module = LoadLibraryExW(
      interposer_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (impl_->module == nullptr) {
    impl_->status = "Secure loading of sl.interposer.dll failed";
    xpbd::log::error(impl_->status);
    return false;
  }

  impl_->sl_init = loadExport<PFun_slInit *>(impl_->module, "slInit");
  impl_->sl_shutdown =
      loadExport<PFun_slShutdown *>(impl_->module, "slShutdown");
  impl_->sl_is_feature_supported =
      loadExport<PFun_slIsFeatureSupported *>(
          impl_->module, "slIsFeatureSupported");
  impl_->sl_is_feature_loaded =
      loadExport<PFun_slIsFeatureLoaded *>(
          impl_->module, "slIsFeatureLoaded");
  impl_->sl_set_feature_loaded =
      loadExport<PFun_slSetFeatureLoaded *>(
          impl_->module, "slSetFeatureLoaded");
  impl_->sl_get_feature_requirements =
      loadExport<PFun_slGetFeatureRequirements *>(
          impl_->module, "slGetFeatureRequirements");
  impl_->sl_set_tag_for_frame =
      loadExport<PFun_slSetTagForFrame *>(
          impl_->module, "slSetTagForFrame");
  impl_->sl_set_constants =
      loadExport<PFun_slSetConstants *>(
          impl_->module, "slSetConstants");
  impl_->sl_evaluate_feature =
      loadExport<PFun_slEvaluateFeature *>(
          impl_->module, "slEvaluateFeature");
  impl_->sl_get_feature_function =
      loadExport<PFun_slGetFeatureFunction *>(
          impl_->module, "slGetFeatureFunction");
  impl_->sl_get_new_frame_token =
      loadExport<PFun_slGetNewFrameToken *>(
          impl_->module, "slGetNewFrameToken");
  impl_->sl_free_resources =
      loadExport<PFun_slFreeResources *>(
          impl_->module, "slFreeResources");
  impl_->get_instance_proc_addr =
      loadExport<PFN_vkGetInstanceProcAddr>(
          impl_->module, "vkGetInstanceProcAddr");
  impl_->get_device_proc_addr =
      loadExport<PFN_vkGetDeviceProcAddr>(
          impl_->module, "vkGetDeviceProcAddr");
  if (impl_->sl_init == nullptr || impl_->sl_shutdown == nullptr ||
      impl_->sl_is_feature_supported == nullptr ||
      impl_->sl_is_feature_loaded == nullptr ||
      impl_->sl_set_feature_loaded == nullptr ||
      impl_->sl_get_feature_requirements == nullptr ||
      impl_->sl_set_tag_for_frame == nullptr ||
      impl_->sl_set_constants == nullptr ||
      impl_->sl_evaluate_feature == nullptr ||
      impl_->sl_get_feature_function == nullptr ||
      impl_->sl_get_new_frame_token == nullptr ||
      impl_->sl_free_resources == nullptr ||
      impl_->get_instance_proc_addr == nullptr ||
      impl_->get_device_proc_addr == nullptr) {
    impl_->status = "Streamline interposer is missing required exports";
    xpbd::log::error(impl_->status);
    releaseAfterVulkan();
    return false;
  }

  const std::wstring plugin_directory = directory.native();
  const wchar_t *plugin_paths[] = {plugin_directory.c_str()};
  sl::Preferences preferences{};
  preferences.showConsole = false;
  preferences.logLevel = sl::LogLevel::eDefault;
  preferences.pathsToPlugins = plugin_paths;
  preferences.numPathsToPlugins = 1;
  preferences.pathToLogsAndData = nullptr;
  preferences.logMessageCallback = streamlineLog;
  preferences.flags =
      sl::PreferenceFlags::eDisableCLStateTracking |
      sl::PreferenceFlags::eDisableDebugText |
      sl::PreferenceFlags::eUseManualHooking |
      sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  preferences.featuresToLoad = requested_features.data();
  preferences.numFeaturesToLoad = requested_feature_count;
  preferences.engine = sl::EngineType::eCustom;
  preferences.engineVersion = "1.0";
  // NGX accepts custom-engine GUIDs but rejects identifiers containing long
  // repeated digit runs; keep this stable project identity human-readable
  // without the zero padding commonly used by internal UUIDs.
  preferences.projectId = "50504244-4241-4b45-92ab-c0d1e2f3a4b5";
  preferences.renderAPI = sl::RenderAPI::eVulkan;

  const sl::Result result = impl_->sl_init(preferences, sl::kSDKVersion);
  if (result != sl::Result::eOk) {
    impl_->status =
        std::string("Streamline initialization failed: ") +
        sl::getResultAsStr(result);
    xpbd::log::warn(impl_->status);
    releaseAfterVulkan();
    return false;
  }
  impl_->initialized = true;
  impl_->shutdown = false;
  // Every requested feature starts loaded. DLSS-G is unloaded after
  // device/support discovery and before the first swapchain is created, so
  // the user-facing default remains genuinely Off.
  impl_->frame_generation_feature_loaded =
      impl_->frame_generation_plugin_available;
  impl_->frame_generation_state_ok = false;
  if (impl_->frame_generation_plugin_available) {
    sl::FeatureRequirements frame_generation_requirements{};
    const sl::Result requirements_result =
        impl_->sl_get_feature_requirements(
            sl::kFeatureDLSS_G, frame_generation_requirements);
    impl_->dlss_g_vulkan_supported =
        requirements_result == sl::Result::eOk &&
        (static_cast<std::uint32_t>(
             frame_generation_requirements.flags) &
         static_cast<std::uint32_t>(
             sl::FeatureRequirementFlags::eVulkanSupported)) != 0u;
    if (!impl_->dlss_g_vulkan_supported) {
      impl_->frame_generation_status =
          requirements_result == sl::Result::eOk
              ? "DLSS Frame Generation plugin does not advertise Vulkan support"
              : std::string(
                    "DLSS Frame Generation requirements query failed: ") +
                    sl::getResultAsStr(requirements_result);
    } else {
      impl_->frame_generation_status =
          "DLSS Frame Generation loaded; waiting for Vulkan adapter";
    }
  } else {
    impl_->dlss_g_vulkan_supported = false;
    impl_->frame_generation_status =
        "DLSS Frame Generation runtime is not packaged or signed";
  }
  impl_->create_instance =
      reinterpret_cast<PFN_vkCreateInstance>(
          impl_->get_instance_proc_addr(nullptr, "vkCreateInstance"));
  if (impl_->create_instance == nullptr) {
    impl_->status =
        "Streamline Vulkan instance proxy is unavailable";
    xpbd::log::error(impl_->status);
    shutdownBeforeVulkan();
    releaseAfterVulkan();
    return false;
  }
  impl_->status =
      "Streamline 2.12.0 initialized; packaged features "
      "SR=" + std::to_string(impl_->dlss_plugin_available ? 1 : 0) +
      " RR=" + std::to_string(impl_->dlss_rr_plugin_available ? 1 : 0) +
      " FG=" +
      std::to_string(impl_->frame_generation_plugin_available ? 1 : 0) +
      " Reflex=" +
      std::to_string(impl_->reflex_plugin_available ? 1 : 0) +
      " PCL=" + std::to_string(impl_->pcl_plugin_available ? 1 : 0) +
      "; waiting for Vulkan adapter";
  xpbd::log::info(impl_->status);
  return true;
#else
  xpbd::log::info(impl_->status);
  return false;
#endif
}

void StreamlineVulkanRuntime::inspectPhysicalDevice(
    VkPhysicalDevice physical_device) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!impl_->initialized || physical_device == VK_NULL_HANDLE) {
    return;
  }
  sl::AdapterInfo adapter{};
  adapter.vkPhysicalDevice =
      reinterpret_cast<void *>(physical_device);
  auto feature_loaded = [&](bool plugin_available, sl::Feature feature,
                            const char *name) {
    if (!plugin_available || impl_->sl_is_feature_loaded == nullptr) {
      return false;
    }
    bool loaded = false;
    const sl::Result result =
        impl_->sl_is_feature_loaded(feature, loaded);
    if (result != sl::Result::eOk || !loaded) {
      xpbd::log::warnf(
          "Streamline %s plugin was packaged but did not initialize: %s",
          name, sl::getResultAsStr(result));
      return false;
    }
    return true;
  };
  impl_->dlss_feature_loaded = feature_loaded(
      impl_->dlss_plugin_available, sl::kFeatureDLSS, "DLSS SR");
  impl_->dlss_rr_feature_loaded = feature_loaded(
      impl_->dlss_rr_plugin_available, sl::kFeatureDLSS_RR, "DLSS RR");
  impl_->reflex_feature_loaded = feature_loaded(
      impl_->reflex_plugin_available, sl::kFeatureReflex, "Reflex");
  impl_->pcl_feature_loaded = feature_loaded(
      impl_->pcl_plugin_available, sl::kFeaturePCL, "PCL");
  impl_->frame_generation_feature_loaded = feature_loaded(
      impl_->frame_generation_plugin_available, sl::kFeatureDLSS_G,
      "DLSS FG");

  const sl::Result dlss_result =
      impl_->dlss_feature_loaded
          ? impl_->sl_is_feature_supported(sl::kFeatureDLSS, adapter)
          : sl::Result::eErrorFeatureMissing;
  impl_->dlss_supported = dlss_result == sl::Result::eOk;
  if (impl_->dlss_supported) {
    void *optimal = nullptr;
    void *set_options = nullptr;
    const sl::Result optimal_result =
        impl_->sl_get_feature_function(
            sl::kFeatureDLSS, "slDLSSGetOptimalSettings", optimal);
    const sl::Result options_result =
        impl_->sl_get_feature_function(
            sl::kFeatureDLSS, "slDLSSSetOptions", set_options);
    if (optimal_result == sl::Result::eOk &&
        options_result == sl::Result::eOk) {
      impl_->sl_dlss_get_optimal_settings =
          reinterpret_cast<PFun_slDLSSGetOptimalSettings *>(optimal);
      impl_->sl_dlss_set_options =
          reinterpret_cast<PFun_slDLSSSetOptions *>(set_options);
    } else {
      impl_->dlss_supported = false;
    }
  }
  const sl::Result rr_result =
      impl_->dlss_rr_feature_loaded
          ? impl_->sl_is_feature_supported(sl::kFeatureDLSS_RR, adapter)
          : sl::Result::eErrorFeatureMissing;
  impl_->dlss_rr_supported =
      impl_->dlss_supported && rr_result == sl::Result::eOk;
  if (impl_->dlss_rr_supported) {
    void *optimal = nullptr;
    void *set_options = nullptr;
    const sl::Result optimal_result =
        impl_->sl_get_feature_function(
            sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", optimal);
    const sl::Result options_result =
        impl_->sl_get_feature_function(
            sl::kFeatureDLSS_RR, "slDLSSDSetOptions", set_options);
    if (optimal_result == sl::Result::eOk &&
        options_result == sl::Result::eOk) {
      impl_->sl_dlssd_get_optimal_settings =
          reinterpret_cast<PFun_slDLSSDGetOptimalSettings *>(optimal);
      impl_->sl_dlssd_set_options =
          reinterpret_cast<PFun_slDLSSDSetOptions *>(set_options);
    } else {
      impl_->dlss_rr_supported = false;
      impl_->status =
          std::string("DLSS Ray Reconstruction entry points unavailable: ") +
          sl::getResultAsStr(
              optimal_result != sl::Result::eOk ? optimal_result
                                                 : options_result);
    }
  }

  const sl::Result pcl_result =
      impl_->pcl_feature_loaded
          ? impl_->sl_is_feature_supported(sl::kFeaturePCL, adapter)
          : sl::Result::eErrorFeatureMissing;
  impl_->pcl_supported = pcl_result == sl::Result::eOk;
  if (impl_->pcl_supported) {
    void *get_state = nullptr;
    void *set_options = nullptr;
    void *set_marker = nullptr;
    const sl::Result get_state_result =
        impl_->sl_get_feature_function(
            sl::kFeaturePCL, "slPCLGetState", get_state);
    const sl::Result options_result =
        impl_->sl_get_feature_function(
            sl::kFeaturePCL, "slPCLSetOptions", set_options);
    const sl::Result marker_result =
        impl_->sl_get_feature_function(
            sl::kFeaturePCL, "slPCLSetMarker", set_marker);
    if (get_state_result == sl::Result::eOk &&
        options_result == sl::Result::eOk &&
        marker_result == sl::Result::eOk && get_state != nullptr &&
        set_options != nullptr && set_marker != nullptr) {
      impl_->sl_pcl_get_state =
          reinterpret_cast<PFun_slPCLGetState *>(get_state);
      impl_->sl_pcl_set_options =
          reinterpret_cast<PFun_slPCLSetOptions *>(set_options);
      impl_->sl_pcl_set_marker =
          reinterpret_cast<PFun_slPCLSetMarker *>(set_marker);

      // Use the official message-based path (no hidden F13-F15 binding) and
      // target the SDL/main thread that owns the Win32 message pump.
      sl::PCLOptions pcl_options{};
      pcl_options.virtualKey = sl::PCLHotKey::eUsePingMessage;
      pcl_options.idThread = GetCurrentThreadId();
      const sl::Result pcl_options_result =
          impl_->sl_pcl_set_options(pcl_options);
      sl::PCLState pcl_state{};
      const sl::Result pcl_state_result =
          pcl_options_result == sl::Result::eOk
              ? impl_->sl_pcl_get_state(pcl_state)
              : pcl_options_result;
      if (pcl_options_result == sl::Result::eOk &&
          pcl_state_result == sl::Result::eOk) {
        impl_->pcl_stats_window_message = pcl_state.statsWindowMessage;
      } else {
        xpbd::log::warnf(
            "Streamline PCL latency-ping setup failed: options=%s state=%s",
            sl::getResultAsStr(pcl_options_result),
            sl::getResultAsStr(pcl_state_result));
      }
    } else {
      impl_->pcl_supported = false;
    }
  }

  const sl::Result reflex_result =
      impl_->reflex_feature_loaded
          ? impl_->sl_is_feature_supported(sl::kFeatureReflex, adapter)
          : sl::Result::eErrorFeatureMissing;
  impl_->reflex_supported = reflex_result == sl::Result::eOk;
  if (impl_->reflex_supported) {
    void *get_state = nullptr;
    void *sleep = nullptr;
    void *set_options = nullptr;
    const sl::Result get_state_result =
        impl_->sl_get_feature_function(
            sl::kFeatureReflex, "slReflexGetState", get_state);
    const sl::Result sleep_result =
        impl_->sl_get_feature_function(
            sl::kFeatureReflex, "slReflexSleep", sleep);
    const sl::Result options_result =
        impl_->sl_get_feature_function(
            sl::kFeatureReflex, "slReflexSetOptions", set_options);
    if (get_state_result == sl::Result::eOk &&
        sleep_result == sl::Result::eOk &&
        options_result == sl::Result::eOk && get_state != nullptr &&
        sleep != nullptr && set_options != nullptr) {
      impl_->sl_reflex_get_state =
          reinterpret_cast<PFun_slReflexGetState *>(get_state);
      impl_->sl_reflex_sleep =
          reinterpret_cast<PFun_slReflexSleep *>(sleep);
      impl_->sl_reflex_set_options =
          reinterpret_cast<PFun_slReflexSetOptions *>(set_options);
      // NVIDIA's Reflex QA contract requires the default state to be On and
      // requires at least one options call even when the user later selects
      // Off.
      sl::ReflexOptions reflex_options{};
      reflex_options.mode = sl::ReflexMode::eLowLatency;
      const sl::Result set_result =
          impl_->sl_reflex_set_options(reflex_options);
      impl_->reflex_options_valid = set_result == sl::Result::eOk;
      impl_->configured_reflex_mode = PathTraceReflexMode::On;
      if (!impl_->reflex_options_valid) {
        impl_->reflex_supported = false;
      }
    } else {
      impl_->reflex_supported = false;
    }
  }

  const sl::Result frame_generation_result =
      impl_->frame_generation_feature_loaded
          ? impl_->sl_is_feature_supported(sl::kFeatureDLSS_G, adapter)
          : sl::Result::eErrorFeatureMissing;
  // Streamline returns eErrorNoSupportedAdapterFound and does not create a
  // DLSS-G feature context on pre-supported adapters (for example RTX 3060
  // with a current driver). Never call slIsFeatureLoaded/slSetFeatureLoaded
  // for such an adapter: those calls themselves return eErrorFeatureMissing
  // and would incorrectly abort native Vulkan startup.
  impl_->frame_generation_context_available =
      frame_generation_result == sl::Result::eOk;
  impl_->frame_generation_supported =
      impl_->dlss_g_vulkan_supported &&
      impl_->reflex_supported &&
      frame_generation_result == sl::Result::eOk;
  if (impl_->frame_generation_supported) {
    void *get_state = nullptr;
    void *set_options = nullptr;
    const sl::Result get_state_result =
        impl_->sl_get_feature_function(
            sl::kFeatureDLSS_G, "slDLSSGGetState", get_state);
    const sl::Result options_result =
        impl_->sl_get_feature_function(
            sl::kFeatureDLSS_G, "slDLSSGSetOptions", set_options);
    if (get_state_result == sl::Result::eOk &&
        options_result == sl::Result::eOk && get_state != nullptr &&
        set_options != nullptr) {
      impl_->sl_dlss_g_get_state =
          reinterpret_cast<PFun_slDLSSGGetState *>(get_state);
      impl_->sl_dlss_g_set_options =
          reinterpret_cast<PFun_slDLSSGSetOptions *>(set_options);
      impl_->frame_generation_status =
          "DLSS Frame Generation and Reflex are supported";
    } else {
      impl_->frame_generation_supported = false;
      impl_->frame_generation_status =
          std::string("DLSS Frame Generation entry points unavailable: ") +
          sl::getResultAsStr(
              get_state_result != sl::Result::eOk
                  ? get_state_result
                  : options_result);
    }
  } else if (!impl_->dlss_g_vulkan_supported) {
    // Keep the requirements-query diagnostic produced during initialization.
  } else if (!impl_->reflex_supported) {
    impl_->frame_generation_status =
        std::string("DLSS Frame Generation unavailable because Reflex is "
                    "unsupported: ") +
        sl::getResultAsStr(reflex_result);
  } else {
    impl_->frame_generation_status =
        std::string("DLSS Frame Generation unavailable: ") +
        sl::getResultAsStr(frame_generation_result);
  }

  if (impl_->dlss_supported && impl_->dlss_rr_supported) {
    impl_->status =
        "DLSS Super Resolution and Ray Reconstruction are supported";
  } else if (impl_->dlss_supported) {
    if (impl_->status.rfind(
            "DLSS Ray Reconstruction entry points unavailable:", 0u) != 0u) {
      impl_->status = impl_->dlss_rr_plugin_available
                          ? std::string(
                                "DLSS Super Resolution supported; Ray "
                                "Reconstruction unavailable: ") +
                                sl::getResultAsStr(rr_result)
                          : "DLSS Super Resolution supported; Ray "
                            "Reconstruction runtime not packaged";
    }
  } else {
    impl_->status = impl_->dlss_plugin_available
                        ? std::string("DLSS Super Resolution unavailable: ") +
                              sl::getResultAsStr(dlss_result)
                        : "DLSS Super Resolution runtime not packaged; "
                          "independent Streamline features remain available";
  }
  if (impl_->dlss_supported) {
    xpbd::log::info(impl_->status);
  } else {
    xpbd::log::warn(impl_->status);
  }
  if (impl_->frame_generation_supported) {
    xpbd::log::info(impl_->frame_generation_status);
  } else {
    xpbd::log::warn(impl_->frame_generation_status);
  }
  if (impl_->pcl_plugin_available && !impl_->pcl_supported) {
    xpbd::log::warnf("Streamline PCL unavailable: %s",
                     sl::getResultAsStr(pcl_result));
  } else if (!impl_->pcl_plugin_available) {
    xpbd::log::info("Streamline PCL runtime not packaged");
  }
  // featuresToLoad makes DLSS-G loaded immediately after slInit only when a
  // usable feature context exists. The UI contract is opt-in, so unhook it
  // before the first swapchain. Unsupported adapters have no context and must
  // take the native fallback without a lifecycle query.
  if (!impl_->frame_generation_context_available) {
    impl_->frame_generation_feature_loaded = false;
    impl_->frame_generation_options_enabled = false;
    impl_->frame_generation_active = false;
    impl_->frame_generation_state_ok = false;
    impl_->frames_actually_presented = 1u;
    impl_->sl_dlss_g_get_state = nullptr;
    impl_->sl_dlss_g_set_options = nullptr;
    impl_->frame_generation.state =
        FrameGenerationRuntimeState::Unsupported;
    impl_->frame_generation_status =
        "DLSS Frame Generation unavailable on this adapter; using Native";
  } else if (!setFrameGenerationFeatureLoaded(false)) {
    impl_->frame_generation_supported = false;
    xpbd::log::warn(impl_->frame_generation_status);
  }
  setFrameGenerationState(*impl_,
                          impl_->frame_generation_supported
                              ? FrameGenerationRuntimeState::NativeOff
                              : FrameGenerationRuntimeState::Unsupported,
                          0u, "physical-device capability discovery");
#else
  (void)physical_device;
#endif
}

void StreamlineVulkanRuntime::shutdownBeforeVulkan() noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  const bool unsafe_frame_generation_shutdown =
      impl_->frame_generation_options_enabled ||
      frameGenerationOptionsKeyIsValid(*impl_) ||
      impl_->frame_generation.tag_generation != 0u ||
      impl_->swapchain_ownership ==
          SwapchainOwnership::StreamlineFrameGenerationProxy ||
      (impl_->device != VK_NULL_HANDLE &&
       impl_->frame_generation_feature_loaded);
  if (unsafe_frame_generation_shutdown) {
    impl_->frame_generation_failure_latched = true;
    impl_->frame_generation_recovery_required = true;
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_status =
        "Streamline shutdown blocked by FG disable failure";
    xpbd::log::error(impl_->frame_generation_status);
    return;
  }
  if (impl_->device != VK_NULL_HANDLE) {
    if (impl_->initialized && impl_->dlss_supported &&
        impl_->sl_free_resources != nullptr &&
        impl_->configured_output_width != 0u) {
      const sl::ViewportHandle viewport{0u};
      const sl::Result free_result =
          impl_->sl_free_resources(
              impl_->configured_ray_reconstruction
                  ? sl::kFeatureDLSS_RR
                  : sl::kFeatureDLSS,
              viewport);
      if (free_result != sl::Result::eOk) {
        xpbd::log::warnf(
            "Streamline DLSS resource release failed: %s",
            sl::getResultAsStr(free_result));
      }
    }
    for (auto &output : impl_->dlss_outputs) {
      destroyDlssImage(impl_->device, output);
    }
  }
  if (impl_->initialized && !impl_->shutdown &&
      impl_->sl_shutdown != nullptr) {
    const sl::Result result = impl_->sl_shutdown();
    if (result != sl::Result::eOk) {
      xpbd::log::warnf("Streamline shutdown failed: %s",
                       sl::getResultAsStr(result));
      impl_->frame_generation_status =
          std::string("Streamline shutdown failed; module retained: ") +
          sl::getResultAsStr(result);
      return;
    }
    impl_->shutdown = true;
    impl_->initialized = false;
    impl_->dlss_feature_loaded = false;
    impl_->dlss_rr_feature_loaded = false;
    impl_->reflex_feature_loaded = false;
    impl_->pcl_feature_loaded = false;
    impl_->dlss_runtime_faulted = false;
    impl_->dlss_rr_runtime_faulted = false;
    impl_->dlss_supported = false;
    impl_->dlss_rr_supported = false;
    impl_->frame_generation_supported = false;
    impl_->frame_generation_context_available = false;
    impl_->frame_generation_feature_loaded = false;
    impl_->reflex_supported = false;
    impl_->pcl_supported = false;
  }
  impl_->configured_mode = PathTraceUpscale::Off;
  impl_->configured_output_width = 0u;
  impl_->configured_output_height = 0u;
  impl_->configured_ray_reconstruction = false;
  impl_->current_frame_token = nullptr;
  impl_->current_frame_index =
      (std::numeric_limits<std::uint32_t>::max)();
  impl_->constants_frame_index =
      (std::numeric_limits<std::uint32_t>::max)();
  impl_->simulation_marker_open = false;
  impl_->frame_generation_active = false;
  impl_->frame_generation_state_ok = false;
  impl_->frames_actually_presented = 1u;
  impl_->requested_frame_generation.store(false, std::memory_order_release);
  impl_->frame_generation_failure_latched = false;
  impl_->frame_generation_recovery_required = false;
  impl_->swapchain_ownership = SwapchainOwnership::Native;
  impl_->swapchain_present = false;
  impl_->frame_generation.state = FrameGenerationRuntimeState::ShuttingDown;
  impl_->force_history_reset = true;
#endif
}

void StreamlineVulkanRuntime::releaseAfterVulkan() noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->module != nullptr &&
      (impl_->initialized ||
       impl_->frame_generation_feature_loaded ||
       impl_->frame_generation_options_enabled ||
       frameGenerationOptionsKeyIsValid(*impl_) ||
       impl_->frame_generation.tag_generation != 0u ||
       impl_->swapchain_ownership ==
           SwapchainOwnership::StreamlineFrameGenerationProxy)) {
    impl_->frame_generation_status =
        "Streamline module release blocked by incomplete FG teardown";
    xpbd::log::error(impl_->frame_generation_status);
    return;
  }
  if (impl_->module != nullptr) {
    FreeLibrary(impl_->module);
  }
  impl_->module = nullptr;
  impl_->sl_init = nullptr;
  impl_->sl_shutdown = nullptr;
  impl_->sl_is_feature_supported = nullptr;
  impl_->sl_is_feature_loaded = nullptr;
  impl_->sl_set_feature_loaded = nullptr;
  impl_->sl_get_feature_requirements = nullptr;
  impl_->sl_set_tag_for_frame = nullptr;
  impl_->sl_set_constants = nullptr;
  impl_->sl_evaluate_feature = nullptr;
  impl_->sl_get_feature_function = nullptr;
  impl_->sl_get_new_frame_token = nullptr;
  impl_->sl_free_resources = nullptr;
  impl_->sl_dlss_get_optimal_settings = nullptr;
  impl_->sl_dlss_set_options = nullptr;
  impl_->sl_dlssd_get_optimal_settings = nullptr;
  impl_->sl_dlssd_set_options = nullptr;
  impl_->sl_dlss_g_get_state = nullptr;
  impl_->sl_dlss_g_set_options = nullptr;
  impl_->sl_reflex_get_state = nullptr;
  impl_->sl_reflex_sleep = nullptr;
  impl_->sl_reflex_set_options = nullptr;
  impl_->sl_pcl_get_state = nullptr;
  impl_->sl_pcl_set_options = nullptr;
  impl_->sl_pcl_set_marker = nullptr;
  impl_->pcl_stats_window_message = 0u;
  impl_->get_instance_proc_addr = nullptr;
  impl_->get_device_proc_addr = nullptr;
  impl_->create_instance = nullptr;
  impl_->enumerate_physical_devices = nullptr;
  impl_->create_device = nullptr;
  impl_->create_win32_surface = nullptr;
  impl_->destroy_surface = nullptr;
  impl_->create_swapchain = nullptr;
  impl_->destroy_swapchain = nullptr;
  impl_->get_swapchain_images = nullptr;
  impl_->acquire_next_image = nullptr;
  impl_->queue_present = nullptr;
  impl_->device_wait_idle = nullptr;
  impl_->instance = VK_NULL_HANDLE;
  impl_->physical_device = VK_NULL_HANDLE;
  impl_->device = VK_NULL_HANDLE;
  impl_->frame_generation_feature_loaded = false;
  impl_->frame_generation_context_available = false;
  impl_->frame_generation_state_ok = false;
  impl_->dlss_feature_loaded = false;
  impl_->dlss_rr_feature_loaded = false;
  impl_->reflex_feature_loaded = false;
  impl_->pcl_feature_loaded = false;
  impl_->dlss_runtime_faulted = false;
  impl_->dlss_rr_runtime_faulted = false;
  impl_->dlss_plugin_available = false;
  impl_->dlss_rr_plugin_available = false;
  impl_->frame_generation_plugin_available = false;
  impl_->reflex_plugin_available = false;
  impl_->pcl_plugin_available = false;
  impl_->swapchain_ownership = SwapchainOwnership::Native;
  impl_->swapchain_present = false;
  if (impl_->exception_diagnostic != nullptr) {
    RemoveVectoredExceptionHandler(impl_->exception_diagnostic);
    impl_->exception_diagnostic = nullptr;
  }
#endif
}

bool StreamlineVulkanRuntime::initialized() const noexcept {
  return impl_->initialized;
}

bool StreamlineVulkanRuntime::dlssSupported() const noexcept {
  return impl_->dlss_supported;
}

bool StreamlineVulkanRuntime::
dlssRayReconstructionSupported() const noexcept {
  return impl_->dlss_rr_supported;
}

bool StreamlineVulkanRuntime::frameGenerationSupported() const noexcept {
  return impl_->frame_generation_supported;
}

void StreamlineVulkanRuntime::requestFrameGeneration(bool enabled) noexcept {
  const bool previous =
      impl_->requested_frame_generation.exchange(
          enabled, std::memory_order_acq_rel);
  if (!previous && enabled) {
    // A deliberate Off -> On toggle is the user's acknowledgement of a
    // previous recoverable failure; permit one fresh activation attempt.
    impl_->frame_generation_failure_latched = false;
    impl_->frame_generation_recovery_required = false;
    impl_->frame_generation_disable = {};
    impl_->frame_generation_disable_result = 0;
  }
  impl_->frame_generation.requested = enabled;
}

bool StreamlineVulkanRuntime::frameGenerationRequested() const noexcept {
  return impl_->requested_frame_generation.load(std::memory_order_acquire);
}

bool StreamlineVulkanRuntime::frameGenerationActivationAllowed() const noexcept {
  return impl_->frame_generation_supported &&
         frameGenerationRequested() &&
         !impl_->frame_generation_failure_latched;
}

bool StreamlineVulkanRuntime::frameGenerationRecoveryRequired() const noexcept {
  return impl_->frame_generation_recovery_required;
}

FrameGenerationDiagnostic
StreamlineVulkanRuntime::frameGenerationDiagnostic() const {
  FrameGenerationDiagnostic diagnostic = impl_->frame_generation;
  diagnostic.requested =
      impl_->requested_frame_generation.load(std::memory_order_acquire);
  diagnostic.supported = impl_->frame_generation_supported;
  diagnostic.plugin_loaded = impl_->frame_generation_feature_loaded;
  diagnostic.proxy_swapchain =
      impl_->swapchain_ownership ==
      SwapchainOwnership::StreamlineFrameGenerationProxy;
  diagnostic.ownership = impl_->swapchain_ownership;
  diagnostic.options_on = impl_->frame_generation_options_enabled;
  diagnostic.state_ok = impl_->frame_generation_state_ok;
  diagnostic.frames_actually_presented = impl_->frames_actually_presented;
  diagnostic.disable_attempts = impl_->frame_generation_disable.attempts;
  diagnostic.disable_completed_drains =
      impl_->frame_generation_disable.completed_drains;
  diagnostic.disable_result = impl_->frame_generation_disable_result;
  diagnostic.disable_confirmed_off =
      impl_->frame_generation_disable.confirmed_off;
  diagnostic.disable_exhausted =
      impl_->frame_generation_disable.exhausted;
  diagnostic.failure_latched = impl_->frame_generation_failure_latched;
  diagnostic.recovery_required = impl_->frame_generation_recovery_required;
  diagnostic.present_thread_id =
      impl_->frame_generation_present_thread_id;
  diagnostic.present_thread_bound =
      impl_->frame_generation_present_thread_id != 0u;
  diagnostic.status = impl_->frame_generation_status;
  return diagnostic;
}

SwapchainOwnership
StreamlineVulkanRuntime::swapchainOwnership() const noexcept {
  return impl_->swapchain_ownership;
}

bool StreamlineVulkanRuntime::bindFrameGenerationPresentThread() noexcept {
#if defined(_WIN32)
  const std::uint64_t thread_id =
      static_cast<std::uint64_t>(GetCurrentThreadId());
  if (impl_->frame_generation_present_thread_id == 0u) {
    impl_->frame_generation_present_thread_id = thread_id;
    impl_->frame_generation.present_thread_id = thread_id;
    impl_->frame_generation.present_thread_bound = true;
    return true;
  }
  if (impl_->frame_generation_present_thread_id != thread_id) {
#ifndef NDEBUG
    assert(false && "DLSS-G options/present moved to another thread");
#endif
    impl_->frame_generation_status =
        "DLSS Frame Generation rejected a Present-thread change";
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_failure_latched = true;
    impl_->frame_generation_recovery_required = true;
    return false;
  }
#endif
  return true;
}

bool StreamlineVulkanRuntime::beginFrameGenerationShutdown(
    std::uint64_t frame_index) noexcept {
  if (!bindFrameGenerationPresentThread()) {
    return false;
  }
  impl_->frame_generation_shutdown_requested = true;
  setFrameGenerationState(*impl_, FrameGenerationRuntimeState::ShuttingDown,
                          frame_index, "Vulkan shutdown begins");
  if (!disableFrameGeneration()) {
    return false;
  }
  if (impl_->frame_generation.tag_generation != 0u &&
      !clearFrameGenerationInputs(
          VK_NULL_HANDLE, impl_->current_frame_index, 0u, 0u, 0u, 0u, 0u,
          0u)) {
    return false;
  }
  setFrameGenerationState(*impl_, FrameGenerationRuntimeState::ShuttingDown,
                          frame_index, "Vulkan shutdown begins");
  return true;
}

bool StreamlineVulkanRuntime::retryFrameGenerationDisableAfterDrain(
    std::uint64_t frame_index, const char *reason) noexcept {
  if (!bindFrameGenerationPresentThread()) {
    return false;
  }
  (void)frameGenerationRecordDisableDrain(
      impl_->frame_generation_disable);
  if (impl_->frame_generation_disable.exhausted) {
    synchronizeFrameGenerationDisableFailure(*impl_);
    impl_->frame_generation_status =
        "DLSS Frame Generation Options-Off retries exhausted; resource "
        "destruction blocked";
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
        frame_index, reason);
    return false;
  }
  if (!impl_->frame_generation_disable.confirmed_off &&
      !disableFrameGeneration()) {
    return false;
  }
  if (impl_->frame_generation.tag_generation != 0u &&
      !clearFrameGenerationInputs(
          VK_NULL_HANDLE, impl_->current_frame_index, 0u, 0u, 0u, 0u, 0u,
          0u)) {
    return false;
  }
  if (!frameGenerationRuntimeMayDestroy(*impl_)) {
    impl_->frame_generation_status =
        "DLSS Frame Generation Options-Off confirmed; final GPU/Present "
        "drain required";
    return false;
  }
  xpbd::log::infof(
      "DLSSG disable transaction safe after drain: attempts=%u drains=%u",
      impl_->frame_generation_disable.attempts,
      impl_->frame_generation_disable.completed_drains);
  impl_->frame_generation_recovery_required = false;
  setFrameGenerationState(
      *impl_, impl_->frame_generation_shutdown_requested
                  ? FrameGenerationRuntimeState::ShuttingDown
                  : FrameGenerationRuntimeState::DisablingDrain,
      frame_index, reason);
  return true;
}

bool StreamlineVulkanRuntime::
unloadFrameGenerationForShutdown() noexcept {
  const bool unloaded = setFrameGenerationFeatureLoaded(false);
  setFrameGenerationState(
      *impl_, FrameGenerationRuntimeState::ShuttingDown,
      impl_->frame_generation_present_frame_index,
      unloaded ? "DLSS-G plugin unloaded for shutdown"
               : "DLSS-G plugin unload failed during shutdown");
  return unloaded;
}

bool StreamlineVulkanRuntime::beginFrameGenerationSwapchainTransition(
    SwapchainOwnership target, std::uint64_t frame_index,
    const char *reason) noexcept {
  if (!bindFrameGenerationPresentThread()) {
    return false;
  }
  impl_->frame_generation_shutdown_requested = false;
  const bool proxy_target =
      target == SwapchainOwnership::StreamlineFrameGenerationProxy &&
      frameGenerationActivationAllowed();
  setFrameGenerationState(*impl_,
                           FrameGenerationRuntimeState::DisablingOptions,
                           frame_index, reason);
  if (!disableFrameGeneration()) {
    return false;
  }
  // Expire any tags associated with the previous token before the GPU drain.
  // A null command buffer is intentional here: the next intercepted Present
  // must not inherit a stale resource or Backbuffer extent.
  if (impl_->frame_generation.tag_generation != 0u &&
      !clearFrameGenerationInputs(
          VK_NULL_HANDLE, impl_->current_frame_index, 0u, 0u, 0u, 0u, 0u,
          0u)) {
    return false;
  }
  setFrameGenerationState(
      *impl_, proxy_target ? FrameGenerationRuntimeState::EnablingDrain
                           : FrameGenerationRuntimeState::DisablingDrain,
      frame_index, reason);
  return true;
}

bool StreamlineVulkanRuntime::configureFrameGenerationFeature(
    SwapchainOwnership target, std::uint64_t frame_index,
    const char *reason) noexcept {
  if (!bindFrameGenerationPresentThread()) {
    return false;
  }
  const bool load =
      target == SwapchainOwnership::StreamlineFrameGenerationProxy &&
      frameGenerationActivationAllowed();
  setFrameGenerationState(
      *impl_,
      load ? FrameGenerationRuntimeState::EnablingLoadPlugin
           : FrameGenerationRuntimeState::DisablingUnloadPlugin,
      frame_index, reason);
  const bool result = setFrameGenerationFeatureLoaded(load);
  if (!result && load) {
    requestFrameGenerationNativeRecovery(
        "DLSS-G plugin could not be loaded; restoring Native", true);
  } else if (result && load) {
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::EnablingCreateProxySwapchain,
        frame_index, reason);
  } else if (result && !load) {
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::DisablingUnloadPlugin,
        frame_index, reason);
  }
  return result;
}

bool StreamlineVulkanRuntime::completeFrameGenerationSwapchainTransition(
    SwapchainOwnership ownership, bool resources_ready,
    std::uint64_t frame_index, const char *reason) noexcept {
  if (ownership == SwapchainOwnership::Native &&
      !frameGenerationRuntimeMayDestroy(*impl_)) {
    impl_->frame_generation_failure_latched = true;
    impl_->frame_generation_recovery_required = true;
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_status =
        "DLSS Frame Generation refused NativeOff before confirmed teardown";
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
        frame_index, reason);
    return false;
  }
  impl_->swapchain_ownership = ownership;
  impl_->swapchain_present = true;
  ++impl_->frame_generation_generation;
  impl_->frame_generation.proxy_swapchain =
      ownership == SwapchainOwnership::StreamlineFrameGenerationProxy;
  impl_->frame_generation.ownership = ownership;
  impl_->frame_generation.valid_inputs_tagged = false;
  impl_->frame_generation.tag_generation = 0u;
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  impl_->frame_generation_options_key.valid = false;
#endif
  impl_->frame_generation_state_ok = false;
  impl_->frame_generation_recovery_required = false;
  if (ownership == SwapchainOwnership::StreamlineFrameGenerationProxy &&
      resources_ready && impl_->frame_generation_feature_loaded) {
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::ProxyArmed, frame_index, reason);
  } else {
    impl_->swapchain_ownership = SwapchainOwnership::Native;
    impl_->frame_generation.proxy_swapchain = false;
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::NativeOff, frame_index, reason);
  }
  impl_->frame_generation_disable = {};
  impl_->frame_generation_disable_result = 0;
  return true;
}

bool StreamlineVulkanRuntime::notifyFrameGenerationSwapchainDestroyed(
    std::uint64_t frame_index, const char *reason) noexcept {
  if (!frameGenerationRuntimeMayDestroy(*impl_)) {
    impl_->frame_generation_failure_latched = true;
    impl_->frame_generation_recovery_required = true;
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_status =
        "DLSS Frame Generation blocked unsafe proxy swapchain destruction";
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
        frame_index, reason);
    return false;
  }
  xpbd::log::infof(
      "DLSSG swapchain destruction authorized: ownership=%s attempts=%u "
      "drains=%u",
      swapchainOwnershipName(impl_->swapchain_ownership),
      impl_->frame_generation_disable.attempts,
      impl_->frame_generation_disable.completed_drains);
  impl_->swapchain_present = false;
  impl_->swapchain_ownership = SwapchainOwnership::Native;
  impl_->frame_generation.valid_inputs_tagged = false;
  impl_->frame_generation.tag_generation = 0u;
  impl_->frame_generation_state_ok = false;
  if (impl_->frame_generation.state ==
      FrameGenerationRuntimeState::ShuttingDown) {
    setFrameGenerationState(*impl_, FrameGenerationRuntimeState::ShuttingDown,
                            frame_index, reason);
    return true;
  }
  if (impl_->frame_generation_feature_loaded) {
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::DisablingDestroyProxySwapchain,
        frame_index, reason);
  } else {
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::NativeOff, frame_index, reason);
  }
  return true;
}

void StreamlineVulkanRuntime::requestFrameGenerationNativeRecovery(
    const char *reason, bool latch_failure) noexcept {
  impl_->frame_generation_recovery_required = true;
  impl_->frame_generation_failure_latched |= latch_failure;
  impl_->frame_generation_state_poll_pending = false;
  impl_->frame_generation_state_poll_inputs_ready = false;
  impl_->frame_generation_status =
      reason != nullptr ? reason : "DLSS-G requested Native recovery";
  impl_->frame_generation_state_ok = false;
  setFrameGenerationState(
      *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
      impl_->frame_generation_present_frame_index, reason);
}

VkResult StreamlineVulkanRuntime::consumeFrameGenerationApiError() noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  const std::int32_t error =
      g_dlss_g_api_error.exchange(VK_SUCCESS, std::memory_order_acq_rel);
  return static_cast<VkResult>(error);
#else
  return VK_SUCCESS;
#endif
}

FrameGenerationTransitionResult
StreamlineVulkanRuntime::classifyFrameGenerationVkResult(
    VkResult result, const char *stage) noexcept {
  const VkResult callback_result = consumeFrameGenerationApiError();
  if (result == VK_SUCCESS && callback_result != VK_SUCCESS) {
    result = callback_result;
  }
  if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
    return FrameGenerationTransitionResult::NoAction;
  }
  if (result == VK_ERROR_DEVICE_LOST) {
    requestFrameGenerationNativeRecovery(
        "DLSS-G reported VK_ERROR_DEVICE_LOST", true);
    return FrameGenerationTransitionResult::FatalDeviceLost;
  }
  if (result == VK_ERROR_OUT_OF_DATE_KHR ||
      result == VK_ERROR_SURFACE_LOST_KHR) {
    requestFrameGenerationNativeRecovery(
        stage != nullptr ? stage : "DLSS-G swapchain requires Native recovery",
        false);
    return FrameGenerationTransitionResult::RecoverNative;
  }
  std::string message = "DLSS-G ";
  message += stage != nullptr ? stage : "Vulkan call";
  message += " failed; recovering Native";
  requestFrameGenerationNativeRecovery(message.c_str(), true);
  return FrameGenerationTransitionResult::RecoverNative;
}

bool StreamlineVulkanRuntime::setFrameGenerationFeatureLoaded(
    bool loaded) noexcept {
  if (!loaded &&
      (impl_->frame_generation_options_enabled ||
       frameGenerationOptionsKeyIsValid(*impl_) ||
       impl_->frame_generation.tag_generation != 0u ||
       impl_->swapchain_ownership ==
           SwapchainOwnership::StreamlineFrameGenerationProxy)) {
    impl_->frame_generation_failure_latched = true;
    impl_->frame_generation_recovery_required = true;
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_status =
        "DLSS Frame Generation plugin unload blocked before confirmed "
        "Options-Off, tag clear, and proxy destruction";
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
        impl_->frame_generation_present_frame_index,
        "unsafe DLSS-G plugin unload rejected");
    return false;
  }
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!impl_->frame_generation_context_available) {
    impl_->frame_generation_feature_loaded = false;
    impl_->frame_generation_options_enabled = false;
    impl_->frame_generation_active = false;
    impl_->frame_generation_state_ok = false;
    impl_->frames_actually_presented = 1u;
    impl_->sl_dlss_g_get_state = nullptr;
    impl_->sl_dlss_g_set_options = nullptr;
    impl_->frame_generation_status =
        "DLSS Frame Generation context is unavailable on this adapter";
    return !loaded;
  }
  if (!impl_->initialized || impl_->device == VK_NULL_HANDLE ||
      impl_->sl_is_feature_loaded == nullptr ||
      impl_->sl_set_feature_loaded == nullptr ||
      impl_->sl_get_feature_function == nullptr) {
    if (!loaded && !impl_->initialized) {
      impl_->frame_generation_feature_loaded = false;
      impl_->frame_generation_state_ok = false;
      return true;
    }
    impl_->frame_generation_status =
        "DLSS Frame Generation feature lifecycle is unavailable";
    return false;
  }
  if (loaded && !impl_->frame_generation_supported) {
    return false;
  }

  bool currently_loaded = impl_->frame_generation_feature_loaded;
  const sl::Result query_result =
      impl_->sl_is_feature_loaded(sl::kFeatureDLSS_G, currently_loaded);
  if (query_result != sl::Result::eOk) {
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_status =
        std::string("DLSS Frame Generation load-state query failed: ") +
        sl::getResultAsStr(query_result);
    return false;
  }
  if (currently_loaded != loaded) {
    const sl::Result set_result =
        impl_->sl_set_feature_loaded(sl::kFeatureDLSS_G, loaded);
    if (set_result != sl::Result::eOk) {
      impl_->frame_generation_state_ok = false;
      impl_->frame_generation_status =
          std::string(loaded ? "DLSS Frame Generation plugin load failed: "
                             : "DLSS Frame Generation plugin unload failed: ") +
          sl::getResultAsStr(set_result);
      return false;
    }
  }

  impl_->frame_generation_feature_loaded = loaded;
  impl_->frame_generation_options_enabled = false;
  impl_->frame_generation_active = false;
  impl_->frame_generation_state_ok = false;
  impl_->frames_actually_presented = 1u;
  impl_->frame_generation_options_key.valid = false;
  impl_->frame_generation.valid_inputs_tagged = false;
  if (!loaded) {
    // Feature functions belong to sl.dlss_g.dll and must never survive an
    // unload/reload boundary.
    impl_->sl_dlss_g_get_state = nullptr;
    impl_->sl_dlss_g_set_options = nullptr;
    if (impl_->frame_generation_supported) {
      impl_->frame_generation_status =
          "DLSS Frame Generation available; disabled until manually enabled";
    }
    xpbd::log::info("DLSSG plugin unload confirmed");
    return true;
  }

  void *get_state = nullptr;
  void *set_options = nullptr;
  const sl::Result get_state_result =
      impl_->sl_get_feature_function(
          sl::kFeatureDLSS_G, "slDLSSGGetState", get_state);
  const sl::Result options_result =
      impl_->sl_get_feature_function(
          sl::kFeatureDLSS_G, "slDLSSGSetOptions", set_options);
  if (get_state_result != sl::Result::eOk ||
      options_result != sl::Result::eOk || get_state == nullptr ||
      set_options == nullptr) {
    const sl::Result unload_result =
        impl_->sl_set_feature_loaded(sl::kFeatureDLSS_G, false);
    impl_->frame_generation_feature_loaded = false;
    impl_->frame_generation_state_ok = false;
    impl_->sl_dlss_g_get_state = nullptr;
    impl_->sl_dlss_g_set_options = nullptr;
    impl_->frame_generation_status =
        std::string("DLSS Frame Generation entry points unavailable after "
                    "plugin load: ") +
        sl::getResultAsStr(
            get_state_result != sl::Result::eOk
                ? get_state_result
                : options_result);
    if (unload_result != sl::Result::eOk) {
      xpbd::log::warnf(
          "Streamline DLSS Frame Generation rollback unload failed: %s",
          sl::getResultAsStr(unload_result));
    }
    return false;
  }
  impl_->sl_dlss_g_get_state =
      reinterpret_cast<PFun_slDLSSGGetState *>(get_state);
  impl_->sl_dlss_g_set_options =
      reinterpret_cast<PFun_slDLSSGSetOptions *>(set_options);
  g_dlss_g_api_error.store(VK_SUCCESS, std::memory_order_release);
  impl_->frame_generation_status =
      "DLSS Frame Generation plugin loaded; waiting for valid frame inputs";
  return true;
#else
  (void)loaded;
  return !loaded;
#endif
}

bool StreamlineVulkanRuntime::reflexSupported() const noexcept {
  return impl_->reflex_supported;
}

bool StreamlineVulkanRuntime::pclSupported() const noexcept {
  return impl_->pcl_supported;
}

bool StreamlineVulkanRuntime::frameGenerationActive() const noexcept {
  return impl_->frame_generation_active &&
         impl_->frame_generation.state == FrameGenerationRuntimeState::Active;
}

std::uint32_t
StreamlineVulkanRuntime::framesActuallyPresented() const noexcept {
  return impl_->frames_actually_presented;
}

std::string StreamlineVulkanRuntime::status() const {
  return impl_->status;
}

std::string StreamlineVulkanRuntime::frameGenerationStatus() const {
  return impl_->frame_generation_status;
}

StreamlineDlssOptimalSettings
StreamlineVulkanRuntime::queryDlssOptimalSettings(
    PathTraceUpscale mode, std::uint32_t output_width,
    std::uint32_t output_height) {
  StreamlineDlssOptimalSettings result{};
  result.mode = mode;
  result.output_width = output_width;
  result.output_height = output_height;
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!impl_->initialized || !impl_->dlss_supported ||
      impl_->sl_dlss_get_optimal_settings == nullptr ||
      mode == PathTraceUpscale::Off || output_width == 0u ||
      output_height == 0u) {
    return result;
  }
  sl::DLSSOptimalSettings settings{};
  const sl::DLSSOptions options =
      makeDlssOptions(mode, output_width, output_height);
  const sl::Result query_result =
      impl_->sl_dlss_get_optimal_settings(options, settings);
  if (query_result == sl::Result::eOk &&
      settings.optimalRenderWidth > 0u &&
      settings.optimalRenderHeight > 0u) {
    impl_->dlss_runtime_faulted = false;
    result.valid = true;
    result.render_width = settings.optimalRenderWidth;
    result.render_height = settings.optimalRenderHeight;
  } else {
    impl_->dlss_runtime_faulted = true;
    impl_->status =
        std::string("DLSS optimal-settings query failed: ") +
        sl::getResultAsStr(query_result);
  }
#endif
  return result;
}

StreamlineDlssOptimalSettings
StreamlineVulkanRuntime::queryDlssRayReconstructionOptimalSettings(
    PathTraceUpscale mode, std::uint32_t output_width,
    std::uint32_t output_height) {
  StreamlineDlssOptimalSettings result{};
  result.mode = mode;
  result.output_width = output_width;
  result.output_height = output_height;
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!impl_->initialized || !impl_->dlss_rr_supported ||
      impl_->sl_dlssd_get_optimal_settings == nullptr ||
      mode == PathTraceUpscale::Off ||
      mode == PathTraceUpscale::Dlaa || output_width == 0u ||
      output_height == 0u) {
    return result;
  }
  sl::DLSSDOptimalSettings settings{};
  const sl::DLSSDOptions options =
      makeDlssdOptions(mode, output_width, output_height);
  const sl::Result query_result =
      impl_->sl_dlssd_get_optimal_settings(options, settings);
  if (query_result == sl::Result::eOk &&
      settings.optimalRenderWidth > 0u &&
      settings.optimalRenderHeight > 0u) {
    impl_->dlss_rr_runtime_faulted = false;
    result.valid = true;
    result.render_width = settings.optimalRenderWidth;
    result.render_height = settings.optimalRenderHeight;
  } else {
    impl_->dlss_rr_runtime_faulted = true;
    impl_->status =
        std::string(
            "DLSS Ray Reconstruction optimal-settings query failed for the current request: " ) +
        sl::getResultAsStr(query_result);
  }
#endif
  return result;
}

bool StreamlineVulkanRuntime::recordDlss(
    const StreamlineDlssFrame &frame) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  impl_->dlss_runtime_faulted = true;
  const bool valid =
      impl_->initialized && impl_->dlss_supported &&
      impl_->sl_dlss_set_options != nullptr &&
      impl_->sl_get_new_frame_token != nullptr &&
      impl_->sl_set_tag_for_frame != nullptr &&
      impl_->sl_set_constants != nullptr &&
      impl_->sl_evaluate_feature != nullptr &&
      impl_->device != VK_NULL_HANDLE &&
      impl_->physical_device != VK_NULL_HANDLE &&
      frame.command_buffer != VK_NULL_HANDLE &&
      frame.view != nullptr && frame.projection != nullptr &&
      frame.render_width > 0u && frame.render_height > 0u &&
      frame.output_width > 0u && frame.output_height > 0u &&
      frame.mode != PathTraceUpscale::Off;
  if (!valid) {
    impl_->status = "DLSS SR submission prerequisites invalid";
    impl_->force_history_reset = true;
    return false;
  }

  const bool output_changed =
      impl_->configured_output_width != frame.output_width ||
      impl_->configured_output_height != frame.output_height;
  const bool feature_changed =
      impl_->configured_output_width != 0u &&
      impl_->configured_ray_reconstruction;
  if (output_changed || feature_changed) {
    if (impl_->device_wait_idle == nullptr ||
        impl_->device_wait_idle(impl_->device) != VK_SUCCESS) {
      impl_->status =
          "DLSS output resize failed while waiting for the Vulkan device";
      impl_->force_history_reset = true;
      return false;
    }
    if (impl_->sl_free_resources != nullptr &&
        impl_->configured_output_width != 0u) {
      const sl::ViewportHandle viewport{0u};
      const bool released_rr =
          impl_->configured_ray_reconstruction;
      const sl::Result free_result = impl_->sl_free_resources(
          impl_->configured_ray_reconstruction
              ? sl::kFeatureDLSS_RR
              : sl::kFeatureDLSS,
          viewport);
      if (free_result != sl::Result::eOk) {
        impl_->status =
            std::string("DLSS reconfiguration resource release failed: ") +
            sl::getResultAsStr(free_result);
        impl_->force_history_reset = true;
        return false;
      }
      xpbd::log::infof(
          "Streamline %s resources released for reconfiguration: "
          "old_output=%ux%u",
          released_rr ? "DLSS Ray Reconstruction"
                      : "DLSS Super Resolution",
          impl_->configured_output_width,
          impl_->configured_output_height);
    }
    for (auto &output : impl_->dlss_outputs) {
      destroyDlssImage(impl_->device, output);
    }
    for (auto &output : impl_->dlss_outputs) {
      if (!createDlssImage(
              impl_->physical_device, impl_->device, frame.output_width,
              frame.output_height, output)) {
        for (auto &created_output : impl_->dlss_outputs) {
          destroyDlssImage(impl_->device, created_output);
        }
        impl_->status = "DLSS output image allocation failed";
        impl_->force_history_reset = true;
        return false;
      }
    }
    impl_->configured_output_width = frame.output_width;
    impl_->configured_output_height = frame.output_height;
    impl_->force_history_reset = true;
  }

  const std::uint32_t output_slot =
      frame.frame_slot %
      static_cast<std::uint32_t>(impl_->dlss_outputs.size());
  Impl::Image &output = impl_->dlss_outputs[output_slot];
  transitionDlssOutput(
      frame.command_buffer, output, VK_IMAGE_LAYOUT_GENERAL);

  sl::FrameToken *token = frameTokenFor(*impl_, frame.frame_index);
  if (token == nullptr) {
    impl_->status = "DLSS frame-token acquisition failed";
    impl_->force_history_reset = true;
    return false;
  }
  const sl::ViewportHandle viewport{0u};
  const sl::DLSSOptions options =
      makeDlssOptions(frame.mode, frame.output_width, frame.output_height);
  const sl::Result options_result =
      impl_->sl_dlss_set_options(viewport, options);
  if (options_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS options update failed: ") +
        sl::getResultAsStr(options_result);
    impl_->force_history_reset = true;
    return false;
  }

  constexpr VkImageUsageFlags kInputUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  sl::Resource color = textureResource(
      frame.color_image, frame.color_memory, frame.color_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource depth = textureResource(
      frame.depth_image, frame.depth_memory, frame.depth_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R32_SFLOAT,
      kInputUsage, frame.render_width, frame.render_height);
  sl::Resource motion = textureResource(
      frame.motion_image, frame.motion_memory, frame.motion_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R32G32_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource transparency_and_composition = textureResource(
      frame.transparency_and_composition_image,
      frame.transparency_and_composition_memory,
      frame.transparency_and_composition_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R8_UNORM,
      kInputUsage, frame.render_width, frame.render_height);
  sl::Resource reactive_mask = textureResource(
      frame.reactive_mask_image, frame.reactive_mask_memory,
      frame.reactive_mask_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R8_UNORM, kInputUsage, frame.render_width,
      frame.render_height);
  sl::Resource output_resource = textureResource(
      output.image, output.memory, output.view, VK_IMAGE_LAYOUT_GENERAL,
      VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      frame.output_width, frame.output_height);
  const sl::Extent input_extent{
      0u, 0u, frame.render_width, frame.render_height};
  const sl::Extent output_extent{
      0u, 0u, frame.output_width, frame.output_height};
  const std::array<sl::ResourceTag, 6> tags{{
      {&color, sl::kBufferTypeScalingInputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&output_resource, sl::kBufferTypeScalingOutputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
      {&depth, sl::kBufferTypeDepth,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&motion, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&transparency_and_composition,
       sl::kBufferTypeTransparencyAndCompositionMaskHint,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&reactive_mask, sl::kBufferTypeReactiveMaskHint,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
  }};
  const std::array<TaggedTextureExpectation, 6> expectations{{
      {sl::kBufferTypeScalingInputColor,
       VK_FORMAT_R16G16B16A16_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeScalingOutputColor,
       VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
       frame.output_width, frame.output_height},
      {sl::kBufferTypeDepth, VK_FORMAT_R32_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeMotionVectors, VK_FORMAT_R32G32_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeTransparencyAndCompositionMaskHint,
       VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
       kInputUsage, frame.render_width, frame.render_height},
      {sl::kBufferTypeReactiveMaskHint, VK_FORMAT_R8_UNORM,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
  }};
  if (!validateTaggedTextureSet(
          "DLSS-SR", frame.frame_index, tags, expectations,
          impl_->last_tag_validation_failure, impl_->status)) {
    impl_->force_history_reset = true;
    return false;
  }
  sl::CommandBuffer *command_buffer =
      reinterpret_cast<sl::CommandBuffer *>(frame.command_buffer);
  const sl::Result tag_result = impl_->sl_set_tag_for_frame(
      *token, viewport, tags.data(),
      static_cast<std::uint32_t>(tags.size()), command_buffer);
  if (tag_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS resource tagging failed: ") +
        sl::getResultAsStr(tag_result);
    impl_->force_history_reset = true;
    return false;
  }

  StreamlineDlssFrame constants_frame = frame;
  constants_frame.reset_history =
      frame.reset_history || impl_->force_history_reset ||
      impl_->configured_mode != frame.mode;
  const sl::Constants constants = makeDlssConstants(constants_frame);
  const sl::Result constants_result =
      impl_->sl_set_constants(constants, *token, viewport);
  if (constants_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS constants update failed: ") +
        sl::getResultAsStr(constants_result);
    impl_->force_history_reset = true;
    return false;
  }
  impl_->constants_frame_index = frame.frame_index;
  const sl::BaseStructure *inputs[] = {&viewport};
  const sl::Result evaluate_result = impl_->sl_evaluate_feature(
      sl::kFeatureDLSS, *token, inputs, 1u, command_buffer);
  if (evaluate_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS evaluation failed: ") +
        sl::getResultAsStr(evaluate_result);
    impl_->force_history_reset = true;
    return false;
  }

  transitionDlssOutput(
      frame.command_buffer, output,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  impl_->last_output_slot = output_slot;
  impl_->configured_mode = frame.mode;
  impl_->configured_ray_reconstruction = false;
  impl_->force_history_reset = false;
  impl_->dlss_runtime_faulted = false;
  impl_->status = "DLSS Super Resolution active";
  return true;
#else
  (void)frame;
  return false;
#endif
}

bool StreamlineVulkanRuntime::recordDlssRayReconstruction(
    const StreamlineDlssRayReconstructionFrame &frame) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  impl_->dlss_rr_runtime_faulted = true;
  const bool valid =
      impl_->initialized && impl_->dlss_supported &&
      impl_->dlss_rr_supported &&
      impl_->sl_dlss_set_options != nullptr &&
      impl_->sl_dlssd_set_options != nullptr &&
      impl_->sl_get_new_frame_token != nullptr &&
      impl_->sl_set_tag_for_frame != nullptr &&
      impl_->sl_set_constants != nullptr &&
      impl_->sl_evaluate_feature != nullptr &&
      impl_->device != VK_NULL_HANDLE &&
      impl_->physical_device != VK_NULL_HANDLE &&
      frame.command_buffer != VK_NULL_HANDLE &&
      frame.view != nullptr && frame.projection != nullptr &&
      frame.render_width > 0u && frame.render_height > 0u &&
      frame.output_width > 0u && frame.output_height > 0u &&
      frame.mode != PathTraceUpscale::Off &&
      frame.mode != PathTraceUpscale::Dlaa;
  if (!valid) {
    impl_->status =
        "DLSS Ray Reconstruction submission prerequisites invalid";
    impl_->force_history_reset = true;
    return false;
  }

  const bool output_changed =
      impl_->configured_output_width != frame.output_width ||
      impl_->configured_output_height != frame.output_height;
  const bool feature_changed =
      impl_->configured_output_width != 0u &&
      !impl_->configured_ray_reconstruction;
  if (output_changed || feature_changed) {
    if (impl_->device_wait_idle == nullptr ||
        impl_->device_wait_idle(impl_->device) != VK_SUCCESS) {
      impl_->status =
          "DLSS Ray Reconstruction reconfiguration failed while waiting "
          "for the Vulkan device";
      impl_->force_history_reset = true;
      return false;
    }
    if (impl_->sl_free_resources != nullptr &&
        impl_->configured_output_width != 0u) {
      const sl::ViewportHandle viewport{0u};
      const bool released_rr =
          impl_->configured_ray_reconstruction;
      const sl::Result free_result = impl_->sl_free_resources(
          impl_->configured_ray_reconstruction
              ? sl::kFeatureDLSS_RR
              : sl::kFeatureDLSS,
          viewport);
      if (free_result != sl::Result::eOk) {
        impl_->status =
            std::string(
                "DLSS Ray Reconstruction reconfiguration resource "
                "release failed: ") +
            sl::getResultAsStr(free_result);
        impl_->force_history_reset = true;
        return false;
      }
      xpbd::log::infof(
          "Streamline %s resources released for reconfiguration: "
          "old_output=%ux%u",
          released_rr ? "DLSS Ray Reconstruction"
                      : "DLSS Super Resolution",
          impl_->configured_output_width,
          impl_->configured_output_height);
    }
    for (auto &output : impl_->dlss_outputs) {
      destroyDlssImage(impl_->device, output);
    }
    for (auto &output : impl_->dlss_outputs) {
      if (!createDlssImage(
              impl_->physical_device, impl_->device, frame.output_width,
              frame.output_height, output)) {
        for (auto &created_output : impl_->dlss_outputs) {
          destroyDlssImage(impl_->device, created_output);
        }
        impl_->status =
            "DLSS Ray Reconstruction output image allocation failed";
        impl_->force_history_reset = true;
        return false;
      }
    }
    impl_->configured_output_width = frame.output_width;
    impl_->configured_output_height = frame.output_height;
    impl_->force_history_reset = true;
  }

  const std::uint32_t output_slot =
      frame.frame_slot %
      static_cast<std::uint32_t>(impl_->dlss_outputs.size());
  Impl::Image &output = impl_->dlss_outputs[output_slot];
  transitionDlssOutput(
      frame.command_buffer, output, VK_IMAGE_LAYOUT_GENERAL);

  sl::FrameToken *token = frameTokenFor(*impl_, frame.frame_index);
  if (token == nullptr) {
    impl_->status =
        "DLSS Ray Reconstruction frame-token acquisition failed";
    impl_->force_history_reset = true;
    return false;
  }
  const sl::ViewportHandle viewport{0u};
  const sl::DLSSOptions sr_options =
      makeDlssOptions(frame.mode, frame.output_width, frame.output_height);
  const sl::Result sr_options_result =
      impl_->sl_dlss_set_options(viewport, sr_options);
  if (sr_options_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS companion options update for Ray "
                    "Reconstruction failed: ") +
        sl::getResultAsStr(sr_options_result);
    impl_->force_history_reset = true;
    return false;
  }
  const sl::DLSSDOptions rr_options =
      makeDlssdOptions(frame.mode, frame.output_width,
                       frame.output_height, frame.view);
  const sl::Result rr_options_result =
      impl_->sl_dlssd_set_options(viewport, rr_options);
  if (rr_options_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS Ray Reconstruction options update failed: ") +
        sl::getResultAsStr(rr_options_result);
    impl_->force_history_reset = true;
    return false;
  }

  constexpr VkImageUsageFlags kInputUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  sl::Resource color = textureResource(
      frame.color_image, frame.color_memory, frame.color_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource depth = textureResource(
      frame.depth_image, frame.depth_memory, frame.depth_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R32_SFLOAT,
      kInputUsage, frame.render_width, frame.render_height);
  sl::Resource motion = textureResource(
      frame.motion_image, frame.motion_memory, frame.motion_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R32G32_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource transparency_and_composition = textureResource(
      frame.transparency_and_composition_image,
      frame.transparency_and_composition_memory,
      frame.transparency_and_composition_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R8_UNORM,
      kInputUsage, frame.render_width, frame.render_height);
  sl::Resource reactive_mask = textureResource(
      frame.reactive_mask_image, frame.reactive_mask_memory,
      frame.reactive_mask_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R8_UNORM, kInputUsage, frame.render_width,
      frame.render_height);
  sl::Resource diffuse_albedo = textureResource(
      frame.diffuse_albedo_image, frame.diffuse_albedo_memory,
      frame.diffuse_albedo_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource specular_albedo = textureResource(
      frame.specular_albedo_image, frame.specular_albedo_memory,
      frame.specular_albedo_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource normal_roughness = textureResource(
      frame.normal_roughness_image, frame.normal_roughness_memory,
      frame.normal_roughness_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R16G16B16A16_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource specular_hit_distance = textureResource(
      frame.specular_hit_distance_image,
      frame.specular_hit_distance_memory,
      frame.specular_hit_distance_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R32_SFLOAT, kInputUsage,
      frame.render_width, frame.render_height);
  sl::Resource output_resource = textureResource(
      output.image, output.memory, output.view, VK_IMAGE_LAYOUT_GENERAL,
      VK_FORMAT_R16G16B16A16_SFLOAT,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      frame.output_width, frame.output_height);
  const sl::Extent input_extent{
      0u, 0u, frame.render_width, frame.render_height};
  const sl::Extent output_extent{
      0u, 0u, frame.output_width, frame.output_height};
  // The RR contract requires either reflected-surface motion or world-space
  // specular hit distance. XPBD supplies the latter as a dedicated scalar
  // R32F guide, paired with world/view transforms in DLSSDOptions.
  const std::array<sl::ResourceTag, 10> tags{{
      {&color, sl::kBufferTypeScalingInputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&output_resource, sl::kBufferTypeScalingOutputColor,
       sl::ResourceLifecycle::eOnlyValidNow, &output_extent},
      {&depth, sl::kBufferTypeDepth,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&motion, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&diffuse_albedo, sl::kBufferTypeAlbedo,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&specular_albedo, sl::kBufferTypeSpecularAlbedo,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&normal_roughness, sl::kBufferTypeNormalRoughness,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&specular_hit_distance, sl::kBufferTypeSpecularHitDistance,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&transparency_and_composition,
       sl::kBufferTypeTransparencyAndCompositionMaskHint,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
      {&reactive_mask, sl::kBufferTypeReactiveMaskHint,
       sl::ResourceLifecycle::eOnlyValidNow, &input_extent},
  }};
  const std::array<TaggedTextureExpectation, 10> expectations{{
      {sl::kBufferTypeScalingInputColor,
       VK_FORMAT_R16G16B16A16_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeScalingOutputColor,
       VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_LAYOUT_GENERAL,
       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
       frame.output_width, frame.output_height},
      {sl::kBufferTypeDepth, VK_FORMAT_R32_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeMotionVectors, VK_FORMAT_R32G32_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeAlbedo, VK_FORMAT_R16G16B16A16_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeSpecularAlbedo,
       VK_FORMAT_R16G16B16A16_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeNormalRoughness,
       VK_FORMAT_R16G16B16A16_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeSpecularHitDistance, VK_FORMAT_R32_SFLOAT,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
      {sl::kBufferTypeTransparencyAndCompositionMaskHint,
       VK_FORMAT_R8_UNORM, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
       kInputUsage, frame.render_width, frame.render_height},
      {sl::kBufferTypeReactiveMaskHint, VK_FORMAT_R8_UNORM,
       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, kInputUsage,
       frame.render_width, frame.render_height},
  }};
  if (!validateTaggedTextureSet(
          "DLSS-RR", frame.frame_index, tags, expectations,
          impl_->last_tag_validation_failure, impl_->status)) {
    impl_->force_history_reset = true;
    return false;
  }
  sl::CommandBuffer *command_buffer =
      reinterpret_cast<sl::CommandBuffer *>(frame.command_buffer);
  const sl::Result tag_result = impl_->sl_set_tag_for_frame(
      *token, viewport, tags.data(),
      static_cast<std::uint32_t>(tags.size()), command_buffer);
  if (tag_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS Ray Reconstruction resource tagging failed: ") +
        sl::getResultAsStr(tag_result);
    impl_->force_history_reset = true;
    return false;
  }

  StreamlineDlssFrame constants_frame = frame;
  constants_frame.reset_history =
      frame.reset_history || impl_->force_history_reset ||
      impl_->configured_mode != frame.mode ||
      !impl_->configured_ray_reconstruction;
  const sl::Constants constants = makeDlssConstants(constants_frame);
  const sl::Result constants_result =
      impl_->sl_set_constants(constants, *token, viewport);
  if (constants_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS Ray Reconstruction constants update failed: ") +
        sl::getResultAsStr(constants_result);
    impl_->force_history_reset = true;
    return false;
  }
  impl_->constants_frame_index = frame.frame_index;
  const sl::BaseStructure *inputs[] = {&viewport};
  const sl::Result evaluate_result = impl_->sl_evaluate_feature(
      sl::kFeatureDLSS_RR, *token, inputs, 1u, command_buffer);
  if (evaluate_result != sl::Result::eOk) {
    impl_->status =
        std::string("DLSS Ray Reconstruction evaluation failed: ") +
        sl::getResultAsStr(evaluate_result);
    impl_->force_history_reset = true;
    return false;
  }

  transitionDlssOutput(
      frame.command_buffer, output,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  impl_->last_output_slot = output_slot;
  impl_->configured_mode = frame.mode;
  impl_->configured_ray_reconstruction = true;
  impl_->force_history_reset = false;
  impl_->dlss_rr_runtime_faulted = false;
  impl_->status = "DLSS Ray Reconstruction active";
  return true;
#else
  (void)frame;
  return false;
#endif
}

VkImageView StreamlineVulkanRuntime::dlssOutputView() const noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  return impl_->dlss_outputs[impl_->last_output_slot].view;
#else
  return VK_NULL_HANDLE;
#endif
}

void StreamlineVulkanRuntime::invalidateDlssHistory() noexcept {
  impl_->force_history_reset = true;
}

void StreamlineVulkanRuntime::beginLatencyFrame(
    std::uint32_t frame_index, PathTraceReflexMode mode,
    bool frame_generation_requested) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  requestFrameGeneration(frame_generation_requested);
  (void)bindFrameGenerationPresentThread();
  if (!impl_->initialized) {
    return;
  }
  impl_->simulation_marker_open = false;
  sl::FrameToken *token = frameTokenFor(*impl_, frame_index);
  if (token == nullptr) {
    return;
  }

  const PathTraceReflexMode effective_mode =
      frame_generation_requested && mode == PathTraceReflexMode::Off
          ? PathTraceReflexMode::On
          : mode;
  if (impl_->reflex_supported &&
      impl_->sl_reflex_set_options != nullptr) {
    if (!impl_->reflex_options_valid ||
        impl_->configured_reflex_mode != effective_mode) {
      sl::ReflexOptions options{};
      options.mode = reflexMode(mode, frame_generation_requested);
      const sl::Result result =
          impl_->sl_reflex_set_options(options);
      impl_->reflex_options_valid = result == sl::Result::eOk;
      if (impl_->reflex_options_valid) {
        impl_->configured_reflex_mode = effective_mode;
      } else {
        xpbd::log::warnf("Streamline Reflex options update failed: %s",
                         sl::getResultAsStr(result));
      }
    }
    // NVIDIA requires this call whenever Reflex is supported, including when
    // the effective low-latency mode is Off.
    if (impl_->sl_reflex_sleep != nullptr) {
      const sl::Result sleep_result =
          impl_->sl_reflex_sleep(*token);
      if (sleep_result != sl::Result::eOk) {
        xpbd::log::warnf("Streamline Reflex sleep failed: %s",
                         sl::getResultAsStr(sleep_result));
      }
    }
  }
  setPclMarker(*impl_, sl::PCLMarker::eSimulationStart);
  impl_->simulation_marker_open = true;
#else
  (void)frame_index;
  (void)mode;
  (void)frame_generation_requested;
#endif
}

void StreamlineVulkanRuntime::endLatencySimulation() {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!impl_->simulation_marker_open) {
    return;
  }
  setPclMarker(*impl_, sl::PCLMarker::eSimulationEnd);
  impl_->simulation_marker_open = false;
#endif
}

void StreamlineVulkanRuntime::markRenderSubmitStart() {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  setPclMarker(*impl_, sl::PCLMarker::eRenderSubmitStart);
#endif
}

void StreamlineVulkanRuntime::markRenderSubmitEnd() {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  setPclMarker(*impl_, sl::PCLMarker::eRenderSubmitEnd);
#endif
}

void StreamlineVulkanRuntime::markPresentStart() {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  setPclMarker(*impl_, sl::PCLMarker::ePresentStart);
#endif
}

void StreamlineVulkanRuntime::markPresentEnd() {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  setPclMarker(*impl_, sl::PCLMarker::ePresentEnd);
#endif
}

std::uint32_t
StreamlineVulkanRuntime::pclLatencyPingMessage() const noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  return impl_->pcl_stats_window_message;
#else
  return 0u;
#endif
}

void StreamlineVulkanRuntime::markPclLatencyPing() {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  setPclMarker(*impl_, sl::PCLMarker::ePCLatencyPing);
#endif
}

bool StreamlineVulkanRuntime::recordFrameGenerationInputs(
    const StreamlineFrameGenerationFrame &frame) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!bindFrameGenerationPresentThread() ||
      impl_->swapchain_ownership !=
          SwapchainOwnership::StreamlineFrameGenerationProxy ||
      !impl_->frame_generation_supported ||
      impl_->frame_generation_failure_latched) {
    impl_->frame_generation_status =
        "DLSS Frame Generation inputs rejected outside an armed proxy";
    impl_->frame_generation_state_ok = false;
    return false;
  }
  const bool valid =
      impl_->initialized && impl_->frame_generation_supported &&
      impl_->sl_dlss_g_get_state != nullptr &&
      impl_->sl_dlss_g_set_options != nullptr &&
      impl_->sl_set_tag_for_frame != nullptr &&
      impl_->sl_set_constants != nullptr &&
      impl_->device != VK_NULL_HANDLE &&
      frame.command_buffer != VK_NULL_HANDLE &&
      frame.depth_image != VK_NULL_HANDLE &&
      frame.depth_memory != VK_NULL_HANDLE &&
      frame.depth_view != VK_NULL_HANDLE &&
      frame.motion_image != VK_NULL_HANDLE &&
      frame.motion_memory != VK_NULL_HANDLE &&
      frame.motion_view != VK_NULL_HANDLE &&
      frame.hudless_image != VK_NULL_HANDLE &&
      frame.hudless_memory != VK_NULL_HANDLE &&
      frame.hudless_view != VK_NULL_HANDLE &&
      frame.ui_image != VK_NULL_HANDLE &&
      frame.ui_memory != VK_NULL_HANDLE &&
      frame.ui_view != VK_NULL_HANDLE &&
      frame.view != nullptr && frame.projection != nullptr &&
      frame.color_format != VK_FORMAT_UNDEFINED &&
      frame.render_width > 0u && frame.render_height > 0u &&
      frameGenerationViewportIsValid(
          frame.output_width, frame.output_height, frame.viewport_x,
          frame.viewport_y, frame.viewport_width, frame.viewport_height) &&
      frame.swapchain_image_count > 0u;
  if (!valid) {
    impl_->frame_generation_status =
        "DLSS Frame Generation inputs are incomplete";
    impl_->frame_generation_active = false;
    impl_->frame_generation_state_ok = false;
    impl_->frames_actually_presented = 1u;
    impl_->frame_generation_recovery_required = true;
    return false;
  }

  sl::FrameToken *token = frameTokenFor(*impl_, frame.frame_index);
  if (token == nullptr) {
    impl_->frame_generation_status =
        "DLSS Frame Generation frame token is unavailable";
    impl_->frame_generation_state_ok = false;
    return false;
  }

  const bool resume_reset = impl_->frame_generation_resume_reset_pending;
  if (impl_->constants_frame_index != frame.frame_index) {
    StreamlineDlssFrame constants_frame{};
    constants_frame.view = frame.view;
    constants_frame.projection = frame.projection;
    constants_frame.previous_view = frame.previous_view;
    constants_frame.previous_projection =
        frame.previous_projection;
    constants_frame.render_width = frame.render_width;
    constants_frame.render_height = frame.render_height;
    constants_frame.output_width = frame.output_width;
    constants_frame.output_height = frame.output_height;
    constants_frame.frame_index = frame.frame_index;
    constants_frame.jitter_x = frame.jitter_x;
    constants_frame.jitter_y = frame.jitter_y;
    constants_frame.reset_history = frame.reset_history || resume_reset;
    const sl::Constants constants =
        makeDlssConstants(constants_frame);
    const sl::ViewportHandle constants_viewport{0u};
    const sl::Result constants_result =
        impl_->sl_set_constants(
            constants, *token, constants_viewport);
    if (constants_result != sl::Result::eOk) {
      impl_->frame_generation_status =
          std::string(
              "DLSS Frame Generation constants update failed: ") +
          sl::getResultAsStr(constants_result);
      impl_->frame_generation_state_ok = false;
      return false;
    }
    impl_->constants_frame_index = frame.frame_index;
  }

  sl::DLSSGOptions options{};
  options.mode = sl::DLSSGMode::eOn;
  options.numFramesToGenerate = 1u;
  options.numBackBuffers = frame.swapchain_image_count;
  options.mvecDepthWidth = frame.render_width;
  options.mvecDepthHeight = frame.render_height;
  options.colorWidth = frame.output_width;
  options.colorHeight = frame.output_height;
  options.colorBufferFormat =
      static_cast<std::uint32_t>(frame.color_format);
  options.mvecBufferFormat =
      static_cast<std::uint32_t>(VK_FORMAT_R32G32_SFLOAT);
  options.depthBufferFormat =
      static_cast<std::uint32_t>(VK_FORMAT_R32_SFLOAT);
  options.hudLessBufferFormat =
      static_cast<std::uint32_t>(frame.color_format);
  options.uiBufferFormat =
      static_cast<std::uint32_t>(frame.color_format);
  options.queueParallelismMode =
      sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
  options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
  options.onErrorCallback = onDlssGApiError;

  Impl::FrameGenerationOptionsKey options_key{};
  options_key.valid = true;
  options_key.mode = options.mode;
  options_key.num_frames_to_generate = options.numFramesToGenerate;
  options_key.num_back_buffers = options.numBackBuffers;
  options_key.mvec_depth_width = options.mvecDepthWidth;
  options_key.mvec_depth_height = options.mvecDepthHeight;
  options_key.color_width = options.colorWidth;
  options_key.color_height = options.colorHeight;
  options_key.color_format = options.colorBufferFormat;
  options_key.mvec_format = options.mvecBufferFormat;
  options_key.depth_format = options.depthBufferFormat;
  options_key.hudless_format = options.hudLessBufferFormat;
  options_key.ui_format = options.uiBufferFormat;
  options_key.queue_mode = options.queueParallelismMode;
  options_key.ui_recomposition = options.enableUserInterfaceRecomposition;
  const bool options_changed =
      !impl_->frame_generation_options_enabled ||
      !(impl_->frame_generation_options_key == options_key);

  const sl::ViewportHandle viewport{0u};
  if (options_changed) {
    sl::DLSSGState prospective_state{};
    const sl::Result state_result =
        impl_->sl_dlss_g_get_state(viewport, prospective_state, &options);
    if (state_result != sl::Result::eOk) {
      impl_->frame_generation_status =
          std::string("DLSS Frame Generation state query failed: ") +
          sl::getResultAsStr(state_result);
      impl_->frame_generation_state_ok = false;
      impl_->frame_generation_recovery_required = true;
      impl_->frame_generation_failure_latched = true;
      return false;
    }
    if (prospective_state.status != sl::DLSSGStatus::eOk) {
      impl_->frame_generation_status =
          "DLSS Frame Generation cannot be enabled in the current runtime "
          "state (" +
          std::to_string(
              static_cast<std::uint32_t>(prospective_state.status)) +
          ")";
      impl_->frame_generation_state_ok = false;
      impl_->frame_generation_recovery_required = true;
      impl_->frame_generation_failure_latched = true;
      return false;
    }
    if ((prospective_state.minWidthOrHeight > 0u &&
         (frame.viewport_width < prospective_state.minWidthOrHeight ||
          frame.viewport_height < prospective_state.minWidthOrHeight)) ||
        prospective_state.numFramesToGenerateMax < 1u) {
      impl_->frame_generation_status =
          "DLSS Frame Generation is unsupported at the current preview size";
      impl_->frame_generation_state_ok = false;
      impl_->frame_generation_recovery_required = true;
      return false;
    }
    // The prospective query is the SDK's authoritative validation for the
    // options that will be submitted below.
    impl_->frame_generation_state_ok = true;
  }

  constexpr VkImageUsageFlags kGuideUsage =
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  sl::Resource depth = textureResource(
      frame.depth_image, frame.depth_memory, frame.depth_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R32_SFLOAT,
      kGuideUsage, frame.render_width, frame.render_height);
  sl::Resource motion = textureResource(
      frame.motion_image, frame.motion_memory, frame.motion_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_FORMAT_R32G32_SFLOAT, kGuideUsage,
      frame.render_width, frame.render_height);
  sl::Resource hudless = textureResource(
      frame.hudless_image, frame.hudless_memory, frame.hudless_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, frame.color_format,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      frame.output_width, frame.output_height);
  sl::Resource ui = textureResource(
      frame.ui_image, frame.ui_memory, frame.ui_view,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, frame.color_format,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      frame.output_width, frame.output_height);
  const sl::Extent guide_extent{
      0u, 0u, frame.render_width, frame.render_height};
  const FrameGenerationTagExtent tag_extent =
      makeFrameGenerationTagExtent(
          frame.viewport_x, frame.viewport_y, frame.viewport_width,
          frame.viewport_height);
  sl::Extent viewport_extent{};
  viewport_extent.top = tag_extent.top;
  viewport_extent.left = tag_extent.left;
  viewport_extent.width = tag_extent.width;
  viewport_extent.height = tag_extent.height;
  // FG is intentionally limited to the preview subrect. NVIDIA requires the
  // HUD-less and UI tag extents to exactly match the tagged backbuffer extent,
  // even when their underlying images cover the full swapchain.
  const sl::Extent &color_extent = viewport_extent;
  const std::array<sl::ResourceTag, 5> tags{{
      {&depth, sl::kBufferTypeDepth,
       sl::ResourceLifecycle::eOnlyValidNow, &guide_extent},
      {&motion, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eOnlyValidNow, &guide_extent},
      {&hudless, sl::kBufferTypeHUDLessColor,
       sl::ResourceLifecycle::eValidUntilPresent, &color_extent},
      {&ui, sl::kBufferTypeUIColorAndAlpha,
       sl::ResourceLifecycle::eValidUntilPresent, &color_extent},
      {nullptr, sl::kBufferTypeBackbuffer,
       sl::ResourceLifecycle::eValidUntilPresent, &color_extent},
  }};
  sl::CommandBuffer *command_buffer =
      reinterpret_cast<sl::CommandBuffer *>(frame.command_buffer);
  const sl::Result tag_result = impl_->sl_set_tag_for_frame(
      *token, viewport, tags.data(),
      static_cast<std::uint32_t>(tags.size()), command_buffer);
  if (tag_result != sl::Result::eOk) {
    impl_->frame_generation_status =
        std::string("DLSS Frame Generation resource tagging failed: ") +
        sl::getResultAsStr(tag_result);
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_recovery_required = true;
    impl_->frame_generation_failure_latched = true;
    return false;
  }
  if (options_changed) {
    const sl::Result options_result =
        impl_->sl_dlss_g_set_options(viewport, options);
    if (options_result != sl::Result::eOk) {
      impl_->frame_generation_status =
          std::string("DLSS Frame Generation options update failed: ") +
          sl::getResultAsStr(options_result);
      impl_->frame_generation_state_ok = false;
      impl_->frame_generation_recovery_required = true;
      impl_->frame_generation_failure_latched = true;
      return false;
    }
    impl_->frame_generation_options = options;
    impl_->frame_generation_options_key = options_key;
    impl_->frame_generation_options_enabled = true;
    impl_->frame_generation_disable = {};
    impl_->frame_generation_disable_result = 0;
    ++impl_->frame_generation.options_generation;
  }
  ++impl_->frame_generation.tag_generation;
  impl_->frame_generation_tag_frame_index = frame.frame_index;
  impl_->frame_generation.valid_inputs_tagged = true;
  impl_->frame_generation_resume_reset_pending = false;
  impl_->frame_generation_recovery_required = false;
  impl_->frame_generation_status =
      "DLSS Frame Generation inputs ready for present";
  setFrameGenerationState(
      *impl_, frameGenerationStateAfterValidInputTagging(
                  impl_->frame_generation.state),
      frame.frame_index, "valid inputs tagged");
  return true;
#else
  (void)frame;
  return false;
#endif
}

bool StreamlineVulkanRuntime::clearFrameGenerationInputs(
    VkCommandBuffer command_buffer,
    std::uint32_t frame_index,
    std::uint32_t output_width,
    std::uint32_t output_height,
    std::uint32_t viewport_x,
    std::uint32_t viewport_y,
    std::uint32_t viewport_width,
    std::uint32_t viewport_height) noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  (void)output_width;
  (void)output_height;
  (void)viewport_x;
  (void)viewport_y;
  (void)viewport_width;
  (void)viewport_height;
  if (!impl_->initialized || !impl_->frame_generation_supported) {
    return !impl_->frame_generation_options_enabled &&
           impl_->frame_generation.tag_generation == 0u;
  }
  // Invalid guide frames keep the proxy swapchain but must not leave the
  // interpolation feature enabled. Pause options before clearing tags so the
  // SDK cannot interpolate a current backbuffer from stale guide resources.
  if (impl_->frame_generation_options_enabled &&
      !disableFrameGeneration()) {
    return false;
  }
  impl_->frame_generation_resume_reset_pending = true;
  impl_->force_history_reset = true;
  impl_->constants_frame_index =
      (std::numeric_limits<std::uint32_t>::max)();
  impl_->frame_generation_tag_frame_index = frame_index;

  if (impl_->frame_generation.tag_generation == 0u) {
    impl_->frame_generation.valid_inputs_tagged = false;
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_active = false;
    impl_->frames_actually_presented = 1u;
    if (impl_->frame_generation_feature_loaded &&
        impl_->swapchain_ownership ==
            SwapchainOwnership::StreamlineFrameGenerationProxy &&
        frameGenerationInputClearMayArmProxy(
            impl_->frame_generation.state)) {
      setFrameGenerationState(*impl_, FrameGenerationRuntimeState::ProxyArmed,
                              frame_index, "invalid inputs paused");
    }
    return true;
  }

  const sl::ViewportHandle viewport{0u};
  sl::FrameToken *token = frameTokenFor(*impl_, frame_index);
  bool null_tags_ok = false;
  // A true null-tag clears both the resource pointer and the extent. Keeping
  // the previous Backbuffer subrect here makes Streamline treat an invalid
  // frame as a valid subregion and can leave stale interpolation state alive.
  const std::array<sl::ResourceTag, 5> tags{{
      {nullptr, sl::kBufferTypeDepth,
       sl::ResourceLifecycle::eValidUntilPresent, nullptr},
      {nullptr, sl::kBufferTypeMotionVectors,
       sl::ResourceLifecycle::eValidUntilPresent, nullptr},
      {nullptr, sl::kBufferTypeHUDLessColor,
       sl::ResourceLifecycle::eValidUntilPresent, nullptr},
      {nullptr, sl::kBufferTypeUIColorAndAlpha,
       sl::ResourceLifecycle::eValidUntilPresent, nullptr},
      {nullptr, sl::kBufferTypeBackbuffer,
       sl::ResourceLifecycle::eValidUntilPresent, nullptr},
  }};
  sl::CommandBuffer *sl_command_buffer =
      reinterpret_cast<sl::CommandBuffer *>(command_buffer);
  sl::Result tag_result = sl::Result::eErrorIO;
  if (token != nullptr && impl_->sl_set_tag_for_frame != nullptr) {
    tag_result = impl_->sl_set_tag_for_frame(
        *token, viewport, tags.data(),
        static_cast<std::uint32_t>(tags.size()), sl_command_buffer);
    null_tags_ok = tag_result == sl::Result::eOk;
  }
  if (!null_tags_ok) {
    impl_->frame_generation_status =
        token == nullptr
            ? "DLSS Frame Generation paused; frame token unavailable for "
              "null tags"
            : impl_->sl_set_tag_for_frame == nullptr
                ? "DLSS Frame Generation paused; null-tag API unavailable"
            : std::string("DLSS Frame Generation null-tag update failed: ") +
                  sl::getResultAsStr(tag_result);
    frameGenerationRecordDisableCleanupFailure(
        impl_->frame_generation_disable);
    synchronizeFrameGenerationDisableFailure(*impl_);
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
        frame_index, "DLSS-G null-tag clear failed");
    return false;
  }
  impl_->frame_generation_status =
      "DLSS Frame Generation paused until stable inputs resume";
  impl_->frame_generation.valid_inputs_tagged = false;
  impl_->frame_generation.tag_generation = 0u;
  impl_->frame_generation_state_ok = false;
  impl_->frame_generation_active = false;
  impl_->frames_actually_presented = 1u;
  xpbd::log::infof("DLSSG resource tags cleared: frame=%u", frame_index);
  if (impl_->frame_generation_feature_loaded &&
      impl_->swapchain_ownership ==
          SwapchainOwnership::StreamlineFrameGenerationProxy &&
      frameGenerationInputClearMayArmProxy(
          impl_->frame_generation.state)) {
    setFrameGenerationState(*impl_, FrameGenerationRuntimeState::ProxyArmed,
                            frame_index, "invalid inputs paused");
  }
  return true;
#else
  (void)command_buffer;
  (void)frame_index;
  (void)output_width;
  (void)output_height;
  (void)viewport_x;
  (void)viewport_y;
  (void)viewport_width;
  (void)viewport_height;
  return true;
#endif
}

bool StreamlineVulkanRuntime::disableFrameGeneration() noexcept {
  if (!bindFrameGenerationPresentThread()) {
    return false;
  }
  if (impl_->frame_generation_disable.confirmed_off) {
    return true;
  }
  if (!frameGenerationDisableAttemptAllowed(
          impl_->frame_generation_disable)) {
    if (impl_->frame_generation_disable.exhausted) {
      synchronizeFrameGenerationDisableFailure(*impl_);
      impl_->frame_generation_status =
          "DLSS Frame Generation Options-Off retry limit reached";
    } else {
      impl_->frame_generation_status =
          "DLSS Frame Generation Options-Off retry requires a completed "
          "GPU/Present drain";
    }
    return false;
  }

  bool sdk_confirmed_off = !impl_->frame_generation_options_enabled;
  impl_->frame_generation_disable_result = 0;
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->frame_generation_options_enabled &&
      impl_->frame_generation_injected_disable_failures_remaining != 0u) {
    --impl_->frame_generation_injected_disable_failures_remaining;
    sdk_confirmed_off = false;
    impl_->frame_generation_disable_result = -2;
    impl_->frame_generation_status =
        "R0F G03 injected DLSS Frame Generation Options-Off failure";
    xpbd::log::warnf(
        "R0F_G03_INJECTED_DISABLE_FAILURE attempt=%u/%u "
        "completed_drains=%u remaining=%u",
        impl_->frame_generation_disable.attempts + 1u,
        kFrameGenerationDisableMaxAttempts,
        impl_->frame_generation_disable.completed_drains,
        impl_->frame_generation_injected_disable_failures_remaining);
  } else if (impl_->initialized &&
             impl_->sl_dlss_g_set_options != nullptr &&
      impl_->frame_generation_supported &&
      impl_->frame_generation_options_enabled) {
    sl::DLSSGOptions options = impl_->frame_generation_options;
    options.mode = sl::DLSSGMode::eOff;
    const sl::ViewportHandle viewport{0u};
    const sl::Result result =
        impl_->sl_dlss_g_set_options(viewport, options);
    impl_->frame_generation_disable_result =
        static_cast<std::int32_t>(result);
    sdk_confirmed_off = result == sl::Result::eOk;
    if (sdk_confirmed_off) {
      impl_->frame_generation_options = options;
      ++impl_->frame_generation.options_generation;
    } else {
      impl_->frame_generation_status =
          std::string("DLSS Frame Generation Options-Off failed: ") +
          sl::getResultAsStr(result);
      xpbd::log::warnf(
          "Streamline DLSS Frame Generation disable attempt %u/%u failed: %s",
          impl_->frame_generation_disable.attempts + 1u,
          kFrameGenerationDisableMaxAttempts, sl::getResultAsStr(result));
    }
  } else if (impl_->frame_generation_options_enabled) {
    sdk_confirmed_off = false;
    impl_->frame_generation_disable_result = -1;
    impl_->frame_generation_status =
        "DLSS Frame Generation Options-Off API is unavailable";
  }
#else
  if (impl_->frame_generation_options_enabled) {
    sdk_confirmed_off = false;
    impl_->frame_generation_disable_result = -1;
    impl_->frame_generation_status =
        "DLSS Frame Generation Options-Off is unavailable in this build";
  }
#endif

  const bool recorded = frameGenerationRecordDisableAttempt(
      impl_->frame_generation_disable, sdk_confirmed_off);
  if (!recorded) {
    return false;
  }
  if (!sdk_confirmed_off) {
    synchronizeFrameGenerationDisableFailure(*impl_);
    setFrameGenerationState(
        *impl_, FrameGenerationRuntimeState::FaultedRecoveringNative,
        impl_->frame_generation_present_frame_index,
        "DLSS-G Options-Off was not confirmed");
    return false;
  }

  impl_->frame_generation_options_enabled = false;
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  impl_->frame_generation_options_key.valid = false;
#endif
  impl_->frame_generation_state_ok = false;
  impl_->frame_generation_active = false;
  impl_->frames_actually_presented = 1u;
  impl_->frame_generation_state_poll_pending = false;
  impl_->frame_generation_state_poll_inputs_ready = false;
  xpbd::log::infof(
      "DLSSG Options-Off confirmed: attempt=%u/%u result=%d",
      impl_->frame_generation_disable.attempts,
      kFrameGenerationDisableMaxAttempts,
      impl_->frame_generation_disable_result);
  return true;
}

FrameGenerationTransitionResult StreamlineVulkanRuntime::
pollFrameGenerationStateBeforePresent() noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!impl_->frame_generation_state_poll_pending) {
    return FrameGenerationTransitionResult::NoAction;
  }
  if (!bindFrameGenerationPresentThread()) {
    return FrameGenerationTransitionResult::RecoverNative;
  }
  const bool inputs_ready = impl_->frame_generation_state_poll_inputs_ready;
  const std::uint32_t presented_frame_index =
      impl_->frame_generation_state_poll_frame_index;
  impl_->frame_generation_state_poll_pending = false;
  impl_->frame_generation_state_poll_inputs_ready = false;
  if (!impl_->frame_generation_supported ||
      impl_->sl_dlss_g_get_state == nullptr) {
    impl_->frame_generation_active = false;
    impl_->frame_generation_state_ok = false;
    impl_->frames_actually_presented = 1u;
    return FrameGenerationTransitionResult::NoAction;
  }
  const sl::ViewportHandle viewport{0u};
  sl::DLSSGState state{};
  const sl::Result result =
      impl_->sl_dlss_g_get_state(viewport, state, nullptr);
  if (result != sl::Result::eOk ||
      state.status != sl::DLSSGStatus::eOk) {
    impl_->frame_generation_state_ok = false;
    impl_->frame_generation_status =
        result != sl::Result::eOk
            ? std::string("DLSS Frame Generation post-present state failed: ") +
                  sl::getResultAsStr(result)
            : "DLSS Frame Generation reported an invalid runtime state (" +
                  std::to_string(
                      static_cast<std::uint32_t>(state.status)) +
                  ")";
    requestFrameGenerationNativeRecovery(
        impl_->frame_generation_status.c_str(), true);
    return FrameGenerationTransitionResult::RecoverNative;
  }
  if (!inputs_ready) {
    impl_->frame_generation_active = false;
    impl_->frame_generation_state_ok = true;
    impl_->frames_actually_presented = 1u;
    impl_->frame_generation_status =
        "DLSS Frame Generation paused; presented-frame options, constants, "
        "or tags were incomplete";
    if (impl_->frame_generation_feature_loaded &&
        impl_->swapchain_ownership ==
            SwapchainOwnership::StreamlineFrameGenerationProxy) {
      setFrameGenerationState(
          *impl_, FrameGenerationRuntimeState::ProxyArmed,
          presented_frame_index,
          "presented-frame inputs were incomplete");
    }
    return FrameGenerationTransitionResult::NoAction;
  }
  const bool was_active = impl_->frame_generation_active;
  const std::uint32_t previous_presented =
      impl_->frames_actually_presented;
  impl_->frames_actually_presented =
      (std::max)(1u, state.numFramesActuallyPresented);
  impl_->frame_generation_active =
      state.numFramesActuallyPresented > 1u;
  if (impl_->frame_generation_active != was_active ||
      impl_->frames_actually_presented != previous_presented) {
    xpbd::log::infof(
        "Streamline DLSS-G present state: active=%d "
        "frames_actually_presented=%u",
        impl_->frame_generation_active ? 1 : 0,
        impl_->frames_actually_presented);
  }
  impl_->frame_generation_status =
      impl_->frame_generation_active
          ? "DLSS Frame Generation active"
          : "DLSS Frame Generation enabled; waiting for generated presents";
  impl_->frame_generation_state_ok = true;
  impl_->frame_generation_present_frame_index =
      presented_frame_index;
  setFrameGenerationState(
      *impl_, impl_->frame_generation_active
                  ? FrameGenerationRuntimeState::Active
                  : FrameGenerationRuntimeState::ProxyArmed,
      presented_frame_index,
      impl_->frame_generation_active
          ? "generated present confirmed"
          : "waiting for generated present");
  return FrameGenerationTransitionResult::NoAction;
#else
  impl_->frame_generation_active = false;
  impl_->frames_actually_presented = 1u;
  return FrameGenerationTransitionResult::NoAction;
#endif
}

FrameGenerationTransitionResult StreamlineVulkanRuntime::
updateFrameGenerationStateAfterPresent(
    VkResult present_result) noexcept {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (!bindFrameGenerationPresentThread()) {
    return FrameGenerationTransitionResult::RecoverNative;
  }
  const FrameGenerationTransitionResult api_result =
      classifyFrameGenerationVkResult(present_result, "Present");
  if (api_result != FrameGenerationTransitionResult::NoAction) {
    impl_->frame_generation_state_poll_pending = false;
    impl_->frame_generation_state_poll_inputs_ready = false;
    return api_result;
  }

  const bool current_frame_inputs_ready =
      frameGenerationCurrentFrameInputsReady(
          impl_->frame_generation_options_enabled,
          impl_->frame_generation_options_key.valid,
          impl_->frame_generation.tag_generation != 0u,
          impl_->current_frame_index, impl_->constants_frame_index,
          impl_->frame_generation_tag_frame_index);
  impl_->frame_generation_present_frame_index = impl_->current_frame_index;
  impl_->frame_generation_state_poll_frame_index = impl_->current_frame_index;
  impl_->frame_generation_state_poll_inputs_ready = current_frame_inputs_ready;
  impl_->frame_generation_state_poll_pending =
      impl_->frame_generation_feature_loaded &&
      impl_->frame_generation_supported &&
      impl_->swapchain_ownership ==
          SwapchainOwnership::StreamlineFrameGenerationProxy &&
      impl_->sl_dlss_g_get_state != nullptr;
  if (!current_frame_inputs_ready) {
    impl_->frame_generation_active = false;
    impl_->frame_generation_state_ok = false;
    impl_->frames_actually_presented = 1u;
    impl_->frame_generation_status =
        "DLSS Frame Generation paused; current-frame options, constants, "
        "or tags are incomplete";
    if (impl_->frame_generation_feature_loaded &&
        impl_->swapchain_ownership ==
            SwapchainOwnership::StreamlineFrameGenerationProxy) {
      setFrameGenerationState(
          *impl_, FrameGenerationRuntimeState::ProxyArmed,
          impl_->current_frame_index,
          "current-frame inputs are incomplete after present");
    }
  }
  return FrameGenerationTransitionResult::NoAction;
#else
  impl_->frame_generation_active = false;
  impl_->frames_actually_presented = 1u;
  (void)present_result;
  return FrameGenerationTransitionResult::NoAction;
#endif
}

VkResult StreamlineVulkanRuntime::createInstance(
    const VkInstanceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkInstance *instance) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->create_instance != nullptr) {
    const VkResult result =
        impl_->create_instance(create_info, allocator, instance);
    if (result == VK_SUCCESS) {
      impl_->instance = *instance;
      impl_->enumerate_physical_devices =
          reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
              impl_->get_instance_proc_addr(
                  *instance, "vkEnumeratePhysicalDevices"));
      impl_->create_win32_surface =
          reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
              impl_->get_instance_proc_addr(
                  *instance, "vkCreateWin32SurfaceKHR"));
      impl_->destroy_surface =
          reinterpret_cast<PFN_vkDestroySurfaceKHR>(
              impl_->get_instance_proc_addr(
                  *instance, "vkDestroySurfaceKHR"));
    }
    return result;
  }
#endif
  return vkCreateInstance(create_info, allocator, instance);
}

VkResult StreamlineVulkanRuntime::enumeratePhysicalDevices(
    VkInstance instance, std::uint32_t *count,
    VkPhysicalDevice *physical_devices) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized &&
      impl_->enumerate_physical_devices != nullptr) {
    return impl_->enumerate_physical_devices(
        instance, count, physical_devices);
  }
#endif
  return vkEnumeratePhysicalDevices(instance, count, physical_devices);
}

VkResult StreamlineVulkanRuntime::createDevice(
    VkInstance instance, VkPhysicalDevice physical_device,
    const VkDeviceCreateInfo *create_info,
    const VkAllocationCallbacks *allocator, VkDevice *device) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized) {
    if (impl_->create_device == nullptr) {
      impl_->create_device =
          reinterpret_cast<PFN_vkCreateDevice>(
              impl_->get_instance_proc_addr(instance, "vkCreateDevice"));
    }
    if (impl_->create_device != nullptr) {
      const VkResult result = impl_->create_device(
          physical_device, create_info, allocator, device);
      if (result == VK_SUCCESS) {
        impl_->instance = instance;
        impl_->physical_device = physical_device;
        impl_->device = *device;
        impl_->create_swapchain =
            reinterpret_cast<PFN_vkCreateSwapchainKHR>(
                impl_->get_device_proc_addr(
                    *device, "vkCreateSwapchainKHR"));
        impl_->destroy_swapchain =
            reinterpret_cast<PFN_vkDestroySwapchainKHR>(
                impl_->get_device_proc_addr(
                    *device, "vkDestroySwapchainKHR"));
        impl_->get_swapchain_images =
            reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
                impl_->get_device_proc_addr(
                    *device, "vkGetSwapchainImagesKHR"));
        impl_->acquire_next_image =
            reinterpret_cast<PFN_vkAcquireNextImageKHR>(
                impl_->get_device_proc_addr(
                    *device, "vkAcquireNextImageKHR"));
        impl_->queue_present =
            reinterpret_cast<PFN_vkQueuePresentKHR>(
                impl_->get_device_proc_addr(*device, "vkQueuePresentKHR"));
        impl_->device_wait_idle =
            reinterpret_cast<PFN_vkDeviceWaitIdle>(
                impl_->get_device_proc_addr(*device, "vkDeviceWaitIdle"));
        const bool mandatory_hooks_ready =
            impl_->create_win32_surface != nullptr &&
            impl_->destroy_surface != nullptr &&
            impl_->create_swapchain != nullptr &&
            impl_->destroy_swapchain != nullptr &&
            impl_->get_swapchain_images != nullptr &&
            impl_->acquire_next_image != nullptr &&
            impl_->queue_present != nullptr &&
            impl_->device_wait_idle != nullptr;
        if (!mandatory_hooks_ready) {
          impl_->status =
              "Streamline Vulkan mandatory hooks are incomplete; "
              "using native Vulkan";
          xpbd::log::warn(impl_->status);
          shutdownBeforeVulkan();
        }
      }
      return result;
    }
  }
#else
  (void)instance;
#endif
  return vkCreateDevice(physical_device, create_info, allocator, device);
}

VkResult StreamlineVulkanRuntime::createWin32Surface(
    VkInstance instance, void *window_handle,
    const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->create_win32_surface != nullptr &&
      window_handle != nullptr) {
    VkWin32SurfaceCreateInfoKHR create_info{
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    create_info.hinstance = GetModuleHandleW(nullptr);
    create_info.hwnd = static_cast<HWND>(window_handle);
    return impl_->create_win32_surface(instance, &create_info, allocator,
                                       surface);
  }
#endif
#if defined(_WIN32)
  if (window_handle != nullptr && surface != nullptr) {
    VkWin32SurfaceCreateInfoKHR create_info{
        VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    create_info.hinstance = GetModuleHandleW(nullptr);
    create_info.hwnd = static_cast<HWND>(window_handle);
    return vkCreateWin32SurfaceKHR(instance, &create_info, allocator, surface);
  }
#else
  (void)instance;
  (void)window_handle;
  (void)allocator;
  (void)surface;
#endif
  return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void StreamlineVulkanRuntime::destroySurface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks *allocator) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->destroy_surface != nullptr) {
    impl_->destroy_surface(instance, surface, allocator);
    return;
  }
#endif
  vkDestroySurfaceKHR(instance, surface, allocator);
}

VkResult StreamlineVulkanRuntime::createSwapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR *create_info,
    const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain,
    SwapchainOwnership *ownership) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->create_swapchain != nullptr) {
    const VkResult result =
        impl_->create_swapchain(device, create_info, allocator, swapchain);
    if (result == VK_SUCCESS) {
      impl_->swapchain_ownership =
          impl_->frame_generation_feature_loaded &&
                  impl_->frame_generation_context_available
              ? SwapchainOwnership::StreamlineFrameGenerationProxy
              : SwapchainOwnership::Native;
      impl_->swapchain_present = true;
      if (ownership != nullptr) {
        *ownership = impl_->swapchain_ownership;
      }
    }
    return result;
  }
#endif
  const VkResult result =
      vkCreateSwapchainKHR(device, create_info, allocator, swapchain);
  if (result == VK_SUCCESS) {
    impl_->swapchain_ownership = SwapchainOwnership::Native;
    impl_->swapchain_present = true;
    if (ownership != nullptr) {
      *ownership = SwapchainOwnership::Native;
    }
  }
  return result;
}

void StreamlineVulkanRuntime::destroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks *allocator) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->destroy_swapchain != nullptr) {
    impl_->destroy_swapchain(device, swapchain, allocator);
    return;
  }
#endif
  vkDestroySwapchainKHR(device, swapchain, allocator);
}

VkResult StreamlineVulkanRuntime::getSwapchainImages(
    VkDevice device, VkSwapchainKHR swapchain, std::uint32_t *count,
    VkImage *images) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->get_swapchain_images != nullptr) {
    return impl_->get_swapchain_images(device, swapchain, count, images);
  }
#endif
  return vkGetSwapchainImagesKHR(device, swapchain, count, images);
}

VkResult StreamlineVulkanRuntime::acquireNextImage(
    VkDevice device, VkSwapchainKHR swapchain, std::uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, std::uint32_t *image_index) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->acquire_next_image != nullptr) {
    return impl_->acquire_next_image(device, swapchain, timeout, semaphore,
                                     fence, image_index);
  }
#endif
  return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence,
                               image_index);
}

VkResult StreamlineVulkanRuntime::queuePresent(
    VkQueue queue, const VkPresentInfoKHR *present_info) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->queue_present != nullptr) {
    return impl_->queue_present(queue, present_info);
  }
#endif
  return vkQueuePresentKHR(queue, present_info);
}

VkResult StreamlineVulkanRuntime::deviceWaitIdle(VkDevice device) {
#if XPBD_WITH_STREAMLINE && defined(_WIN32)
  if (impl_->initialized && impl_->device_wait_idle != nullptr) {
    return impl_->device_wait_idle(device);
  }
#endif
  return vkDeviceWaitIdle(device);
}

} // namespace xpbd::gfx
