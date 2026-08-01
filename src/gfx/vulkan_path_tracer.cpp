#include "xpbd/gfx/vulkan_path_tracer.hpp"

#include "xpbd/gfx/labpbr_authoring.hpp"
#include "xpbd/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace xpbd::gfx {
namespace {

static const std::uint32_t kSpvPathTraceComp[] = {
#include "spirv/path_trace.comp.spv.inc"
};
static const std::uint32_t kSpvPtCompositeVert[] = {
#include "spirv/pt_composite.vert.spv.inc"
};
static const std::uint32_t kSpvPtCompositeFrag[] = {
#include "spirv/pt_composite.frag.spv.inc"
};

struct PathTracePushConstants {
  float inv_view_proj[16]{};
  float view_proj[16]{};
  float camera_pos[4]{0, 0, 0, 0};
  float light_dir_amb[4]{0.35f, 0.85f, 0.40f, 0.38f};
  float light_color_int[4]{1, 1, 1, 0.85f};
  float clear_color[4]{0.1f, 0.12f, 0.16f, 1.15f};
  std::uint32_t size_frame[4]{1, 1, 0, 0};
  // xy: pixel-space camera jitter; z: temporal reconstruction input flag;
  // w: optional-output mask, bit-cast from uint32_t.
  float camera_jitter[4]{0, 0, 0, 0};
};
static_assert(sizeof(PathTracePushConstants) == 224);

struct CompositePushConstants {
  // exposure multiplier, white balance K, bloom, tone-map enum.
  float display[4]{1.0f, 6500.0f, 0.0f, 0.0f};
  // bit 0: transparent background.
  std::uint32_t flags[4]{};
};
static_assert(sizeof(CompositePushConstants) == 32u);

struct alignas(16) PathTraceMotionFrameGpu {
  float previous_view_projection[16]{};
  // x previous camera valid, y scene motion valid, z/w reserved.
  std::uint32_t info[4]{};
};
static_assert(sizeof(PathTraceMotionFrameGpu) == 80u);

bool invertMatrix4(const float *m, float *out) {
  float inv[16];
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
           m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
           m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
           m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
            m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
           m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
           m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
           m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
            m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
           m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
           m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
            m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
            m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
           m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
           m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
            m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
            m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::fabs(det) < 1e-12f) {
    return false;
  }
  det = 1.0f / det;
  for (int i = 0; i < 16; ++i) {
    out[i] = inv[i] * det;
  }
  return true;
}

void mulMat4(const float *a, const float *b, float *o) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                     a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    }
  }
}

void glMvpToVulkan(const float *matrix, float *out) {
  for (int column = 0; column < 4; ++column) {
    const float x = matrix[column * 4 + 0];
    const float y = matrix[column * 4 + 1];
    const float z = matrix[column * 4 + 2];
    const float w = matrix[column * 4 + 3];
    out[column * 4 + 0] = x;
    out[column * 4 + 1] = -y;
    out[column * 4 + 2] = 0.5f * z + 0.5f * w;
    out[column * 4 + 3] = w;
  }
}

[[nodiscard]] float halfToFloat(std::uint16_t half) noexcept {
  const std::uint32_t sign =
      static_cast<std::uint32_t>(half & 0x8000u) << 16u;
  std::uint32_t exponent = (half >> 10u) & 0x1fu;
  std::uint32_t mantissa = half & 0x03ffu;
  std::uint32_t bits = 0u;
  if (exponent == 0u) {
    if (mantissa == 0u) {
      bits = sign;
    } else {
      std::int32_t unbiased = -14;
      while ((mantissa & 0x0400u) == 0u) {
        mantissa <<= 1u;
        --unbiased;
      }
      mantissa &= 0x03ffu;
      bits = sign |
             (static_cast<std::uint32_t>(unbiased + 127) << 23u) |
             (mantissa << 13u);
    }
  } else if (exponent == 0x1fu) {
    bits = sign | 0x7f800000u | (mantissa << 13u);
  } else {
    bits = sign | ((exponent - 15u + 127u) << 23u) |
           (mantissa << 13u);
  }
  return std::bit_cast<float>(bits);
}

} // namespace

std::uint32_t VulkanPathTracer::findMemoryType(std::uint32_t bits,
                                               VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  return std::numeric_limits<std::uint32_t>::max();
}

VkShaderModule VulkanPathTracer::makeModule(const std::uint32_t *words,
                                            std::size_t count) const {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = count * 4;
  ci.pCode = words;
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &ci, nullptr, &m) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return m;
}

bool VulkanPathTracer::init(VkPhysicalDevice phys, VkDevice device,
                            VkRenderPass render_pass,
                            bool enable_diagnostic_capture) {
  shutdown();
  phys_ = phys;
  device_ = device;
  if (enable_diagnostic_capture) {
    if (const char *capture = std::getenv("XPBD_PT_CAPTURE");
        capture != nullptr && capture[0] != '\0') {
      capture_path_ = capture;
    }
    if (const char *summary = std::getenv("XPBD_PT_AOV_SUMMARY");
        summary != nullptr && summary[0] != '\0') {
      aov_summary_path_ = summary;
    }
    if (const char *samples = std::getenv("XPBD_PT_AOV_CAPTURE_SAMPLES");
        samples != nullptr && samples[0] != '\0') {
      char *end = nullptr;
      const unsigned long parsed = std::strtoul(samples, &end, 10);
      if (end != samples && end != nullptr && *end == '\0' &&
          parsed > 0u &&
          parsed <=
              static_cast<unsigned long>(
                  (std::numeric_limits<std::uint32_t>::max)())) {
        aov_capture_samples_ = static_cast<std::uint32_t>(parsed);
      }
    }
  }
  if (!createPipelines(render_pass)) {
    shutdown();
    return false;
  }
  if (!rt_pipeline_.init(phys_, device_)) {
    xpbd::log::warn(
        "Vulkan RT Pipeline unavailable; compute ray-query fallback remains "
        "active");
  }
  if (!ensureMotionFrame()) {
    shutdown();
    return false;
  }
  xpbd::log::info("Built-in Vulkan path tracer ready");
  return true;
}

bool VulkanPathTracer::requestStillCapture(
    const std::filesystem::path &path, StillImageFormat format,
    bool transparent_background, std::uint64_t job_id,
    const PathTraceStillBackgroundInput *background) {
  if (path.empty() || !ready() ||
      runtime_capture_state_ == PathTraceCaptureState::Requested ||
      runtime_capture_state_ ==
          PathTraceCaptureState::PendingGpuReadback) {
    return false;
  }
  runtime_capture_background_face_size_ = 0u;
  runtime_capture_background_rgba8_.clear();
  runtime_capture_background_inverse_view_projection_ = {};
  runtime_capture_background_camera_position_ = {};
  if (background != nullptr) {
    const std::size_t face_pixels =
        static_cast<std::size_t>(background->face_size) *
        background->face_size;
    const bool valid_background =
        background->face_size > 0u && background->rgba8 != nullptr &&
        face_pixels <= (static_cast<std::size_t>(-1) / 24u) &&
        background->rgba8_size == face_pixels * 24u &&
        background->view != nullptr && background->proj != nullptr;
    float view_projection[16]{};
    float inverse_view[16]{};
    if (!valid_background) {
      return false;
    }
    mulMat4(background->proj, background->view, view_projection);
    if (!invertMatrix4(view_projection,
                       runtime_capture_background_inverse_view_projection_
                           .data()) ||
        !invertMatrix4(background->view, inverse_view)) {
      return false;
    }
    runtime_capture_background_face_size_ = background->face_size;
    runtime_capture_background_rgba8_.assign(
        background->rgba8, background->rgba8 + background->rgba8_size);
    runtime_capture_background_camera_position_ = {
        inverse_view[12], inverse_view[13], inverse_view[14]};
  }
  runtime_capture_path_ = path.string();
  runtime_capture_format_ = format;
  runtime_capture_transparent_background_ = transparent_background;
  runtime_capture_job_id_ = job_id;
  runtime_capture_error_.clear();
  runtime_capture_state_ = PathTraceCaptureState::Requested;
  return true;
}

void VulkanPathTracer::cancelStillCapture() noexcept {
  const bool active =
      runtime_capture_state_ == PathTraceCaptureState::Requested ||
      runtime_capture_state_ == PathTraceCaptureState::PendingGpuReadback;
  if (active && pending_capture_is_runtime_) {
    capture_pending_ = false;
    pending_capture_path_.clear();
    pending_aov_summary_path_.clear();
    pending_capture_is_runtime_ = false;
    pending_capture_job_id_ = 0u;
    pending_capture_background_face_size_ = 0u;
    pending_capture_background_rgba8_.clear();
    pending_capture_background_inverse_view_projection_ = {};
    pending_capture_background_camera_position_ = {};
  }
  runtime_capture_path_.clear();
  runtime_capture_error_.clear();
  runtime_capture_job_id_ = 0u;
  runtime_capture_background_face_size_ = 0u;
  runtime_capture_background_rgba8_.clear();
  runtime_capture_background_inverse_view_projection_ = {};
  runtime_capture_background_camera_position_ = {};
  if (active) {
    runtime_capture_state_ = PathTraceCaptureState::Cancelled;
  }
}

