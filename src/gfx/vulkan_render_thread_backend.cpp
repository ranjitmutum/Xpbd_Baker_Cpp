#include "xpbd/gfx/dedicated_render_thread.hpp"
#include "xpbd/log.hpp"

#include "vulkan/vulkan_backend_internal.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>

namespace xpbd::gfx::detail {
namespace {

[[nodiscard]] bool environmentFlagEnabled(const char *name) {
  const char *value = std::getenv(name);
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

[[nodiscard]] std::uint64_t environmentUnsigned(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return 0u;
  }
  std::uint64_t parsed = 0u;
  const char *end = value + std::strlen(value);
  const auto result = std::from_chars(value, end, parsed);
  return result.ec == std::errc{} && result.ptr == end ? parsed : 0u;
}

} // namespace

class VulkanRenderThreadBackend final : public IRenderThreadBackend {
public:
  RenderThreadBackendOutcome initialize(
      const VulkanWindowBootstrap &bootstrap,
      std::shared_ptr<RenderThreadControl> control) override {
    if (!bootstrap.renderThreadCompatible() || !control) {
      return failed("VulkanRenderThreadBackend::initialize",
                    "RenderThread Vulkan bootstrap/control is invalid");
    }
    control_ = std::move(control);
    h1_inject_fatal_after_presents_ =
        environmentUnsigned("XPBD_H1_INJECT_FATAL_AFTER_PRESENTS");
    h1_shutdown_during_upload_ =
        environmentFlagEnabled("XPBD_H1_SHUTDOWN_DURING_UPLOAD");
    backend_ = std::make_unique<VulkanBackend>();
    backend_->render_thread_control_ = control_;
    if (!backend_->init(bootstrap)) {
      return backendOutcome(
          "VulkanBackend::init",
          "Vulkan RenderThread backend initialization failed");
    }
    if (h1_inject_fatal_after_presents_ > 0u) {
      xpbd::log::infof(
          "H1_FATAL_INJECTION_ARMED after_presents=%llu",
          static_cast<unsigned long long>(
              h1_inject_fatal_after_presents_));
    }
    if (h1_shutdown_during_upload_) {
      xpbd::log::info(
          "H1_UPLOAD_SHUTDOWN_ARMED command=UploadStaticAsset delay_ms=1");
    }
    RenderThreadBackendOutcome outcome;
    outcome.device_name = backend_->device_name_;
    outcome.ray_tracing_ready = backend_->supportsRayTracing();
    outcome.streamline_ready =
        backend_->streamline_vulkan_runtime_.initialized();
    outcome.ray_tracing_capability = backend_->rayTracingCapability();
    outcome.post_process_capabilities =
        backend_->pathTracePostProcessCapabilities();
    outcome.post_process_status =
        std::string(backend_->pathTracePostProcessStatus());
    outcome.latency_ping_message = backend_->latencyPingMessage();
    return outcome;
  }

