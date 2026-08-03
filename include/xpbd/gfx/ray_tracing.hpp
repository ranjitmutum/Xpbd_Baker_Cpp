#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace xpbd::gfx {

// PCI vendor ID for NVIDIA Corporation.
inline constexpr std::uint32_t kVendorIdNvidia = 0x10DEu;

// Which primary visibility path the viewport uses this frame.
enum class RenderPath : std::uint8_t {
  Raster = 0,
  RayTracing = 1,
};

enum class RtAlphaMode : std::uint8_t {
  Opaque = 0,
  Cutout = 1,
  Blend = 2,
};

enum class RtDebugView : std::uint8_t {
  Off = 0,
  Instance = 1,
  Primitive = 2,
  Cube = 3,
  Face = 4,
  Material = 5,
  Normal = 6,
  Albedo = 7,
  Roughness = 8,
  Emission = 9,
  DirectLighting = 10,
  IndirectLighting = 11,
  SampleCount = 12,
  PathLength = 13,
  Firefly = 14,
};

enum class PathTracePreset : std::uint8_t {
  Realtime = 0,
  Balanced = 1,
  HighQuality = 2,
  Reference = 3,
  Custom = 4,
};

enum class PathTraceDenoiser : std::uint8_t {
  Auto = 0,
  DlssRayReconstruction = 1,
  // Values 2 and 3 were used by retired NRD modes. Keep Raw at 4 so older
  // settings migrate safely instead of selecting RR accidentally.
  Raw = 4,
};

enum class PathTraceUpscale : std::uint8_t {
  Off = 0,
  Auto = 1, // Legacy persisted value; normalized to Quality.
  Dlaa = 2,
  UltraQuality = 3, // Legacy persisted value; normalized to Quality.
  Quality = 4,
  Balanced = 5,
  Performance = 6,
  UltraPerformance = 7,
};

enum class PathTraceToneMapping : std::uint8_t {
  None = 0,
  Reinhard = 1,
  Aces = 2,
};

enum class PathTraceInteractiveQuality : std::uint8_t {
  Full = 0,
  Balanced = 1,
  Fast = 2,
};

// The path integrator has exactly three legal ways to combine BSDF endpoint
// sampling with explicit light sampling. PathTraceSettings retains the legacy
// NEE/MIS booleans for persistence and UI compatibility; normalization and the
// resolver below map them onto this closed set.
enum class PathTraceLightSamplingMode : std::uint8_t {
  BsdfOnly = 0,
  LightOnly = 1,
  Combined = 2,
};

enum class PathTraceFrameGeneration : std::uint8_t {
  Off = 0,
  On = 1,
};

enum class PathTraceReflexMode : std::uint8_t {
  Off = 0,
  On = 1,
  OnBoost = 2,
};

enum class PathTraceChangeClass : std::uint32_t {
  None = 0u,
  SamplingSchedule = 1u << 0u,
  ResetAccumulation = 1u << 1u,
  RecreateTarget = 1u << 2u,
  ReconfigurePostProcess = 1u << 3u,
  DisplayOnly = 1u << 4u,
};

[[nodiscard]] constexpr PathTraceChangeClass
operator|(PathTraceChangeClass lhs, PathTraceChangeClass rhs) noexcept {
  return static_cast<PathTraceChangeClass>(
      static_cast<std::uint32_t>(lhs) |
      static_cast<std::uint32_t>(rhs));
}

constexpr PathTraceChangeClass &
operator|=(PathTraceChangeClass &lhs, PathTraceChangeClass rhs) noexcept {
  lhs = lhs | rhs;
  return lhs;
}

[[nodiscard]] constexpr bool
hasPathTraceChange(PathTraceChangeClass value,
                   PathTraceChangeClass flag) noexcept {
  return (static_cast<std::uint32_t>(value) &
          static_cast<std::uint32_t>(flag)) != 0u;
}