void VulkanPathTracer::destroyImage() {
  history_key_ = 0;
  accumulated_samples_ = 0;
  history_valid_ = false;
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  for (VkImageView &view : aov_layer_views_) {
    if (view != VK_NULL_HANDLE) {
      vkDestroyImageView(device_, view, nullptr);
      view = VK_NULL_HANDLE;
    }
  }
  if (aov_array_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, aov_array_view_, nullptr);
    aov_array_view_ = VK_NULL_HANDLE;
  }
  if (aov_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, aov_image_, nullptr);
    aov_image_ = VK_NULL_HANDLE;
  }
  if (aov_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, aov_image_memory_, nullptr);
    aov_image_memory_ = VK_NULL_HANDLE;
  }
  if (statistics_image_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, statistics_image_view_, nullptr);
    statistics_image_view_ = VK_NULL_HANDLE;
  }
  if (statistics_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, statistics_image_, nullptr);
    statistics_image_ = VK_NULL_HANDLE;
  }
  if (statistics_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, statistics_image_memory_, nullptr);
    statistics_image_memory_ = VK_NULL_HANDLE;
  }
  if (rr_motion_image_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, rr_motion_image_view_, nullptr);
    rr_motion_image_view_ = VK_NULL_HANDLE;
  }
  if (rr_motion_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, rr_motion_image_, nullptr);
    rr_motion_image_ = VK_NULL_HANDLE;
  }
  if (rr_motion_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, rr_motion_image_memory_, nullptr);
    rr_motion_image_memory_ = VK_NULL_HANDLE;
  }
  if (rr_diffuse_albedo_image_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, rr_diffuse_albedo_image_view_, nullptr);
    rr_diffuse_albedo_image_view_ = VK_NULL_HANDLE;
  }
  if (rr_diffuse_albedo_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, rr_diffuse_albedo_image_, nullptr);
    rr_diffuse_albedo_image_ = VK_NULL_HANDLE;
  }
  if (rr_diffuse_albedo_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, rr_diffuse_albedo_image_memory_, nullptr);
    rr_diffuse_albedo_image_memory_ = VK_NULL_HANDLE;
  }
  if (rr_specular_albedo_image_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, rr_specular_albedo_image_view_, nullptr);
    rr_specular_albedo_image_view_ = VK_NULL_HANDLE;
  }
  if (rr_specular_albedo_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, rr_specular_albedo_image_, nullptr);
    rr_specular_albedo_image_ = VK_NULL_HANDLE;
  }
  if (rr_specular_albedo_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, rr_specular_albedo_image_memory_, nullptr);
    rr_specular_albedo_image_memory_ = VK_NULL_HANDLE;
  }
  if (rr_normal_roughness_image_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(device_, rr_normal_roughness_image_view_, nullptr);
    rr_normal_roughness_image_view_ = VK_NULL_HANDLE;
  }
  if (rr_normal_roughness_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(device_, rr_normal_roughness_image_, nullptr);
    rr_normal_roughness_image_ = VK_NULL_HANDLE;
  }
  if (rr_normal_roughness_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(device_, rr_normal_roughness_image_memory_, nullptr);
    rr_normal_roughness_image_memory_ = VK_NULL_HANDLE;
  }
  if (rr_specular_hit_distance_image_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(
        device_, rr_specular_hit_distance_image_view_, nullptr);
    rr_specular_hit_distance_image_view_ = VK_NULL_HANDLE;
  }
  if (rr_specular_hit_distance_image_ != VK_NULL_HANDLE) {
    vkDestroyImage(
        device_, rr_specular_hit_distance_image_, nullptr);
    rr_specular_hit_distance_image_ = VK_NULL_HANDLE;
  }
  if (rr_specular_hit_distance_image_memory_ != VK_NULL_HANDLE) {
    vkFreeMemory(
        device_, rr_specular_hit_distance_image_memory_, nullptr);
    rr_specular_hit_distance_image_memory_ = VK_NULL_HANDLE;
  }
  if (depth_image_view_) {
    vkDestroyImageView(device_, depth_image_view_, nullptr);
    depth_image_view_ = VK_NULL_HANDLE;
  }
  if (depth_image_) {
    vkDestroyImage(device_, depth_image_, nullptr);
    depth_image_ = VK_NULL_HANDLE;
  }
  if (depth_image_memory_) {
    vkFreeMemory(device_, depth_image_memory_, nullptr);
    depth_image_memory_ = VK_NULL_HANDLE;
  }
  if (image_view_) {
    vkDestroyImageView(device_, image_view_, nullptr);
    image_view_ = VK_NULL_HANDLE;
  }
  if (image_) {
    vkDestroyImage(device_, image_, nullptr);
    image_ = VK_NULL_HANDLE;
  }
  if (image_memory_) {
    vkFreeMemory(device_, image_memory_, nullptr);
    image_memory_ = VK_NULL_HANDLE;
  }
  image_w_ = image_h_ = 0;
  image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  depth_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  aov_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  statistics_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_motion_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_diffuse_albedo_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_specular_albedo_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_normal_roughness_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_specular_hit_distance_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanPathTracer::destroyMotionFrame() {
  if (device_ != VK_NULL_HANDLE) {
    if (motion_frame_mapped_ != nullptr &&
        motion_frame_buffer_.memory != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, motion_frame_buffer_.memory);
    }
    if (motion_frame_buffer_.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, motion_frame_buffer_.buffer, nullptr);
    }
    if (motion_frame_buffer_.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, motion_frame_buffer_.memory, nullptr);
    }
  }
  motion_frame_buffer_ = {};
  motion_frame_mapped_ = nullptr;
}

bool VulkanPathTracer::ensureMotionFrame() {
  if (motion_frame_buffer_.buffer != VK_NULL_HANDLE &&
      motion_frame_mapped_ != nullptr &&
      motion_frame_buffer_.capacity >= sizeof(PathTraceMotionFrameGpu)) {
    return true;
  }
  destroyMotionFrame();
  VkBufferCreateInfo buffer_info{
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = sizeof(PathTraceMotionFrameGpu);
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &buffer_info, nullptr,
                     &motion_frame_buffer_.buffer) != VK_SUCCESS) {
    destroyMotionFrame();
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, motion_frame_buffer_.buffer,
                                &requirements);
  const std::uint32_t memory_type = findMemoryType(
      requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memory_type == (std::numeric_limits<std::uint32_t>::max)()) {
    destroyMotionFrame();
    return false;
  }
  VkMemoryAllocateInfo allocate_info{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &allocate_info, nullptr,
                       &motion_frame_buffer_.memory) != VK_SUCCESS ||
      vkBindBufferMemory(device_, motion_frame_buffer_.buffer,
                         motion_frame_buffer_.memory, 0) != VK_SUCCESS ||
      vkMapMemory(device_, motion_frame_buffer_.memory, 0,
                  sizeof(PathTraceMotionFrameGpu), 0,
                  &motion_frame_mapped_) != VK_SUCCESS) {
    destroyMotionFrame();
    return false;
  }
  motion_frame_buffer_.capacity = sizeof(PathTraceMotionFrameGpu);
  return true;
}

void VulkanPathTracer::destroyCaptureBuffer() {
  if (device_ != VK_NULL_HANDLE) {
    if (capture_mapped_ != nullptr &&
        capture_buffer_.memory != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, capture_buffer_.memory);
    }
    if (capture_buffer_.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, capture_buffer_.buffer, nullptr);
    }
    if (capture_buffer_.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, capture_buffer_.memory, nullptr);
    }
  }
  capture_buffer_ = {};
  capture_mapped_ = nullptr;
  capture_pending_ = false;
  pending_capture_path_.clear();
  pending_aov_summary_path_.clear();
  pending_capture_width_ = 0;
  pending_capture_height_ = 0;
  pending_capture_format_ = StillImageFormat::Png;
  pending_capture_display_ = {};
  pending_capture_transparent_background_ = true;
  pending_capture_is_runtime_ = false;
  pending_capture_job_id_ = 0u;
  pending_capture_background_face_size_ = 0u;
  pending_capture_background_rgba8_.clear();
  pending_capture_background_inverse_view_projection_ = {};
  pending_capture_background_camera_position_ = {};
  pending_aov_offset_ = 0;
  pending_statistics_offset_ = 0;
  pending_depth_offset_ = 0;
}

bool VulkanPathTracer::ensureCaptureBuffer(VkDeviceSize size) {
  if (capture_buffer_.buffer != VK_NULL_HANDLE &&
      capture_buffer_.capacity >= size && capture_mapped_ != nullptr) {
    return true;
  }
  destroyCaptureBuffer();
  VkBufferCreateInfo buffer_info{
      VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &buffer_info, nullptr,
                     &capture_buffer_.buffer) != VK_SUCCESS) {
    destroyCaptureBuffer();
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, capture_buffer_.buffer,
                                &requirements);
  const std::uint32_t memory_type = findMemoryType(
      requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memory_type ==
      (std::numeric_limits<std::uint32_t>::max)()) {
    destroyCaptureBuffer();
    return false;
  }
  VkMemoryAllocateInfo allocate_info{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &allocate_info, nullptr,
                       &capture_buffer_.memory) != VK_SUCCESS ||
      vkBindBufferMemory(device_, capture_buffer_.buffer,
                         capture_buffer_.memory, 0) != VK_SUCCESS ||
      vkMapMemory(device_, capture_buffer_.memory, 0, size, 0,
                  &capture_mapped_) != VK_SUCCESS) {
    destroyCaptureBuffer();
    return false;
  }
  capture_buffer_.capacity = size;
  return true;
}

