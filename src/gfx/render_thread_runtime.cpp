#include "xpbd/gfx/render_thread_runtime.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace xpbd::gfx {
namespace {

bool eventsCoalesce(const RenderEvent &left,
                    const RenderEvent &right) noexcept {
  if (std::holds_alternative<RenderStatsEvent>(left) &&
      std::holds_alternative<RenderStatsEvent>(right)) {
    return true;
  }
  const auto *left_progress =
      std::get_if<StillRenderProgressEvent>(&left);
  const auto *right_progress =
      std::get_if<StillRenderProgressEvent>(&right);
  return left_progress != nullptr && right_progress != nullptr &&
         left_progress->job_id == right_progress->job_id;
}

} // namespace

MailboxPublishOutcome LatestFrameMailbox::publish(
    std::shared_ptr<const RenderFramePacket> packet) {
  MailboxPublishOutcome outcome;
  if (!packet || packet->packet_serial == 0u) {
    return outcome;
  }
  outcome.accepted_packet_serial = packet->packet_serial;
  std::shared_ptr<const RenderFramePacket> displaced;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      outcome.result = MailboxPublishResult::Closed;
      outcome.accepted_packet_serial = 0u;
      return outcome;
    }
    displaced = std::move(pending_);
    pending_ = std::move(packet);
    if (displaced) {
      outcome.result = MailboxPublishResult::Replaced;
      outcome.displaced_packet_serial = displaced->packet_serial;
    } else {
      outcome.result = MailboxPublishResult::Published;
    }
  }
  ready_.notify_one();
  return outcome;
}

bool LatestFrameMailbox::tryTake(
    std::shared_ptr<const RenderFramePacket> &packet) {
  std::lock_guard lock(mutex_);
  if (closed_ || !pending_) {
    return false;
  }
  packet = std::move(pending_);
  return true;
}

QueueWaitResult LatestFrameMailbox::waitTake(
    std::shared_ptr<const RenderFramePacket> &packet,
    std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!ready_.wait_for(lock, timeout,
                       [&]() { return closed_ || pending_ != nullptr; })) {
    return QueueWaitResult::Timeout;
  }
  if (closed_) {
    return QueueWaitResult::Closed;
  }
  packet = std::move(pending_);
  return QueueWaitResult::Item;
}

void LatestFrameMailbox::close() noexcept {
  std::shared_ptr<const RenderFramePacket> discarded;
  {
    std::lock_guard lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    discarded = std::move(pending_);
  }
  ready_.notify_all();
}

bool LatestFrameMailbox::closed() const noexcept {
  std::lock_guard lock(mutex_);
  return closed_;
}

bool LatestFrameMailbox::hasPendingFrame() const noexcept {
  std::lock_guard lock(mutex_);
  return pending_ != nullptr;
}

bool UploadFontAtlasRenderCommand::valid() const noexcept {
  if (width == 0u || height == 0u) {
    return false;
  }
  const std::size_t channels = rgba ? 4u : 1u;
  constexpr std::size_t kMaximum =
      (std::numeric_limits<std::size_t>::max)();
  if (width > kMaximum / height ||
      static_cast<std::size_t>(width) * height > kMaximum / channels) {
    return false;
  }
  return pixels.size() ==
         static_cast<std::size_t>(width) * height * channels;
}

RenderCommandKind renderCommandKind(const RenderCommand &command) noexcept {
  return std::visit(
      [](const auto &value) -> RenderCommandKind {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ResizeRenderCommand>) {
          return RenderCommandKind::Resize;
        } else if constexpr (std::is_same_v<T, SetVSyncRenderCommand>) {
          return RenderCommandKind::SetVSync;
        } else if constexpr (
            std::is_same_v<T, SuspendPresentationRenderCommand>) {
          return RenderCommandKind::SuspendPresentation;
        } else if constexpr (
            std::is_same_v<T, ResumePresentationRenderCommand>) {
          return RenderCommandKind::ResumePresentation;
        } else if constexpr (
            std::is_same_v<T, MarkLatencyPingRenderCommand>) {
          return RenderCommandKind::MarkLatencyPing;
        } else if constexpr (
            std::is_same_v<T, UploadFontAtlasRenderCommand>) {
          return RenderCommandKind::UploadFontAtlas;
        } else if constexpr (
            std::is_same_v<T, UploadStaticAssetRenderCommand>) {
          return RenderCommandKind::UploadStaticAsset;
        } else if constexpr (
            std::is_same_v<T, UploadEnvironmentRenderCommand>) {
          return RenderCommandKind::UploadEnvironment;
        } else if constexpr (std::is_same_v<T, StartStillRenderCommand>) {
          return RenderCommandKind::StartStillRender;
        } else if constexpr (std::is_same_v<T, CancelStillRenderCommand>) {
          return RenderCommandKind::CancelStillRender;
        } else {
          static_assert(std::is_same_v<T, ShutdownRenderCommand>);
          return RenderCommandKind::Shutdown;
        }
      },
      command);
}