  RenderThreadBackendOutcome executeCommand(
      const ReliableRenderCommand &command) override {
    if (!backend_) {
      return failed("VulkanRenderThreadBackend::executeCommand",
                    "Vulkan RenderThread backend is not initialized");
    }

    std::uint64_t uploaded_bytes = 0u;
    bool success = true;
    bool command_pending = false;
    std::string failure;
    const RenderCommandKind kind = renderCommandKind(command.payload);
    switch (kind) {
    case RenderCommandKind::Resize: {
      const auto &resize = std::get<ResizeRenderCommand>(command.payload);
      backend_->resize(resize.pixel_width, resize.pixel_height);
      break;
    }
    case RenderCommandKind::SetVSync: {
      const auto &vsync = std::get<SetVSyncRenderCommand>(command.payload);
      backend_->setVSync(vsync.enabled);
      break;
    }
    case RenderCommandKind::SuspendPresentation:
      success = backend_->prepareForSystemDialog() &&
                backend_->presentationSuspended();
      if (!success) {
        failure = "Vulkan backend did not reach the Suspend safe boundary";
      }
      break;
    case RenderCommandKind::ResumePresentation:
      success = backend_->resumeAfterSystemDialog() &&
                !backend_->presentationSuspended();
      if (!success) {
        failure = "Vulkan backend remained suspended after Resume";
      }
      break;
    case RenderCommandKind::MarkLatencyPing:
      backend_->markLatencyPing();
      break;
    case RenderCommandKind::UploadFontAtlas: {
      const auto &upload =
          std::get<UploadFontAtlasRenderCommand>(command.payload);
      success = backend_->uploadFontAtlas(
          upload.pixels.data(), static_cast<int>(upload.width),
          static_cast<int>(upload.height), upload.rgba);
      if (success) {
        const std::uint64_t channels = upload.rgba ? 4u : 1u;
        uploaded_bytes = static_cast<std::uint64_t>(upload.width) *
                         upload.height * channels;
      } else {
        failure = "Vulkan font-atlas transaction failed";
      }
      break;
    }
    case RenderCommandKind::UploadStaticAsset: {
      const auto &upload =
          std::get<UploadStaticAssetRenderCommand>(command.payload);
      std::thread shutdown_request;
      if (h1_shutdown_during_upload_ &&
          !h1_upload_shutdown_injected_) {
        h1_upload_shutdown_injected_ = true;
        const std::shared_ptr<RenderThreadControl> control = control_;
        shutdown_request = std::thread([control]() {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          if (control) {
            (void)control->requestShutdown();
          }
        });
        xpbd::log::infof(
            "H1_UPLOAD_SHUTDOWN_BEGIN command_serial=%llu",
            static_cast<unsigned long long>(command.command_serial));
      }
      success = upload.source_packet &&
                backend_->uploadStaticAssetPacket(*upload.source_packet,
                                                  uploaded_bytes, true);
      command_pending =
          success && backend_->static_asset_pending_.active;
      if (shutdown_request.joinable()) {
        shutdown_request.join();
        xpbd::log::infof(
            "H1_UPLOAD_SHUTDOWN_REQUESTED command_serial=%llu "
            "upload_success=%d bytes=%llu quarantine=%d",
            static_cast<unsigned long long>(command.command_serial),
            success ? 1 : 0,
            static_cast<unsigned long long>(uploaded_bytes),
            backend_->quarantine_required_ ? 1 : 0);
      }
      if (!success) {
        failure = "Vulkan static CPU/GPU/RT transaction failed";
      }
      break;
    }
    case RenderCommandKind::UploadEnvironment: {
      const auto &upload =
          std::get<UploadEnvironmentRenderCommand>(command.payload);
      success = upload.source_packet &&
                backend_->uploadEnvironmentPacket(*upload.source_packet,
                                                  uploaded_bytes, true);
      command_pending =
          success && backend_->world_environment_pending_.active;
      if (!success) {
        failure = "Vulkan environment transaction failed";
      }
      break;
    }
    case RenderCommandKind::StartStillRender: {
      const auto &start =
          std::get<StartStillRenderCommand>(command.payload);
      if (active_still_) {
        success = false;
        failure = "A Vulkan RenderThread Still job is already active";
        break;
      }
      ActiveStillJob job;
      job.command = start;
      job.status.state = StillRenderJobState::Queued;
      job.status.job_id = start.job_id;
      job.status.target_samples = start.target_samples;
      job.status.output_path = start.output_path;
      active_still_ = std::move(job);
      break;
    }
    case RenderCommandKind::CancelStillRender: {
      const auto &cancel =
          std::get<CancelStillRenderCommand>(command.payload);
      if (active_still_ &&
          active_still_->command.job_id == cancel.job_id) {
        active_still_->cancel_requested = true;
      }
      // A late cancellation is an idempotent acknowledgement.
      break;
    }
    case RenderCommandKind::Shutdown:
      break;
    }

    if (!success) {
      return backendOutcome("RenderCommand", std::move(failure));
    }
    RenderThreadBackendOutcome outcome = backendOutcome("RenderCommand", {});
    outcome.uploaded_bytes = uploaded_bytes;
    if (command_pending && outcome.success()) {
      outcome.status = RenderThreadBackendStatus::Pending;
    }
    return outcome;
  }

