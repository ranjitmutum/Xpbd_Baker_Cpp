#include "xpbd/gfx/dedicated_render_thread.hpp"

#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/vulkan_window_bootstrap.hpp"
#include "xpbd/log.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace xpbd::gfx {
namespace {

using namespace std::chrono_literals;

constexpr auto kInitializeBudget = 30s;
constexpr auto kControlCommandBudget = 10s;
constexpr auto kUploadCommandBudget = 120s;
constexpr auto kShutdownBudget = 10s;

class ThreadedVulkanBackend final : public IGpuBackend {
public:
  ~ThreadedVulkanBackend() override { shutdown(); }

  bool init(SDL_Window *window) override {
    VulkanWindowBootstrap bootstrap;
    std::string error;
    if (!captureVulkanWindowBootstrap(window, bootstrap, &error)) {
      xpbd::log::errorf("Vulkan RenderThread bootstrap failed: %s",
                        error.c_str());
      return false;
    }
    return init(bootstrap);
  }

  bool init(const VulkanWindowBootstrap &bootstrap) override {
    if (worker_ || initialized_ || !bootstrap.renderThreadCompatible()) {
      return false;
    }

    worker_ = std::make_unique<DedicatedRenderThread>(
        bootstrap, []() { return createVulkanRenderThreadBackend(); });
    if (!worker_->start() ||
        !worker_->waitUntilRunning(kInitializeBudget)) {
      pumpEvents();
      failClient("Vulkan RenderThread failed to enter Running");
      (void)worker_->shutdown(kShutdownBudget);
      worker_.reset();
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          kControlCommandBudget;
    while (!initialized_ && !fatal_) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }
      RenderEvent event;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                now);
      if (worker_->waitEvent(event, remaining) != QueueWaitResult::Item) {
        break;
      }
      processEvent(event);
    }
    if (!initialized_) {
      failClient("Vulkan RenderThread initialization event timed out");
      (void)worker_->shutdown(kShutdownBudget);
      worker_.reset();
      return false;
    }