void VulkanPathTracer::flushPendingCapture() {
  if (!capture_pending_ || capture_mapped_ == nullptr ||
      pending_capture_width_ == 0u ||
      pending_capture_height_ == 0u ||
      (pending_capture_path_.empty() &&
       pending_aov_summary_path_.empty())) {
    return;
  }
  const std::size_t pixel_count =
      static_cast<std::size_t>(pending_capture_width_) *
      pending_capture_height_;
  const auto *half_pixels =
      static_cast<const std::uint16_t *>(capture_mapped_);
  const auto *device_depth =
      pending_capture_is_runtime_ && pending_depth_offset_ > 0u
          ? reinterpret_cast<const float *>(
                static_cast<const std::byte *>(capture_mapped_) +
                pending_depth_offset_)
          : nullptr;
  bool capture_wrote = pending_capture_path_.empty();
  std::string capture_error;
  if (!pending_capture_path_.empty()) {
    const std::filesystem::path output(pending_capture_path_);
    StillImageCubemapBackground background{};
    const StillImageCubemapBackground *background_ptr = nullptr;
    if (pending_capture_background_face_size_ > 0u &&
        !pending_capture_background_rgba8_.empty()) {
      background.face_size = pending_capture_background_face_size_;
      background.rgba8 = pending_capture_background_rgba8_.data();
      background.rgba8_size = pending_capture_background_rgba8_.size();
      background.inverse_view_projection =
          pending_capture_background_inverse_view_projection_;
      background.camera_position =
          pending_capture_background_camera_position_;
      if (background.valid()) {
        background_ptr = &background;
      }
    }
    capture_wrote = writeStillImageRgba16f(
        output, pending_capture_format_, pending_capture_width_,
        pending_capture_height_, half_pixels, pixel_count * 4u,
        pending_capture_display_,
        pending_capture_transparent_background_, device_depth,
        device_depth != nullptr ? pixel_count : 0u, &capture_error,
        background_ptr);
    if (pending_capture_is_runtime_) {
      xpbd::log::infof(
          "STILL_JOB save job_id=%llu path=%s result=%s",
          static_cast<unsigned long long>(pending_capture_job_id_),
          pending_capture_path_.c_str(),
          capture_wrote ? "passed" : "failed");
    }
    if (capture_wrote) {
      std::error_code size_error;
      const auto output_bytes =
          std::filesystem::file_size(output, size_error);
      xpbd::log::infof(
          "%s path_trace_capture path=%s width=%u height=%u bytes=%llu "
          "format=%s",
          pending_capture_is_runtime_ ? "STILL" : "VKDIAG",
          pending_capture_path_.c_str(), pending_capture_width_,
          pending_capture_height_,
          static_cast<unsigned long long>(
              size_error ? 0u : output_bytes),
          pending_capture_format_ == StillImageFormat::Exr ? "exr" : "png");
    } else {
      xpbd::log::warnf(
          "%s path-trace capture failed for '%s': %s",
          pending_capture_is_runtime_ ? "Still" : "Diagnostic",
          pending_capture_path_.c_str(),
          capture_error.empty() ? "filesystem write failed"
                                : capture_error.c_str());
    }
  }

  bool summary_wrote = pending_aov_summary_path_.empty();
  if (!pending_aov_summary_path_.empty() && pending_aov_offset_ > 0u &&
      pending_statistics_offset_ > pending_aov_offset_ &&
      pending_depth_offset_ > pending_statistics_offset_) {
    struct LayerSummary {
      std::array<double, 4> minimum{
          std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::infinity(),
          std::numeric_limits<double>::infinity()};
      std::array<double, 4> maximum{
          -std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()};
      std::array<double, 4> sum{};
      std::uint64_t finite_values = 0;
      std::uint64_t nonfinite_values = 0;
      std::uint64_t nonzero_pixels = 0;
    };
    constexpr std::array<const char *, 9> layer_names{
        "geometry_normal_linear_depth", "shading_normal_roughness",
        "diffuse_albedo", "specular_albedo_hit_distance",
        "diffuse_radiance_hit_distance",
        "specular_radiance_hit_distance", "motion_disocclusion",
        "emission", "transparency_overlay"};
    std::array<LayerSummary, 9> summaries{};
    const auto *aov_half = reinterpret_cast<const std::uint16_t *>(
        static_cast<const std::byte *>(capture_mapped_) +
        pending_aov_offset_);
    std::uint64_t motion_valid_pixels = 0;
    std::uint64_t disocclusion_pixels = 0;
    std::uint64_t surface_pixels = 0;
    for (std::size_t layer = 0; layer < summaries.size(); ++layer) {
      LayerSummary &summary = summaries[layer];
      for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        bool nonzero = false;
        for (std::size_t channel = 0; channel < 4u; ++channel) {
          const std::size_t index =
              (layer * pixel_count + pixel) * 4u + channel;
          const double value =
              static_cast<double>(halfToFloat(aov_half[index]));
          if (!std::isfinite(value)) {
            ++summary.nonfinite_values;
            continue;
          }
          ++summary.finite_values;
          summary.minimum[channel] =
              (std::min)(summary.minimum[channel], value);
          summary.maximum[channel] =
              (std::max)(summary.maximum[channel], value);
          summary.sum[channel] += value;
          nonzero = nonzero || std::abs(value) > 1.0e-7;
        }
        summary.nonzero_pixels += nonzero ? 1u : 0u;
        if (layer ==
            static_cast<std::size_t>(
                PathTraceAovLayer::GeometryNormalLinearDepth)) {
          const float depth =
              halfToFloat(aov_half[(layer * pixel_count + pixel) * 4u + 3u]);
          surface_pixels += depth > 0.0f && std::isfinite(depth) ? 1u : 0u;
        } else if (
            layer == static_cast<std::size_t>(
                         PathTraceAovLayer::MotionDisocclusion)) {
          const float disocclusion =
              halfToFloat(aov_half[(layer * pixel_count + pixel) * 4u + 2u]);
          const float valid =
              halfToFloat(aov_half[(layer * pixel_count + pixel) * 4u + 3u]);
          disocclusion_pixels += disocclusion > 0.5f ? 1u : 0u;
          motion_valid_pixels += valid > 0.5f ? 1u : 0u;
        }
      }
    }

    const auto *statistics = reinterpret_cast<const float *>(
        static_cast<const std::byte *>(capture_mapped_) +
        pending_statistics_offset_);
    const auto *depth = reinterpret_cast<const float *>(
        static_cast<const std::byte *>(capture_mapped_) +
        pending_depth_offset_);
    std::uint64_t statistics_nonfinite = 0;
    std::uint64_t negative_variance = 0;
    double sample_count_min = std::numeric_limits<double>::infinity();
    double sample_count_max = 0.0;
    double variance_max = 0.0;
    double depth_min = std::numeric_limits<double>::infinity();
    double depth_max = 0.0;
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
      for (std::size_t channel = 0; channel < 4u; ++channel) {
        statistics_nonfinite +=
            std::isfinite(statistics[pixel * 4u + channel]) ? 0u : 1u;
      }
      const double variance = statistics[pixel * 4u + 0u];
      negative_variance += variance < 0.0 ? 1u : 0u;
      if (std::isfinite(variance)) {
        variance_max = (std::max)(variance_max, variance);
      }
      const double samples = statistics[pixel * 4u + 2u];
      if (std::isfinite(samples)) {
        sample_count_min = (std::min)(sample_count_min, samples);
        sample_count_max = (std::max)(sample_count_max, samples);
      }
      if (std::isfinite(depth[pixel])) {
        depth_min = (std::min)(depth_min, static_cast<double>(depth[pixel]));
        depth_max = (std::max)(depth_max, static_cast<double>(depth[pixel]));
      }
    }
    if (!std::isfinite(sample_count_min)) {
      sample_count_min = 0.0;
      sample_count_max = 0.0;
    }
    if (!std::isfinite(depth_min)) {
      depth_min = 0.0;
      depth_max = 0.0;
    }
    for (LayerSummary &summary : summaries) {
      for (std::size_t channel = 0; channel < 4u; ++channel) {
        if (!std::isfinite(summary.minimum[channel]) ||
            !std::isfinite(summary.maximum[channel])) {
          summary.minimum[channel] = 0.0;
          summary.maximum[channel] = 0.0;
        }
      }
    }

    const std::filesystem::path output(pending_aov_summary_path_);
    std::error_code filesystem_error;
    if (!output.parent_path().empty()) {
      std::filesystem::create_directories(output.parent_path(),
                                          filesystem_error);
    }
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!filesystem_error && stream) {
      stream << "{\n  \"schema\": \"xpbd-pt-aov-summary/1\",\n"
             << "  \"width\": " << pending_capture_width_ << ",\n"
             << "  \"height\": " << pending_capture_height_ << ",\n"
             << "  \"surface_pixels\": " << surface_pixels << ",\n"
             << "  \"motion_valid_pixels\": " << motion_valid_pixels
             << ",\n  \"disocclusion_pixels\": "
             << disocclusion_pixels << ",\n"
             << "  \"statistics_nonfinite_values\": "
             << statistics_nonfinite << ",\n"
             << "  \"negative_variance_pixels\": "
             << negative_variance << ",\n"
             << "  \"sample_count_range\": [" << sample_count_min << ", "
             << sample_count_max << "],\n"
             << "  \"variance_max\": " << variance_max << ",\n"
             << "  \"device_depth_range\": [" << depth_min << ", "
             << depth_max << "],\n  \"layers\": {\n";
      for (std::size_t layer = 0; layer < summaries.size(); ++layer) {
        const LayerSummary &summary = summaries[layer];
        stream << "    \"" << layer_names[layer] << "\": {\n"
               << "      \"finite_values\": " << summary.finite_values
               << ",\n      \"nonfinite_values\": "
               << summary.nonfinite_values
               << ",\n      \"nonzero_pixels\": "
               << summary.nonzero_pixels << ",\n"
               << "      \"min\": [" << summary.minimum[0] << ", "
               << summary.minimum[1] << ", " << summary.minimum[2]
               << ", " << summary.minimum[3] << "],\n"
               << "      \"max\": [" << summary.maximum[0] << ", "
               << summary.maximum[1] << ", " << summary.maximum[2]
               << ", " << summary.maximum[3] << "],\n"
               << "      \"mean\": [";
        for (std::size_t channel = 0; channel < 4u; ++channel) {
          const double denominator =
              static_cast<double>(pixel_count);
          stream << summary.sum[channel] / denominator
                 << (channel + 1u < 4u ? ", " : "");
        }
        stream << "]\n    }"
               << (layer + 1u < summaries.size() ? "," : "") << "\n";
      }
      stream << "  }\n}\n";
      summary_wrote = stream.good();
    }
    if (summary_wrote) {
      xpbd::log::infof(
          "VKDIAG path_trace_aov_summary path=%s surfaces=%llu "
          "motion_valid=%llu disocclusion=%llu sample_range=%.0f..%.0f "
          "nonfinite=%llu negative_variance=%llu",
          pending_aov_summary_path_.c_str(),
          static_cast<unsigned long long>(surface_pixels),
          static_cast<unsigned long long>(motion_valid_pixels),
          static_cast<unsigned long long>(disocclusion_pixels),
          sample_count_min, sample_count_max,
          static_cast<unsigned long long>(statistics_nonfinite),
          static_cast<unsigned long long>(negative_variance));
    } else {
      xpbd::log::warnf("Path-trace AOV summary write failed for '%s'",
                       pending_aov_summary_path_.c_str());
    }
  }
  capture_pending_ = false;
  capture_completed_ = capture_wrote && summary_wrote;
  if (pending_capture_is_runtime_) {
    runtime_capture_error_ =
        capture_completed_ ? std::string{} : std::move(capture_error);
    runtime_capture_state_ =
        capture_completed_ ? PathTraceCaptureState::Completed
                           : PathTraceCaptureState::Failed;
    runtime_capture_path_.clear();
  }
  pending_capture_path_.clear();
  pending_aov_summary_path_.clear();
  pending_capture_is_runtime_ = false;
  pending_capture_job_id_ = 0u;
  pending_capture_background_face_size_ = 0u;
  pending_capture_background_rgba8_.clear();
  pending_capture_background_inverse_view_projection_ = {};
  pending_capture_background_camera_position_ = {};
  runtime_capture_job_id_ = 0u;
  runtime_capture_background_face_size_ = 0u;
  runtime_capture_background_rgba8_.clear();
  runtime_capture_background_inverse_view_projection_ = {};
  runtime_capture_background_camera_position_ = {};
}

void VulkanPathTracer::destroyFallbackAlbedo() {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  if (fallback_albedo_view_) {
    vkDestroyImageView(device_, fallback_albedo_view_, nullptr);
    fallback_albedo_view_ = VK_NULL_HANDLE;
  }
  if (fallback_albedo_image_) {
    vkDestroyImage(device_, fallback_albedo_image_, nullptr);
    fallback_albedo_image_ = VK_NULL_HANDLE;
  }
  if (fallback_albedo_memory_) {
    vkFreeMemory(device_, fallback_albedo_memory_, nullptr);
    fallback_albedo_memory_ = VK_NULL_HANDLE;
  }
  fallback_albedo_cleared_ = false;
}

bool VulkanPathTracer::ensureFallbackAlbedo() {
  if (fallback_albedo_view_) {
    return true;
  }
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {1, 1, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ii, nullptr, &fallback_albedo_image_) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, fallback_albedo_image_, &req);
  const auto type =
      findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == std::numeric_limits<std::uint32_t>::max()) {
    destroyFallbackAlbedo();
    return false;
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (vkAllocateMemory(device_, &ai, nullptr, &fallback_albedo_memory_) !=
      VK_SUCCESS) {
    destroyFallbackAlbedo();
    return false;
  }
  vkBindImageMemory(device_, fallback_albedo_image_, fallback_albedo_memory_, 0);

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = fallback_albedo_image_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vi, nullptr, &fallback_albedo_view_) !=
      VK_SUCCESS) {
    destroyFallbackAlbedo();
    return false;
  }
  // Cleared to white on first recordDispatch (needs a command buffer).
  return true;
}

