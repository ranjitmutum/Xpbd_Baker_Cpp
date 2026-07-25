#pragma once

#include "xpbd/baker/baked_preview_sampler.hpp"
#include "xpbd/baker/bone_mapper.hpp"
#include "xpbd/baker/physics_baker.hpp"
#include "xpbd/baker/transition_bake_request.hpp"
#include "xpbd/gfx/texture_image.hpp"
#include "xpbd/loader/animation_loader.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/loader/model_loader.hpp"
#include "xpbd/loader/molang_keyframe_detector.hpp"
#include "xpbd/render/skeleton_viewport.hpp"
#include "xpbd/render/viewport_camera.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace xpbd::app {

enum class PresentationMode { SourcePreview, LiveSimulation, FinalBakedPreview };
enum class BakeState {
  Idle,
  Initialized,
  Running,
  AwaitingFinalize,
  Completed,
  Invalid,
  Cancelling,
  Cancelled,
  Failed
};
enum class PlaybackState { Paused, Playing };
enum class WorkerPhase {
  Preparing,
  Simulating,
  Finalizing,
  Auditing,
  Committing,
  Finished
};
enum class InvalidationReason {
  GlobalConfig,
  PerBoneDraft,
  PerBoneApplied,
  PhysicsBones,
  CollisionRoots,
  Animation,
  LoopOrSeam,
  Transition,
  Solver,
  Model,
  Cancel,
  Failure,
  Reset,
  Other
};

struct SessionFingerprint {
  std::uint64_t value = 0;
  friend bool operator==(const SessionFingerprint &,
                         const SessionFingerprint &) = default;
  [[nodiscard]] std::string hex() const;
};

struct BakeTimingConfig {
  int output_fps = 60;
  [[nodiscard]] double nominalOutputDt() const;
};

struct EffectiveConfigValue {
  std::string name;
  std::string ui_value;
  std::string committed_value;
  std::string effective_value;
  std::string source;
  std::string reason;
};

struct EffectiveConfigSnapshot {
  std::vector<EffectiveConfigValue> global;
  std::map<std::string, std::vector<EffectiveConfigValue>> per_bone;
};

struct TimingStats {
  int output_fps = 60;
  double nominal_output_dt = 1.0 / 60.0;
  double effective_output_dt = 1.0 / 60.0;
  int resumed_from_step = 0;
  int total_steps = 0;
  int completed_steps = 0;
};

struct RuntimeCollisionStats {
  int current_contact_count = 0;
  int maximum_contact_count = 0;
  double maximum_penetration = 0.0;
};

struct VelocityStats {
  bool available = false;
  double maximum_linear_speed = 0.0;
  std::string maximum_speed_bone;
  int maximum_speed_frame = -1;
  double maximum_frame_velocity_jump = 0.0;
  std::string maximum_jump_bone;
  int maximum_jump_frame = -1;
};

struct JointEulerSingularityStats {
  std::string parent_bone;
  std::string child_bone;
  std::array<double, 4> relative_rotation_xyzw{0.0, 0.0, 0.0, 1.0};
  std::string rotation_order = "XYZ";
};

struct JointStats {
  int unsafe_final_count = 0;
  double maximum_anchor_separation = 0.0;
  double maximum_angular_excess_radians = 0.0;
  std::string worst_linear_parent;
  std::string worst_linear_child;
  std::string worst_angular_parent;
  std::string worst_angular_child;
  int worst_angular_axis = -1;
  std::optional<JointEulerSingularityStats> euler_singularity;
};