  RenderThreadBackendOutcome pollCommand(
      const ReliableRenderCommand &command) override {
    if (!backend_) {
      return failed("VulkanRenderThreadBackend::pollCommand",
                    "Vulkan RenderThread backend is not initialized");
    }
    const RenderCommandKind kind = renderCommandKind(command.payload);
    if (kind != RenderCommandKind::UploadStaticAsset &&
        kind != RenderCommandKind::UploadEnvironment) {
      return failed("VulkanRenderThreadBackend::pollCommand",
                    "Only asset Candidate uploads can remain pending");
    }
    std::uint64_t uploaded_bytes = 0u;
    bool complete = false;
    bool superseded = false;
    const char *api = nullptr;
    const char *failure = nullptr;
    bool success = false;
    if (kind == RenderCommandKind::UploadStaticAsset) {
      api = "VulkanBackend::pollStaticAssetPacket";
      failure = "Vulkan static Pending Candidate failed";
      success = backend_->pollStaticAssetPacket(
          uploaded_bytes, false, complete, superseded);
    } else {
      api = "VulkanBackend::pollEnvironmentPacket";
      failure = "Vulkan HDRI Pending Candidate failed";
      const auto &upload =
          std::get<UploadEnvironmentRenderCommand>(command.payload);
      success = upload.source_packet &&
                backend_->pollEnvironmentPacket(
                    *upload.source_packet, uploaded_bytes, false,
                    complete, superseded);
    }
    if (!success) {
      return backendOutcome(api, failure);
    }
    RenderThreadBackendOutcome outcome =
        backendOutcome(api, {});
    outcome.uploaded_bytes = uploaded_bytes;
    if (superseded && outcome.success()) {
      outcome.status = RenderThreadBackendStatus::Superseded;
    } else if (!complete && outcome.success()) {
      outcome.status = RenderThreadBackendStatus::Pending;
    }
    return outcome;
  }

  RenderThreadBackendOutcome supersedeCommand(
      const ReliableRenderCommand &current,
      const ReliableRenderCommand &replacement) override {
    if (!backend_) {
      return failed("VulkanRenderThreadBackend::supersedeCommand",
                    "Vulkan RenderThread backend is not initialized");
    }
    const RenderCommandKind current_kind =
        renderCommandKind(current.payload);
    if (current_kind != renderCommandKind(replacement.payload) ||
        (current_kind != RenderCommandKind::UploadStaticAsset &&
         current_kind != RenderCommandKind::UploadEnvironment)) {
      return failed("VulkanRenderThreadBackend::supersedeCommand",
                    "Asset Supersede requires a same-kind replacement");
    }
    if (current_kind == RenderCommandKind::UploadStaticAsset) {
      const auto &upload =
          std::get<UploadStaticAssetRenderCommand>(replacement.payload);
      if (!upload.source_packet ||
          !backend_->static_asset_pending_.active) {
        return failed("VulkanRenderThreadBackend::supersedeCommand",
                      "No valid Vulkan static Candidate is pending");
      }
      backend_->static_asset_pending_.superseded = true;
      return backendOutcome(
          "VulkanRenderThreadBackend::supersedeCommand", {});
    }
    const auto &upload =
        std::get<UploadEnvironmentRenderCommand>(replacement.payload);
    if (!upload.source_packet ||
        !backend_->world_environment_pending_.active) {
      return failed("VulkanRenderThreadBackend::supersedeCommand",
                    "No valid Vulkan HDRI Candidate is pending");
    }
    backend_->world_environment_pending_.superseded = true;
    return backendOutcome("VulkanRenderThreadBackend::supersedeCommand", {});
  }