// Streamline owns temporal reconstruction history independently from the raw
// path-trace sample accumulator. Ordinary camera/object motion is represented
// by dense motion vectors and must not be treated as a camera cut.
[[nodiscard]] constexpr bool shouldResetTemporalReconstructionHistory(
    bool history_valid, std::uint64_t previous_compatibility_key,
    std::uint64_t current_compatibility_key,
    bool motion_history_valid) noexcept {
  return !history_valid || !motion_history_valid ||
         previous_compatibility_key != current_compatibility_key;
}

inline constexpr float kDefaultPathTraceExposureEv = 0.0f;
inline constexpr float kPathTraceShadingNormalCorrectionLimit = 4.0f;

struct PathTraceSettings {
  PathTracePreset preset = PathTracePreset::Realtime;
  // The last non-Custom preset is retained so "Restore preset" remains useful
  // after any manual edit.
  PathTracePreset source_preset = PathTracePreset::Realtime;
  // NVIDIA-only preference for the full Vulkan RT Pipeline. Unsupported
  // hardware resolves to the retained compatibility path and the UI disables
  // this request.
  bool nvidia_rt_core_acceleration = true;
  std::uint32_t samples_per_frame = 1;
  // Zero means no user-selected limit. The uint32 sample counter still
  // saturates instead of wrapping.
  std::uint32_t maximum_samples = 512;
  std::uint32_t max_bounces = 4;
  std::uint32_t max_diffuse_bounces = 16;
  std::uint32_t max_glossy_bounces = 16;
  std::uint32_t max_transmission_bounces = 32;
  std::uint32_t max_transparent_bounces = 64;
  std::uint32_t russian_roulette_start = 3;
  bool russian_roulette = true;
  bool automatic_seed = true;
  std::uint32_t seed = 1;
  // Adaptive sampling remains an explicit capability-gated request until the
  // variance/AOV path is available.
  bool adaptive_sampling = false;
  float adaptive_noise_threshold = 0.01f;
  std::uint32_t adaptive_minimum_samples = 16;

  // Lighting/integrator controls. Environment and Sun/Moon master switches
  // remain owned by World/Sky; these control how the path tracer samples the
  // enabled sources.
  bool analytic_lights = true;
  bool emissive_surfaces = true;
  bool next_event_estimation = true;
  bool multiple_importance_sampling = true;
  bool environment_importance_sampling = true;
  bool emissive_mesh_sampling = true;
  float emissive_multiplier = 1.0f;
  std::uint32_t light_samples_per_path = 1;
  // Zero disables clamping.
  float direct_clamp = 0.0f;
  float indirect_clamp = 0.0f;

  PathTraceDenoiser requested_denoiser = PathTraceDenoiser::Raw;
  PathTraceUpscale requested_upscale = PathTraceUpscale::Off;
  // DLSS Frame Generation is a presentation feature and never participates in
  // still rendering. Reflex defaults to On per NVIDIA's integration guidance;
  // enabling FG forces the effective Reflex mode to at least On.
  PathTraceFrameGeneration requested_frame_generation =
      PathTraceFrameGeneration::Off;
  PathTraceReflexMode requested_reflex_mode =
      PathTraceReflexMode::On;

  // Film/display controls never alter the accumulated linear HDR radiance.
  bool transparent_background = false;
  std::uint64_t reset_generation = 0;
  // Sky Rendering is independently Off by default. A positive value enables
  // the temporary Phase 4 analytic safety environment.
  float analytic_environment_strength = 0.0f;
  // Display-only whole-frame exposure in photographic stops. It is applied
  // after linear HDR accumulation, so changing it preserves compatible PT
  // history. Explicitly saved user values are restored by AppSession.
  float display_exposure_ev = kDefaultPathTraceExposureEv;
  PathTraceToneMapping tone_mapping = PathTraceToneMapping::Aces;
  float white_balance_kelvin = 6500.0f;
  float bloom_strength = 0.0f;

