#pragma once

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

} // namespace xpbd::gfx