struct LoopBoundaryBodyDiagnostics {
  std::string bone_name;
  std::array<double, 3> pivot_position{};
  bool has_pivot_position = false;
  std::array<double, 4> pivot_rotation_xyzw{0.0, 0.0, 0.0, 1.0};
  bool has_pivot_rotation = false;
  std::array<double, 3> pivot_linear_velocity{};
  bool has_pivot_linear_velocity = false;
  std::array<double, 3> com_position{};
  bool has_com_position = false;
  std::array<double, 4> com_rotation_xyzw{0.0, 0.0, 0.0, 1.0};
  bool has_com_rotation = false;
  std::array<double, 3> com_linear_velocity{};
  bool has_com_linear_velocity = false;
  std::array<double, 3> angular_velocity{};
  bool has_angular_velocity = false;
};

struct LoopBoundaryContactDiagnostics {
  std::string pair;
  bool meaningful_penetration = false;
  double penetration = 0.0;
  int penetration_bucket = 0;
};

struct LoopBoundaryDiagnostics {
  double sample_time = 0.0;
  bool has_sample_time = false;
  double maximum_penetration = 0.0;
  bool has_maximum_penetration = false;
  double maximum_penetration_time = -1.0;
  bool has_maximum_penetration_time = false;
  std::vector<LoopBoundaryBodyDiagnostics> bodies;
  std::vector<LoopBoundaryContactDiagnostics> contacts;
};

struct LoopCycleDiagnostics {
  int cycle = 0;
  bool valid = false;
  bool collision_safe = false;
  bool within_tolerances = false;
  double score = 0.0;
  double pose_error = 0.0;
  double velocity_error = 0.0;
  int contact_difference_count = 0;
  double maximum_penetration = 0.0;
  double maximum_penetration_time = -1.0;
  bool selected = false;
  std::string invalid_reason;
  std::vector<std::string> rejection_reasons;
  LoopBoundaryDiagnostics start_boundary;
  LoopBoundaryDiagnostics end_boundary;
};

struct LoopSeamWindowDiagnostics {
  double window_duration_seconds = 0.0;
  double window_ratio = 0.0;
  double window_start_time = 0.0;
  bool corrected = false;
  bool valid = false;
  bool c0_pass = false;
  bool c1_pass = false;
  bool c2_pass = false;
  bool driver_pass = false;
  bool driver_c0_pass = false;
  bool driver_c1_pass = false;
  bool driver_c2_pass = false;
  bool validation_pass = false;
  bool physics_seam_pass = false;
  bool driver_seam_pass = false;
  bool quantization_pass = false;
  bool collision_pass = false;
  bool joint_pass = false;
  bool export_pass = false;
  bool collision_safe = false;
  bool joint_safe = false;
  bool accepted = false;
  bool best_preview = false;
  bool best_safe_export = false;
  bool selected_for_output = false;
  double score = 0.0;
  double maximum_penetration = 0.0;
  double maximum_penetration_time = -1.0;
  double joint_failure_time = -1.0;
  double interpolation_failure_time = -1.0;
  int interpolated_sample_count = 0;
  int canonicalized_bone_count = 0;
  int preserved_driver_bone_count = 0;
  int driver_endpoint_conflict_count = 0;
  std::string invalid_item;
  std::vector<std::string> rejection_reasons;
};

struct LoopAnchorCoverageDiagnostics {
  std::string chain_root;
  std::string fixed_anchor;
  std::size_t expected_bone_count = 0;
  std::size_t measured_bone_count = 0;
  bool complete = false;
};

struct LoopDangerMarker {
  std::string kind;
  double time = -1.0;
  std::string item;
};

