#pragma once

#include <cstdint>

namespace xpbd::gfx {

enum class FrameGenerationRuntimeState {
  Unsupported,
  NativeOff,
  EnablingDrain,
  EnablingLoadPlugin,
  EnablingCreateProxySwapchain,
  ProxyArmed,
  Active,
  DisablingOptions,
  DisablingDrain,
  DisablingDestroyProxySwapchain,
  DisablingUnloadPlugin,
  FaultedRecoveringNative,
  ShuttingDown,
};

enum class SwapchainOwnership {
  Native,
  StreamlineFrameGenerationProxy,
};

enum class FrameGenerationTransitionResult {
  NoAction,
  RecreateNative,
  RecreateProxy,
  RecoverNative,
  FatalDeviceLost,
};

// Streamline names subrect coordinates `top` and `left`, in that order.
// Keep this SDK-independent representation testable so callers cannot
// accidentally aggregate-initialize an sl::Extent as {x, y, width, height}.
struct FrameGenerationTagExtent {
  std::uint32_t top = 0u;
  std::uint32_t left = 0u;
  std::uint32_t width = 0u;
  std::uint32_t height = 0u;
};

[[nodiscard]] constexpr FrameGenerationTagExtent
makeFrameGenerationTagExtent(std::uint32_t viewport_x,
                             std::uint32_t viewport_y,
                             std::uint32_t viewport_width,
                             std::uint32_t viewport_height) noexcept {
  return FrameGenerationTagExtent{viewport_y, viewport_x, viewport_width,
                                  viewport_height};
}

[[nodiscard]] constexpr bool frameGenerationViewportIsValid(
    std::uint32_t output_width, std::uint32_t output_height,
    std::uint32_t viewport_x, std::uint32_t viewport_y,
    std::uint32_t viewport_width,
    std::uint32_t viewport_height) noexcept {
  return output_width > 0u && output_height > 0u &&
         viewport_width > 0u && viewport_height > 0u &&
         viewport_x <= output_width && viewport_y <= output_height &&
         viewport_width <= output_width - viewport_x &&
         viewport_height <= output_height - viewport_y;
}

// If a temporal reconstruction pass was requested, FG must consume only its
// successful output. Feeding the low-resolution raw fallback to FG magnifies
// path-tracing noise and mismatches the intended color/guide contract.
[[nodiscard]] constexpr bool frameGenerationTemporalInputIsReady(
    bool temporal_reconstruction_requested,
    bool temporal_reconstruction_active) noexcept {
  return !temporal_reconstruction_requested ||
         temporal_reconstruction_active;
}

// A native acquire semaphore only protects the swapchain image, so work that
// does not touch it may run before COLOR_ATTACHMENT_OUTPUT. Streamline's
// Vulkan DLSS-G acquire semaphore is also the frame-start handoff: its wait
// must precede every command in the next frame.
[[nodiscard]] constexpr bool frameGenerationAcquireMustPrecedeFrame(
    SwapchainOwnership ownership) noexcept {
  return ownership ==
         SwapchainOwnership::StreamlineFrameGenerationProxy;
}

[[nodiscard]] const char *frameGenerationRuntimeStateName(
    FrameGenerationRuntimeState state) noexcept;
[[nodiscard]] const char *swapchainOwnershipName(
    SwapchainOwnership ownership) noexcept;

// Pure legality table used by runtime assertions and regression tests.  It
// intentionally accepts an idempotent Native recreate through the disabling
// states because resize/F11/dialog transitions share the same transaction.
[[nodiscard]] constexpr bool frameGenerationRuntimeCombinationIsLegal(
    FrameGenerationRuntimeState state, bool plugin_loaded,
    SwapchainOwnership ownership, bool options_on,
    bool valid_inputs_tagged) noexcept {
  if (state == FrameGenerationRuntimeState::Unsupported ||
      state == FrameGenerationRuntimeState::NativeOff) {
    return !plugin_loaded && ownership == SwapchainOwnership::Native &&
           !options_on && !valid_inputs_tagged;
  }
  if (state == FrameGenerationRuntimeState::EnablingDrain ||
      state == FrameGenerationRuntimeState::EnablingLoadPlugin) {
    return !options_on && !valid_inputs_tagged &&
           (ownership == SwapchainOwnership::Native ||
            ownership ==
                SwapchainOwnership::StreamlineFrameGenerationProxy);
  }
  if (state ==
      FrameGenerationRuntimeState::EnablingCreateProxySwapchain) {
    return plugin_loaded && ownership == SwapchainOwnership::Native &&
           !options_on && !valid_inputs_tagged;
  }
  if (state == FrameGenerationRuntimeState::ProxyArmed) {
    return plugin_loaded &&
           ownership ==
               SwapchainOwnership::StreamlineFrameGenerationProxy &&
           (!valid_inputs_tagged || options_on);
  }
  if (state == FrameGenerationRuntimeState::Active) {
    return plugin_loaded &&
           ownership ==
               SwapchainOwnership::StreamlineFrameGenerationProxy &&
           options_on && valid_inputs_tagged;
  }
  if (state == FrameGenerationRuntimeState::DisablingOptions) {
    const bool already_native =
        !plugin_loaded && ownership == SwapchainOwnership::Native &&
        !options_on && !valid_inputs_tagged;
    const bool disabling_proxy =
        plugin_loaded &&
        ownership ==
            SwapchainOwnership::StreamlineFrameGenerationProxy &&
        (!valid_inputs_tagged || options_on);
    return already_native || disabling_proxy;
  }
  if (state == FrameGenerationRuntimeState::DisablingDrain ||
      state ==
          FrameGenerationRuntimeState::DisablingDestroyProxySwapchain ||
      state == FrameGenerationRuntimeState::DisablingUnloadPlugin ||
      state == FrameGenerationRuntimeState::FaultedRecoveringNative) {
    return !options_on && !valid_inputs_tagged;
  }
  return state == FrameGenerationRuntimeState::ShuttingDown;
}

// DLSS-G may only become Active after the options and both per-frame SDK
// submissions all refer to the frame that is about to be presented.  Keeping
// this predicate pure makes stale-tag/constant transition bugs testable
// without loading Streamline.
[[nodiscard]] constexpr bool frameGenerationCurrentFrameInputsReady(
    bool options_on, bool options_key_valid, bool valid_inputs_tagged,
    std::uint32_t current_frame_index,
    std::uint32_t constants_frame_index,
    std::uint32_t tag_frame_index) noexcept {
  return options_on && options_key_valid && valid_inputs_tagged &&
         constants_frame_index == current_frame_index &&
         tag_frame_index == current_frame_index;
}

// Tagging a new valid frame does not make an already confirmed Active proxy
// less active.  The post-Present state query is the only authority that may
// move Active back to ProxyArmed.
[[nodiscard]] constexpr FrameGenerationRuntimeState
frameGenerationStateAfterValidInputTagging(
    FrameGenerationRuntimeState current) noexcept {
  return current == FrameGenerationRuntimeState::Active
             ? FrameGenerationRuntimeState::Active
             : FrameGenerationRuntimeState::ProxyArmed;
}

// A transient invalid preview frame may pause an armed/active proxy, but input
// cleanup is also used inside disable and shutdown transactions.  Those
// lifecycle states must never be overwritten by ProxyArmed.
[[nodiscard]] constexpr bool
frameGenerationInputClearMayArmProxy(
    FrameGenerationRuntimeState current) noexcept {
  return current == FrameGenerationRuntimeState::ProxyArmed ||
         current == FrameGenerationRuntimeState::Active;
}

} // namespace xpbd::gfx