  // Performance/interaction controls.
  float preview_resolution_scale = 1.0f;
  float target_frame_time_ms = 33.3f;
  PathTraceInteractiveQuality interactive_quality =
      PathTraceInteractiveQuality::Balanced;
  bool accumulate_while_moving = false;
  bool pause_accumulation = false;

  bool developer_controls = false;
  bool force_software_fallback = false;
  std::uint64_t target_generation = 0;
  std::uint64_t post_process_generation = 0;
  std::uint64_t display_generation = 0;
};

[[nodiscard]] PathTraceSettings
normalizePathTraceSettings(PathTraceSettings settings) noexcept;
[[nodiscard]] PathTraceLightSamplingMode resolvedPathTraceLightSamplingMode(
    const PathTraceSettings &settings) noexcept;
// Returns the BSDF-sampled environment/emissive endpoint weight. A primary or
// delta path is never suppressed, and LightOnly suppresses an endpoint only
// when the same endpoint has a non-zero explicit-light sampling PDF.
[[nodiscard]] float pathTraceLightEndpointWeight(
    PathTraceLightSamplingMode mode, bool primary_or_previous_delta,
    float bsdf_pdf, float light_pdf) noexcept;
[[nodiscard]] PathTraceSettings
pathTraceSettingsForPreset(PathTracePreset preset) noexcept;
[[nodiscard]] PathTraceSettings
applyPathTracePreset(const PathTraceSettings &current,
                     PathTracePreset preset) noexcept;
[[nodiscard]] PathTraceSettings
restorePathTraceSourcePreset(const PathTraceSettings &current) noexcept;
[[nodiscard]] PathTraceChangeClass classifyPathTraceSettingsChange(
    const PathTraceSettings &before,
    const PathTraceSettings &after) noexcept;
[[nodiscard]] std::uint32_t
resolvedPathTraceSeed(const PathTraceSettings &settings) noexcept;

struct PathTracePostProcessCapabilities {
  bool dlss_ray_reconstruction = false;
  bool dlss_super_resolution = false;
  bool dlaa = false;
  bool dlss_frame_generation = false;
  bool reflex = false;
};

struct PathTracePostProcessState {
  PathTraceDenoiser requested_denoiser = PathTraceDenoiser::Raw;
  PathTraceDenoiser active_denoiser = PathTraceDenoiser::Raw;
  PathTraceUpscale requested_upscale = PathTraceUpscale::Off;
  PathTraceUpscale active_upscale = PathTraceUpscale::Off;
  // RR uses this quality tier internally while suppressing separate SR.
  PathTraceUpscale reconstruction_mode = PathTraceUpscale::Off;
  bool denoiser_supported = true;
  bool upscale_supported = true;
  bool conflict_resolved = false;
  bool rr_mode_required = false;
};

[[nodiscard]] PathTracePostProcessState resolvePathTracePostProcess(
    const PathTraceSettings &settings,
    const PathTracePostProcessCapabilities &capabilities) noexcept;

struct PathTraceRenderSnapshot {
  PathTraceSettings settings{};
  std::uint64_t source_generation = 0;
};

[[nodiscard]] PathTraceRenderSnapshot
makePathTraceRenderSnapshot(const PathTraceSettings &settings) noexcept;

// Integer-only seed expansion shared by the CPU regression reference and the
// RayGen shader. The result is stable for a fixed pixel/sample/dimension/seed.
[[nodiscard]] std::uint32_t
pathTraceRandomBits(std::uint32_t pixel_x, std::uint32_t pixel_y,
                    std::uint32_t sample_index, std::uint32_t dimension,
                    std::uint32_t seed) noexcept;
[[nodiscard]] float
pathTraceRandom01(std::uint32_t pixel_x, std::uint32_t pixel_y,
                  std::uint32_t sample_index, std::uint32_t dimension,
                  std::uint32_t seed) noexcept;

// NVIDIA-style Halton(2,3) camera jitter for temporal reconstruction. Values
// are pixel offsets from the pixel center in [-0.5, 0.5]. The sequence period
// follows round(8 * (output_width / render_width)^2).
[[nodiscard]] std::array<float, 2>
pathTraceTemporalJitter(std::uint32_t frame_index,
                        std::uint32_t render_width,
                        std::uint32_t output_width) noexcept;