bool renderCommandValid(const RenderCommand &command) noexcept {
  return std::visit(
      [](const auto &value) -> bool {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ResizeRenderCommand>) {
          return value.pixel_width >= 0 && value.pixel_height >= 0 &&
                 ((value.pixel_width == 0) == (value.pixel_height == 0));
        } else if constexpr (
            std::is_same_v<T, UploadFontAtlasRenderCommand> ||
            std::is_same_v<T, UploadStaticAssetRenderCommand> ||
            std::is_same_v<T, UploadEnvironmentRenderCommand> ||
            std::is_same_v<T, StartStillRenderCommand>) {
          return value.valid();
        } else if constexpr (std::is_same_v<T, CancelStillRenderCommand>) {
          return value.job_id != 0u;
        } else {
          return true;
        }
      },
      command);
}

std::optional<std::uint64_t>
ReliableRenderCommandQueue::push(RenderCommand command) {
  if (!renderCommandValid(command)) {
    return std::nullopt;
  }
  std::uint64_t command_serial = 0u;
  {
    std::lock_guard lock(mutex_);
    if (closed_ || next_command_serial_ == 0u ||
        next_command_serial_ ==
            (std::numeric_limits<std::uint64_t>::max)()) {
      return std::nullopt;
    }
    command_serial = next_command_serial_++;
    commands_.push_back(
        ReliableRenderCommand{command_serial, std::move(command)});
  }
  ready_.notify_one();
  return command_serial;
}

std::optional<RenderCommandKind>
ReliableRenderCommandQueue::frontKind() const noexcept {
  std::lock_guard lock(mutex_);
  if (commands_.empty()) {
    return std::nullopt;
  }
  return renderCommandKind(commands_.front().payload);
}

bool ReliableRenderCommandQueue::tryPop(ReliableRenderCommand &command) {
  std::lock_guard lock(mutex_);
  if (commands_.empty()) {
    return false;
  }
  command = std::move(commands_.front());
  commands_.pop_front();
  return true;
}

QueueWaitResult ReliableRenderCommandQueue::waitPop(
    ReliableRenderCommand &command, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!ready_.wait_for(lock, timeout,
                       [&]() { return closed_ || !commands_.empty(); })) {
    return QueueWaitResult::Timeout;
  }
  if (commands_.empty()) {
    return QueueWaitResult::Closed;
  }
  command = std::move(commands_.front());
  commands_.pop_front();
  return QueueWaitResult::Item;
}

void ReliableRenderCommandQueue::close() noexcept {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  ready_.notify_all();
}

bool ReliableRenderCommandQueue::closed() const noexcept {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t ReliableRenderCommandQueue::size() const noexcept {
  std::lock_guard lock(mutex_);
  return commands_.size();
}

RenderEventDelivery renderEventDelivery(const RenderEvent &event) noexcept {
  return std::holds_alternative<RenderStatsEvent>(event) ||
                 std::holds_alternative<StillRenderProgressEvent>(event)
             ? RenderEventDelivery::Coalescible
             : RenderEventDelivery::Reliable;
}

BoundedRenderEventQueue::BoundedRenderEventQueue(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 1u)) {}

EventPublishResult
BoundedRenderEventQueue::publishLocked(const RenderEvent &event) {
  if (closed_) {
    return EventPublishResult::Closed;
  }
  const RenderEventDelivery delivery = renderEventDelivery(event);
  if (delivery == RenderEventDelivery::Coalescible) {
    const auto existing =
        std::find_if(events_.begin(), events_.end(), [&](const auto &queued) {
          return eventsCoalesce(queued, event);
        });
    if (existing != events_.end()) {
      *existing = event;
      return EventPublishResult::Coalesced;
    }
    if (events_.size() >= capacity_) {
      return EventPublishResult::DroppedCoalescible;
    }
    events_.push_back(event);
    return EventPublishResult::Enqueued;
  }

  if (events_.size() >= capacity_) {
    const auto coalescible =
        std::find_if(events_.begin(), events_.end(), [](const auto &queued) {
          return renderEventDelivery(queued) ==
                 RenderEventDelivery::Coalescible;
        });
    if (coalescible == events_.end()) {
      return EventPublishResult::RetryRequired;
    }
    events_.erase(coalescible);
    events_.push_back(event);
    return EventPublishResult::EnqueuedAfterCoalescibleEviction;
  }
  events_.push_back(event);
  return EventPublishResult::Enqueued;
}

