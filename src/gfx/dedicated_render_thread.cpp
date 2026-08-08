#include "xpbd/gfx/dedicated_render_thread.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace xpbd::gfx {
namespace {

constexpr auto kWorkerWakePoll = std::chrono::milliseconds(25);
constexpr auto kReliableEventSlice = std::chrono::milliseconds(50);
constexpr auto kDestructorShutdownBudget = std::chrono::seconds(2);

void quarantineBackend(std::unique_ptr<IRenderThreadBackend> backend) {
  if (!backend) {
    return;
  }
  struct Registry {
    std::mutex mutex;
    std::vector<IRenderThreadBackend *> backends;
  };
  // Deliberately process-lifetime storage: quarantined Vulkan ownership must
  // not be destructed merely because completion could not be proven.
  static Registry *registry = new Registry();
  std::lock_guard lock(registry->mutex);
  registry->backends.push_back(backend.release());
}

bool isUploadCommand(RenderCommandKind kind) noexcept {
  return kind == RenderCommandKind::UploadFontAtlas ||
         kind == RenderCommandKind::UploadStaticAsset ||
         kind == RenderCommandKind::UploadEnvironment;
}

bool isSupersedableAssetUploadCommand(
    RenderCommandKind kind) noexcept {
  return kind == RenderCommandKind::UploadStaticAsset ||
         kind == RenderCommandKind::UploadEnvironment;
}

} // namespace

struct DedicatedRenderThread::SharedState {
  SharedState(VulkanWindowBootstrap captured_bootstrap,
              RenderThreadBackendFactory factory,
              std::size_t event_capacity)
      : bootstrap(std::move(captured_bootstrap)),
        backend_factory(std::move(factory)), events(event_capacity),
        control(std::make_shared<RenderThreadControl>()) {}

  VulkanWindowBootstrap bootstrap;
  RenderThreadBackendFactory backend_factory;
  std::unique_ptr<IRenderThreadBackend> backend;
  LatestFrameMailbox mailbox;
  ReliableRenderCommandQueue commands;
  BoundedRenderEventQueue events;
  RenderThreadLifecycle lifecycle;
  std::shared_ptr<RenderThreadControl> control;
  TemporalHistoryLedger history;
  std::shared_ptr<const LastPresentedSnapshot> last_presented;
  std::uint64_t next_render_serial = 1u;
  std::uint64_t next_present_serial = 1u;

  mutable std::mutex wake_mutex;
  std::condition_variable wake;
  std::uint64_t wake_epoch = 0u;

  mutable std::mutex completion_mutex;
  std::condition_variable completion;
  bool thread_finished = false;
  std::atomic<bool> backend_quarantined{false};
  std::atomic<bool> fatal_event_published{false};

  void signalWork() {
    {
      std::lock_guard lock(wake_mutex);
      ++wake_epoch;
    }
    wake.notify_all();
  }

  void signalLifecycle() {
    completion.notify_all();
  }

  bool publishReliable(const RenderEvent &event) {
    for (;;) {
      const EventPublishResult result =
          events.waitPublishReliable(event, kReliableEventSlice);
      if (result == EventPublishResult::Enqueued ||
          result == EventPublishResult::EnqueuedAfterCoalescibleEviction ||
          result == EventPublishResult::Coalesced) {
        return true;
      }
      if (result == EventPublishResult::Closed) {
        return false;
      }
      // RetryRequired is explicit backpressure. The event remains owned by
      // this stack frame until Main drains capacity or closes the channel.
    }
  }

  void publishFatalOnce(const RenderThreadBackendOutcome &outcome) {
    bool expected = false;
    if (!fatal_event_published.compare_exchange_strong(expected, true)) {
      return;
    }
    (void)publishReliable(RenderEvent{RendererFatalErrorEvent{
        outcome.result_code, outcome.api, outcome.message}});
  }
};

DedicatedRenderThread::DedicatedRenderThread(
    VulkanWindowBootstrap bootstrap,
    RenderThreadBackendFactory backend_factory, std::size_t event_capacity)
    : state_(std::make_shared<SharedState>(
          std::move(bootstrap), std::move(backend_factory), event_capacity)) {}

DedicatedRenderThread::~DedicatedRenderThread() {
  if (thread_.joinable() && !detached_) {
    (void)shutdown(kDestructorShutdownBudget);
  }
}

