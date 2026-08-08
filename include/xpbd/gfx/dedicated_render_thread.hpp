#pragma once

#include "xpbd/gfx/render_thread_runtime.hpp"
#include "xpbd/gfx/vulkan_window_bootstrap.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace xpbd::gfx {

enum class RenderThreadBackendStatus : std::uint8_t {
  Success,
  Pending,
  Superseded,
  Failed,
  FatalQuarantine,
};

struct RenderThreadBackendOutcome {
  RenderThreadBackendStatus status = RenderThreadBackendStatus::Success;
  std::string message;
  std::string api;
  std::string device_name;
  std::int64_t result_code = 0;
  std::uint64_t uploaded_bytes = 0;
  FrameStats stats{};
  bool ray_tracing_ready = false;
  bool streamline_ready = false;
  RayTracingCapability ray_tracing_capability{};
  PathTracePostProcessCapabilities post_process_capabilities{};
  std::string post_process_status;
  std::uint32_t latency_ping_message = 0u;
  std::uint64_t previous_history_render_serial = 0u;
  bool temporal_history_accepted = false;
  bool presented = false;
  std::optional<StillRenderStatus> still_render_status;

  [[nodiscard]] bool success() const noexcept {
    return status == RenderThreadBackendStatus::Success;
  }
  [[nodiscard]] bool pending() const noexcept {
    return status == RenderThreadBackendStatus::Pending;
  }
  [[nodiscard]] bool superseded() const noexcept {
    return status == RenderThreadBackendStatus::Superseded;
  }
};

enum class RenderThreadBackendShutdown : std::uint8_t {
  Clean,
  Quarantined,
};

// Worker-side Vulkan contract. Implementations are constructed, initialized,
// called, and normally destroyed only by the RenderThread.
class IRenderThreadBackend {
public:
  virtual ~IRenderThreadBackend() = default;

  [[nodiscard]] virtual RenderThreadBackendOutcome initialize(
      const VulkanWindowBootstrap &bootstrap,
      std::shared_ptr<RenderThreadControl> control) = 0;
  [[nodiscard]] virtual RenderThreadBackendOutcome executeCommand(
      const ReliableRenderCommand &command) = 0;
  // Poll the single command previously returned as Pending. Implementations
  // must not publish candidate resources before returning Success.
  [[nodiscard]] virtual RenderThreadBackendOutcome pollCommand(
      const ReliableRenderCommand &command) = 0;
  // Mark the current Pending command non-publishable because a later command
  // of the same supersedable kind is now at the reliable FIFO front.
  [[nodiscard]] virtual RenderThreadBackendOutcome supersedeCommand(
      const ReliableRenderCommand &current,
      const ReliableRenderCommand &replacement) = 0;
  [[nodiscard]] virtual RenderThreadBackendOutcome renderFrame(
      const RenderFramePacket &packet, std::uint64_t render_serial,
      const HistoryCommitSnapshot *previous_history) = 0;
  [[nodiscard]] virtual RenderThreadBackendShutdown shutdown() noexcept = 0;
};

using RenderThreadBackendFactory =
    std::function<std::unique_ptr<IRenderThreadBackend>()>;

// Worker-owned Vulkan adapter factory. The public R6 client constructs this
// only inside DedicatedRenderThread; Main never receives the concrete backend.
[[nodiscard]] std::unique_ptr<IRenderThreadBackend>
createVulkanRenderThreadBackend();

enum class DedicatedRenderThreadShutdown : std::uint8_t {
  NotStarted,
  Clean,
  BackendQuarantined,
  JoinTimedOutQuarantined,
};

// RenderThread safety owner used by the active R6 Vulkan client.
class DedicatedRenderThread final {
public:
  DedicatedRenderThread(VulkanWindowBootstrap bootstrap,
                        RenderThreadBackendFactory backend_factory,
                        std::size_t event_capacity = 256u);
  ~DedicatedRenderThread();

  DedicatedRenderThread(const DedicatedRenderThread &) = delete;
  DedicatedRenderThread &operator=(const DedicatedRenderThread &) = delete;
  DedicatedRenderThread(DedicatedRenderThread &&) = delete;
  DedicatedRenderThread &operator=(DedicatedRenderThread &&) = delete;

  [[nodiscard]] bool start();
  [[nodiscard]] bool
  waitUntilRunning(std::chrono::milliseconds timeout) const;
  [[nodiscard]] bool waitForState(RenderThreadState desired,
                                  std::chrono::milliseconds timeout) const;

  [[nodiscard]] MailboxPublishOutcome
  publishFrame(std::shared_ptr<const RenderFramePacket> packet);
  [[nodiscard]] std::optional<std::uint64_t>
  enqueue(RenderCommand command);

  [[nodiscard]] bool tryPopEvent(RenderEvent &event);
  [[nodiscard]] QueueWaitResult waitEvent(
      RenderEvent &event, std::chrono::milliseconds timeout);

  [[nodiscard]] DedicatedRenderThreadShutdown
  shutdown(std::chrono::milliseconds timeout);

  [[nodiscard]] RenderThreadState state() const noexcept;
  [[nodiscard]] std::shared_ptr<RenderThreadControl> control() const noexcept;

private:
  struct SharedState;
  static void run(const std::shared_ptr<SharedState> &state) noexcept;

  std::shared_ptr<SharedState> state_;
  std::thread thread_;
  bool start_called_ = false;
  bool detached_ = false;
};

} // namespace xpbd::gfx
