#pragma once

#include "xpbd/gfx/render_thread_contract.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace xpbd::gfx {

enum class QueueWaitResult : std::uint8_t {
  Item,
  Timeout,
  Closed,
};

enum class MailboxPublishResult : std::uint8_t {
  Published,
  Replaced,
  RejectedInvalid,
  Closed,
};

struct MailboxPublishOutcome {
  MailboxPublishResult result = MailboxPublishResult::RejectedInvalid;
  std::uint64_t accepted_packet_serial = 0;
  std::uint64_t displaced_packet_serial = 0;
};

// Capacity-one mailbox for disposable preview work. Publication never assigns
// a render/history/Streamline serial; only the consumer may do that after take.
class LatestFrameMailbox final {
public:
  [[nodiscard]] MailboxPublishOutcome
  publish(std::shared_ptr<const RenderFramePacket> packet);
  [[nodiscard]] bool
  tryTake(std::shared_ptr<const RenderFramePacket> &packet);
  [[nodiscard]] QueueWaitResult
  waitTake(std::shared_ptr<const RenderFramePacket> &packet,
           std::chrono::milliseconds timeout);
  void close() noexcept;

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] bool hasPendingFrame() const noexcept;

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::shared_ptr<const RenderFramePacket> pending_;
  bool closed_ = false;
};

struct ResizeRenderCommand {
  int pixel_width = 0;
  int pixel_height = 0;
};

struct SetVSyncRenderCommand {
  bool enabled = true;
};

struct SuspendPresentationRenderCommand {};
struct ResumePresentationRenderCommand {};
struct MarkLatencyPingRenderCommand {};

struct UploadFontAtlasRenderCommand {
  std::vector<std::byte> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  bool rgba = false;

  [[nodiscard]] bool valid() const noexcept;
};

struct UploadStaticAssetRenderCommand {
  std::shared_ptr<const RenderFramePacket> source_packet;

  [[nodiscard]] bool valid() const noexcept {
    return source_packet != nullptr &&
           source_packet->scene.static_model != nullptr &&
           source_packet->scene.static_model_frame != nullptr;
  }
};

struct UploadEnvironmentRenderCommand {
  std::shared_ptr<const RenderFramePacket> source_packet;

  [[nodiscard]] bool valid() const noexcept {
    return source_packet != nullptr &&
           (source_packet->scene.world_environment != nullptr ||
            source_packet->scene.preview_skybox != nullptr);
  }
};

struct CancelStillRenderCommand {
  std::uint64_t job_id = 0;
};

struct ShutdownRenderCommand {};

using RenderCommand =
    std::variant<ResizeRenderCommand, SetVSyncRenderCommand,
                 SuspendPresentationRenderCommand,
                 ResumePresentationRenderCommand,
                 MarkLatencyPingRenderCommand,
                 UploadFontAtlasRenderCommand,
                 UploadStaticAssetRenderCommand,
                 UploadEnvironmentRenderCommand, StartStillRenderCommand,
                 CancelStillRenderCommand, ShutdownRenderCommand>;

enum class RenderCommandKind : std::uint8_t {
  Resize,
  SetVSync,
  SuspendPresentation,
  ResumePresentation,
  MarkLatencyPing,
  UploadFontAtlas,
  UploadStaticAsset,
  UploadEnvironment,
  StartStillRender,
  CancelStillRender,
  Shutdown,
};

[[nodiscard]] RenderCommandKind
renderCommandKind(const RenderCommand &command) noexcept;
[[nodiscard]] bool renderCommandValid(const RenderCommand &command) noexcept;

struct ReliableRenderCommand {
  std::uint64_t command_serial = 0;
  RenderCommand payload;
};