struct LoopDiagnostics {
  std::string source_policy;
  std::string physical_state = "Not Applicable";
  std::string seam_state = "Not Applicable";
  std::string physics_seam_state = "Not Applicable";
  std::string driver_state = "Not Applicable";
  std::string quantization_state = "Not Applicable";
  std::string collision_state = "Not Applicable";
  std::string joint_state = "Not Applicable";
  std::string export_state = "Blocked";
  bool converged = false;
  bool fallback_used = false;
  bool seam_correction_rejected = false;
  int completed_cycles = 0;
  std::optional<int> selected_cycle;
  std::optional<double> best_cycle_score;
  std::string seam_strategy;
  double maximum_position_error = 0.0;
  std::string position_bone;
  double maximum_rotation_error_radians = 0.0;
  std::string rotation_bone;
  double maximum_linear_velocity_error = 0.0;
  std::string linear_velocity_bone;
  double maximum_angular_velocity_error = 0.0;
  std::string angular_velocity_bone;
  bool contact_set_changed = false;
  int contact_difference_count = 0;
  int contact_pair_added_count = 0;
  int contact_pair_removed_count = 0;
  int contact_state_changed_count = 0;
  int meaningful_penetration_changed_count = 0;
  bool cycle_valid = true;
  std::string cycle_validation_state = "Valid";
  std::string invalid_numeric_bone;
  std::string invalid_numeric_field;
  double cycle_maximum_penetration = 0.0;
  double cycle_maximum_penetration_time = -1.0;
  LoopBoundaryDiagnostics start_boundary;
  LoopBoundaryDiagnostics end_boundary;
  bool seam_corrected = false;
  double configured_seam_window_ratio = 0.0;
  double configured_seam_window_seconds = 0.0;
  double effective_seam_window_seconds = 0.0;
  double effective_seam_window_ratio = 0.0;
  double seam_position_error = 0.0;
  double seam_rotation_error_radians = 0.0;
  double seam_linear_velocity_jump = 0.0;
  double seam_angular_velocity_jump = 0.0;
  double seam_linear_acceleration_jump = 0.0;
  double seam_angular_acceleration_jump = 0.0;
  bool seam_valid = true;
  bool seam_sample_count_sufficient = false;
  int seam_verified_continuity_order = -1;
  bool physics_relative_available = false;
  std::string physics_relative_fallback_reason;
  std::string missing_bone;
  std::string affected_metric_space;
  bool driver_available = false;
  bool driver_safe = true;
  bool seam_physics_safe = true;
  bool seam_quantization_safe = true;
  bool seam_export_safe = true;
  std::string worst_driver_bone;
  double driver_position_error = 0.0;
  double driver_rotation_error_radians = 0.0;
  double driver_linear_velocity_jump = 0.0;
  double driver_angular_velocity_jump = 0.0;
  bool seam_collision_safe = true;
  bool seam_joint_safe = true;
  double seam_maximum_penetration = 0.0;
  double configured_loop_seam_penetration_limit = 0.0;
  std::vector<LoopCycleDiagnostics> cycle_candidates;
  std::vector<LoopSeamWindowDiagnostics> seam_windows;
  std::vector<LoopAnchorCoverageDiagnostics> anchor_coverage;
  std::vector<LoopDangerMarker> danger_markers;
};

struct FinalAuditStats {
  bool collision_safe = true;
  int unsafe_collision_count = 0;
  double maximum_penetration = 0.0;
  std::optional<std::pair<std::string, std::string>> worst_collision_pair;
  bool joint_safe = true;
  int unsafe_joint_count = 0;
  bool numerical_safe = true;
};

struct DeterminismSignature {
  std::string forcing_algorithm_version = "stable-sine-v1";
  std::string stable_name_hash_version =
      "java-utf16-string-hashcode-v1";
  int bullet_solver_seed = 0;
  int bullet_version = 0;
  std::string simulation_mode;
  SessionFingerprint timing_fingerprint{};
  SessionFingerprint config_fingerprint{};
  SessionFingerprint content_fingerprint{};
};

enum class ExportBlockCode {
  NotFinalized,
  StaleResult,
  SeamRejected,
  CollisionUnsafe,
  JointUnsafe,
  NumericalFailure,
  UnsupportedInput,
  TransitionReference,
  Cancelled,
  Failed,
  LoopValidationFailed,
  PhysicsSeamUnsafe,
  DriverSeamUnsafe,
  QuantizationUnsafe,
  LoopCollisionUnsafe,
  LoopJointUnsafe
};

struct ExportBlockReason {
  ExportBlockCode code = ExportBlockCode::NotFinalized;
  std::string detail;
};