bool DedicatedRenderThread::start() {
  if (start_called_ || !state_ || !state_->backend_factory ||
      !state_->bootstrap.renderThreadCompatible()) {
    return false;
  }
  if (!state_->lifecycle.transitionTo(RenderThreadState::Starting)) {
    return false;
  }
  start_called_ = true;
  try {
    const std::shared_ptr<SharedState> state = state_;
    thread_ = std::thread([state]() { run(state); });
  } catch (...) {
    state_->control->requestFatalQuarantine();
    (void)state_->lifecycle.transitionTo(
        RenderThreadState::FatalQuarantined);
    (void)state_->lifecycle.transitionTo(RenderThreadState::Stopping);
    (void)state_->lifecycle.transitionTo(RenderThreadState::Stopped);
    {
      std::lock_guard lock(state_->completion_mutex);
      state_->thread_finished = true;
      state_->backend_quarantined.store(true, std::memory_order_release);
    }
    state_->signalLifecycle();
    return false;
  }
  return true;
}

bool DedicatedRenderThread::waitUntilRunning(
    std::chrono::milliseconds timeout) const {
  return waitForState(RenderThreadState::Running, timeout);
}

bool DedicatedRenderThread::waitForState(
    RenderThreadState desired, std::chrono::milliseconds timeout) const {
  if (!state_) {
    return false;
  }
  std::unique_lock lock(state_->completion_mutex);
  const bool reached = state_->completion.wait_for(lock, timeout, [&]() {
    const RenderThreadState current = state_->lifecycle.state();
    if (current == desired) {
      return true;
    }
    return desired != RenderThreadState::Stopped &&
           (current == RenderThreadState::Stopped ||
            current == RenderThreadState::FatalQuarantined);
  });
  return reached && state_->lifecycle.state() == desired;
}

MailboxPublishOutcome DedicatedRenderThread::publishFrame(
    std::shared_ptr<const RenderFramePacket> packet) {
  if (!state_) {
    return {};
  }
  const RenderThreadState current = state_->lifecycle.state();
  if (current == RenderThreadState::Disabled ||
      current == RenderThreadState::Starting ||
      current == RenderThreadState::FatalQuarantined ||
      current == RenderThreadState::Stopping ||
      current == RenderThreadState::Stopped) {
    return {MailboxPublishResult::Closed, 0u, 0u};
  }
  MailboxPublishOutcome outcome = state_->mailbox.publish(std::move(packet));
  if (outcome.result == MailboxPublishResult::Published ||
      outcome.result == MailboxPublishResult::Replaced) {
    state_->signalWork();
  }
  return outcome;
}

std::optional<std::uint64_t>
DedicatedRenderThread::enqueue(RenderCommand command) {
  if (!state_) {
    return std::nullopt;
  }
  const RenderThreadState current = state_->lifecycle.state();
  if (current == RenderThreadState::Disabled ||
      current == RenderThreadState::FatalQuarantined ||
      current == RenderThreadState::Stopping ||
      current == RenderThreadState::Stopped) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> serial =
      state_->commands.push(std::move(command));
  if (serial) {
    state_->signalWork();
  }
  return serial;
}

bool DedicatedRenderThread::tryPopEvent(RenderEvent &event) {
  return state_ && state_->events.tryPop(event);
}

QueueWaitResult DedicatedRenderThread::waitEvent(
    RenderEvent &event, std::chrono::milliseconds timeout) {
  if (!state_) {
    return QueueWaitResult::Closed;
  }
  return state_->events.waitPop(event, timeout);
}