// Reliable FIFO: no command is overwritten or coalesced. close() rejects new
// work but lets the consumer drain commands already accepted by the queue.
class ReliableRenderCommandQueue final {
public:
  [[nodiscard]] std::optional<std::uint64_t> push(RenderCommand command);
  [[nodiscard]] std::optional<RenderCommandKind> frontKind() const noexcept;
  [[nodiscard]] bool tryPop(ReliableRenderCommand &command);
  [[nodiscard]] QueueWaitResult
  waitPop(ReliableRenderCommand &command,
          std::chrono::milliseconds timeout);
  void close() noexcept;

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;

private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<ReliableRenderCommand> commands_;
  std::uint64_t next_command_serial_ = 1;
  bool closed_ = false;
};

struct RendererInitializedEvent {
  std::string device_name;
  bool ray_tracing_ready = false;
  bool streamline_ready = false;
  RayTracingCapability ray_tracing_capability{};
  PathTracePostProcessCapabilities post_process_capabilities{};
  std::string post_process_status;
  std::uint32_t latency_ping_message = 0u;
};

struct RenderStatsEvent {
  FrameStats stats{};
  std::uint64_t render_serial = 0;
  std::uint64_t previous_history_render_serial = 0;
  std::uint64_t history_serial = 0;
  std::uint64_t present_serial = 0;
  std::shared_ptr<const LastPresentedSnapshot> last_presented;
  RayTracingCapability ray_tracing_capability{};
  PathTracePostProcessCapabilities post_process_capabilities{};
  std::string post_process_status;
  std::uint32_t latency_ping_message = 0u;
};

struct RenderCommandAckEvent {
  std::uint64_t command_serial = 0;
  RenderCommandKind command_kind = RenderCommandKind::Resize;
};

struct RenderCommandFailedEvent {
  std::uint64_t command_serial = 0;
  RenderCommandKind command_kind = RenderCommandKind::Resize;
  std::string message;
};

struct UploadCompletedEvent {
  std::uint64_t command_serial = 0;
  RenderCommandKind command_kind = RenderCommandKind::UploadStaticAsset;
  std::uint64_t uploaded_bytes = 0;
};

struct UploadFailedEvent {
  std::uint64_t command_serial = 0;
  RenderCommandKind command_kind = RenderCommandKind::UploadStaticAsset;
  std::string message;
};

struct UploadSupersededEvent {
  std::uint64_t command_serial = 0;
  std::uint64_t replacement_command_serial = 0;
  RenderCommandKind command_kind = RenderCommandKind::UploadStaticAsset;
};

struct StillRenderProgressEvent {
  std::uint64_t job_id = 0;
  StillRenderJobState state = StillRenderJobState::Rendering;
  std::uint32_t completed_samples = 0;
  std::uint32_t target_samples = 0;
};

struct StillRenderCompletedEvent {
  std::uint64_t job_id = 0;
  std::uint32_t completed_samples = 0;
  std::uint32_t target_samples = 0;
  std::string output_path;
};

struct StillRenderFailedEvent {
  std::uint64_t job_id = 0;
  std::uint32_t completed_samples = 0;
  std::uint32_t target_samples = 0;
  std::string message;
};

struct StillRenderCancelledEvent {
  std::uint64_t job_id = 0;
  std::uint32_t completed_samples = 0;
  std::uint32_t target_samples = 0;
};

struct RendererFatalErrorEvent {
  std::int64_t result_code = 0;
  std::string api;
  std::string message;
};

struct RendererStoppedEvent {
  bool fatal_quarantine = false;
};

using RenderEvent =
    std::variant<RendererInitializedEvent, RenderStatsEvent,
                 RenderCommandAckEvent, RenderCommandFailedEvent,
                 UploadCompletedEvent, UploadFailedEvent,
                 UploadSupersededEvent,
                 StillRenderProgressEvent, StillRenderCompletedEvent,
                 StillRenderFailedEvent, StillRenderCancelledEvent,
                 RendererFatalErrorEvent, RendererStoppedEvent>;

enum class RenderEventDelivery : std::uint8_t {
  Coalescible,
  Reliable,
};

[[nodiscard]] RenderEventDelivery
renderEventDelivery(const RenderEvent &event) noexcept;