struct ExportPreflight {
  bool finalized = false;
  bool animation_allowed = false;
  bool velocity_allowed = false;
  std::vector<ExportBlockReason> block_reasons;
};

struct BakeDiagnostics {
  SessionFingerprint fingerprint{};
  bool bullet_safety_applicable = false;
  TimingStats timing{};
  std::optional<rigidbody::CollisionDetectionSnapshot> initial_collision;
  RuntimeCollisionStats runtime_collision{};
  VelocityStats velocity{};
  std::optional<rigidbody::FixedSubstepStats> substeps;
  std::optional<rigidbody::KinematicHistoryStats> kinematic_history;
  std::optional<rigidbody::JointPreflightDiagnostics> joint_preflight;
  std::optional<rigidbody::ColliderPreflightDiagnostics> collider_preflight;
  std::optional<rigidbody::JointSpringDiagnostics> joint_spring;
  std::optional<rigidbody::RigidBodyRuntimeFingerprint> runtime_fingerprint;
  std::optional<rigidbody::RigidBodyStepTrace> step_trace;
  JointStats joints{};
  LoopDiagnostics loop{};
  FinalAuditStats final_audit{};
  DeterminismSignature determinism{};
  EffectiveConfigSnapshot effective_config{};
  WorkerPhase terminal_phase = WorkerPhase::Finished;
  std::string chain_stability = "Unknown";
};

struct LiveSimulationFrame {
  SessionFingerprint fingerprint{};
  int current_step = 0;
  int total_steps = 0;
  double sample_time = 0.0;
  double output_frame_interval = 0.0;
  baker::BakedFrame frame;
  std::shared_ptr<const loader::Animation> reference_animation;
  double reference_time = 0.0;
  RuntimeCollisionStats collision{};
  std::optional<rigidbody::FixedSubstepStats> substeps;
};

struct BakeJobInput {
  std::uint64_t generation = 0;
  std::uint64_t model_generation = 0;
  std::uint64_t animation_generation = 0;
  SessionFingerprint fingerprint{};
  baker::BoneMapper mapper;
  loader::Animation source_animation;
  std::string source_animation_name;
  std::optional<loader::Animation> transition_target_animation;
  std::string transition_target_animation_name;
  int transition_mode = 0;
  double transition_duration = 0.0;
  double transition_source_exit = 0.0;
  double transition_target_entry = 0.0;
  std::map<std::string, double> transition_follow_weights;
  BakeTimingConfig timing{};
  bool allow_input_molang_zero = false;
  bool allow_selected_molang_zero = false;
};

struct BakeJobResult {
  std::uint64_t generation = 0;
  SessionFingerprint fingerprint{};
  BakeState terminal_state = BakeState::Failed;
  std::shared_ptr<const std::vector<baker::BakedFrame>> frames;
  BakeDiagnostics diagnostics{};
  ExportPreflight export_preflight{};
  std::string error;
};

struct BakeExecutionState;
struct BakeWorkerMailbox;


// 桌面应用共享状态：连接 UI、后台烘焙任务、预览资源与用户配置。
class AppSession {
public:
  static AppSession &instance();
  ~AppSession();


  std::string status = "No model loaded";
  std::string model_path;
  std::string animation_path;
  std::string last_error;

  loader::Geometry geometry;
  loader::AnimationRoot animation_root;
  std::string selected_animation_name;
  const loader::Animation *selected_animation = nullptr;
  std::string selected_bone_name;


  baker::BoneMapper bone_mapper;


  std::atomic<bool> bake_busy{false};
  std::atomic<int> bake_current{0};
  std::atomic<int> bake_total{0};
  std::string bake_message;
  PresentationMode presentation_mode = PresentationMode::SourcePreview;
  BakeState bake_state = BakeState::Idle;
  PlaybackState playback_state = PlaybackState::Paused;
  WorkerPhase worker_phase = WorkerPhase::Finished;