struct PathTraceHemisphereSample {
  std::array<float, 3> direction{0.0f, 1.0f, 0.0f};
  bool used_fallback = false;
};

[[nodiscard]] PathTraceHemisphereSample
samplePathTraceCosineHemisphere(const std::array<float, 3> &normal,
                                float sample_u, float sample_v) noexcept;

enum class PathTraceLobe : std::uint8_t {
  Diffuse = 0,
  Glossy = 1,
  Transmission = 2,
  Transparent = 3,
};

struct PathTraceDepthState {
  std::uint32_t total = 0;
  std::uint32_t diffuse = 0;
  std::uint32_t glossy = 0;
  std::uint32_t transmission = 0;
  std::uint32_t transparent = 0;
};

[[nodiscard]] bool pathTraceBounceAllowed(
    const PathTraceSettings &settings, const PathTraceDepthState &state,
    PathTraceLobe lobe) noexcept;
[[nodiscard]] std::optional<PathTraceDepthState> advancePathTraceDepth(
    const PathTraceSettings &settings, const PathTraceDepthState &state,
    PathTraceLobe lobe) noexcept;

struct PathTraceRussianRouletteStep {
  float continuation_probability = 1.0f;
  float throughput_scale = 1.0f;
  bool applied = false;
  bool survives = true;
};

// Evaluates RR after a completed bounce. throughput_max is the maximum
// non-negative RGB component and sample_u must be in [0,1) for deterministic
// CPU/GPU agreement.
[[nodiscard]] PathTraceRussianRouletteStep
evaluatePathTraceRussianRoulette(
    const PathTraceSettings &settings, const PathTraceDepthState &state,
    float throughput_max, float sample_u) noexcept;

struct RtBsdfMaterial {
  std::array<float, 3> base_color{1.0f, 1.0f, 1.0f};
  std::array<float, 3> f0{0.04f, 0.04f, 0.04f};
  // GGX alpha. LabPBR's perceptual roughness is squared before reaching this
  // read-only resolved material representation.
  float roughness = 0.25f;
  // Phase 5 compatibility bridge: only Blend materials derive this from
  // 1-opacity. Full layered transparency remains Phase 7.
  float transmission = 0.0f;
  float ior = 1.5f;
  bool metal = false;
};

struct RtBsdfLobeProbabilities {
  float diffuse = 0.0f;
  float glossy = 1.0f;
  float transmission = 0.0f;
};

struct RtBsdfEval {
  std::array<float, 3> value{};
  float pdf = 0.0f;
  bool valid = false;
};

struct RtBsdfSample {
  std::array<float, 3> direction{0.0f, 1.0f, 0.0f};
  std::array<float, 3> value{};
  std::array<float, 3> weight{};
  float pdf = 0.0f;
  PathTraceLobe lobe = PathTraceLobe::Diffuse;
  bool delta = false;
  bool total_internal_reflection = false;
  bool valid = false;
};

[[nodiscard]] float rtDielectricF0FromIor(float ior) noexcept;
[[nodiscard]] float rtDielectricIorFromF0(float f0) noexcept;
[[nodiscard]] float rtFresnelDielectric(float cosine_incident,
                                        float eta_incident,
                                        float eta_transmitted) noexcept;
[[nodiscard]] std::array<float, 3> rtFresnelSchlick(
    const std::array<float, 3> &f0, float cosine) noexcept;
[[nodiscard]] float rtGgxDistribution(float normal_dot_half,
                                      float roughness) noexcept;
[[nodiscard]] float rtSmithGgxG1(float normal_dot_direction,
                                 float roughness) noexcept;
[[nodiscard]] RtBsdfLobeProbabilities
rtBsdfLobeProbabilities(const RtBsdfMaterial &material) noexcept;