DedicatedRenderThreadShutdown DedicatedRenderThread::shutdown(
    std::chrono::milliseconds timeout) {
  if (!start_called_ || !state_) {
    return DedicatedRenderThreadShutdown::NotStarted;
  }
  if (thread_.joinable() && !detached_) {
    (void)state_->commands.push(RenderCommand{ShutdownRenderCommand{}});
    (void)state_->control->requestShutdown();
    state_->mailbox.close();
    state_->signalWork();

    std::unique_lock lock(state_->completion_mutex);
    const bool finished = state_->completion.wait_for(
        lock, timeout, [&]() { return state_->thread_finished; });
    lock.unlock();
    if (finished) {
      thread_.join();
      return state_->backend_quarantined.load(std::memory_order_acquire)
                 ? DedicatedRenderThreadShutdown::BackendQuarantined
                 : DedicatedRenderThreadShutdown::Clean;
    }

    state_->control->requestFatalQuarantine();
    (void)state_->lifecycle.transitionTo(
        RenderThreadState::FatalQuarantined);
    state_->backend_quarantined.store(true, std::memory_order_release);
    state_->signalWork();
    // A worker may be blocked retrying a reliable event while Main is no
    // longer draining the queue. Closing only after the outer deadline keeps
    // normal reliable delivery intact and gives fatal teardown a finite exit.
    state_->events.close();
    thread_.detach();
    detached_ = true;
    return DedicatedRenderThreadShutdown::JoinTimedOutQuarantined;
  }
  return state_->backend_quarantined.load(std::memory_order_acquire)
             ? DedicatedRenderThreadShutdown::BackendQuarantined
             : DedicatedRenderThreadShutdown::Clean;
}

RenderThreadState DedicatedRenderThread::state() const noexcept {
  return state_ ? state_->lifecycle.state() : RenderThreadState::Stopped;
}

std::shared_ptr<RenderThreadControl>
DedicatedRenderThread::control() const noexcept {
  return state_ ? state_->control : nullptr;
}