  bool molang_confirmation_pending = false;
  std::vector<loader::MolangBakeWarning> pending_molang_warnings;

  bool force_export_confirmation_pending = false;


  int solver_mode = 0;
  float gravity_y = -9.8f;
  float compliance = 1e-6f;
  float damping = 1e-5f;
  float air_drag = 2.0f;
  float turbulence = 1.5f;
  float wind_speed = 6.0f;
  float wind_dir = 20.0f;
  float wind_elev = 20.0f;
  float pull = 0.0f;
  int solver_iters = 8;
  int loop_mode = 0;
  bool enable_ground = false;
  bool enable_angle = true;
  bool enable_real_gravity = false;
  float max_bend = 75.0f;
  float collision_skin = 0.1f;
  int rigid_substeps = 2;
  int output_fps = 60;
  int output_timeline_mode = 0;
  float unit_scale = 0.0625f;
  int loop_seam_strategy = 0;



  int transition_mode = 0;
  std::string transition_target_animation_name;
  float transition_duration = 0.25f;
  float transition_source_exit = 0.0f;
  float transition_target_entry = 0.0f;


  float particle_mass = 1.0f;
  float bend_compliance = 1e-5f;
  float xpbd_restitution = 0.0f;
  bool use_wind_components = false;
  float wind_x = 0.0f;
  float wind_y = 0.0f;
  float wind_z = 0.0f;
  float movement_speed = 0.0f;
  float movement_dir = 0.0f;
  float movement_elev = 0.0f;
  float loop_seam_window_ratio = 0.25f;
  float rb_joint_stiffness = 12.0f;
  float rb_joint_damping = 0.8f;
  float rb_linear_damping = 0.02f;
  float rb_angular_damping = 0.05f;
  float rb_friction = 0.5f;
  float rb_restitution = 0.0f;
  bool rb_ccd = true;
  float rb_max_safe_pen = 0.2f;
  float rb_max_bend_x = 75.0f;
  float rb_max_bend_y = 75.0f;
  float rb_max_bend_z = 75.0f;


  bool bone_ov_mass = false;
  bool bone_ov_compliance = false;
  bool bone_ov_damping = false;
  bool bone_ov_max_bend = false;
  bool bone_ov_bend_compliance = false;
  bool bone_ov_rb_bend_x = false;
  bool bone_ov_rb_bend_y = false;
  bool bone_ov_rb_bend_z = false;
  bool bone_ov_pull = false;
  bool bone_ov_transition_follow = false;
  bool bone_ov_gravity = false;
  bool bone_ov_wind = false;
  bool bone_ov_turbulence = false;
  bool bone_ov_fixed = false;
  bool bone_fixed = false;
  float bone_mass = 1.0f;
  float bone_compliance = 1e-6f;
  float bone_damping = 1e-5f;
  float bone_max_bend = 75.0f;
  float bone_bend_compliance = 1e-5f;
  float bone_rb_bend_x = 75.0f;
  float bone_rb_bend_y = 75.0f;
  float bone_rb_bend_z = 75.0f;
  float bone_pull = 0.0f;
  float bone_transition_follow = 1.0f;
  float bone_gravity_scale = 1.0f;
  float bone_wind = 1.0f;
  float bone_turbulence = 1.0f;
  std::map<std::string, double> transition_follow_weights;
  bool per_bone_draft_dirty = false;
  std::uint64_t per_bone_edit_revision = 0;



  render::ViewportCamera camera;
  render::SkeletonViewport skeleton_view;
  int preview_frame_index = 0;
  double preview_time = 0.0;
  bool camera_needs_fit = true;
  bool viewport_right_drag = false;
  bool show_bones = true;


  std::set<std::string> hidden_bone_names;

  bool bone_context_open = false;
  std::string bone_context_bone_name;

  bool show_ground = true;





  bool camera_follow_preview = false;