EventPublishResult BoundedRenderEventQueue::publish(const RenderEvent &event) {
  EventPublishResult result;
  {
    std::lock_guard lock(mutex_);
    result = publishLocked(event);
  }
  if (result == EventPublishResult::Enqueued ||
      result == EventPublishResult::Coalesced ||
      result == EventPublishResult::EnqueuedAfterCoalescibleEviction) {
    changed_.notify_one();
  }
  return result;
}

EventPublishResult BoundedRenderEventQueue::waitPublishReliable(
    const RenderEvent &event, std::chrono::milliseconds timeout) {
  if (renderEventDelivery(event) != RenderEventDelivery::Reliable) {
    return publish(event);
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lock(mutex_);
  for (;;) {
    const EventPublishResult result = publishLocked(event);
    if (result != EventPublishResult::RetryRequired) {
      lock.unlock();
      if (result == EventPublishResult::Enqueued ||
          result == EventPublishResult::EnqueuedAfterCoalescibleEviction) {
        changed_.notify_one();
      }
      return result;
    }
    if (timeout <= std::chrono::milliseconds::zero() ||
        changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
      return EventPublishResult::RetryRequired;
    }
  }
}

bool BoundedRenderEventQueue::tryPop(RenderEvent &event) {
  {
    std::lock_guard lock(mutex_);
    if (events_.empty()) {
      return false;
    }
    event = std::move(events_.front());
    events_.pop_front();
  }
  changed_.notify_all();
  return true;
}

QueueWaitResult BoundedRenderEventQueue::waitPop(
    RenderEvent &event, std::chrono::milliseconds timeout) {
  std::unique_lock lock(mutex_);
  if (!changed_.wait_for(lock, timeout,
                         [&]() { return closed_ || !events_.empty(); })) {
    return QueueWaitResult::Timeout;
  }
  if (events_.empty()) {
    return QueueWaitResult::Closed;
  }
  event = std::move(events_.front());
  events_.pop_front();
  lock.unlock();
  changed_.notify_all();
  return QueueWaitResult::Item;
}

void BoundedRenderEventQueue::close() noexcept {
  {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }
  changed_.notify_all();
}

bool BoundedRenderEventQueue::closed() const noexcept {
  std::lock_guard lock(mutex_);
  return closed_;
}

std::size_t BoundedRenderEventQueue::size() const noexcept {
  std::lock_guard lock(mutex_);
  return events_.size();
}

const char *renderThreadStateName(RenderThreadState state) noexcept {
  switch (state) {
  case RenderThreadState::Disabled:
    return "Disabled";
  case RenderThreadState::Starting:
    return "Starting";
  case RenderThreadState::Running:
    return "Running";
  case RenderThreadState::SuspendRequested:
    return "SuspendRequested";
  case RenderThreadState::Suspended:
    return "Suspended";
  case RenderThreadState::FatalQuarantined:
    return "FatalQuarantined";
  case RenderThreadState::Stopping:
    return "Stopping";
  case RenderThreadState::Stopped:
    return "Stopped";
  }
  return "Unknown";
}

bool RenderThreadLifecycle::transition(RenderThreadState expected,
                                       RenderThreadState desired) noexcept {
  if (!legalRenderThreadTransition(expected, desired)) {
    return false;
  }
  return state_.compare_exchange_strong(expected, desired,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
}

bool RenderThreadLifecycle::transitionTo(RenderThreadState desired) noexcept {
  RenderThreadState current = state();
  for (;;) {
    if (!legalRenderThreadTransition(current, desired)) {
      return false;
    }
    if (state_.compare_exchange_weak(current, desired,
                                     std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      return true;
    }
  }
}

bool RenderThreadControl::requestShutdown() noexcept {
  RenderThreadStopReason expected = RenderThreadStopReason::None;
  return reason_.compare_exchange_strong(
      expected, RenderThreadStopReason::ShutdownRequested,
      std::memory_order_acq_rel, std::memory_order_acquire);
}

void RenderThreadControl::requestFatalQuarantine() noexcept {
  reason_.store(RenderThreadStopReason::FatalQuarantine,
                std::memory_order_release);
}

} // namespace xpbd::gfx