// All directions point away from the surface. Reflection eval is continuous;
// ideal refraction is returned as a delta sample by sampleRtBsdf.
[[nodiscard]] RtBsdfEval evaluateRtBsdf(
    const RtBsdfMaterial &material,
    const std::array<float, 3> &shading_normal,
    const std::array<float, 3> &view_direction,
    const std::array<float, 3> &light_direction) noexcept;
[[nodiscard]] RtBsdfSample sampleRtBsdf(
    const RtBsdfMaterial &material,
    const std::array<float, 3> &shading_normal,
    const std::array<float, 3> &view_direction, bool front_face,
    float lobe_sample, float direction_sample_u,
    float direction_sample_v) noexcept;

// Veach-style radiance correction for a shading normal. Returns a bounded,
// finite factor and rejects directions that cross only one of the geometric
// and shading-normal hemispheres.
[[nodiscard]] float rtShadingNormalCorrection(
    const std::array<float, 3> &geometric_normal,
    const std::array<float, 3> &shading_normal,
    const std::array<float, 3> &view_direction,
    const std::array<float, 3> &light_direction) noexcept;

struct PathTraceAccumulationRequest {
  std::uint64_t history_key = 0;
  std::uint64_t previous_history_key = 0;
  std::uint32_t accumulated_samples = 0;
  PathTraceSettings settings{};
  bool history_valid = false;
};

struct PathTraceAccumulationStep {
  std::uint64_t history_key = 0;
  std::uint32_t sample_base = 0;
  std::uint32_t dispatch_samples = 0;
  std::uint32_t accumulated_samples_after_dispatch = 0;
  bool history_reset = false;
  bool maximum_reached = false;
};

// Computes one slot-local accumulation transition. samples_per_frame and
// maximum_samples do not participate in history compatibility; callers build
// history_key only from radiance-affecting inputs.
[[nodiscard]] PathTraceAccumulationStep
advancePathTraceAccumulation(
    const PathTraceAccumulationRequest &request) noexcept;

[[nodiscard]] RtDebugView
rtDebugViewFromName(std::string_view name) noexcept;
[[nodiscard]] const char *rtDebugViewName(RtDebugView view) noexcept;

struct RtSbtLayoutRequest {
  std::uint32_t shader_group_handle_size = 0;
  std::uint32_t shader_group_handle_alignment = 0;
  std::uint32_t shader_group_base_alignment = 0;
  std::uint32_t max_shader_group_stride = 0;
  std::uint32_t miss_group_count = 0;
  std::uint32_t hit_group_count = 0;
  std::uint64_t buffer_device_address = 0;
  std::uint64_t buffer_bytes = 0;
};

struct RtSbtLayout {
  std::uint32_t shader_group_stride = 0;
  std::uint64_t base_offset = 0;
  std::uint64_t raygen_offset = 0;
  std::uint64_t miss_offset = 0;
  std::uint64_t hit_offset = 0;
  // Bytes occupied from the aligned base address through the final record.
  std::uint64_t layout_bytes = 0;
};

// Computes the one-RayGen/many-Miss/many-Hit SBT layout and rejects invalid
// power-of-two alignments, excessive stride, address overflow, or undersized
// allocation. Offsets are relative to the aligned SBT base except base_offset.
[[nodiscard]] std::optional<RtSbtLayout>
computeRtSbtLayout(const RtSbtLayoutRequest &request) noexcept;

struct RtDispatchBufferBounds {
  std::uint64_t vertex_count = 0;
  std::uint64_t primitive_count = 0;
  std::uint64_t instance_count = 0;
  std::uint64_t normal_bytes = 0;
  std::uint64_t tangent_bytes = 0;
  std::uint64_t index_bytes = 0;
  std::uint64_t uv_bytes = 0;
  std::uint64_t color_bytes = 0;
  std::uint64_t primitive_flag_bytes = 0;
  std::uint64_t primitive_metadata_bytes = 0;
  std::uint64_t instance_metadata_bytes = 0;
};