  bool use_mcbe_coords = false;
  gfx::TextureImage model_texture;
  std::string texture_path;


  [[nodiscard]] std::uint64_t modelGeneration() const noexcept {
    return model_generation_;
  }

  [[nodiscard]] std::uint64_t textureGeneration() const noexcept {
    return texture_generation_;
  }

  bool loadTexture(const std::filesystem::path &path);
  void clearTexture();


  float bone_list_scroll = 0.0f;
  float anim_list_scroll = 0.0f;
  float property_scroll = 0.0f;


  bool show_debug_hud = false;


  bool debug_instant_sample = false;
  float debug_fps = 0.0f;
  float debug_frame_ms = 0.0f;
  float debug_ema_frame_ms = 16.0f;
  float debug_mesh_ms = 0.0f;
  float debug_upload_ms = 0.0f;

  double debug_upload_bytes = 0.0;
  double debug_static_bone_upload_bytes = 0.0;
  double debug_static_resource_upload_bytes = 0.0;
  double debug_buffer_reallocations = 0.0;

  std::uint64_t debug_static_resource_rebuilds = 0;
  std::uint64_t debug_total_buffer_reallocations = 0;
  std::uint64_t debug_static_model_vertex_bytes = 0;
  std::uint64_t debug_static_model_index_bytes = 0;
  std::uint32_t debug_static_opaque_index_count = 0;
  std::uint32_t debug_static_cutout_index_count = 0;
  std::uint32_t debug_static_blend_index_count = 0;
  float debug_backend_cpu_ms = 0.0f;
  bool debug_gpu_timestamp_valid = false;
  float debug_gpu_timestamp_total_ms = 0.0f;
  float debug_gpu_timestamp_ui_ms = 0.0f;
  float debug_gpu_timestamp_opaque_ms = 0.0f;
  float debug_gpu_timestamp_transparent_ms = 0.0f;
  float debug_gpu_timestamp_lines_ms = 0.0f;

  float debug_gpu_ms = 0.0f;
  int debug_cube_count = 0;
  int debug_frame_count = 0;
  double debug_last_sample_time = 0.0;



  bool vsync_enabled = true;
  bool vsync_dirty = false;



  [[nodiscard]] static double
  animationFollowComplianceFromUi(double strength) noexcept {
    return baker::BoneMapper::animationFollowStrengthToCompliance(strength);
  }
  [[nodiscard]] static float
  animationFollowStrengthForUi(double compliance) noexcept {
    return static_cast<float>(
        baker::BoneMapper::animationFollowComplianceToStrength(compliance));
  }
  void applyUiToConfig();

  void setSolverMode(int mode);
  void setLoopMode(int mode);
  void loadModel(const std::filesystem::path &path);
  void loadAnimation(const std::filesystem::path &path);
  void selectAnimation(const std::string &name);

  void clearAnimationSelection();


  void updateCameraFollowPreview();
  void togglePhysicsBone(const std::string &name, bool enabled);


  void setCollisionRoot(const std::string &name, bool enabled);