    xpbd::log::infof(
        "Vulkan RenderThread active: device=%s rt=%d streamline=%d",
        device_name_.c_str(), ray_tracing_capability_.supported ? 1 : 0,
        streamline_ready_ ? 1 : 0);
    return true;
  }

  void shutdown() override {
    if (!worker_) {
      initialized_ = false;
      return;
    }
    const DedicatedRenderThreadShutdown result =
        worker_->shutdown(kShutdownBudget);
    if (result != DedicatedRenderThreadShutdown::Clean) {
      fatal_ = true;
      xpbd::log::errorf(
          "Vulkan RenderThread shutdown retained quarantined ownership "
          "(result=%u)",
          static_cast<unsigned>(result));
    }
    worker_.reset();
    initialized_ = false;
  }

  void resize(int fb_w, int fb_h) override {
    if (!enqueueAsync(RenderCommand{ResizeRenderCommand{fb_w, fb_h}})) {
      failClient("Vulkan RenderThread rejected Resize");
    }
  }

  bool uploadFontAtlas(const void *pixels, int width, int height,
                       bool is_rgba) override {
    if (pixels == nullptr || width <= 0 || height <= 0) {
      return false;
    }
    const std::uint64_t channels = is_rgba ? 4u : 1u;
    const std::uint64_t byte_count = static_cast<std::uint64_t>(width) *
                                     static_cast<std::uint64_t>(height) *
                                     channels;
    if (byte_count > (std::numeric_limits<std::size_t>::max)()) {
      return false;
    }
    UploadFontAtlasRenderCommand upload;
    upload.width = static_cast<std::uint32_t>(width);
    upload.height = static_cast<std::uint32_t>(height);
    upload.rgba = is_rgba;
    const auto *begin = static_cast<const std::byte *>(pixels);
    upload.pixels.assign(begin,
                         begin + static_cast<std::size_t>(byte_count));
    return executeSynchronous(
        RenderCommand{std::move(upload)},
        RenderCommandKind::UploadFontAtlas, true, kUploadCommandBudget);
  }

  unsigned int fontTextureId() const override {
    return static_cast<unsigned int>(
        uiLogicalTextureId(UiLogicalTexture::FontAtlas));
  }
  bool supportsStaticModel() const override { return true; }

  void render(const FrameInput &frame) override {
    pumpEvents();
    applyStillClientStatus(frame.still_render);
    if (!readyForWork() || presentationSuspended()) {
      return;
    }

    std::shared_ptr<const RenderFramePacket> packet =
        packet_builder_.build(next_ui_frame_serial_++, frame);
    if (!packet) {
      failClient("Vulkan RenderThread packet construction failed");
      return;
    }

    const StillRenderFrameRequest *still_request = frame.still_render;
    const bool start_still =
        still_request != nullptr && still_request->status != nullptr &&
        still_request->job_id != 0u &&
        still_request->status->state == StillRenderJobState::Queued &&
        !stillClientActive();
    // The reliable Start command owns the exact assets uploaded immediately
    // before it. Do not let newer preview packets replace those GPU resources
    // until the terminal Still event arrives.
    if (!stillClientActive()) {
      const AssetSyncState static_sync =
          synchronizeStaticAsset(packet);
      if (static_sync == AssetSyncState::Failed) {
        return;
      }
      if (static_sync == AssetSyncState::Pending) {
        publishAcceptedPreviewDuringPending();
        return;
      }
      const AssetSyncState environment_sync =
          synchronizeEnvironment(packet);
      if (environment_sync == AssetSyncState::Failed) {
        return;
      }
      if (environment_sync == AssetSyncState::Pending) {
        publishAcceptedPreviewDuringPending();
        return;
      }
    }
    if (start_still && !enqueueStillStart(*packet, *still_request)) {
      applyStillClientStatus(still_request);
    }
    if (still_request != nullptr && stillClientActive() &&
        still_status_.job_id == still_request->job_id &&
        still_request->cancel_requested && !still_cancel_enqueued_) {
      if (worker_->enqueue(RenderCommand{CancelStillRenderCommand{
              still_request->job_id}})) {
        still_cancel_enqueued_ = true;
      } else {
        failStillClient(still_request->job_id,
                        "RenderThread rejected Still cancellation");
      }
    }
    applyStillClientStatus(still_request);

    const MailboxPublishOutcome published = worker_->publishFrame(packet);
    latest_ui_frame_serial_ = packet->ui_frame_serial;
    latest_packet_serial_ = packet->packet_serial;
    if (published.result == MailboxPublishResult::Replaced) {
      ++mailbox_replacement_count_;
    }
    if (published.result != MailboxPublishResult::Published &&
        published.result != MailboxPublishResult::Replaced) {
      failClient("Vulkan RenderThread rejected the latest frame packet");
    } else {
      last_committed_packet_ = packet;
    }
  }

  // Main produces UI packets only. The worker assigns the Streamline frame
  // token from its consumed render_serial and closes the simulation marker.
  void beginLatencyFrame(std::uint32_t, PathTraceReflexMode,
                         bool) override {}
  void endLatencySimulation() override {}

  std::uint32_t latencyPingMessage() const override {
    return latency_ping_message_;
  }

  void markLatencyPing() override {
    if (!enqueueAsync(RenderCommand{MarkLatencyPingRenderCommand{}})) {
      failClient("Vulkan RenderThread rejected the PCL latency marker");
    }
  }

  void setVSync(bool enabled) override {
    vsync_enabled_ = enabled;
    if (!enqueueAsync(RenderCommand{SetVSyncRenderCommand{enabled}})) {
      failClient("Vulkan RenderThread rejected SetVSync");
    }
  }

  bool vsyncEnabled() const override { return vsync_enabled_; }
  BackendKind kind() const override { return BackendKind::Vulkan; }
  const char *name() const override { return "Vulkan"; }
  const char *deviceName() const override { return device_name_.c_str(); }

  FrameStats stats() const override {
    const_cast<ThreadedVulkanBackend *>(this)->pumpEvents();
    return stats_;
  }

  std::shared_ptr<const LastPresentedSnapshot>
  lastPresentedSnapshot() const override {
    const_cast<ThreadedVulkanBackend *>(this)->pumpEvents();
    return last_presented_snapshot_;
  }

  RenderThreadDiagnostics renderThreadDiagnostics() const override {
    auto *self = const_cast<ThreadedVulkanBackend *>(this);
    self->pumpEvents();
    return {
        self->readyForWork(),
        self->fatal_,
        self->latest_ui_frame_serial_,
        self->latest_packet_serial_,
        self->latest_render_serial_,
        self->latest_previous_history_render_serial_,
        self->latest_history_serial_,
        self->latest_present_serial_,
        self->last_presented_snapshot_,
        self->mailbox_replacement_count_,
        self->last_world_generation_,
        self->last_world_hdri_runtime_generation_,
        self->pending_environment_upload_.has_value() ||
            self->queued_environment_upload_.has_value(),
    };
  }

  PathTracePostProcessCapabilities
  pathTracePostProcessCapabilities() const override {
    return post_process_capabilities_;
  }

  std::string_view pathTracePostProcessStatus() const override {
    return post_process_status_;
  }

  RayTracingCapability rayTracingCapability() const override {
    return ray_tracing_capability_;
  }

  bool supportsRayTracing() const override {
    return ray_tracing_capability_.supported &&
           ray_tracing_capability_.device_extensions_enabled;
  }

  RenderPath activeRenderPath() const override {
    return stats_.active_render_path ==
                   static_cast<int>(RenderPath::RayTracing)
               ? RenderPath::RayTracing
               : RenderPath::Raster;
  }

  bool prepareForSystemDialog() override {
    if (!worker_) {
      return false;
    }
    if (worker_->state() == RenderThreadState::Suspended) {
      return true;
    }
    return executeSynchronous(
        RenderCommand{SuspendPresentationRenderCommand{}},
        RenderCommandKind::SuspendPresentation, false,
        kControlCommandBudget) &&
           worker_->state() == RenderThreadState::Suspended;
  }

  bool resumeAfterSystemDialog() override {
    if (!worker_) {
      return false;
    }
    if (worker_->state() == RenderThreadState::Running) {
      return true;
    }
    return executeSynchronous(
        RenderCommand{ResumePresentationRenderCommand{}},
        RenderCommandKind::ResumePresentation, false,
        kControlCommandBudget) &&
           worker_->state() == RenderThreadState::Running;
  }

  bool presentationSuspended() const override {
    return worker_ && worker_->state() == RenderThreadState::Suspended;
  }