// Validates every buffer range indexed by the Phase 3/5 Closest/Any Hit
// shaders.
[[nodiscard]] bool
rtDispatchBuffersInBounds(const RtDispatchBufferBounds &bounds) noexcept;

inline constexpr float kRtAlphaCutoff = 0.02f;

[[nodiscard]] constexpr float
rtAcceptedOpacity(RtAlphaMode mode, float alpha,
                  float cutoff = kRtAlphaCutoff) noexcept {
  if (!(alpha >= cutoff)) {
    return 0.0f;
  }
  if (mode != RtAlphaMode::Blend) {
    return 1.0f;
  }
  return alpha > 1.0f ? 1.0f : alpha;
}

struct RtFrontToBackAccumulator {
  float premultiplied_r = 0.0f;
  float premultiplied_g = 0.0f;
  float premultiplied_b = 0.0f;
  float remaining = 1.0f;

  [[nodiscard]] constexpr float alpha() const noexcept {
    return 1.0f - remaining;
  }

  // Returns true when the accumulated result is effectively opaque.
  constexpr bool add(float r, float g, float b, float source_alpha,
                     RtAlphaMode mode,
                     float cutoff = kRtAlphaCutoff) noexcept {
    const float opacity = rtAcceptedOpacity(mode, source_alpha, cutoff);
    premultiplied_r += remaining * opacity * r;
    premultiplied_g += remaining * opacity * g;
    premultiplied_b += remaining * opacity * b;
    remaining *= 1.0f - opacity;
    return remaining <= 0.01f;
  }
};

[[nodiscard]] constexpr float
rtShadowVisibilityAfter(float visibility, float source_alpha,
                        RtAlphaMode mode,
                        float cutoff = kRtAlphaCutoff) noexcept {
  const float clamped_visibility =
      visibility < 0.0f ? 0.0f : (visibility > 1.0f ? 1.0f : visibility);
  return clamped_visibility *
         (1.0f - rtAcceptedOpacity(mode, source_alpha, cutoff));
}

struct RtRayTriangleHit {
  float distance = 0.0f;
  // Vulkan-style barycentrics for vertices 1 and 2. Vertex 0 weight is 1-x-y.
  std::array<float, 2> barycentrics{};
};

// Deterministic two-sided CPU reference for validating the triangle AS
// contract. The production Vulkan query remains the runtime intersection path.
[[nodiscard]] std::optional<RtRayTriangleHit>
intersectRtTriangleTwoSided(
    const std::array<float, 3> &origin,
    const std::array<float, 3> &direction,
    const std::array<float, 3> &vertex0,
    const std::array<float, 3> &vertex1,
    const std::array<float, 3> &vertex2, float t_min,
    float t_max) noexcept;

struct RtHitCandidate {
  float distance = 0.0f;
  std::uint32_t primitive_identity = 0;
  RtAlphaMode alpha_mode = RtAlphaMode::Opaque;
  float alpha = 1.0f;
};

struct RtNearestValidHit {
  float distance = 0.0f;
  std::uint32_t primitive_identity = 0;
  float accepted_opacity = 1.0f;
};

[[nodiscard]] std::optional<RtNearestValidHit>
selectRtNearestValidHit(
    std::span<const RtHitCandidate> candidates, float t_min,
    float t_max, float cutoff = kRtAlphaCutoff) noexcept;

struct RtMotionProjectionInput {
  // Both UVs use the Vulkan framebuffer convention used by the path tracer:
  // (0,0) is the viewport's top-left corner.
  std::array<float, 2> current_uv{0.5f, 0.5f};
  // Previous-frame homogeneous clip position after previousViewProj.
  std::array<float, 4> previous_clip{0.0f, 0.0f, 0.0f, 1.0f};
  std::uint32_t viewport_width = 1;
  std::uint32_t viewport_height = 1;
  bool camera_history_valid = false;
  bool geometry_history_valid = false;
};

struct RtMotionProjectionResult {
  // Pixel-space vector from the current sample to its previous-frame sample.
  std::array<float, 2> current_to_previous_pixels{};
  float disocclusion = 1.0f;
  bool valid = false;
};