void VulkanPathTracer::shutdown() {
  if (device_ != VK_NULL_HANDLE) {
    rt_pipeline_.shutdown();
    destroyImage();
    destroyMotionFrame();
    destroyFallbackAlbedo();
    destroyCaptureBuffer();
    if (compute_pipeline_) {
      vkDestroyPipeline(device_, compute_pipeline_, nullptr);
    }
    if (compute_pipe_layout_) {
      vkDestroyPipelineLayout(device_, compute_pipe_layout_, nullptr);
    }
    if (compute_pool_) {
      vkDestroyDescriptorPool(device_, compute_pool_, nullptr);
    }
    if (compute_layout_) {
      vkDestroyDescriptorSetLayout(device_, compute_layout_, nullptr);
    }
    if (composite_pipeline_) {
      vkDestroyPipeline(device_, composite_pipeline_, nullptr);
    }
    if (composite_pipe_layout_) {
      vkDestroyPipelineLayout(device_, composite_pipe_layout_, nullptr);
    }
    if (composite_pool_) {
      vkDestroyDescriptorPool(device_, composite_pool_, nullptr);
    }
    if (composite_layout_) {
      vkDestroyDescriptorSetLayout(device_, composite_layout_, nullptr);
    }
    if (sampler_) {
      vkDestroySampler(device_, sampler_, nullptr);
    }
    if (albedo_sampler_) {
      vkDestroySampler(device_, albedo_sampler_, nullptr);
    }
  }
  compute_pipeline_ = VK_NULL_HANDLE;
  compute_pipe_layout_ = VK_NULL_HANDLE;
  compute_pool_ = VK_NULL_HANDLE;
  compute_set_ = VK_NULL_HANDLE;
  compute_layout_ = VK_NULL_HANDLE;
  composite_pipeline_ = VK_NULL_HANDLE;
  composite_pipe_layout_ = VK_NULL_HANDLE;
  composite_pool_ = VK_NULL_HANDLE;
  composite_set_ = VK_NULL_HANDLE;
  composite_layout_ = VK_NULL_HANDLE;
  sampler_ = VK_NULL_HANDLE;
  albedo_sampler_ = VK_NULL_HANDLE;
  rt_fallback_logged_ = false;
  compute_descriptor_key_ = {};
  compute_descriptor_key_valid_ = false;
  descriptor_write_calls_ = 0;
  descriptor_cache_hits_ = 0;
  descriptor_entries_written_ = 0;
  descriptor_update_ms_ = 0.0f;
  rt_pipeline_.resetDescriptorStats();
  history_key_ = 0;
  history_reset_count_ = 0;
  accumulated_samples_ = 0;
  history_valid_ = false;
  device_ = VK_NULL_HANDLE;
  phys_ = VK_NULL_HANDLE;
  capture_path_.clear();
  aov_summary_path_.clear();
  aov_capture_samples_ = 0;
  capture_completed_ = false;
  runtime_capture_path_.clear();
  runtime_capture_error_.clear();
  runtime_capture_format_ = StillImageFormat::Png;
  runtime_capture_state_ = PathTraceCaptureState::Idle;
  runtime_capture_transparent_background_ = false;
  runtime_capture_job_id_ = 0u;
  runtime_capture_background_face_size_ = 0u;
  runtime_capture_background_rgba8_.clear();
  runtime_capture_background_inverse_view_projection_ = {};
  runtime_capture_background_camera_position_ = {};
}