  RenderThreadBackendOutcome renderFrame(
      const RenderFramePacket &packet, std::uint64_t render_serial,
      const HistoryCommitSnapshot *previous_history) override {
    if (!backend_) {
      return failed("VulkanRenderThreadBackend::renderFrame",
                    "Vulkan RenderThread backend is not initialized");
    }
    if (control_ && control_->stopRequested()) {
      return failed("VulkanRenderThreadBackend::renderFrame",
                    "RenderThread stop requested before frame execution");
    }
    if (h1_inject_fatal_after_presents_ > 0u &&
        !h1_fatal_injected_ &&
        backend_->stats_.present_success_count >=
            h1_inject_fatal_after_presents_) {
      h1_fatal_injected_ = true;
      xpbd::log::warnf(
          "H1_FATAL_INJECTION_TRIGGER presents=%llu render_serial=%llu",
          static_cast<unsigned long long>(
              backend_->stats_.present_success_count),
          static_cast<unsigned long long>(render_serial));
      backend_->recordFatalError(
          "H1SyntheticDeviceLost",
          "H1 safe synthetic device-lost injection after successful "
          "presentation",
          static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST));
      RenderThreadBackendOutcome injected =
          backendOutcome("H1SyntheticDeviceLost", {});
      injected.result_code =
          static_cast<std::int64_t>(VK_ERROR_DEVICE_LOST);
      injected.previous_history_render_serial =
          previous_history != nullptr ? previous_history->render_serial : 0u;
      return injected;
    }

    RenderFramePacketView packet_view(packet);
    FrameInput worker_frame = packet_view.frame();
    StillRenderFrameRequest still_request{};
    if (active_still_) {
      ActiveStillJob &job = *active_still_;
      const StartStillRenderCommand &still = job.command;
      // The latest packet supplies only presentation extent and owned UI.
      // Every input that can affect the Still result comes from the immutable
      // reliable Start command, including asset references and generations.
      worker_frame.view_matrix = still.view_matrix.data();
      worker_frame.proj_matrix = still.proj_matrix.data();
      worker_frame.scene = still.scene.scene.get();
      worker_frame.static_model = still.scene.static_model.get();
      worker_frame.static_model_frame = still.scene.static_model_frame.get();
      worker_frame.static_model_texture =
          still.scene.static_model_texture.get();
      worker_frame.static_model_material =
          still.scene.static_model_material.get();
      worker_frame.static_model_generation = still.static_model_generation;
      worker_frame.static_texture_generation =
          still.static_texture_generation;
      worker_frame.material_debug_view = still.material_debug_view;
      worker_frame.rt_debug_view = still.rt_debug_view;
      worker_frame.path_trace_settings = still.path_trace_settings;
      worker_frame.world_environment = still.scene.world_environment.get();
      worker_frame.raster_scene = still.scene.raster_scene.get();
      worker_frame.clear_r = still.clear_r;
      worker_frame.clear_g = still.clear_g;
      worker_frame.clear_b = still.clear_b;
      worker_frame.prefer_ray_tracing = true;
      worker_frame.interactive_viewport_resize = false;
      worker_frame.rt_scene_generations = still.rt_scene_generations;
      worker_frame.rt_scene_generations_valid =
          still.rt_scene_generations_valid;

      still_request.job_id = still.job_id;
      still_request.width = still.width;
      still_request.height = still.height;
      still_request.target_samples = still.target_samples;
      still_request.samples_per_submit = still.samples_per_submit;
      still_request.format = still.format;
      still_request.transparent_background =
          still.transparent_background;
      still_request.cancel_requested = job.cancel_requested;
      still_request.output_path = still.output_path;
      still_request.view_matrix = still.view_matrix.data();
      still_request.proj_matrix = still.proj_matrix.data();
      still_request.path_trace_settings = still.path_trace_settings;
      still_request.material_debug_view = still.material_debug_view;
      still_request.rt_debug_view = still.rt_debug_view;
      still_request.preview_skybox = still.scene.preview_skybox.get();
      still_request.status = &job.status;
      worker_frame.still_render = &still_request;
    }
    const PathTraceSettings settings =
        normalizePathTraceSettings(worker_frame.path_trace_settings);
    backend_->setRenderThreadFrameContext(render_serial, previous_history);
    backend_->beginLatencyFrame(
        static_cast<std::uint32_t>(
            render_serial &
            (std::numeric_limits<std::uint32_t>::max)()),
        settings.requested_reflex_mode,
        settings.requested_frame_generation ==
            PathTraceFrameGeneration::On);
    backend_->endLatencySimulation();
    backend_->render(worker_frame);