// CPU reference for the motion/disocclusion contract shared by the compute and
// RT-pipeline shaders. Invalid history, non-finite/behind-camera clip values,
// and previous samples outside the viewport are explicitly disoccluded.
[[nodiscard]] RtMotionProjectionResult
evaluateRtMotionProjection(const RtMotionProjectionInput &input) noexcept;

// Hardware / driver capability for NVIDIA hardware ray tracing via Vulkan RT
// extensions (acceleration structures + ray query / pipelines). The built-in
// primary path prefers the Vulkan RT Pipeline and retains Ray Query as its
// compatibility fallback.
struct RayTracingCapability {
  bool is_nvidia = false;
  // True only for NVIDIA RTX 20-series (Turing) and newer generations.
  // Older NVIDIA adapters must not expose the path-tracing renderer even if a
  // driver advertises a partial Vulkan RT extension set.
  bool is_rtx20_or_newer = false;
  bool has_required_extensions = false;
  bool has_required_features = false;
  // True only when the GPU is NVIDIA and exposes usable RT extensions+features.
  bool supported = false;
  // Device created with RT extensions/features enabled (may be true when supported).
  bool device_extensions_enabled = false;

  std::uint32_t vendor_id = 0;
  std::uint32_t device_id = 0;
  std::uint32_t api_version = 0;
  std::uint32_t driver_version = 0;
  std::uint32_t max_ray_recursion_depth = 0;
  std::string device_name;
  // Human-readable reason when supported == false (for UI / logs).
  std::string unsupported_reason;
};

enum class VulkanPathTraceImplementation : std::uint8_t {
  None = 0,
  RayQuery = 1,
  RayTracingPipeline = 2,
};

struct VulkanPathTraceAvailability {
  bool path_tracer_ready = false;
  bool ray_tracing_pipeline_ready = false;
};

// Updated by the Vulkan backend after path-tracer pipeline creation and
// teardown. This reports only built-in runtime capability; no vendor source
// tree discovery participates in renderer selection.
void setVulkanPathTraceAvailability(bool path_tracer_ready,
                                    bool ray_tracing_pipeline_ready) noexcept;
[[nodiscard]] VulkanPathTraceAvailability
queryVulkanPathTraceAvailability() noexcept;

[[nodiscard]] VulkanPathTraceImplementation
selectVulkanPathTraceImplementation(
    bool user_wants_rt, const RayTracingCapability &hardware,
    const VulkanPathTraceAvailability &availability) noexcept;

[[nodiscard]] const char *vulkanPathTraceImplementationName(
    VulkanPathTraceImplementation implementation) noexcept;

// Pure helpers (unit-tested). Inputs describe a probed physical device.
[[nodiscard]] bool isNvidiaVendorId(std::uint32_t vendor_id) noexcept;
[[nodiscard]] bool isNvidiaRtx20OrNewer(
    std::uint32_t device_id, std::string_view device_name) noexcept;

// Evaluate whether RT should be advertised. Requires NVIDIA + extensions + features.
[[nodiscard]] RayTracingCapability evaluateRayTracingCapability(
    std::uint32_t vendor_id, std::uint32_t device_id, std::string_view device_name,
    bool has_required_extensions, bool has_required_features,
    std::uint32_t max_ray_recursion_depth = 0,
    std::uint32_t api_version = 0, std::uint32_t driver_version = 0);

// User preference is honored only when capability.supported. Otherwise raster.
[[nodiscard]] RenderPath
resolveRenderPath(bool user_wants_ray_tracing,
                  const RayTracingCapability &capability) noexcept;

[[nodiscard]] const char *renderPathName(RenderPath path) noexcept;

// Force-clear a persisted user preference when hardware cannot do RT.
[[nodiscard]] bool clampRayTracingPreference(bool user_wants_ray_tracing,
                                             bool hardware_supported) noexcept;

} // namespace xpbd::gfx