bool VulkanPathTracer::createPipelines(VkRenderPass render_pass) {
  // Compute: AS + color/depth/AOV targets + geometry/motion attributes.
  std::array<VkDescriptorSetLayoutBinding, 24> cbind{};
  cbind[0].binding = 0;
  cbind[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  cbind[0].descriptorCount = 1;
  cbind[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[1].binding = 1;
  cbind[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  cbind[1].descriptorCount = 1;
  cbind[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[2].binding = 2;
  cbind[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[2].descriptorCount = 1;
  cbind[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[3].binding = 3;
  cbind[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[3].descriptorCount = 1;
  cbind[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[4].binding = 4;
  cbind[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[4].descriptorCount = 1;
  cbind[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[5].binding = 5;
  cbind[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  cbind[5].descriptorCount = 1;
  cbind[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[6].binding = 6;
  cbind[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[6].descriptorCount = 1;
  cbind[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[7].binding = 7;
  cbind[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[7].descriptorCount = 1;
  cbind[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[8].binding = 8;
  cbind[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  cbind[8].descriptorCount = 1;
  cbind[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  cbind[9].binding = 9;
  cbind[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[9].descriptorCount = 1;
  cbind[9].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  for (std::uint32_t binding = 10; binding <= 11; ++binding) {
    cbind[binding].binding = binding;
    cbind[binding].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    cbind[binding].descriptorCount = 1;
    cbind[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  cbind[12].binding = 12;
  cbind[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  cbind[12].descriptorCount = 1;
  cbind[12].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  for (std::uint32_t binding = 13; binding <= 16; ++binding) {
    cbind[binding].binding = binding;
    cbind[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    cbind[binding].descriptorCount = 1;
    cbind[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  for (std::uint32_t binding = 17; binding <= 23; ++binding) {
    cbind[binding].binding = binding;
    cbind[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    cbind[binding].descriptorCount = 1;
    cbind[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo cli{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  cli.bindingCount = static_cast<std::uint32_t>(cbind.size());
  cli.pBindings = cbind.data();
  if (vkCreateDescriptorSetLayout(device_, &cli, nullptr, &compute_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.offset = 0;
  pcr.size = sizeof(PathTracePushConstants);
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &compute_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &compute_pipe_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  VkShaderModule comp =
      makeModule(kSpvPathTraceComp, sizeof(kSpvPathTraceComp) / 4);
  if (!comp) {
    return false;
  }
  VkPipelineShaderStageCreateInfo stage{
      VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = comp;
  stage.pName = "main";
  VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  cpi.stage = stage;
  cpi.layout = compute_pipe_layout_;
  const VkResult cr = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpi,
                                               nullptr, &compute_pipeline_);
  vkDestroyShaderModule(device_, comp, nullptr);
  if (cr != VK_SUCCESS) {
    return false;
  }

  std::array<VkDescriptorPoolSize, 4> cps{};
  cps[0] = {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1};
  cps[1] = {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9};
  cps[2] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 11};
  cps[3] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = static_cast<std::uint32_t>(cps.size());
  dpi.pPoolSizes = cps.data();
  if (vkCreateDescriptorPool(device_, &dpi, nullptr, &compute_pool_) !=
      VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = compute_pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &compute_layout_;
  if (vkAllocateDescriptorSets(device_, &ai, &compute_set_) != VK_SUCCESS) {
    return false;
  }

  // Composite graphics pipeline
  std::array<VkDescriptorSetLayoutBinding, 3> sbind{};
  for (std::uint32_t binding = 0; binding < sbind.size(); ++binding) {
    sbind[binding].binding = binding;
    sbind[binding].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sbind[binding].descriptorCount = 1;
    sbind[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  cli.bindingCount = static_cast<std::uint32_t>(sbind.size());
  cli.pBindings = sbind.data();
  if (vkCreateDescriptorSetLayout(device_, &cli, nullptr, &composite_layout_) !=
      VK_SUCCESS) {
    return false;
  }
  pli.pSetLayouts = &composite_layout_;
  VkPushConstantRange composite_push_range{};
  composite_push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  composite_push_range.offset = 0;
  composite_push_range.size = sizeof(CompositePushConstants);
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &composite_push_range;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &composite_pipe_layout_) !=
      VK_SUCCESS) {
    return false;
  }

  // Nearest filtering: 1:1 composite and pixel-art albedo (no filter moiré).
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = VK_FILTER_NEAREST;
  si.minFilter = VK_FILTER_NEAREST;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &si, nullptr, &sampler_) != VK_SUCCESS) {
    return false;
  }
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  if (vkCreateSampler(device_, &si, nullptr, &albedo_sampler_) != VK_SUCCESS) {
    return false;
  }
  if (!ensureFallbackAlbedo()) {
    return false;
  }

  VkDescriptorPoolSize sps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &sps;
  dpi.maxSets = 1;
  if (vkCreateDescriptorPool(device_, &dpi, nullptr, &composite_pool_) !=
      VK_SUCCESS) {
    return false;
  }
  ai.descriptorPool = composite_pool_;
  ai.pSetLayouts = &composite_layout_;
  if (vkAllocateDescriptorSets(device_, &ai, &composite_set_) != VK_SUCCESS) {
    return false;
  }

  VkShaderModule vs =
      makeModule(kSpvPtCompositeVert, sizeof(kSpvPtCompositeVert) / 4);
  VkShaderModule fs =
      makeModule(kSpvPtCompositeFrag, sizeof(kSpvPtCompositeFrag) / 4);
  if (!vs || !fs) {
    if (vs)
      vkDestroyShaderModule(device_, vs, nullptr);
    if (fs)
      vkDestroyShaderModule(device_, fs, nullptr);
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_TRUE;
  ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 0xF;
  blend.blendEnable = VK_TRUE;
  // The compute shader stores front-to-back premultiplied color.
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  std::array<VkDynamicState, 2> dyn = {VK_DYNAMIC_STATE_VIEWPORT,
                                       VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dsi{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dsi.dynamicStateCount = 2;
  dsi.pDynamicStates = dyn.data();

  VkGraphicsPipelineCreateInfo gpi{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpi.stageCount = 2;
  gpi.pStages = stages;
  gpi.pVertexInputState = &vi;
  gpi.pInputAssemblyState = &ia;
  gpi.pViewportState = &vp;
  gpi.pRasterizationState = &rs;
  gpi.pMultisampleState = &ms;
  gpi.pDepthStencilState = &ds;
  gpi.pColorBlendState = &cb;
  gpi.pDynamicState = &dsi;
  gpi.layout = composite_pipe_layout_;
  gpi.renderPass = render_pass;
  gpi.subpass = 0;
  const VkResult gr = vkCreateGraphicsPipelines(
      device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &composite_pipeline_);
  vkDestroyShaderModule(device_, vs, nullptr);
  vkDestroyShaderModule(device_, fs, nullptr);
  return gr == VK_SUCCESS;
}

bool VulkanPathTracer::ensureTarget(std::uint32_t width, std::uint32_t height) {
  width = (std::max)(1u, width);
  height = (std::max)(1u, height);
  if (image_ && depth_image_ && aov_image_ && statistics_image_ &&
      rr_motion_image_ && rr_diffuse_albedo_image_ &&
      rr_specular_albedo_image_ && rr_normal_roughness_image_ &&
      rr_specular_hit_distance_image_ &&
      image_w_ == width && image_h_ == height) {
    return true;
  }
  destroyImage();

  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  ii.extent = {width, height, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &ii, nullptr, &image_) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, image_, &req);
  const auto type =
      findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == std::numeric_limits<std::uint32_t>::max()) {
    destroyImage();
    return false;
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (vkAllocateMemory(device_, &ai, nullptr, &image_memory_) != VK_SUCCESS) {
    destroyImage();
    return false;
  }
  vkBindImageMemory(device_, image_, image_memory_, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vi, nullptr, &image_view_) != VK_SUCCESS) {
    destroyImage();
    return false;
  }

  ii.format = VK_FORMAT_R32_SFLOAT;
  if (vkCreateImage(device_, &ii, nullptr, &depth_image_) != VK_SUCCESS) {
    destroyImage();
    return false;
  }
  VkMemoryRequirements depth_req{};
  vkGetImageMemoryRequirements(device_, depth_image_, &depth_req);
  const auto depth_type = findMemoryType(
      depth_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (depth_type == (std::numeric_limits<std::uint32_t>::max)()) {
    destroyImage();
    return false;
  }
  VkMemoryAllocateInfo depth_ai{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  depth_ai.allocationSize = depth_req.size;
  depth_ai.memoryTypeIndex = depth_type;
  if (vkAllocateMemory(device_, &depth_ai, nullptr, &depth_image_memory_) !=
      VK_SUCCESS) {
    destroyImage();
    return false;
  }
  if (vkBindImageMemory(device_, depth_image_, depth_image_memory_, 0) !=
      VK_SUCCESS) {
    destroyImage();
    return false;
  }
  VkImageViewCreateInfo depth_vi{
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  depth_vi.image = depth_image_;
  depth_vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  depth_vi.format = VK_FORMAT_R32_SFLOAT;
  depth_vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &depth_vi, nullptr, &depth_image_view_) !=
      VK_SUCCESS) {
    destroyImage();
    return false;
  }

  ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  ii.arrayLayers =
      static_cast<std::uint32_t>(PathTraceAovLayer::Count);
  if (vkCreateImage(device_, &ii, nullptr, &aov_image_) != VK_SUCCESS) {
    destroyImage();
    return false;
  }
  VkMemoryRequirements aov_req{};
  vkGetImageMemoryRequirements(device_, aov_image_, &aov_req);
  const auto aov_type = findMemoryType(
      aov_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (aov_type == (std::numeric_limits<std::uint32_t>::max)()) {
    destroyImage();
    return false;
  }
  VkMemoryAllocateInfo aov_ai{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  aov_ai.allocationSize = aov_req.size;
  aov_ai.memoryTypeIndex = aov_type;
  if (vkAllocateMemory(device_, &aov_ai, nullptr, &aov_image_memory_) !=
          VK_SUCCESS ||
      vkBindImageMemory(device_, aov_image_, aov_image_memory_, 0) !=
          VK_SUCCESS) {
    destroyImage();
    return false;
  }
  VkImageViewCreateInfo aov_view_info{
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  aov_view_info.image = aov_image_;
  aov_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  aov_view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  aov_view_info.subresourceRange = {
      VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
      static_cast<std::uint32_t>(PathTraceAovLayer::Count)};
  if (vkCreateImageView(device_, &aov_view_info, nullptr,
                        &aov_array_view_) != VK_SUCCESS) {
    destroyImage();
    return false;
  }
  aov_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  aov_view_info.subresourceRange.layerCount = 1;
  for (std::size_t layer = 0; layer < aov_layer_views_.size(); ++layer) {
    aov_view_info.subresourceRange.baseArrayLayer =
        static_cast<std::uint32_t>(layer);
    if (vkCreateImageView(device_, &aov_view_info, nullptr,
                          &aov_layer_views_[layer]) != VK_SUCCESS) {
      destroyImage();
      return false;
    }
  }

  ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  ii.arrayLayers = 1;
  if (vkCreateImage(device_, &ii, nullptr, &statistics_image_) !=
      VK_SUCCESS) {
    destroyImage();
    return false;
  }
  VkMemoryRequirements statistics_req{};
  vkGetImageMemoryRequirements(device_, statistics_image_,
                               &statistics_req);
  const auto statistics_type = findMemoryType(
      statistics_req.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (statistics_type == (std::numeric_limits<std::uint32_t>::max)()) {
    destroyImage();
    return false;
  }
  VkMemoryAllocateInfo statistics_ai{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  statistics_ai.allocationSize = statistics_req.size;
  statistics_ai.memoryTypeIndex = statistics_type;
  if (vkAllocateMemory(device_, &statistics_ai, nullptr,
                       &statistics_image_memory_) != VK_SUCCESS ||
      vkBindImageMemory(device_, statistics_image_,
                        statistics_image_memory_, 0) != VK_SUCCESS) {
    destroyImage();
    return false;
  }
  VkImageViewCreateInfo statistics_view_info{
      VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  statistics_view_info.image = statistics_image_;
  statistics_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  statistics_view_info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  statistics_view_info.subresourceRange =
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &statistics_view_info, nullptr,
                        &statistics_image_view_) != VK_SUCCESS) {
    destroyImage();
    return false;
  }

  auto create_rr_guide =
      [&](VkFormat format, VkImage &image, VkDeviceMemory &memory,
          VkImageView &view) {
        ii.format = format;
        ii.arrayLayers = 1u;
        if (vkCreateImage(device_, &ii, nullptr, &image) != VK_SUCCESS) {
          return false;
        }
        VkMemoryRequirements guide_requirements{};
        vkGetImageMemoryRequirements(
            device_, image, &guide_requirements);
        const std::uint32_t guide_type = findMemoryType(
            guide_requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (guide_type ==
            (std::numeric_limits<std::uint32_t>::max)()) {
          return false;
        }
        VkMemoryAllocateInfo guide_allocation{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        guide_allocation.allocationSize = guide_requirements.size;
        guide_allocation.memoryTypeIndex = guide_type;
        if (vkAllocateMemory(
                device_, &guide_allocation, nullptr, &memory) !=
                VK_SUCCESS ||
            vkBindImageMemory(device_, image, memory, 0u) !=
                VK_SUCCESS) {
          return false;
        }
        VkImageViewCreateInfo guide_view{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        guide_view.image = image;
        guide_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        guide_view.format = format;
        guide_view.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        return vkCreateImageView(
                   device_, &guide_view, nullptr, &view) ==
               VK_SUCCESS;
      };
  if (!create_rr_guide(
          VK_FORMAT_R32G32_SFLOAT, rr_motion_image_,
          rr_motion_image_memory_, rr_motion_image_view_) ||
      !create_rr_guide(
          VK_FORMAT_R16G16B16A16_SFLOAT, rr_diffuse_albedo_image_,
          rr_diffuse_albedo_image_memory_,
          rr_diffuse_albedo_image_view_) ||
      !create_rr_guide(
          VK_FORMAT_R16G16B16A16_SFLOAT, rr_specular_albedo_image_,
          rr_specular_albedo_image_memory_,
          rr_specular_albedo_image_view_) ||
      !create_rr_guide(
          VK_FORMAT_R16G16B16A16_SFLOAT, rr_normal_roughness_image_,
          rr_normal_roughness_image_memory_,
          rr_normal_roughness_image_view_) ||
      !create_rr_guide(
          VK_FORMAT_R32_SFLOAT, rr_specular_hit_distance_image_,
          rr_specular_hit_distance_image_memory_,
          rr_specular_hit_distance_image_view_)) {
    destroyImage();
    return false;
  }

  image_w_ = width;
  image_h_ = height;
  image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  depth_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  aov_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  statistics_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_motion_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_diffuse_albedo_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_specular_albedo_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_normal_roughness_image_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  rr_specular_hit_distance_image_layout_ =
      VK_IMAGE_LAYOUT_UNDEFINED;
  ++history_generation_;

  std::array<VkDescriptorImageInfo, 3> descriptor_images{};
  descriptor_images[0].imageView = image_view_;
  descriptor_images[1].imageView = depth_image_view_;
  descriptor_images[2].imageView = image_view_;
  for (auto &descriptor_image : descriptor_images) {
    descriptor_image.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_image.sampler = sampler_;
  }
  std::array<VkWriteDescriptorSet, 3> writes{};
  static_assert(writes.size() == descriptor_images.size(),
                "composite descriptor writes must match image bindings");
  for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = composite_set_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[binding].descriptorCount = 1;
    writes[binding].pImageInfo = &descriptor_images[binding];
  }
  vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
  return true;
}

void VulkanPathTracer::recordDispatch(VkCommandBuffer cmd,
                                      const VulkanRtScene &scene,
                                      const PathTraceFrameParams &params,
                                      VkImageView albedo_view,
                                      VkImageView normal_view,
                                      VkImageView specular_view,
                                      VkSampler albedo_sampler) {
  descriptor_write_calls_ = 0;
  descriptor_cache_hits_ = 0;
  descriptor_entries_written_ = 0;
  descriptor_update_ms_ = 0.0f;
  rt_pipeline_.resetDescriptorStats();
  const bool debug_requested =
      params.rt_debug_view != RtDebugView::Off ||
      params.material_debug_view != LabPbrDebugView::Shaded;
  const bool diagnostic_aovs_requested =
      !aov_summary_path_.empty() && !capture_completed_;
  last_output_write_mask_ = params.output_write_mask;
  if (debug_requested || diagnostic_aovs_requested) {
    last_output_write_mask_ |=
        kPathTraceAllAovOutputMask | kPathTraceStatisticsOutputMask;
  }
  last_output_write_mask_ &= kPathTraceAllOptionalOutputMask;
  // This slot's frame fence has been waited before the backend records its
  // next frame, so a copy queued on the prior use is now host-visible.
  flushPendingCapture();
  if (!ready() || !scene.ready() || !image_ || !depth_image_ ||
      !aov_image_ || !statistics_image_ || !rr_motion_image_ ||
      !rr_diffuse_albedo_image_ || !rr_specular_albedo_image_ ||
      !rr_normal_roughness_image_ ||
      !rr_specular_hit_distance_image_ || !ensureMotionFrame()) {
    return;
  }
  if (!scene.normalBuffer() || !scene.indexAttribBuffer() ||
      !scene.uvBuffer() || !scene.colorBuffer() ||
      !scene.primitiveFlagBuffer() || !scene.tangentBuffer() ||
      !scene.instanceMetadataBuffer() || !scene.positionBuffer() ||
      !scene.previousPositionBuffer() || !scene.instanceMotionBuffer()) {
    return;
  }
  if (!ensureFallbackAlbedo()) {
    return;
  }

  const bool use_fallback = albedo_view == VK_NULL_HANDLE;
  const PathTraceSettings settings =
      normalizePathTraceSettings(params.settings);
  if (settings.pause_accumulation && history_valid_ &&
      params.history_key == history_key_) {
    return;
  }
  if (debug_requested) {
    // Debug views overwrite the shared output image with an immediate value.
    // The old path average must never be reused after returning to mode Off.
    history_key_ = 0;
    accumulated_samples_ = 0;
    history_valid_ = false;
  }
  PathTraceAccumulationStep accumulation_step{};
  if (!debug_requested) {
    if (params.temporal_reconstruction_input) {
      // DLSS SR/RR own temporal history. Feed them a newly sampled noisy
      // frame without presenting that overwrite as a camera/scene cut.
      accumulation_step.history_key = params.history_key;
      accumulation_step.history_reset =
          !history_valid_ || params.history_key != history_key_;
      accumulation_step.sample_base = 0u;
      accumulation_step.dispatch_samples =
          (std::max)(settings.samples_per_frame, 1u);
      accumulation_step.accumulated_samples_after_dispatch =
          accumulation_step.dispatch_samples;
      accumulation_step.maximum_reached = false;
    } else {
      PathTraceAccumulationRequest request;
      request.history_key = params.history_key;
      request.previous_history_key = history_key_;
      request.accumulated_samples = accumulated_samples_;
      request.settings = settings;
      if (settings.pause_accumulation) {
        // A newly allocated image needs one defined sample before it can be
        // composited. Subsequent paused frames preserve that history exactly.
        request.settings.samples_per_frame = 1u;
      }
      request.history_valid = history_valid_;
      accumulation_step = advancePathTraceAccumulation(request);
    }
    if (rt_pipeline_.ready() &&
        accumulation_step.dispatch_samples == 0u) {
      return;
    }
  }
  const bool try_rt_pipeline =
      rt_pipeline_.ready() &&
      settings.nvidia_rt_core_acceleration &&
      !settings.force_software_fallback;
  const VkPipelineStageFlags dispatch_stages =
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      (try_rt_pipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR : 0u);
  if (!try_rt_pipeline && !rt_fallback_logged_) {
    if (rt_pipeline_.ready() &&
        (!settings.nvidia_rt_core_acceleration ||
         settings.force_software_fallback)) {
      xpbd::log::infof(
          "NVIDIA RT Pipeline acceleration disabled by Renderer setting; "
          "using compatibility ray-query path (mode=%s)",
          rtDebugViewName(params.rt_debug_view));
    } else {
      xpbd::log::warnf(
          "Vulkan RT mode '%s' requested without a ready RT Pipeline; using "
          "compute ray-query fallback",
          rtDebugViewName(params.rt_debug_view));
    }
    rt_fallback_logged_ = true;
  }
  if (use_fallback && !fallback_albedo_cleared_) {
    // One-shot clear of 1x1 white fallback albedo.
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = fallback_albedo_image_;
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);
    VkClearColorValue white{};
    white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] =
        1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, fallback_albedo_image_,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1,
                         &range);
    VkImageMemoryBarrier to_sample{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_sample.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_sample.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_sample.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_sample.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_sample.image = fallback_albedo_image_;
    to_sample.subresourceRange = range;
    to_sample.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_sample.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         dispatch_stages, 0, 0, nullptr, 0,
                         nullptr, 1, &to_sample);
    fallback_albedo_cleared_ = true;
  }

  // Descriptors for this frame's AS + attribute buffers + albedo.
  VkWriteDescriptorSetAccelerationStructureKHR as_info{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  VkAccelerationStructureKHR tlas = scene.tlas();
  as_info.accelerationStructureCount = 1;
  as_info.pAccelerationStructures = &tlas;

  VkDescriptorImageInfo img{};
  img.imageView = image_view_;
  img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo depth_img{};
  depth_img.imageView = depth_image_view_;
  depth_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo aov_img{};
  aov_img.imageView = aov_array_view_;
  aov_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo statistics_img{};
  statistics_img.imageView = statistics_image_view_;
  statistics_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo rr_motion_img{};
  rr_motion_img.imageView = rr_motion_image_view_;
  rr_motion_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo rr_diffuse_albedo_img{};
  rr_diffuse_albedo_img.imageView = rr_diffuse_albedo_image_view_;
  rr_diffuse_albedo_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo rr_specular_albedo_img{};
  rr_specular_albedo_img.imageView = rr_specular_albedo_image_view_;
  rr_specular_albedo_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo rr_normal_roughness_img{};
  rr_normal_roughness_img.imageView = rr_normal_roughness_image_view_;
  rr_normal_roughness_img.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo rr_specular_hit_distance_img{};
  rr_specular_hit_distance_img.imageView =
      rr_specular_hit_distance_image_view_;
  rr_specular_hit_distance_img.imageLayout =
      VK_IMAGE_LAYOUT_GENERAL;

  VkDescriptorBufferInfo nbuf{};
  nbuf.buffer = scene.normalBuffer();
  nbuf.offset = 0;
  nbuf.range = scene.normalBufferBytes();
  VkDescriptorBufferInfo ibuf{};
  ibuf.buffer = scene.indexAttribBuffer();
  ibuf.offset = 0;
  ibuf.range = scene.indexAttribBufferBytes();
  VkDescriptorBufferInfo uvbuf{};
  uvbuf.buffer = scene.uvBuffer();
  uvbuf.offset = 0;
  uvbuf.range = scene.uvBufferBytes();
  VkDescriptorBufferInfo colorbuf{};
  colorbuf.buffer = scene.colorBuffer();
  colorbuf.offset = 0;
  colorbuf.range = scene.colorBufferBytes();
  VkDescriptorBufferInfo flagbuf{};
  flagbuf.buffer = scene.primitiveFlagBuffer();
  flagbuf.offset = 0;
  flagbuf.range = scene.primitiveFlagBufferBytes();
  VkDescriptorBufferInfo tangentbuf{};
  tangentbuf.buffer = scene.tangentBuffer();
  tangentbuf.offset = 0;
  tangentbuf.range = scene.tangentBufferBytes();
  VkDescriptorBufferInfo instancebuf{};
  instancebuf.buffer = scene.instanceMetadataBuffer();
  instancebuf.offset = 0;
  instancebuf.range = scene.instanceMetadataBufferBytes();
  VkDescriptorBufferInfo positionbuf{};
  positionbuf.buffer = scene.positionBuffer();
  positionbuf.offset = 0;
  positionbuf.range = scene.positionBufferBytes();
  VkDescriptorBufferInfo previous_positionbuf{};
  previous_positionbuf.buffer = scene.previousPositionBuffer();
  previous_positionbuf.offset = 0;
  previous_positionbuf.range = scene.previousPositionBufferBytes();
  VkDescriptorBufferInfo instance_motionbuf{};
  instance_motionbuf.buffer = scene.instanceMotionBuffer();
  instance_motionbuf.offset = 0;
  instance_motionbuf.range = scene.instanceMotionBufferBytes();
  VkDescriptorBufferInfo motion_framebuf{};
  motion_framebuf.buffer = motion_frame_buffer_.buffer;
  motion_framebuf.offset = 0;
  motion_framebuf.range = sizeof(PathTraceMotionFrameGpu);

  VkDescriptorImageInfo albedo{};
  albedo.imageView =
      use_fallback ? fallback_albedo_view_ : albedo_view;
  albedo.sampler = albedo_sampler != VK_NULL_HANDLE ? albedo_sampler
                                                    : albedo_sampler_;
  albedo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo normal_map = albedo;
  normal_map.imageView =
      normal_view != VK_NULL_HANDLE ? normal_view : fallback_albedo_view_;
  VkDescriptorImageInfo specular_map = albedo;
  specular_map.imageView =
      specular_view != VK_NULL_HANDLE ? specular_view : fallback_albedo_view_;

  VkWriteDescriptorSet writes[24]{};
  writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].pNext = &as_info;
  writes[0].dstSet = compute_set_;
  writes[0].dstBinding = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

  writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet = compute_set_;
  writes[1].dstBinding = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &img;

  writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet = compute_set_;
  writes[2].dstBinding = 2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[2].pBufferInfo = &nbuf;

  writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet = compute_set_;
  writes[3].dstBinding = 3;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo = &ibuf;

  writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[4].dstSet = compute_set_;
  writes[4].dstBinding = 4;
  writes[4].descriptorCount = 1;
  writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[4].pBufferInfo = &uvbuf;

  writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[5].dstSet = compute_set_;
  writes[5].dstBinding = 5;
  writes[5].descriptorCount = 1;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[5].pImageInfo = &albedo;
  writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[6].dstSet = compute_set_;
  writes[6].dstBinding = 6;
  writes[6].descriptorCount = 1;
  writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[6].pBufferInfo = &colorbuf;

  writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[7].dstSet = compute_set_;
  writes[7].dstBinding = 7;
  writes[7].descriptorCount = 1;
  writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[7].pBufferInfo = &flagbuf;

  writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[8].dstSet = compute_set_;
  writes[8].dstBinding = 8;
  writes[8].descriptorCount = 1;
  writes[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[8].pImageInfo = &depth_img;
  writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[9].dstSet = compute_set_;
  writes[9].dstBinding = 9;
  writes[9].descriptorCount = 1;
  writes[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[9].pBufferInfo = &tangentbuf;

  writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[10].dstSet = compute_set_;
  writes[10].dstBinding = 10;
  writes[10].descriptorCount = 1;
  writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[10].pImageInfo = &normal_map;

  writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[11].dstSet = compute_set_;
  writes[11].dstBinding = 11;
  writes[11].descriptorCount = 1;
  writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[11].pImageInfo = &specular_map;
  writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[12].dstSet = compute_set_;
  writes[12].dstBinding = 12;
  writes[12].descriptorCount = 1;
  writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[12].pBufferInfo = &instancebuf;
  const std::array<VkDescriptorBufferInfo *, 4> motion_buffers{
      &positionbuf, &previous_positionbuf, &instance_motionbuf,
      &motion_framebuf};
  for (std::uint32_t index = 0; index < motion_buffers.size(); ++index) {
    VkWriteDescriptorSet &write = writes[13u + index];
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = compute_set_;
    write.dstBinding = 13u + index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = motion_buffers[index];
  }
  writes[17].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[17].dstSet = compute_set_;
  writes[17].dstBinding = 17;
  writes[17].descriptorCount = 1;
  writes[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[17].pImageInfo = &aov_img;
  writes[18].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[18].dstSet = compute_set_;
  writes[18].dstBinding = 18;
  writes[18].descriptorCount = 1;
  writes[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[18].pImageInfo = &statistics_img;
  writes[19].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[19].dstSet = compute_set_;
  writes[19].dstBinding = 19;
  writes[19].descriptorCount = 1;
  writes[19].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[19].pImageInfo = &rr_motion_img;
  writes[20].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[20].dstSet = compute_set_;
  writes[20].dstBinding = 20;
  writes[20].descriptorCount = 1;
  writes[20].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[20].pImageInfo = &rr_specular_hit_distance_img;
  writes[21].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[21].dstSet = compute_set_;
  writes[21].dstBinding = 21;
  writes[21].descriptorCount = 1;
  writes[21].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[21].pImageInfo = &rr_diffuse_albedo_img;
  writes[22].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[22].dstSet = compute_set_;
  writes[22].dstBinding = 22;
  writes[22].descriptorCount = 1;
  writes[22].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[22].pImageInfo = &rr_specular_albedo_img;
  writes[23].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[23].dstSet = compute_set_;
  writes[23].dstBinding = 23;
  writes[23].descriptorCount = 1;
  writes[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[23].pImageInfo = &rr_normal_roughness_img;
  ComputeDescriptorKey descriptor_key{};
  descriptor_key.tlas = tlas;
  descriptor_key.image_views = {
      img.imageView,
      depth_img.imageView,
      aov_img.imageView,
      statistics_img.imageView,
      rr_motion_img.imageView,
      rr_specular_hit_distance_img.imageView,
      rr_diffuse_albedo_img.imageView,
      rr_specular_albedo_img.imageView,
      rr_normal_roughness_img.imageView,
      albedo.imageView,
      normal_map.imageView,
      specular_map.imageView};
  descriptor_key.image_samplers = {albedo.sampler, normal_map.sampler,
                                   specular_map.sampler};
  descriptor_key.buffers = {
      nbuf.buffer, ibuf.buffer, uvbuf.buffer, colorbuf.buffer, flagbuf.buffer,
      tangentbuf.buffer, instancebuf.buffer, positionbuf.buffer,
      previous_positionbuf.buffer, instance_motionbuf.buffer,
      motion_framebuf.buffer};
  descriptor_key.buffer_sizes = {
      nbuf.range, ibuf.range, uvbuf.range, colorbuf.range, flagbuf.range,
      tangentbuf.range, instancebuf.range, positionbuf.range,
      previous_positionbuf.range, instance_motionbuf.range,
      motion_framebuf.range};
  if (compute_descriptor_key_valid_ &&
      descriptor_key == compute_descriptor_key_) {
    ++descriptor_cache_hits_;
  } else {
    const auto update_begin = std::chrono::steady_clock::now();
    vkUpdateDescriptorSets(device_, 24u, writes, 0, nullptr);
    descriptor_update_ms_ =
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - update_begin)
            .count();
    compute_descriptor_key_ = descriptor_key;
    compute_descriptor_key_valid_ = true;
    descriptor_write_calls_ = 1;
    descriptor_entries_written_ = 24u;
  }

  // Transition color, depth, AOVs and exact-format RR guides to GENERAL.
  std::array<VkImageMemoryBarrier, 9> barriers{};
  auto prepare_store_barrier = [](VkImageMemoryBarrier &barrier,
                                  VkImage image, VkImageLayout old_layout,
                                  std::uint32_t layer_count) {
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_count};
    barrier.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                ? 0
                                : VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  };
  prepare_store_barrier(barriers[0], image_, image_layout_, 1u);
  prepare_store_barrier(barriers[1], depth_image_, depth_image_layout_, 1u);
  prepare_store_barrier(
      barriers[2], aov_image_, aov_image_layout_,
      static_cast<std::uint32_t>(PathTraceAovLayer::Count));
  prepare_store_barrier(barriers[3], statistics_image_,
                        statistics_image_layout_, 1u);
  prepare_store_barrier(
      barriers[4], rr_motion_image_, rr_motion_image_layout_, 1u);
  prepare_store_barrier(
      barriers[5], rr_diffuse_albedo_image_,
      rr_diffuse_albedo_image_layout_, 1u);
  prepare_store_barrier(
      barriers[6], rr_specular_albedo_image_,
      rr_specular_albedo_image_layout_, 1u);
  prepare_store_barrier(
      barriers[7], rr_normal_roughness_image_,
      rr_normal_roughness_image_layout_, 1u);
  prepare_store_barrier(
      barriers[8], rr_specular_hit_distance_image_,
      rr_specular_hit_distance_image_layout_, 1u);
  vkCmdPipelineBarrier(cmd,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       dispatch_stages, 0, 0, nullptr, 0,
                       nullptr, static_cast<std::uint32_t>(barriers.size()),
                       barriers.data());
  image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  depth_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  aov_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  statistics_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  rr_motion_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  rr_diffuse_albedo_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  rr_specular_albedo_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  rr_normal_roughness_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;
  rr_specular_hit_distance_image_layout_ = VK_IMAGE_LAYOUT_GENERAL;

  PathTracePushConstants pc{};
  float view_proj[16]{};
  if (params.view && params.proj) {
    mulMat4(params.proj, params.view, view_proj);
    glMvpToVulkan(view_proj, pc.view_proj);
    if (!invertMatrix4(view_proj, pc.inv_view_proj)) {
      // Identity fallback
      pc.inv_view_proj[0] = pc.inv_view_proj[5] = pc.inv_view_proj[10] =
          pc.inv_view_proj[15] = 1.0f;
    }
    float inv_view[16]{};
    if (invertMatrix4(params.view, inv_view)) {
      pc.camera_pos[0] = inv_view[12];
      pc.camera_pos[1] = inv_view[13];
      pc.camera_pos[2] = inv_view[14];
    }
  } else {
    pc.inv_view_proj[0] = pc.inv_view_proj[5] = pc.inv_view_proj[10] =
        pc.inv_view_proj[15] = 1.0f;
    pc.view_proj[0] = pc.view_proj[5] = pc.view_proj[10] =
        pc.view_proj[15] = 1.0f;
  }
  pc.light_dir_amb[0] = params.light_dir[0];
  pc.light_dir_amb[1] = params.light_dir[1];
  pc.light_dir_amb[2] = params.light_dir[2];
  pc.light_dir_amb[3] = params.ambient;
  pc.light_color_int[0] = params.light_color[0];
  pc.light_color_int[1] = params.light_color[1];
  pc.light_color_int[2] = params.light_color[2];
  pc.light_color_int[3] = params.intensity;
  pc.clear_color[0] = params.clear_r;
  pc.clear_color[1] = params.clear_g;
  pc.clear_color[2] = params.clear_b;
  pc.clear_color[3] = params.exposure;
  pc.size_frame[0] = params.width;
  pc.size_frame[1] = params.height;
  pc.size_frame[2] = params.frame_index;
  constexpr std::uint32_t kDebugViewMask = 0xffu;
  pc.size_frame[3] =
      (static_cast<std::uint32_t>(params.material_debug_view) &
       kDebugViewMask) |
      (params.material_feature_flags << 8u) |
      (params.ray_reconstruction_guides ? 0x80000000u : 0u);
  pc.camera_jitter[0] = params.camera_jitter[0];
  pc.camera_jitter[1] = params.camera_jitter[1];
  pc.camera_jitter[2] =
      params.temporal_reconstruction_input ? 1.0f : 0.0f;
  pc.camera_jitter[3] =
      std::bit_cast<float>(last_output_write_mask_);

  auto *motion_frame =
      static_cast<PathTraceMotionFrameGpu *>(motion_frame_mapped_);
  *motion_frame = {};
  const bool camera_motion_valid =
      params.motion_history_valid && params.previous_view != nullptr &&
      params.previous_proj != nullptr;
  if (camera_motion_valid) {
    float previous_view_projection[16]{};
    mulMat4(params.previous_proj, params.previous_view,
            previous_view_projection);
    glMvpToVulkan(previous_view_projection,
                  motion_frame->previous_view_projection);
  } else {
    std::memcpy(motion_frame->previous_view_projection,
                pc.view_proj,
                sizeof(motion_frame->previous_view_projection));
  }
  motion_frame->info[0] = camera_motion_valid ? 1u : 0u;
  motion_frame->info[1] = scene.motionHistoryValid() ? 1u : 0u;

  bool traced_with_rt_pipeline = false;
  if (try_rt_pipeline) {
    RtPipelineDispatchParams rt_params;
    rt_params.inverse_view_projection = pc.inv_view_proj;
    rt_params.view_projection = pc.view_proj;
    rt_params.camera_position[0] = pc.camera_pos[0];
    rt_params.camera_position[1] = pc.camera_pos[1];
    rt_params.camera_position[2] = pc.camera_pos[2];
    rt_params.width = params.width;
    rt_params.height = params.height;
    rt_params.debug_view = params.rt_debug_view;
    rt_params.sample_base =
        debug_requested ? 0u : accumulation_step.sample_base;
    rt_params.sample_count =
        debug_requested ? 1u : accumulation_step.dispatch_samples;
    rt_params.settings = settings;
    rt_params.material_debug_view =
        static_cast<std::uint32_t>(params.material_debug_view);
    rt_params.material_feature_flags =
        params.material_feature_flags;
    rt_params.temporal_reconstruction_input =
        params.temporal_reconstruction_input;
    rt_params.ray_reconstruction_guides =
        params.ray_reconstruction_guides;
    rt_params.output_write_mask = last_output_write_mask_;
    rt_params.camera_jitter[0] = params.camera_jitter[0];
    rt_params.camera_jitter[1] = params.camera_jitter[1];
    rt_params.frame_index = params.frame_index;
    rt_params.light_direction[0] = params.light_dir[0];
    rt_params.light_direction[1] = params.light_dir[1];
    rt_params.light_direction[2] = params.light_dir[2];
    rt_params.ambient = params.ambient;
    rt_params.light_color[0] = params.light_color[0];
    rt_params.light_color[1] = params.light_color[1];
    rt_params.light_color[2] = params.light_color[2];
    rt_params.light_intensity = params.intensity;
    const bool hdr_environment =
        params.hdr_environment &&
        params.environment_view != VK_NULL_HANDLE &&
        params.environment_sampler != VK_NULL_HANDLE &&
        params.environment_distribution != VK_NULL_HANDLE &&
        params.environment_distribution_bytes > 0u;
    rt_params.hdr_environment = hdr_environment;
    rt_params.environment_view =
        hdr_environment ? params.environment_view : albedo.imageView;
    rt_params.environment_sampler =
        hdr_environment ? params.environment_sampler : albedo.sampler;
    rt_params.environment_distribution =
        hdr_environment ? params.environment_distribution
                        : scene.instanceMetadataBuffer();
    rt_params.environment_distribution_bytes =
        hdr_environment ? params.environment_distribution_bytes
                        : scene.instanceMetadataBufferBytes();
    rt_params.output_view = image_view_;
    rt_params.depth_view = depth_image_view_;
    rt_params.aov_array_view = aov_array_view_;
    rt_params.statistics_view = statistics_image_view_;
    rt_params.rr_motion_view = rr_motion_image_view_;
    rt_params.rr_diffuse_albedo_view = rr_diffuse_albedo_image_view_;
    rt_params.rr_specular_albedo_view = rr_specular_albedo_image_view_;
    rt_params.rr_normal_roughness_view = rr_normal_roughness_image_view_;
    rt_params.rr_specular_hit_distance_view =
        rr_specular_hit_distance_image_view_;
    rt_params.albedo_view = albedo.imageView;
    rt_params.normal_view = normal_map.imageView;
    rt_params.specular_view = specular_map.imageView;
    rt_params.albedo_sampler = albedo.sampler;
    rt_params.motion_frame_buffer = motion_frame_buffer_.buffer;
    rt_params.motion_frame_buffer_bytes =
        sizeof(PathTraceMotionFrameGpu);
    traced_with_rt_pipeline =
        rt_pipeline_.record(cmd, scene, rt_params);
  }
  if (traced_with_rt_pipeline && !debug_requested) {
    history_key_ = accumulation_step.history_key;
    accumulated_samples_ =
        accumulation_step.accumulated_samples_after_dispatch;
    history_valid_ = true;
    if (accumulation_step.history_reset) {
      ++history_reset_count_;
      ++history_generation_;
    }
    if (accumulation_step.history_reset ||
        accumulation_step.maximum_reached) {
      xpbd::log::infof(
          "VKDIAG path_trace slot=%u key=%016llx sample_base=%u "
          "dispatch=%u accumulated=%u maximum=%u reset=%d resets=%llu "
          "viewport=%d,%d,%d,%d",
          params.frame_slot,
          static_cast<unsigned long long>(history_key_),
          accumulation_step.sample_base,
          accumulation_step.dispatch_samples, accumulated_samples_,
          settings.maximum_samples,
          accumulation_step.history_reset ? 1 : 0,
          static_cast<unsigned long long>(history_reset_count_),
          params.viewport_x, params.viewport_y, params.viewport_w,
          params.viewport_h);
    }
  }
  if (!traced_with_rt_pipeline) {
    // The legacy compute ray-query shader does not implement the compatible
    // slot-local average. Its output invalidates any prior RT accumulation.
    history_key_ = 0;
    accumulated_samples_ = 0;
    history_valid_ = false;
    if (try_rt_pipeline && !rt_fallback_logged_) {
      xpbd::log::warnf(
          "Vulkan RT mode '%s' dispatch was rejected; using compute ray-query "
          "fallback",
          rtDebugViewName(params.rt_debug_view));
      rt_fallback_logged_ = true;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      compute_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            compute_pipe_layout_, 0, 1, &compute_set_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, compute_pipe_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    const std::uint32_t gx = (params.width + 7u) / 8u;
    const std::uint32_t gy = (params.height + 7u) / 8u;
    vkCmdDispatch(cmd, gx, gy, 1);
  }

  const VkPipelineStageFlags dispatch_source =
      traced_with_rt_pipeline ? VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR
                              : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  const bool runtime_capture_requested =
      runtime_capture_state_ == PathTraceCaptureState::Requested;
  const bool diagnostic_capture_requested =
      (!capture_path_.empty() || !aov_summary_path_.empty()) &&
      !capture_completed_;
  const bool schedule_capture =
      traced_with_rt_pipeline &&
      (runtime_capture_requested || diagnostic_capture_requested) &&
      !capture_pending_ &&
      params.frame_slot == 0u &&
      (accumulation_step.maximum_reached ||
       (aov_capture_samples_ > 0u &&
        accumulation_step.accumulated_samples_after_dispatch >=
            aov_capture_samples_));
  if (schedule_capture) {
    const bool capture_aovs =
        !runtime_capture_requested && !aov_summary_path_.empty();
    const bool capture_runtime_depth = runtime_capture_requested;
    const VkDeviceSize color_bytes =
        static_cast<VkDeviceSize>(image_w_) * image_h_ * 8u;
    const VkDeviceSize aov_offset = color_bytes;
    const VkDeviceSize statistics_offset =
        aov_offset +
        color_bytes *
            static_cast<VkDeviceSize>(PathTraceAovLayer::Count);
    const VkDeviceSize aov_depth_offset =
        statistics_offset +
        static_cast<VkDeviceSize>(image_w_) * image_h_ * 16u;
    const VkDeviceSize depth_offset =
        capture_aovs ? aov_depth_offset
                     : (capture_runtime_depth ? color_bytes : 0u);
    const VkDeviceSize depth_bytes =
        static_cast<VkDeviceSize>(image_w_) * image_h_ * 4u;
    const VkDeviceSize capture_bytes =
        capture_aovs
            ? aov_depth_offset + depth_bytes
            : (capture_runtime_depth ? color_bytes + depth_bytes
                                     : color_bytes);
    if (ensureCaptureBuffer(capture_bytes)) {
      std::array<VkImageMemoryBarrier, 9> capture_to_transfer = barriers;
      for (VkImageMemoryBarrier &barrier : capture_to_transfer) {
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      }
      const std::uint32_t transfer_barrier_count =
          static_cast<std::uint32_t>(capture_to_transfer.size());
      vkCmdPipelineBarrier(
          cmd, dispatch_source, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
          nullptr, 0, nullptr, transfer_barrier_count,
          capture_to_transfer.data());

      VkBufferImageCopy copy{};
      copy.imageSubresource =
          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {image_w_, image_h_, 1};
      vkCmdCopyImageToBuffer(
          cmd, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          capture_buffer_.buffer, 1, &copy);

      if (capture_aovs) {
        std::array<VkBufferImageCopy,
                   static_cast<std::size_t>(PathTraceAovLayer::Count)>
            aov_copies{};
        for (std::size_t layer = 0; layer < aov_copies.size(); ++layer) {
          aov_copies[layer].bufferOffset =
              aov_offset + static_cast<VkDeviceSize>(layer) * color_bytes;
          aov_copies[layer].imageSubresource = {
              VK_IMAGE_ASPECT_COLOR_BIT, 0,
              static_cast<std::uint32_t>(layer), 1};
          aov_copies[layer].imageExtent = {image_w_, image_h_, 1};
        }
        vkCmdCopyImageToBuffer(
            cmd, aov_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            capture_buffer_.buffer,
            static_cast<std::uint32_t>(aov_copies.size()),
            aov_copies.data());

        VkBufferImageCopy statistics_copy{};
        statistics_copy.bufferOffset = statistics_offset;
        statistics_copy.imageSubresource =
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        statistics_copy.imageExtent = {image_w_, image_h_, 1};
        vkCmdCopyImageToBuffer(
            cmd, statistics_image_,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            capture_buffer_.buffer, 1, &statistics_copy);
      }

      if (capture_aovs || capture_runtime_depth) {
        VkBufferImageCopy depth_copy{};
        depth_copy.bufferOffset = depth_offset;
        depth_copy.imageSubresource =
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        depth_copy.imageExtent = {image_w_, image_h_, 1};
        vkCmdCopyImageToBuffer(
            cmd, depth_image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            capture_buffer_.buffer, 1, &depth_copy);
      }

      std::array<VkImageMemoryBarrier, 9> capture_to_read =
          capture_to_transfer;
      for (VkImageMemoryBarrier &barrier : capture_to_read) {
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      }
      vkCmdPipelineBarrier(
          cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, transfer_barrier_count,
          capture_to_read.data());
      capture_pending_ = true;
      pending_capture_path_ =
          runtime_capture_requested ? runtime_capture_path_ : capture_path_;
      pending_aov_summary_path_ =
          runtime_capture_requested ? std::string{} : aov_summary_path_;
      pending_capture_format_ =
          runtime_capture_requested ? runtime_capture_format_
                                    : StillImageFormat::Png;
      pending_capture_transparent_background_ =
          runtime_capture_requested
              ? runtime_capture_transparent_background_
              : true;
      pending_capture_is_runtime_ = runtime_capture_requested;
      pending_capture_job_id_ =
          runtime_capture_requested ? runtime_capture_job_id_ : 0u;
      if (runtime_capture_requested) {
        pending_capture_background_face_size_ =
            runtime_capture_background_face_size_;
        pending_capture_background_rgba8_ =
            std::move(runtime_capture_background_rgba8_);
        pending_capture_background_inverse_view_projection_ =
            runtime_capture_background_inverse_view_projection_;
        pending_capture_background_camera_position_ =
            runtime_capture_background_camera_position_;
        runtime_capture_background_face_size_ = 0u;
        runtime_capture_background_inverse_view_projection_ = {};
        runtime_capture_background_camera_position_ = {};
        runtime_capture_state_ =
            PathTraceCaptureState::PendingGpuReadback;
      } else {
        pending_capture_background_face_size_ = 0u;
        pending_capture_background_rgba8_.clear();
        pending_capture_background_inverse_view_projection_ = {};
        pending_capture_background_camera_position_ = {};
      }
      pending_capture_width_ = image_w_;
      pending_capture_height_ = image_h_;
      pending_capture_display_.exposure =
          std::isfinite(params.exposure)
              ? std::clamp(params.exposure, 0.0f, 65536.0f)
              : 1.0f;
      pending_capture_display_.white_balance_kelvin =
          settings.white_balance_kelvin;
      pending_capture_display_.bloom_strength =
          settings.bloom_strength;
      pending_capture_display_.tone_mapping =
          static_cast<std::uint32_t>(settings.tone_mapping);
      pending_aov_offset_ = capture_aovs ? aov_offset : 0u;
      pending_statistics_offset_ =
          capture_aovs ? statistics_offset : 0u;
      pending_depth_offset_ =
          capture_aovs || capture_runtime_depth ? depth_offset : 0u;
    } else if (runtime_capture_requested) {
      runtime_capture_error_ = "GPU readback buffer allocation failed";
      runtime_capture_state_ = PathTraceCaptureState::Failed;
    }
  }
  if (!capture_pending_) {
    // GENERAL -> SHADER_READ for composite color/depth sampling.
    for (VkImageMemoryBarrier &barrier : barriers) {
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        cmd, dispatch_source,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(barriers.size()), barriers.data());
  }
  image_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  depth_image_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  aov_image_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  statistics_image_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  rr_motion_image_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  rr_diffuse_albedo_image_layout_ =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  rr_specular_albedo_image_layout_ =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  rr_normal_roughness_image_layout_ =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  rr_specular_hit_distance_image_layout_ =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void VulkanPathTracer::recordComposite(
    VkCommandBuffer cmd, const PathTraceFrameParams &params,
    VkImageView reconstructed_color) {
  if (!ready() || !image_ || !depth_image_) {
    return;
  }
  const bool use_reconstructed =
      reconstructed_color != VK_NULL_HANDLE;
  VkDescriptorImageInfo reconstructed_image{};
  reconstructed_image.imageView =
      use_reconstructed ? reconstructed_color : image_view_;
  reconstructed_image.sampler = sampler_;
  reconstructed_image.imageLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet reconstructed_write{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  reconstructed_write.dstSet = composite_set_;
  reconstructed_write.dstBinding = 2u;
  reconstructed_write.descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  reconstructed_write.descriptorCount = 1;
  reconstructed_write.pImageInfo = &reconstructed_image;
  vkUpdateDescriptorSets(device_, 1u, &reconstructed_write, 0, nullptr);
  VkViewport vp{};
  vp.x = static_cast<float>(params.viewport_x);
  vp.y = static_cast<float>(params.viewport_y);
  vp.width = static_cast<float>((std::max)(1, params.viewport_w));
  vp.height = static_cast<float>((std::max)(1, params.viewport_h));
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;
  VkRect2D sc{};
  sc.offset = {params.viewport_x, params.viewport_y};
  sc.extent = {static_cast<std::uint32_t>((std::max)(1, params.viewport_w)),
               static_cast<std::uint32_t>((std::max)(1, params.viewport_h))};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          composite_pipe_layout_, 0, 1, &composite_set_, 0,
                          nullptr);
  CompositePushConstants composite_push{};
  const PathTraceSettings settings =
      normalizePathTraceSettings(params.settings);
  composite_push.display[0] =
      std::isfinite(params.exposure)
          ? std::clamp(params.exposure, 0.0f, 65536.0f)
          : 1.0f;
  composite_push.display[1] = settings.white_balance_kelvin;
  composite_push.display[2] = settings.bloom_strength;
  composite_push.display[3] =
      static_cast<float>(settings.tone_mapping);
  composite_push.flags[0] =
      (settings.transparent_background ? 1u : 0u) |
      (use_reconstructed ? 2u : 0u) |
      (params.output_requires_srgb_encoding ? 4u : 0u);
  vkCmdPushConstants(cmd, composite_pipe_layout_,
                     VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(composite_push), &composite_push);
  vkCmdDraw(cmd, 3, 1, 0, 0);
}

} // namespace xpbd::gfx