  void clearCollisionRoots();
  void selectBone(const std::string &name);
  [[nodiscard]] bool isBoneVisible(const std::string &name) const {
    return !hidden_bone_names.contains(name);
  }
  void setBoneVisible(const std::string &name, bool visible);
  [[nodiscard]] std::string pickBoneAt(float viewport_x, float viewport_y,
                                       float view_w, float view_h);
  bool openBoneContext(const std::string &name);
  void closeBoneContext();
  void loadSelectedBoneEditors();
  bool applySelectedBoneConfig();
  void clearSelectedBoneConfig();
  void markSelectedBoneDraftDirty();
  void discardSelectedBoneDraft();
  [[nodiscard]] bool hasUnappliedPerBoneDraft() const noexcept {
    return per_bone_draft_dirty;
  }
  void invalidatePhysicsArtifacts(InvalidationReason reason,
                                  const std::string &message,
                                  bool reset_to_source_start = true);
  void startBake();
  void confirmMolangBake(bool proceed);
  void cancelBake();
  void stepBake();
  void resetBake();
  void shutdownBakeWorker();
  bool exportAnimation(const std::filesystem::path &path);
  bool exportAllAnimations(const std::filesystem::path &path);
  void requestAnimationExport(const std::filesystem::path &path);
  void requestAllAnimationsExport(const std::filesystem::path &path);
  void confirmAnimationExport(bool proceed);
  bool exportVelocity(const std::filesystem::path &path);
  bool exportDiagnostics(const std::filesystem::path &path);
  [[nodiscard]] bool hasCompleteBake() const;
  [[nodiscard]] bool canExportAnimation() const;
  [[nodiscard]] bool canForceExportAnimation() const;
  [[nodiscard]] bool canExportVelocity() const;
  [[nodiscard]] const ExportPreflight &exportPreflight() const;
  [[nodiscard]] const BakeDiagnostics *diagnostics() const;
  [[nodiscard]] const BakeJobResult *finalResult() const;
  [[nodiscard]] const LiveSimulationFrame *liveSimulationFrame() const;
  [[nodiscard]] std::size_t previewFrameCount() const;
  [[nodiscard]] const loader::Animation *currentPreviewReferenceAnimation()
      const;
  [[nodiscard]] double currentPreviewReferenceTime() const;
  [[nodiscard]] SessionFingerprint currentSessionFingerprint() const;
  void setPresentationMode(PresentationMode mode);
  void pollBakeProgress();
  void fitCameraToModel();
  void advancePreview(float dt_seconds);
  void togglePreviewPlayback();
  [[nodiscard]] bool canPreview() const;
  [[nodiscard]] double previewLength() const;
  [[nodiscard]] render::SkeletonDrawList buildViewportDrawList(float view_w,
                                                               float view_h);
  [[nodiscard]] const baker::BakedFrame *currentPreviewFrame() const;
  void setPreviewFrameIndex(int index);

private:
  AppSession();
  [[nodiscard]] BakeJobInput
  makeBakeJobInput(bool allow_input_molang_zero,
                   bool allow_selected_molang_zero) const;
  [[nodiscard]] bool applyPendingPerBoneDraft();
  void startWorker(std::unique_ptr<BakeExecutionState> execution);
  void clearCommittedPhysicsArtifacts(bool reset_to_source_start);
  [[nodiscard]] const loader::Animation *
  resolvedTransitionTargetAnimation() const;
  [[nodiscard]] std::vector<loader::MolangBakeWarning>
  collectPendingMolangWarnings() const;
  bool exportAnimationInternal(const std::filesystem::path &path,
                               bool force_unsafe, bool export_all);
  bool molang_approved_once_ = false;
  std::uint64_t model_generation_ = 0;
  std::uint64_t animation_generation_ = 0;
  std::uint64_t texture_generation_ = 0;
  std::uint64_t physics_generation_ = 0;
  SessionFingerprint active_job_fingerprint_{};
  std::optional<std::jthread> bake_thread;
  std::shared_ptr<BakeWorkerMailbox> worker_mailbox_;
  std::unique_ptr<BakeExecutionState> live_execution_;
  std::unique_ptr<BakeExecutionState> final_execution_;
  std::optional<LiveSimulationFrame> live_frame_;
  std::optional<BakeJobResult> final_result_;
  mutable baker::BakedPreviewScratch preview_sample_scratch_;
  ExportPreflight empty_export_preflight_{};
  std::optional<std::filesystem::path> pending_export_animation_path_;
  bool pending_export_all_ = false;
  bool explicit_cancel_requested_ = false;
};


std::optional<std::filesystem::path> openFileDialog(const wchar_t *title,
                                                    const wchar_t *filter);
std::optional<std::filesystem::path>
saveFileDialog(const wchar_t *title, const wchar_t *filter,
               const wchar_t *default_name);

}