private:
  [[nodiscard]] bool readyForWork() const noexcept {
    return worker_ && initialized_ && !fatal_ &&
           worker_->state() != RenderThreadState::FatalQuarantined &&
           worker_->state() != RenderThreadState::Stopping &&
           worker_->state() != RenderThreadState::Stopped;
  }

  void failClient(std::string message) {
    if (!fatal_) {
      fatal_detail_ = std::move(message);
      xpbd::log::errorf("Vulkan RenderThread client fatal: %s",
                        fatal_detail_.c_str());
    }
    fatal_ = true;
    if (worker_ && worker_->control()) {
      worker_->control()->requestFatalQuarantine();
    }
  }

  void processEvent(const RenderEvent &event) {
    if (const auto *initialized =
            std::get_if<RendererInitializedEvent>(&event)) {
      device_name_ = initialized->device_name;
      streamline_ready_ = initialized->streamline_ready;
      ray_tracing_capability_ = initialized->ray_tracing_capability;
      post_process_capabilities_ =
          initialized->post_process_capabilities;
      post_process_status_ = initialized->post_process_status;
      latency_ping_message_ = initialized->latency_ping_message;
      initialized_ = true;
      return;
    }
    if (const auto *stats = std::get_if<RenderStatsEvent>(&event)) {
      stats_ = stats->stats;
      latest_render_serial_ = stats->render_serial;
      latest_previous_history_render_serial_ =
          stats->previous_history_render_serial;
      latest_history_serial_ = stats->history_serial;
      latest_present_serial_ = stats->present_serial;
      last_presented_snapshot_ = stats->last_presented;
      ray_tracing_capability_ = stats->ray_tracing_capability;
      post_process_capabilities_ = stats->post_process_capabilities;
      post_process_status_ = stats->post_process_status;
      latency_ping_message_ = stats->latency_ping_message;
      return;
    }
    if (const auto *progress =
            std::get_if<StillRenderProgressEvent>(&event)) {
      if (still_status_.job_id == progress->job_id) {
        still_status_.state = progress->state;
        still_status_.accumulated_samples =
            progress->completed_samples;
        still_status_.target_samples = progress->target_samples;
      }
      return;
    }
    if (const auto *completed =
            std::get_if<StillRenderCompletedEvent>(&event)) {
      if (still_status_.job_id == completed->job_id) {
        still_status_.state = StillRenderJobState::Completed;
        still_status_.accumulated_samples =
            completed->completed_samples;
        still_status_.target_samples = completed->target_samples;
        still_status_.output_path = completed->output_path;
        still_status_.error.clear();
      }
      return;
    }
    if (const auto *failed =
            std::get_if<StillRenderFailedEvent>(&event)) {
      if (still_status_.job_id == failed->job_id) {
        still_status_.state = StillRenderJobState::Failed;
        still_status_.accumulated_samples = failed->completed_samples;
        still_status_.target_samples = failed->target_samples;
        still_status_.error = failed->message;
      }
      return;
    }
    if (const auto *cancelled =
            std::get_if<StillRenderCancelledEvent>(&event)) {
      if (still_status_.job_id == cancelled->job_id) {
        still_status_.state = StillRenderJobState::Cancelled;
        still_status_.accumulated_samples =
            cancelled->completed_samples;
        still_status_.target_samples = cancelled->target_samples;
        still_status_.error.clear();
      }
      return;
    }
    if (const auto *fatal =
            std::get_if<RendererFatalErrorEvent>(&event)) {
      fatal_ = true;
      fatal_detail_ = fatal->api + ": " + fatal->message;
      if (stillClientActive()) {
        failStillClient(still_status_.job_id, fatal_detail_);
      }
      xpbd::log::errorf("Vulkan RenderThread fatal event: %s",
                        fatal_detail_.c_str());
      return;
    }
    if (const auto *failed =
            std::get_if<RenderCommandFailedEvent>(&event)) {
      xpbd::log::errorf(
          "Vulkan RenderThread command %llu failed (kind=%u): %s",
          static_cast<unsigned long long>(failed->command_serial),
          static_cast<unsigned>(failed->command_kind),
          failed->message.c_str());
      return;
    }
    if (const auto *completed =
            std::get_if<UploadCompletedEvent>(&event)) {
      if (pending_static_upload_ &&
          completed->command_kind ==
              RenderCommandKind::UploadStaticAsset &&
          completed->command_serial ==
              pending_static_upload_->command_serial) {
        acceptStaticUpload(pending_static_upload_->packet);
        xpbd::log::infof(
            "A5_STATIC_PENDING_CLIENT_COMMIT command_serial=%llu "
            "bytes=%llu model_generation=%llu texture_generation=%llu",
            static_cast<unsigned long long>(
                completed->command_serial),
            static_cast<unsigned long long>(
                completed->uploaded_bytes),
            static_cast<unsigned long long>(
                last_static_model_generation_),
            static_cast<unsigned long long>(
                last_static_texture_generation_));
        pending_static_upload_.reset();
        promoteQueuedStaticUpload();
      } else if (pending_environment_upload_ &&
                 completed->command_kind ==
                     RenderCommandKind::UploadEnvironment &&
                 completed->command_serial ==
                     pending_environment_upload_->command_serial) {
        acceptEnvironmentUpload(pending_environment_upload_->packet);
        xpbd::log::infof(
            "A5_ENVIRONMENT_PENDING_CLIENT_COMMIT command_serial=%llu "
            "bytes=%llu world_generation=%llu "
            "hdri_runtime_generation=%llu",
            static_cast<unsigned long long>(completed->command_serial),
            static_cast<unsigned long long>(completed->uploaded_bytes),
            static_cast<unsigned long long>(last_world_generation_),
            static_cast<unsigned long long>(
                last_world_hdri_runtime_generation_));
        pending_environment_upload_.reset();
        promoteQueuedEnvironmentUpload();
      }
      return;
    }
    if (const auto *superseded =
            std::get_if<UploadSupersededEvent>(&event)) {
      if (pending_static_upload_ &&
          superseded->command_kind ==
              RenderCommandKind::UploadStaticAsset &&
          superseded->command_serial ==
              pending_static_upload_->command_serial) {
        if (!queued_static_upload_ ||
            queued_static_upload_->command_serial !=
                superseded->replacement_command_serial) {
          failClient(
              "Vulkan RenderThread static Supersede tracking diverged");
          return;
        }
        xpbd::log::infof(
            "A5_STATIC_PENDING_CLIENT_SUPERSEDED command_serial=%llu "
            "replacement_command_serial=%llu",
            static_cast<unsigned long long>(
                superseded->command_serial),
            static_cast<unsigned long long>(
                superseded->replacement_command_serial));
        pending_static_upload_.reset();
        promoteQueuedStaticUpload();
      } else if (pending_environment_upload_ &&
                 superseded->command_kind ==
                     RenderCommandKind::UploadEnvironment &&
                 superseded->command_serial ==
                     pending_environment_upload_->command_serial) {
        if (!queued_environment_upload_ ||
            queued_environment_upload_->command_serial !=
                superseded->replacement_command_serial) {
          failClient(
              "Vulkan RenderThread environment Supersede tracking diverged");
          return;
        }
        xpbd::log::infof(
            "A5_ENVIRONMENT_PENDING_CLIENT_SUPERSEDED "
            "command_serial=%llu replacement_command_serial=%llu",
            static_cast<unsigned long long>(
                superseded->command_serial),
            static_cast<unsigned long long>(
                superseded->replacement_command_serial));
        pending_environment_upload_.reset();
        promoteQueuedEnvironmentUpload();
      }
      return;
    }
    if (const auto *failed = std::get_if<UploadFailedEvent>(&event)) {
      if (pending_static_upload_ &&
          failed->command_kind ==
              RenderCommandKind::UploadStaticAsset &&
          failed->command_serial ==
              pending_static_upload_->command_serial) {
        pending_static_upload_.reset();
        promoteQueuedStaticUpload();
      } else if (queued_static_upload_ &&
                 failed->command_kind ==
                     RenderCommandKind::UploadStaticAsset &&
                 failed->command_serial ==
                     queued_static_upload_->command_serial) {
        queued_static_upload_.reset();
      } else if (pending_environment_upload_ &&
                 failed->command_kind ==
                     RenderCommandKind::UploadEnvironment &&
                 failed->command_serial ==
                     pending_environment_upload_->command_serial) {
        pending_environment_upload_.reset();
        promoteQueuedEnvironmentUpload();
      } else if (queued_environment_upload_ &&
                 failed->command_kind ==
                     RenderCommandKind::UploadEnvironment &&
                 failed->command_serial ==
                     queued_environment_upload_->command_serial) {
        queued_environment_upload_.reset();
      }
      xpbd::log::errorf(
          "Vulkan RenderThread upload %llu failed (kind=%u): %s",
          static_cast<unsigned long long>(failed->command_serial),
          static_cast<unsigned>(failed->command_kind),
          failed->message.c_str());
      return;
    }
    if (const auto *stopped = std::get_if<RendererStoppedEvent>(&event)) {
      if (stopped->fatal_quarantine) {
        fatal_ = true;
      }
    }
  }

  void pumpEvents() {
    if (!worker_) {
      return;
    }
    RenderEvent event;
    while (worker_->tryPopEvent(event)) {
      processEvent(event);
    }
  }

  [[nodiscard]] bool enqueueAsync(RenderCommand command) {
    pumpEvents();
    return readyForWork() && worker_->enqueue(std::move(command)).has_value();
  }

  [[nodiscard]] bool stillClientActive() const noexcept {
    return still_status_.state == StillRenderJobState::Queued ||
           still_status_.state == StillRenderJobState::Rendering ||
           still_status_.state == StillRenderJobState::Saving;
  }

  void applyStillClientStatus(
      const StillRenderFrameRequest *request) const {
    if (request == nullptr || request->status == nullptr ||
        request->job_id == 0u ||
        request->job_id != still_status_.job_id) {
      return;
    }
    *request->status = still_status_;
  }

  void failStillClient(std::uint64_t job_id, std::string message) {
    if (job_id == 0u || still_status_.job_id != job_id) {
      return;
    }
    still_status_.state = StillRenderJobState::Failed;
    still_status_.error = std::move(message);
    still_cancel_enqueued_ = false;
  }

  [[nodiscard]] bool enqueueStillStart(
      const RenderFramePacket &packet,
      const StillRenderFrameRequest &request) {
    const std::optional<StartStillRenderCommand> start =
        makeStartStillRenderCommand(packet, request);
    if (!start) {
      still_status_ = {};
      still_status_.job_id = request.job_id;
      still_status_.target_samples = request.target_samples;
      still_status_.output_path = request.output_path;
      still_status_.state = StillRenderJobState::Failed;
      still_status_.error =
          "Could not construct an immutable Still render snapshot";
      return false;
    }
    if (!worker_->enqueue(RenderCommand{*start})) {
      still_status_ = {};
      still_status_.job_id = request.job_id;
      still_status_.target_samples = request.target_samples;
      still_status_.output_path = request.output_path;
      still_status_.state = StillRenderJobState::Failed;
      still_status_.error = "RenderThread rejected immutable Still start";
      return false;
    }
    still_status_ = {};
    still_status_.state = StillRenderJobState::Queued;
    still_status_.job_id = start->job_id;
    still_status_.target_samples = start->target_samples;
    still_status_.output_path = start->output_path;
    still_cancel_enqueued_ = false;
    return true;
  }

  [[nodiscard]] bool executeSynchronous(
      RenderCommand command, RenderCommandKind expected_kind,
      bool expect_upload_event, std::chrono::milliseconds timeout) {
    pumpEvents();
    if (!readyForWork()) {
      return false;
    }
    const std::optional<std::uint64_t> command_serial =
        worker_->enqueue(std::move(command));
    if (!command_serial) {
      return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!fatal_) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        failClient("Vulkan RenderThread synchronous command timed out");
        return false;
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                now);
      RenderEvent event;
      if (worker_->waitEvent(event, remaining) != QueueWaitResult::Item) {
        failClient("Vulkan RenderThread synchronous event wait failed");
        return false;
      }

      bool matched_success = false;
      bool matched_failure = false;
      if (expect_upload_event) {
        if (const auto *completed =
                std::get_if<UploadCompletedEvent>(&event)) {
          matched_success =
              completed->command_serial == *command_serial &&
              completed->command_kind == expected_kind;
        } else if (const auto *failed =
                       std::get_if<UploadFailedEvent>(&event)) {
          matched_failure = failed->command_serial == *command_serial;
        }
      } else {
        if (const auto *ack = std::get_if<RenderCommandAckEvent>(&event)) {
          matched_success = ack->command_serial == *command_serial &&
                            ack->command_kind == expected_kind;
        } else if (const auto *failed =
                       std::get_if<RenderCommandFailedEvent>(&event)) {
          matched_failure = failed->command_serial == *command_serial;
        }
      }
      processEvent(event);
      if (matched_success) {
        return true;
      }
      if (matched_failure) {
        return false;
      }
    }
    return false;
  }

  enum class AssetSyncState : std::uint8_t {
    Ready,
    Pending,
    Failed,
  };

  struct PendingStaticUpload {
    std::uint64_t command_serial = 0u;
    std::shared_ptr<const RenderFramePacket> packet;
    std::chrono::steady_clock::time_point started_at{};
  };

  // Static and environment uploads share the same bounded client lifecycle:
  // one executing command, at most one reliable replacement, and one latest
  // desired immutable packet.
  using PendingEnvironmentUpload = PendingStaticUpload;

  [[nodiscard]] static bool hasStaticAssetTarget(
      const RenderFramePacket &packet) noexcept {
    return packet.scene.static_model &&
           packet.scene.static_model_frame;
  }

  [[nodiscard]] static bool staticAssetTargetsMatch(
      const RenderFramePacket &left,
      const RenderFramePacket &right) noexcept {
    const bool left_present = hasStaticAssetTarget(left);
    const bool right_present = hasStaticAssetTarget(right);
    if (left_present != right_present) {
      return false;
    }
    if (!left_present) {
      return true;
    }
    return left.scene.static_model.get() ==
               right.scene.static_model.get() &&
           left.scene.static_model_texture.get() ==
               right.scene.static_model_texture.get() &&
           left.scene.static_model_material.get() ==
               right.scene.static_model_material.get() &&
           left.static_model_generation ==
               right.static_model_generation &&
           left.static_texture_generation ==
               right.static_texture_generation;
  }

  [[nodiscard]] bool staticAssetMatchesAccepted(
      const RenderFramePacket &packet) const noexcept {
    if (!hasStaticAssetTarget(packet)) {
      return !static_upload_initialized_;
    }
    return static_upload_initialized_ &&
           last_static_model_.lock().get() ==
               packet.scene.static_model.get() &&
           last_static_texture_.lock().get() ==
               packet.scene.static_model_texture.get() &&
           last_static_material_.lock().get() ==
               packet.scene.static_model_material.get() &&
           last_static_model_generation_ ==
               packet.static_model_generation &&
           last_static_texture_generation_ ==
               packet.static_texture_generation;
  }

  [[nodiscard]] static bool hasEnvironmentTarget(
      const RenderFramePacket &packet) noexcept {
    return packet.scene.world_environment != nullptr ||
           packet.scene.preview_skybox != nullptr;
  }

  [[nodiscard]] static bool environmentTargetsMatch(
      const RenderFramePacket &left,
      const RenderFramePacket &right) noexcept {
    if (hasEnvironmentTarget(left) != hasEnvironmentTarget(right) ||
        static_cast<bool>(left.scene.world_environment) !=
            static_cast<bool>(right.scene.world_environment) ||
        static_cast<bool>(left.scene.preview_skybox) !=
            static_cast<bool>(right.scene.preview_skybox)) {
      return false;
    }
    if (!hasEnvironmentTarget(left)) {
      return true;
    }
    if (left.scene.world_environment &&
        (left.scene.world_environment->generation !=
             right.scene.world_environment->generation ||
         left.scene.world_environment->hdri_runtime_generation !=
             right.scene.world_environment->hdri_runtime_generation)) {
      return false;
    }
    return !left.scene.preview_skybox ||
           (left.scene.preview_skybox->generation ==
                right.scene.preview_skybox->generation &&
            left.scene.preview_skybox->source_identity ==
                right.scene.preview_skybox->source_identity);
  }

  [[nodiscard]] bool environmentMatchesAccepted(
      const RenderFramePacket &packet) const noexcept {
    if (!hasEnvironmentTarget(packet)) {
      return !environment_upload_initialized_;
    }
    if (!environment_upload_initialized_) {
      return false;
    }
    const std::uint64_t world_generation =
        packet.scene.world_environment
            ? packet.scene.world_environment->generation
            : 0u;
    const std::uint64_t world_hdri_runtime_generation =
        packet.scene.world_environment
            ? packet.scene.world_environment->hdri_runtime_generation
            : 0u;
    const std::uint64_t skybox_generation =
        packet.scene.preview_skybox
            ? packet.scene.preview_skybox->generation
            : 0u;
    const std::string_view skybox_identity =
        packet.scene.preview_skybox
            ? std::string_view(
                  packet.scene.preview_skybox->source_identity)
            : std::string_view{};
    return last_world_generation_ == world_generation &&
           last_world_hdri_runtime_generation_ ==
               world_hdri_runtime_generation &&
           last_preview_skybox_generation_ == skybox_generation &&
           last_preview_skybox_identity_ == skybox_identity;
  }

  void publishAcceptedPreviewDuringPending() {
    if (!last_committed_packet_ ||
        !staticAssetMatchesAccepted(*last_committed_packet_) ||
        !environmentMatchesAccepted(*last_committed_packet_)) {
      return;
    }
    const MailboxPublishOutcome published =
        worker_->publishFrame(last_committed_packet_);
    if (published.result == MailboxPublishResult::Replaced) {
      ++mailbox_replacement_count_;
    } else if (published.result != MailboxPublishResult::Published) {
      failClient(
          "Vulkan RenderThread rejected the committed Pending preview");
    }
  }

  void promoteQueuedStaticUpload() {
    if (!queued_static_upload_) {
      return;
    }
    pending_static_upload_ = std::move(queued_static_upload_);
    queued_static_upload_.reset();
  }

  void acceptStaticUpload(
      const std::shared_ptr<const RenderFramePacket> &packet) {
    if (!packet) {
      return;
    }
    static_upload_initialized_ = true;
    last_static_model_ = packet->scene.static_model;
    last_static_texture_ = packet->scene.static_model_texture;
    last_static_material_ = packet->scene.static_model_material;
    last_static_model_generation_ = packet->static_model_generation;
    last_static_texture_generation_ =
        packet->static_texture_generation;
  }

  [[nodiscard]] AssetSyncState synchronizeStaticAsset(
      const std::shared_ptr<const RenderFramePacket> &packet) {
    desired_static_packet_ = packet;
    if (!pending_static_upload_ && queued_static_upload_) {
      promoteQueuedStaticUpload();
    }
    if (pending_static_upload_) {
      if (std::chrono::steady_clock::now() -
              pending_static_upload_->started_at >
          kUploadCommandBudget) {
        failClient("Vulkan RenderThread Pending static upload timed out");
        return AssetSyncState::Failed;
      }
      if (staticAssetTargetsMatch(*packet,
                                  *pending_static_upload_->packet) ||
          (queued_static_upload_ &&
           staticAssetTargetsMatch(*packet,
                                   *queued_static_upload_->packet)) ||
          !hasStaticAssetTarget(*packet)) {
        return AssetSyncState::Pending;
      }
      if (!queued_static_upload_) {
        const std::optional<std::uint64_t> replacement_serial =
            worker_->enqueue(RenderCommand{
                UploadStaticAssetRenderCommand{packet}});
        if (replacement_serial) {
          queued_static_upload_ = PendingStaticUpload{
              *replacement_serial, packet,
              std::chrono::steady_clock::now()};
          xpbd::log::infof(
              "A5_STATIC_PENDING_CLIENT_QUEUE command_serial=%llu "
              "replaces_command_serial=%llu model_generation=%llu "
              "texture_generation=%llu",
              static_cast<unsigned long long>(*replacement_serial),
              static_cast<unsigned long long>(
                  pending_static_upload_->command_serial),
              static_cast<unsigned long long>(
                  packet->static_model_generation),
              static_cast<unsigned long long>(
                  packet->static_texture_generation));
        }
      }
      return AssetSyncState::Pending;
    }
    if (!hasStaticAssetTarget(*packet)) {
      static_upload_initialized_ = false;
      last_static_model_.reset();
      last_static_texture_.reset();
      last_static_material_.reset();
      return AssetSyncState::Ready;
    }
    const bool changed =
        !static_upload_initialized_ ||
        last_static_model_.lock().get() != packet->scene.static_model.get() ||
        last_static_texture_.lock().get() !=
            packet->scene.static_model_texture.get() ||
        last_static_material_.lock().get() !=
            packet->scene.static_model_material.get() ||
        last_static_model_generation_ != packet->static_model_generation ||
        last_static_texture_generation_ != packet->static_texture_generation;
    if (!changed) {
      return AssetSyncState::Ready;
    }
    const std::optional<std::uint64_t> command_serial =
        worker_->enqueue(
            RenderCommand{UploadStaticAssetRenderCommand{packet}});
    if (!command_serial) {
      return AssetSyncState::Failed;
    }
    pending_static_upload_ = PendingStaticUpload{
        *command_serial, packet, std::chrono::steady_clock::now()};
    xpbd::log::infof(
        "A5_STATIC_PENDING_CLIENT_BEGIN command_serial=%llu "
        "model_generation=%llu texture_generation=%llu",
        static_cast<unsigned long long>(*command_serial),
        static_cast<unsigned long long>(
            packet->static_model_generation),
        static_cast<unsigned long long>(
            packet->static_texture_generation));
    return AssetSyncState::Pending;
  }

  void promoteQueuedEnvironmentUpload() {
    if (!queued_environment_upload_) {
      return;
    }
    pending_environment_upload_ =
        std::move(queued_environment_upload_);
    queued_environment_upload_.reset();
  }

  void clearAcceptedEnvironment() {
    environment_upload_initialized_ = false;
    last_world_generation_ = 0u;
    last_world_hdri_runtime_generation_ = 0u;
    last_preview_skybox_generation_ = 0u;
    last_preview_skybox_identity_.clear();
  }

  void acceptEnvironmentUpload(
      const std::shared_ptr<const RenderFramePacket> &packet) {
    if (!packet || !hasEnvironmentTarget(*packet)) {
      clearAcceptedEnvironment();
      return;
    }
    environment_upload_initialized_ = true;
    last_world_generation_ =
        packet->scene.world_environment
            ? packet->scene.world_environment->generation
            : 0u;
    last_world_hdri_runtime_generation_ =
        packet->scene.world_environment
            ? packet->scene.world_environment->hdri_runtime_generation
            : 0u;
    last_preview_skybox_generation_ =
        packet->scene.preview_skybox
            ? packet->scene.preview_skybox->generation
            : 0u;
    last_preview_skybox_identity_ =
        packet->scene.preview_skybox
            ? packet->scene.preview_skybox->source_identity
            : std::string{};
  }

  [[nodiscard]] AssetSyncState synchronizeEnvironment(
      const std::shared_ptr<const RenderFramePacket> &packet) {
    desired_environment_packet_ = packet;
    if (!pending_environment_upload_ && queued_environment_upload_) {
      promoteQueuedEnvironmentUpload();
    }
    if (pending_environment_upload_) {
      if (std::chrono::steady_clock::now() -
              pending_environment_upload_->started_at >
          kUploadCommandBudget) {
        failClient("Vulkan RenderThread Pending environment upload timed out");
        return AssetSyncState::Failed;
      }
      if (environmentTargetsMatch(
              *packet, *pending_environment_upload_->packet) ||
          (queued_environment_upload_ &&
           environmentTargetsMatch(
               *packet, *queued_environment_upload_->packet)) ||
          !hasEnvironmentTarget(*packet)) {
        return AssetSyncState::Pending;
      }
      if (!queued_environment_upload_) {
        const std::optional<std::uint64_t> replacement_serial =
            worker_->enqueue(RenderCommand{
                UploadEnvironmentRenderCommand{packet}});
        if (replacement_serial) {
          queued_environment_upload_ = PendingEnvironmentUpload{
              *replacement_serial, packet,
              std::chrono::steady_clock::now()};
          xpbd::log::infof(
              "A5_ENVIRONMENT_PENDING_CLIENT_QUEUE command_serial=%llu "
              "replaces_command_serial=%llu world_generation=%llu "
              "hdri_runtime_generation=%llu",
              static_cast<unsigned long long>(*replacement_serial),
              static_cast<unsigned long long>(
                  pending_environment_upload_->command_serial),
              static_cast<unsigned long long>(
                  packet->scene.world_environment
                      ? packet->scene.world_environment->generation
                      : 0u),
              static_cast<unsigned long long>(
                  packet->scene.world_environment
                      ? packet->scene.world_environment
                            ->hdri_runtime_generation
                      : 0u));
        }
      }
      return AssetSyncState::Pending;
    }
    if (!hasEnvironmentTarget(*packet)) {
      clearAcceptedEnvironment();
      return AssetSyncState::Ready;
    }
    if (environmentMatchesAccepted(*packet)) {
      return AssetSyncState::Ready;
    }
    const std::optional<std::uint64_t> command_serial =
        worker_->enqueue(RenderCommand{
            UploadEnvironmentRenderCommand{packet}});
    if (!command_serial) {
      return AssetSyncState::Failed;
    }
    pending_environment_upload_ = PendingEnvironmentUpload{
        *command_serial, packet, std::chrono::steady_clock::now()};
    xpbd::log::infof(
        "A5_ENVIRONMENT_PENDING_CLIENT_BEGIN command_serial=%llu "
        "world_generation=%llu hdri_runtime_generation=%llu",
        static_cast<unsigned long long>(*command_serial),
        static_cast<unsigned long long>(
            packet->scene.world_environment
                ? packet->scene.world_environment->generation
                : 0u),
        static_cast<unsigned long long>(
            packet->scene.world_environment
                ? packet->scene.world_environment->hdri_runtime_generation
                : 0u));
    return AssetSyncState::Pending;
  }

  std::unique_ptr<DedicatedRenderThread> worker_;
  RenderFramePacketBuilder packet_builder_;
  std::uint64_t next_ui_frame_serial_ = 1u;
  bool initialized_ = false;
  bool fatal_ = false;
  bool streamline_ready_ = false;
  bool vsync_enabled_ = true;
  std::string fatal_detail_;
  std::string device_name_ = "Vulkan RenderThread starting";
  std::string post_process_status_ =
      "Vulkan RenderThread has not initialized Streamline";
  FrameStats stats_{};
  RayTracingCapability ray_tracing_capability_{};
  PathTracePostProcessCapabilities post_process_capabilities_{};
  std::uint32_t latency_ping_message_ = 0u;
  std::uint64_t latest_render_serial_ = 0u;
  std::uint64_t latest_previous_history_render_serial_ = 0u;
  std::uint64_t latest_history_serial_ = 0u;
  std::uint64_t latest_present_serial_ = 0u;
  std::uint64_t latest_ui_frame_serial_ = 0u;
  std::uint64_t latest_packet_serial_ = 0u;
  std::uint64_t mailbox_replacement_count_ = 0u;
  std::shared_ptr<const LastPresentedSnapshot> last_presented_snapshot_;
  StillRenderStatus still_status_{};
  bool still_cancel_enqueued_ = false;

  bool static_upload_initialized_ = false;
  std::weak_ptr<const StaticIndexedModelMesh> last_static_model_;
  std::weak_ptr<const TextureImage> last_static_texture_;
  std::weak_ptr<const ResolvedMaterialTable> last_static_material_;
  std::uint64_t last_static_model_generation_ = 0u;
  std::uint64_t last_static_texture_generation_ = 0u;
  std::optional<PendingStaticUpload> pending_static_upload_;
  std::optional<PendingStaticUpload> queued_static_upload_;
  std::shared_ptr<const RenderFramePacket> desired_static_packet_;
  std::shared_ptr<const RenderFramePacket> last_committed_packet_;

  bool environment_upload_initialized_ = false;
  std::uint64_t last_world_generation_ = 0u;
  std::uint64_t last_world_hdri_runtime_generation_ = 0u;
  std::uint64_t last_preview_skybox_generation_ = 0u;
  std::string last_preview_skybox_identity_;
  std::optional<PendingEnvironmentUpload> pending_environment_upload_;
  std::optional<PendingEnvironmentUpload> queued_environment_upload_;
  std::shared_ptr<const RenderFramePacket> desired_environment_packet_;
};

} // namespace

std::unique_ptr<IGpuBackend> createVulkanBackend() {
  return std::make_unique<ThreadedVulkanBackend>();
}

} // namespace xpbd::gfx