enum class EventPublishResult : std::uint8_t {
  Enqueued,
  Coalesced,
  EnqueuedAfterCoalescibleEviction,
  DroppedCoalescible,
  RetryRequired,
  Closed,
};

// A bounded worker-to-Main queue. Stats and per-job progress may coalesce.
// Reliable events never disappear silently: full reliable-only capacity
// returns RetryRequired, leaving the caller responsible for retaining/retry.
class BoundedRenderEventQueue final {
public:
  explicit BoundedRenderEventQueue(std::size_t capacity);

  [[nodiscard]] EventPublishResult publish(const RenderEvent &event);
  [[nodiscard]] EventPublishResult
  waitPublishReliable(const RenderEvent &event,
                      std::chrono::milliseconds timeout);
  [[nodiscard]] bool tryPop(RenderEvent &event);
  [[nodiscard]] QueueWaitResult
  waitPop(RenderEvent &event, std::chrono::milliseconds timeout);
  void close() noexcept;

  [[nodiscard]] bool closed() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  [[nodiscard]] EventPublishResult
  publishLocked(const RenderEvent &event);

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<RenderEvent> events_;
  std::size_t capacity_ = 1;
  bool closed_ = false;
};

enum class RenderThreadState : std::uint8_t {
  Disabled,
  Starting,
  Running,
  SuspendRequested,
  Suspended,
  FatalQuarantined,
  Stopping,
  Stopped,
};

[[nodiscard]] constexpr bool legalRenderThreadTransition(
    RenderThreadState from, RenderThreadState to) noexcept {
  switch (from) {
  case RenderThreadState::Disabled:
    return to == RenderThreadState::Starting;
  case RenderThreadState::Starting:
    return to == RenderThreadState::Running ||
           to == RenderThreadState::FatalQuarantined ||
           to == RenderThreadState::Stopping;
  case RenderThreadState::Running:
    return to == RenderThreadState::SuspendRequested ||
           to == RenderThreadState::FatalQuarantined ||
           to == RenderThreadState::Stopping;
  case RenderThreadState::SuspendRequested:
    return to == RenderThreadState::Suspended ||
           to == RenderThreadState::FatalQuarantined ||
           to == RenderThreadState::Stopping;
  case RenderThreadState::Suspended:
    return to == RenderThreadState::Running ||
           to == RenderThreadState::FatalQuarantined ||
           to == RenderThreadState::Stopping;
  case RenderThreadState::FatalQuarantined:
    return to == RenderThreadState::Stopping;
  case RenderThreadState::Stopping:
    return to == RenderThreadState::Stopped;
  case RenderThreadState::Stopped:
    return false;
  }
  return false;
}

[[nodiscard]] const char *renderThreadStateName(RenderThreadState state) noexcept;

class RenderThreadLifecycle final {
public:
  [[nodiscard]] RenderThreadState state() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool transition(RenderThreadState expected,
                                RenderThreadState desired) noexcept;
  [[nodiscard]] bool transitionTo(RenderThreadState desired) noexcept;

private:
  std::atomic<RenderThreadState> state_{RenderThreadState::Disabled};
};

enum class RenderThreadStopReason : std::uint8_t {
  None,
  ShutdownRequested,
  FatalQuarantine,
};

// Shared by the worker and every potentially blocking backend wait. Fatal
// quarantine dominates a normal shutdown request and is monotonic.
class RenderThreadControl final {
public:
  [[nodiscard]] RenderThreadStopReason reason() const noexcept {
    return reason_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool stopRequested() const noexcept {
    return reason() != RenderThreadStopReason::None;
  }
  [[nodiscard]] bool fatalQuarantineRequested() const noexcept {
    return reason() == RenderThreadStopReason::FatalQuarantine;
  }

  [[nodiscard]] bool requestShutdown() noexcept;
  void requestFatalQuarantine() noexcept;

private:
  std::atomic<RenderThreadStopReason> reason_{RenderThreadStopReason::None};
};

} // namespace xpbd::gfx