void DedicatedRenderThread::run(
    const std::shared_ptr<SharedState> &state) noexcept {
  const auto enter_fatal = [&](RenderThreadBackendOutcome outcome) {
    if (outcome.status != RenderThreadBackendStatus::FatalQuarantine) {
      outcome.status = RenderThreadBackendStatus::FatalQuarantine;
    }
    state->control->requestFatalQuarantine();
    (void)state->lifecycle.transitionTo(
        RenderThreadState::FatalQuarantined);
    state->signalLifecycle();
    state->publishFatalOnce(outcome);
    state->mailbox.close();
    state->commands.close();
  };
  const auto publish_still_observation =
      [&](const StillRenderStatus &status) {
        if (status.job_id == 0u ||
            status.state == StillRenderJobState::Idle) {
          return;
        }
        switch (status.state) {
        case StillRenderJobState::Completed:
          (void)state->publishReliable(
              RenderEvent{StillRenderCompletedEvent{
                  status.job_id, status.accumulated_samples,
                  status.target_samples, status.output_path}});
          break;
        case StillRenderJobState::Failed:
          (void)state->publishReliable(RenderEvent{StillRenderFailedEvent{
              status.job_id, status.accumulated_samples,
              status.target_samples, status.error}});
          break;
        case StillRenderJobState::Cancelled:
          (void)state->publishReliable(
              RenderEvent{StillRenderCancelledEvent{
                  status.job_id, status.accumulated_samples,
                  status.target_samples}});
          break;
        case StillRenderJobState::Queued:
        case StillRenderJobState::Rendering:
        case StillRenderJobState::Saving:
          (void)state->events.publish(RenderEvent{StillRenderProgressEvent{
              status.job_id, status.state, status.accumulated_samples,
              status.target_samples}});
          break;
        case StillRenderJobState::Idle:
          break;
        }
      };

  try {
    state->backend = state->backend_factory();
    if (!state->backend) {
      enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                   "RenderThread backend factory returned null",
                   "backend_factory"});
    } else {
      RenderThreadBackendOutcome initialized =
          state->backend->initialize(state->bootstrap, state->control);
      if (!initialized.success()) {
        enter_fatal(std::move(initialized));
      } else {
        RendererInitializedEvent initialized_event;
        initialized_event.device_name = initialized.device_name;
        initialized_event.ray_tracing_ready =
            initialized.ray_tracing_ready;
        initialized_event.streamline_ready = initialized.streamline_ready;
        initialized_event.ray_tracing_capability =
            initialized.ray_tracing_capability;
        initialized_event.post_process_capabilities =
            initialized.post_process_capabilities;
        initialized_event.post_process_status =
            initialized.post_process_status;
        initialized_event.latency_ping_message =
            initialized.latency_ping_message;
        (void)state->publishReliable(
            RenderEvent{std::move(initialized_event)});
        if (!state->lifecycle.transitionTo(RenderThreadState::Running)) {
          enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                       "RenderThread could not enter Running state",
                       "lifecycle"});
        } else {
          state->signalLifecycle();
        }
      }
    }

    std::deque<ReliableRenderCommand> deferred_while_suspended;
    std::optional<ReliableRenderCommand> pending_backend_command;
    std::optional<ReliableRenderCommand> superseding_backend_command;
    std::optional<ReliableRenderCommand> ready_backend_command;
    while (state->lifecycle.state() !=
               RenderThreadState::FatalQuarantined &&
           !state->control->stopRequested()) {
      std::uint64_t observed_epoch = 0u;
      {
        std::lock_guard lock(state->wake_mutex);
        observed_epoch = state->wake_epoch;
      }

      if (pending_backend_command) {
        const RenderCommandKind pending_kind =
            renderCommandKind(pending_backend_command->payload);
        if (isSupersedableAssetUploadCommand(pending_kind)) {
          for (;;) {
            const std::optional<RenderCommandKind> front_kind =
                state->commands.frontKind();
            if (!front_kind ||
                *front_kind != pending_kind) {
              break;
            }
            ReliableRenderCommand replacement;
            if (!state->commands.tryPop(replacement)) {
              break;
            }
            RenderThreadBackendOutcome supersede_outcome =
                state->backend->supersedeCommand(
                    *pending_backend_command, replacement);
            if (supersede_outcome.status ==
                RenderThreadBackendStatus::FatalQuarantine) {
              enter_fatal(std::move(supersede_outcome));
              break;
            }
            if (!supersede_outcome.success()) {
              (void)state->publishReliable(RenderEvent{UploadFailedEvent{
                  replacement.command_serial,
                  pending_kind,
                  supersede_outcome.message}});
              continue;
            }
            if (superseding_backend_command) {
              (void)state->publishReliable(
                  RenderEvent{UploadSupersededEvent{
                      superseding_backend_command->command_serial,
                      replacement.command_serial,
                      pending_kind}});
            }
            superseding_backend_command = std::move(replacement);
          }
          if (state->lifecycle.state() ==
              RenderThreadState::FatalQuarantined) {
            break;
          }
        }
        RenderThreadBackendOutcome pending_outcome =
            state->backend->pollCommand(*pending_backend_command);
        if (pending_outcome.status ==
            RenderThreadBackendStatus::FatalQuarantine) {
          enter_fatal(std::move(pending_outcome));
          break;
        }
        if (pending_outcome.superseded()) {
          if (!superseding_backend_command) {
            enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                         "Backend reported Superseded without a reliable replacement",
                         "IRenderThreadBackend::pollCommand"});
            break;
          }
          (void)state->publishReliable(
              RenderEvent{UploadSupersededEvent{
                  pending_backend_command->command_serial,
                  superseding_backend_command->command_serial,
                  pending_kind}});
          pending_backend_command.reset();
          ready_backend_command =
              std::move(superseding_backend_command);
          superseding_backend_command.reset();
          continue;
        }
        if (!pending_outcome.pending()) {
          if (pending_outcome.success() &&
              superseding_backend_command) {
            enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                         "Backend committed a command after accepting its Supersede",
                         "IRenderThreadBackend::pollCommand"});
            break;
          }
          if (pending_outcome.success()) {
            if (isSupersedableAssetUploadCommand(pending_kind)) {
              std::shared_ptr<const RenderFramePacket> stale_packet;
              (void)state->mailbox.tryTake(stale_packet);
            }
            (void)state->publishReliable(RenderEvent{UploadCompletedEvent{
                pending_backend_command->command_serial, pending_kind,
                pending_outcome.uploaded_bytes}});
          } else {
            (void)state->publishReliable(RenderEvent{UploadFailedEvent{
                pending_backend_command->command_serial, pending_kind,
                pending_outcome.message}});
          }
          pending_backend_command.reset();
          if (superseding_backend_command) {
            ready_backend_command =
                std::move(superseding_backend_command);
            superseding_backend_command.reset();
          }
          continue;
        }
      }

      ReliableRenderCommand command;
      bool have_command = false;
      if (!pending_backend_command) {
        if (ready_backend_command) {
          command = std::move(*ready_backend_command);
          ready_backend_command.reset();
          have_command = true;
        } else if (state->lifecycle.state() == RenderThreadState::Running &&
            !deferred_while_suspended.empty()) {
          command = std::move(deferred_while_suspended.front());
          deferred_while_suspended.pop_front();
          have_command = true;
        } else {
          have_command = state->commands.tryPop(command);
        }
      }
      if (have_command) {
        const RenderCommandKind kind = renderCommandKind(command.payload);
        if (kind == RenderCommandKind::Shutdown) {
          (void)state->control->requestShutdown();
          break;
        }

        const RenderThreadState command_state = state->lifecycle.state();
        if ((command_state == RenderThreadState::SuspendRequested ||
             command_state == RenderThreadState::Suspended) &&
            kind != RenderCommandKind::Resize &&
            kind != RenderCommandKind::ResumePresentation &&
            kind != RenderCommandKind::SuspendPresentation &&
            kind != RenderCommandKind::MarkLatencyPing &&
            kind != RenderCommandKind::CancelStillRender) {
          // Main may enqueue unrelated work while performing a native window
          // operation. Retain it reliably, but do not touch GPU ownership
          // until Resume has crossed the backend boundary and been ACKed.
          deferred_while_suspended.push_back(std::move(command));
          continue;
        }

        if (kind == RenderCommandKind::SuspendPresentation) {
          if (!state->lifecycle.transitionTo(
                  RenderThreadState::SuspendRequested)) {
            enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                         "Suspend command arrived in an illegal lifecycle state",
                         "SuspendPresentation"});
            break;
          }
          state->signalLifecycle();
        } else if (kind == RenderCommandKind::ResumePresentation &&
                   state->lifecycle.state() !=
                       RenderThreadState::Suspended) {
          enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                       "Resume command arrived before a Suspend ACK",
                       "ResumePresentation"});
          break;
        }

        RenderThreadBackendOutcome outcome =
            state->backend->executeCommand(command);
        if (outcome.status ==
            RenderThreadBackendStatus::FatalQuarantine) {
          enter_fatal(std::move(outcome));
          break;
        }
        if (outcome.pending()) {
          if (!isUploadCommand(kind)) {
            enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                         "Only reliable upload commands may remain pending",
                         "IRenderThreadBackend::executeCommand"});
            break;
          }
          pending_backend_command = std::move(command);
          continue;
        }
        if (!outcome.success()) {
          if (kind == RenderCommandKind::SuspendPresentation ||
              kind == RenderCommandKind::ResumePresentation) {
            outcome.status = RenderThreadBackendStatus::FatalQuarantine;
            enter_fatal(std::move(outcome));
            break;
          }
          if (isUploadCommand(kind)) {
            (void)state->publishReliable(RenderEvent{UploadFailedEvent{
                command.command_serial, kind, outcome.message}});
          } else if (kind == RenderCommandKind::StartStillRender) {
            const auto &start =
                std::get<StartStillRenderCommand>(command.payload);
            (void)state->publishReliable(RenderEvent{StillRenderFailedEvent{
                start.job_id, 0u, start.target_samples, outcome.message}});
          } else {
            (void)state->publishReliable(RenderEvent{
                RenderCommandFailedEvent{command.command_serial, kind,
                                         outcome.message}});
          }
          continue;
        }

        if (kind == RenderCommandKind::SuspendPresentation) {
          if (!state->lifecycle.transitionTo(RenderThreadState::Suspended)) {
            enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                         "Backend suspended but lifecycle ACK transition failed",
                         "SuspendPresentation"});
            break;
          }
          state->history.invalidate();
          state->signalLifecycle();
        } else if (kind == RenderCommandKind::ResumePresentation) {
          if (!state->lifecycle.transitionTo(RenderThreadState::Running)) {
            enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                         "Backend resumed but lifecycle transition failed",
                         "ResumePresentation"});
            break;
          }
          state->signalLifecycle();
        }

        if (isUploadCommand(kind)) {
          if (isSupersedableAssetUploadCommand(kind)) {
            std::shared_ptr<const RenderFramePacket> stale_packet;
            (void)state->mailbox.tryTake(stale_packet);
          }
          (void)state->publishReliable(RenderEvent{UploadCompletedEvent{
              command.command_serial, kind, outcome.uploaded_bytes}});
        } else {
          (void)state->publishReliable(RenderEvent{RenderCommandAckEvent{
              command.command_serial, kind}});
        }
        continue;
      }

      // P2/P3 permit immutable, generation-compatible preview packets while
      // a static or HDRI Candidate is Pending. Publication submits an ordered
      // graphics-queue retirement marker before replacing the old owner, and
      // successful asset commit drops any stale mailbox packet.
      if (state->lifecycle.state() == RenderThreadState::Running) {
        std::shared_ptr<const RenderFramePacket> packet;
        if (state->mailbox.tryTake(packet) && packet) {
          const std::uint64_t render_serial = state->next_render_serial++;
          const HistoryCommitSnapshot *previous_history =
              state->history.current()
                  ? &state->history.current().value()
                  : nullptr;
          RenderThreadBackendOutcome outcome = state->backend->renderFrame(
              *packet, render_serial, previous_history);
          if (state->control->stopRequested()) {
            // A bounded-shutdown/fatal request that arrived while the backend
            // was inside GPU work invalidates this result as temporal or
            // presentation authority, even if the call later returns success.
            break;
          }
          if (outcome.still_render_status) {
            publish_still_observation(*outcome.still_render_status);
          }
          if (outcome.status ==
              RenderThreadBackendStatus::FatalQuarantine) {
            enter_fatal(std::move(outcome));
            break;
          }
          if (outcome.success() && outcome.temporal_history_accepted) {
            (void)state->history.commit(
                makeHistoryCommitCandidate(*packet, render_serial));
          }
          if (outcome.success() && outcome.presented) {
            LastPresentedSnapshot presented;
            presented.present_serial = state->next_present_serial++;
            presented.previous_history_render_serial =
                outcome.previous_history_render_serial;
            if (state->history.current() &&
                state->history.current()->render_serial == render_serial) {
              presented.history = state->history.current().value();
            } else {
              presented.history =
                  makeHistoryCommitCandidate(*packet, render_serial);
            }
            state->last_presented =
                std::make_shared<const LastPresentedSnapshot>(
                    std::move(presented));
          }
          RenderStatsEvent stats_event;
          stats_event.stats = outcome.stats;
          stats_event.render_serial = render_serial;
          stats_event.previous_history_render_serial =
              outcome.previous_history_render_serial;
          stats_event.history_serial = state->history.historySerial();
          stats_event.present_serial =
              state->last_presented
                  ? state->last_presented->present_serial
                  : 0u;
          stats_event.last_presented = state->last_presented;
          stats_event.ray_tracing_capability =
              outcome.ray_tracing_capability;
          stats_event.post_process_capabilities =
              outcome.post_process_capabilities;
          stats_event.post_process_status =
              std::move(outcome.post_process_status);
          stats_event.latency_ping_message =
              outcome.latency_ping_message;
          (void)state->events.publish(RenderEvent{std::move(stats_event)});
          continue;
        }
      }

      std::unique_lock lock(state->wake_mutex);
      (void)state->wake.wait_for(lock, kWorkerWakePoll, [&]() {
        return state->wake_epoch != observed_epoch ||
               state->control->stopRequested();
      });
    }
  } catch (const std::exception &exception) {
    enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                 std::string("RenderThread exception: ") + exception.what(),
                 "RenderThread::run"});
  } catch (...) {
    enter_fatal({RenderThreadBackendStatus::FatalQuarantine,
                 "RenderThread failed with an unknown exception",
                 "RenderThread::run"});
  }

  state->mailbox.close();
  state->commands.close();
  const RenderThreadState before_stop = state->lifecycle.state();
  if (before_stop != RenderThreadState::Stopping &&
      before_stop != RenderThreadState::Stopped) {
    (void)state->lifecycle.transitionTo(RenderThreadState::Stopping);
    state->signalLifecycle();
  }

  RenderThreadBackendShutdown backend_shutdown =
      RenderThreadBackendShutdown::Clean;
  if (state->backend) {
    backend_shutdown = state->backend->shutdown();
  }
  if (backend_shutdown == RenderThreadBackendShutdown::Quarantined ||
      state->control->fatalQuarantineRequested()) {
    state->backend_quarantined.store(true, std::memory_order_release);
    quarantineBackend(std::move(state->backend));
  } else {
    state->backend.reset();
  }
  (void)state->lifecycle.transitionTo(RenderThreadState::Stopped);
  state->signalLifecycle();
  (void)state->publishReliable(RenderEvent{RendererStoppedEvent{
      state->backend_quarantined.load(std::memory_order_acquire)}});
  state->events.close();
  {
    std::lock_guard lock(state->completion_mutex);
    state->thread_finished = true;
  }
  state->completion.notify_all();
}

} // namespace xpbd::gfx