    RenderThreadBackendOutcome outcome = backendOutcome("render", {});
    outcome.stats = backend_->stats_;
    outcome.previous_history_render_serial =
        previous_history != nullptr ? previous_history->render_serial : 0u;
    if (active_still_) {
      outcome.still_render_status = active_still_->status;
      if (terminalStillState(active_still_->status.state)) {
        active_still_.reset();
      }
    }
    if (outcome.success()) {
      outcome.presented = backend_->stats_.present_succeeded;
      // The supplied previous_history snapshot is authoritative. Only a
      // successfully presented adapter frame is eligible for the outer ledger.
      outcome.temporal_history_accepted = outcome.presented;
    }
    return outcome;
  }

  RenderThreadBackendShutdown shutdown() noexcept override {
    if (!backend_) {
      return RenderThreadBackendShutdown::Clean;
    }
    if (backend_->quarantine_required_ ||
        backend_->gpu_completion_unproven_) {
      return RenderThreadBackendShutdown::Quarantined;
    }
    try {
      backend_->shutdown();
    } catch (...) {
      backend_->quarantine_required_ = true;
      return RenderThreadBackendShutdown::Quarantined;
    }
    if (backend_->device_ != VK_NULL_HANDLE ||
        backend_->instance_ != VK_NULL_HANDLE ||
        backend_->quarantine_required_) {
      return RenderThreadBackendShutdown::Quarantined;
    }
    backend_.reset();
    return RenderThreadBackendShutdown::Clean;
  }

private:
  struct ActiveStillJob {
    StartStillRenderCommand command;
    StillRenderStatus status;
    bool cancel_requested = false;
  };

  [[nodiscard]] static bool
  terminalStillState(StillRenderJobState state) noexcept {
    return state == StillRenderJobState::Completed ||
           state == StillRenderJobState::Failed ||
           state == StillRenderJobState::Cancelled;
  }

  static RenderThreadBackendOutcome failed(const char *api,
                                           std::string message) {
    RenderThreadBackendOutcome outcome;
    outcome.status = RenderThreadBackendStatus::Failed;
    outcome.api = api != nullptr ? api : "VulkanRenderThreadBackend";
    outcome.message = std::move(message);
    return outcome;
  }

  RenderThreadBackendOutcome backendOutcome(const char *api,
                                            std::string fallback) const {
    if (!backend_) {
      return failed(api, std::move(fallback));
    }
    RenderThreadBackendOutcome outcome;
    outcome.api = api != nullptr ? api : "VulkanBackend";
    outcome.device_name = backend_->device_name_;
    outcome.ray_tracing_ready = backend_->supportsRayTracing();
    outcome.streamline_ready =
        backend_->streamline_vulkan_runtime_.initialized();
    outcome.ray_tracing_capability = backend_->rayTracingCapability();
    outcome.post_process_capabilities =
        backend_->pathTracePostProcessCapabilities();
    outcome.post_process_status =
        std::string(backend_->pathTracePostProcessStatus());
    outcome.latency_ping_message = backend_->latencyPingMessage();
    if (backend_->quarantine_required_ || backend_->fatal_error_) {
      outcome.status = RenderThreadBackendStatus::FatalQuarantine;
      outcome.message = backend_->fatal_error_detail_.empty()
                            ? std::move(fallback)
                            : backend_->fatal_error_detail_;
      outcome.result_code = backend_->gpu_completion_unproven_
                                ? static_cast<std::int64_t>(VK_TIMEOUT)
                                : 0;
    } else if (!fallback.empty()) {
      outcome.status = RenderThreadBackendStatus::Failed;
      outcome.message = std::move(fallback);
    }
    return outcome;
  }

  std::shared_ptr<RenderThreadControl> control_;
  std::unique_ptr<VulkanBackend> backend_;
  std::optional<ActiveStillJob> active_still_;
  std::uint64_t h1_inject_fatal_after_presents_ = 0u;
  bool h1_shutdown_during_upload_ = false;
  bool h1_upload_shutdown_injected_ = false;
  bool h1_fatal_injected_ = false;
};

} // namespace xpbd::gfx::detail

namespace xpbd::gfx {

std::unique_ptr<IRenderThreadBackend> createVulkanRenderThreadBackend() {
  return std::make_unique<detail::VulkanRenderThreadBackend>();
}

} // namespace xpbd::gfx
