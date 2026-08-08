#include "vulkan/vulkan_backend_internal.hpp"
#include "xpbd/gfx/labpbr_mip_chain.hpp"
#include "xpbd/gfx/rt_scene_records.hpp"
#include "xpbd/log.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace xpbd::gfx::detail {
namespace {

constexpr std::uint64_t kDefaultStaticRtTransactionPeakBytes =
    std::uint64_t{4} * 1024u * 1024u * 1024u;
constexpr std::uint64_t kStaticRtTransactionSafetyBytes =
    std::uint64_t{64} * 1024u * 1024u;

struct StaticRtTransactionBudget {
  std::uint64_t cpu_persistent_bytes = 0u;
  std::uint64_t cpu_candidate_bytes = 0u;
  std::uint64_t rt_cpu_growth_bytes = 0u;
  std::uint64_t cpu_temporary_bytes = 0u;
  std::uint64_t staging_bytes = 0u;
  std::uint64_t static_gpu_bytes = 0u;
  std::uint64_t new_rt_estimate_bytes = 0u;
  std::uint64_t old_gpu_rt_bytes = 0u;
  std::uint64_t safety_bytes = kStaticRtTransactionSafetyBytes;
  std::uint64_t total_bytes = 0u;
  std::uint64_t maximum_bytes = kDefaultStaticRtTransactionPeakBytes;
};

[[nodiscard]] bool checkedAdd(std::uint64_t value,
                              std::uint64_t &total) noexcept {
  if (value > (std::numeric_limits<std::uint64_t>::max)() - total) {
    return false;
  }
  total += value;
  return true;
}

[[nodiscard]] bool checkedMultiply(std::uint64_t left,
                                   std::uint64_t right,
                                   std::uint64_t &out) noexcept {
  if (left != 0u &&
      right > (std::numeric_limits<std::uint64_t>::max)() / left) {
    return false;
  }
  out = left * right;
  return true;
}

template <typename T>
[[nodiscard]] bool appendVectorBytes(const std::vector<T> &values,
                                     std::uint64_t &total) noexcept {
  std::uint64_t bytes = 0u;
  return checkedMultiply(static_cast<std::uint64_t>(values.capacity()),
                         sizeof(T), bytes) &&
         checkedAdd(bytes, total);
}

[[nodiscard]] bool appendRtRestBytes(const RtRestGeometry &rest,
                                     std::uint64_t &total) noexcept {
  return appendVectorBytes(rest.positions, total) &&
         appendVectorBytes(rest.normals, total) &&
         appendVectorBytes(rest.uvs, total) &&
         appendVectorBytes(rest.tangents, total) &&
         appendVectorBytes(rest.indices, total) &&
         appendVectorBytes(rest.bone_indices, total) &&
         appendVectorBytes(rest.primitive_flags, total) &&
         appendVectorBytes(rest.primitive_metadata, total) &&
         appendVectorBytes(rest.primitive_optics, total) &&
         appendVectorBytes(rest.primitive_emission, total) &&
         appendVectorBytes(rest.geometry_ranges, total);
}

[[nodiscard]] bool appendStringBytes(const std::string &value,
                                     std::uint64_t &total) noexcept {
  return checkedAdd(static_cast<std::uint64_t>(value.capacity()), total);
}

[[nodiscard]] bool appendTextureImageBytes(const TextureImage &image,
                                           std::uint64_t &total) noexcept {
  return appendVectorBytes(image.rgba, total) &&
         appendStringBytes(image.path, total);
}

[[nodiscard]] bool appendStaticMeshBytes(const StaticIndexedModelMesh &mesh,
                                         std::uint64_t &total) noexcept {
  if (!appendVectorBytes(mesh.vertices, total) ||
      !appendVectorBytes(mesh.indices, total) ||
      !appendVectorBytes(mesh.faces, total) ||
      !appendVectorBytes(mesh.bone_names, total)) {
    return false;
  }
  for (const std::string &name : mesh.bone_names) {
    if (!appendStringBytes(name, total)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool appendStaticDrawPlanBytes(
    const StaticModelDrawPlan &plan, std::uint64_t &total) noexcept {
  return appendVectorBytes(plan.indices, total) &&
         appendVectorBytes(plan.primitive_materials, total) &&
         appendVectorBytes(plan.textured_vertices, total);
}

[[nodiscard]] bool appendLabPbrMipChainBytes(
    const LabPbrMipChain &chain, std::uint64_t &total) noexcept {
  if (!appendVectorBytes(chain.levels, total)) {
    return false;
  }
  for (const auto &level : chain.levels) {
    if (!appendVectorBytes(level.rgba, total)) {
      return false;
    }
  }
  return appendStringBytes(chain.stop_reason, total);
}

struct RtSceneBudgetEstimate {
  std::uint64_t gpu_bytes = 0u;
  std::uint64_t cpu_workspace_bytes = 0u;
};

[[nodiscard]] bool estimateRtSceneBudget(
    const RtRestGeometry &rest, bool include_rest_model,
    std::span<const RtColoredGeometryView> colored_geometry,
    RtSceneBudgetEstimate &estimate) noexcept {
  std::uint64_t vertex_count = 0u;
  std::uint64_t triangle_count = 0u;
  std::uint64_t range_count = 0u;
  if (include_rest_model) {
    vertex_count = static_cast<std::uint64_t>(rest.positions.size() / 3u);
    triangle_count = static_cast<std::uint64_t>(rest.indices.size() / 3u);
    range_count = static_cast<std::uint64_t>(rest.geometry_ranges.size());
  }
  for (const RtColoredGeometryView &view : colored_geometry) {
    if (view.vertices == nullptr) {
      continue;
    }
    const std::uint64_t vertices = static_cast<std::uint64_t>(
        view.vertex_count - (view.vertex_count % 3u));
    if (vertices == 0u || !checkedAdd(vertices, vertex_count) ||
        !checkedAdd(vertices / 3u, triangle_count) ||
        !checkedAdd(1u, range_count)) {
      return false;
    }
  }

  // Conservative upper envelopes for the resident RT attributes, AS
  // storage/build scratch, and the CPU packing workspace. The post-build
  // check below replaces these estimates with the driver's actual sizes
  // before publication.
  std::uint64_t bytes = 0u;
  std::uint64_t component = 0u;
  if (!checkedMultiply(vertex_count, 80u, component) ||
      !checkedAdd(component, bytes) ||
      !checkedMultiply(triangle_count, 672u, component) ||
      !checkedAdd(component, bytes) ||
      !checkedMultiply(range_count, 16384u, component) ||
      !checkedAdd(component, bytes)) {
    return false;
  }
  estimate.gpu_bytes = bytes;

  bytes = 0u;
  if (!checkedMultiply(vertex_count, 80u, component) ||
      !checkedAdd(component, bytes) ||
      !checkedMultiply(triangle_count, 160u, component) ||
      !checkedAdd(component, bytes) ||
      !checkedMultiply(range_count, 1024u, component) ||
      !checkedAdd(component, bytes)) {
    return false;
  }
  estimate.cpu_workspace_bytes = bytes;
  return true;
}

[[nodiscard]] std::uint64_t staticRtMaximumPeakBytes() noexcept {
  const char *value = std::getenv("XPBD_A3_STATIC_RT_MAX_PEAK_BYTES");
  if (value == nullptr || value[0] == '\0') {
    return kDefaultStaticRtTransactionPeakBytes;
  }
  char *end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  return end != value && *end == '\0' && parsed > 0u
             ? static_cast<std::uint64_t>(parsed)
             : kDefaultStaticRtTransactionPeakBytes;
}

[[nodiscard]] bool finalizeStaticRtBudget(
    StaticRtTransactionBudget &budget) noexcept {
  budget.total_bytes = 0u;
  const bool valid =
         checkedAdd(budget.cpu_persistent_bytes, budget.total_bytes) &&
         checkedAdd(budget.cpu_candidate_bytes, budget.total_bytes) &&
         checkedAdd(budget.rt_cpu_growth_bytes, budget.total_bytes) &&
         checkedAdd(budget.cpu_temporary_bytes, budget.total_bytes) &&
         checkedAdd(budget.staging_bytes, budget.total_bytes) &&
         checkedAdd(budget.static_gpu_bytes, budget.total_bytes) &&
         checkedAdd(budget.new_rt_estimate_bytes, budget.total_bytes) &&
         checkedAdd(budget.old_gpu_rt_bytes, budget.total_bytes) &&
         checkedAdd(budget.safety_bytes, budget.total_bytes);
  if (!valid) {
    budget.total_bytes = (std::numeric_limits<std::uint64_t>::max)();
    return false;
  }
  return budget.total_bytes <= budget.maximum_bytes;
}

// Developer/fixture-only bridge. The comma-separated value is
// transmission,ior,attenuation-r,g,b,distance,thin(0|1). It is read only when
// static resources rebuild; production defaults remain completely inert.
[[nodiscard]] std::optional<RtSurfaceOptics>
developerRtSurfaceOpticsOverride() noexcept {
  const char *value = std::getenv("XPBD_RT_SURFACE_OPTICS");
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }
  RtSurfaceOptics optics;
  int thin_walled = 0;
  char trailing = '\0';
  const int parsed = std::sscanf(
      value, "%f,%f,%f,%f,%f,%f,%d %c", &optics.transmission, &optics.ior,
      &optics.attenuation_color[0], &optics.attenuation_color[1],
      &optics.attenuation_color[2], &optics.attenuation_distance,
      &thin_walled, &trailing);
  if (parsed != 7 || (thin_walled != 0 && thin_walled != 1)) {
    xpbd::log::warn(
        "Ignoring invalid XPBD_RT_SURFACE_OPTICS developer override");
    return std::nullopt;
  }
  optics.thin_walled = thin_walled != 0;
  xpbd::log::info(
      "Applying XPBD_RT_SURFACE_OPTICS developer fixture override");
  return normalizeRtSurfaceOptics(optics);
}

[[nodiscard]] bool buildStaticRtRestCandidate(
    const StaticIndexedModelMesh &mesh, const StaticModelDrawPlan &draw_plan,
    const ResolvedMaterialTable *material, RtRestGeometry &out,
    std::string &error) {
  error.clear();
  RtRestGeometry candidate;
  std::uint64_t position_values = 0u;
  std::uint64_t uv_values = 0u;
  std::uint64_t tangent_values = 0u;
  if (!checkedMultiply(static_cast<std::uint64_t>(mesh.vertices.size()), 3u,
                       position_values) ||
      !checkedMultiply(static_cast<std::uint64_t>(mesh.vertices.size()), 2u,
                       uv_values) ||
      !checkedMultiply(static_cast<std::uint64_t>(mesh.vertices.size()), 4u,
                       tangent_values) ||
      position_values > (std::numeric_limits<std::size_t>::max)() ||
      uv_values > (std::numeric_limits<std::size_t>::max)() ||
      tangent_values > (std::numeric_limits<std::size_t>::max)()) {
    error = "Vulkan RT Candidate vertex extent overflows host addressing";
    return false;
  }
  try {
    candidate.positions.resize(static_cast<std::size_t>(position_values));
    candidate.normals.resize(static_cast<std::size_t>(position_values));
    candidate.uvs.resize(static_cast<std::size_t>(uv_values));
    candidate.tangents.resize(static_cast<std::size_t>(tangent_values));
    candidate.bone_indices.resize(mesh.vertices.size());
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
      candidate.positions[i * 3u + 0u] = mesh.vertices[i].px;
      candidate.positions[i * 3u + 1u] = mesh.vertices[i].py;
      candidate.positions[i * 3u + 2u] = mesh.vertices[i].pz;
      candidate.normals[i * 3u + 0u] = mesh.vertices[i].nx;
      candidate.normals[i * 3u + 1u] = mesh.vertices[i].ny;
      candidate.normals[i * 3u + 2u] = mesh.vertices[i].nz;
      candidate.uvs[i * 2u + 0u] = mesh.vertices[i].u;
      candidate.uvs[i * 2u + 1u] = mesh.vertices[i].v;
      candidate.tangents[i * 4u + 0u] = mesh.vertices[i].tx;
      candidate.tangents[i * 4u + 1u] = mesh.vertices[i].ty;
      candidate.tangents[i * 4u + 2u] = mesh.vertices[i].tz;
      candidate.tangents[i * 4u + 3u] =
          mesh.vertices[i].tangent_handedness;
      candidate.bone_indices[i] = mesh.vertices[i].bone_index;
    }

    const std::optional<RtSurfaceOptics> optics_override =
        developerRtSurfaceOpticsOverride();
    const RtSceneRecords scene_records = buildRigidModelRtSceneRecords(
        mesh, draw_plan, material,
        optics_override ? &*optics_override : nullptr);
    if (!scene_records.valid()) {
      error = "Vulkan RT scene-record construction failed";
      return false;
    }
    const RtPackedPrimitiveLayout packed_layout =
        buildRtPackedPrimitiveLayout(draw_plan, scene_records);
    if (!packed_layout.valid()) {
      error = "Vulkan RT primitive packing failed";
      return false;
    }
    const auto primitive_flags = [&](std::size_t primitive) {
      std::uint32_t flags = 0u;
      if (primitive < draw_plan.primitive_materials.size()) {
        const StaticModelPrimitiveMaterial &primitive_material =
            draw_plan.primitive_materials[primitive];
        if (primitive_material.textured) {
          flags |= kRtPrimitiveTextured;
        }
        switch (primitive_material.material) {
        case StaticModelMaterialClass::Cutout:
          flags |= kRtPrimitiveCutout;
          break;
        case StaticModelMaterialClass::Blend:
          flags |= kRtPrimitiveBlend;
          break;
        case StaticModelMaterialClass::Opaque:
        default:
          break;
        }
      }
      return flags;
    };

    candidate.indices = packed_layout.indices;
    candidate.geometry_ranges = packed_layout.geometry_ranges;
    candidate.primitive_flags.reserve(
        packed_layout.source_primitive_indices.size());
    candidate.primitive_metadata.reserve(
        packed_layout.source_primitive_indices.size());
    candidate.primitive_optics.reserve(
        packed_layout.source_primitive_indices.size());
    candidate.primitive_emission.reserve(
        packed_layout.source_primitive_indices.size());
    const float emission_alias_support_floor =
        labPbrEmissionAliasSupportFloor(material);
    for (std::size_t packed_primitive = 0u;
         packed_primitive < packed_layout.source_primitive_indices.size();
         ++packed_primitive) {
      const std::uint32_t primitive =
          packed_layout.source_primitive_indices[packed_primitive];
      const std::uint32_t packed_flags = primitive_flags(primitive);
      candidate.primitive_flags.push_back(packed_flags);
      if (primitive >= scene_records.primitives.size()) {
        error = "Vulkan RT primitive metadata packing failed";
        return false;
      }
      const RtPrimitiveRecord &record = scene_records.primitives[primitive];
      candidate.primitive_metadata.push_back(
          {record.cube_index,
           static_cast<std::uint32_t>(record.face_direction),
           record.material_index, record.primitive_index});
      candidate.primitive_optics.push_back(
          record.material_index < scene_records.materials.size()
              ? scene_records.materials[record.material_index].surface_optics
              : RtSurfaceOptics{});

      std::array<float, 3> average_emission{};
      if (material != nullptr && material->valid() &&
          material->specular_map_active &&
          packed_primitive * 3u + 2u < candidate.indices.size()) {
        const std::uint32_t i0 =
            candidate.indices[packed_primitive * 3u + 0u];
        const std::uint32_t i1 =
            candidate.indices[packed_primitive * 3u + 1u];
        const std::uint32_t i2 =
            candidate.indices[packed_primitive * 3u + 2u];
        if (i0 < mesh.vertices.size() && i1 < mesh.vertices.size() &&
            i2 < mesh.vertices.size()) {
          const std::array<std::array<float, 2>, 4> uv_samples{{
              {mesh.vertices[i0].u, mesh.vertices[i0].v},
              {mesh.vertices[i1].u, mesh.vertices[i1].v},
              {mesh.vertices[i2].u, mesh.vertices[i2].v},
              {(mesh.vertices[i0].u + mesh.vertices[i1].u +
                mesh.vertices[i2].u) /
                   3.0f,
               (mesh.vertices[i0].v + mesh.vertices[i1].v +
                mesh.vertices[i2].v) /
                   3.0f},
          }};
          for (const auto &uv : uv_samples) {
            const ResolvedMaterialTexel &texel =
                material->sample(uv[0], uv[1]);
            const float coverage = labPbrEmissionCoverageWeight(
                texel.opacity,
                (packed_flags & kRtPrimitiveCutout) != 0u,
                (packed_flags & kRtPrimitiveBlend) != 0u);
            for (std::size_t channel = 0u; channel < 3u; ++channel) {
              average_emission[channel] +=
                  texel.emission_linear[channel] * coverage * 0.25f;
            }
          }
        }
      }
      if (record.uses_emission_texture &&
          emission_alias_support_floor > 0.0f &&
          average_emission[0] == 0.0f &&
          average_emission[1] == 0.0f &&
          average_emission[2] == 0.0f) {
        average_emission.fill(emission_alias_support_floor);
      }
      candidate.primitive_emission.push_back(average_emission);
    }
  } catch (const std::exception &exception) {
    error = std::string("Vulkan RT Candidate construction failed: ") +
            exception.what();
    return false;
  } catch (...) {
    error = "Vulkan RT Candidate construction failed";
    return false;
  }
  out = std::move(candidate);
  return true;
}

} // namespace

void VulkanBackend::destroyStaticModelResources() {
    discardStaticAssetPending("static-resource-destroy");
    if (gpu_completion_unproven_) {
      return;
    }
    bool retirement_complete = false;
    if (!pollStaticAssetRetirement(true, retirement_complete) ||
        !retirement_complete) {
      return;
    }
    destroyBuffer(static_model_vbo_);
    destroyBuffer(static_model_ibo_);
    destroyImage(static_texture_);
    destroyImage(static_normal_texture_);
    destroyImage(static_specular_texture_);
    static_draw_plan_ = {};
    static_generations_ = {};
    static_bone_count_ = 0;
    static_vertex_bytes_ = 0;
    static_index_bytes_ = 0;
    static_texture_logical_bytes_ = 0;
    static_model_ready_ = false;
  }

bool VulkanBackend::createStaticTexture(std::uint32_t width, std::uint32_t height,
                           VkFormat format, ImageResource &out,
                           std::uint32_t mip_levels) {
    const std::uint32_t maximum_dimension = std::max(width, height);
    std::uint32_t maximum_mip_levels = 1u;
    for (std::uint32_t dimension = maximum_dimension;
         dimension > 1u; dimension >>= 1u) {
      ++maximum_mip_levels;
    }
    mip_levels = std::clamp(mip_levels, 1u, maximum_mip_levels);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &image_info, nullptr, &out.image) !=
        VK_SUCCESS) {
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, out.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    const auto memory_type =
        findMemoryType(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      destroyImage(out);
      return false;
    }
    allocation.memoryTypeIndex = *memory_type;
    if (vkAllocateMemory(device_, &allocation, nullptr, &out.memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(device_, out.image, out.memory, 0) != VK_SUCCESS) {
      destroyImage(out);
      return false;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
    if (vkCreateImageView(device_, &view_info, nullptr, &out.view) !=
        VK_SUCCESS) {
      destroyImage(out);
      return false;
    }
    out.width = width;
    out.height = height;
    out.mip_levels = mip_levels;
    return true;
  }

void VulkanBackend::destroyStaticMaterialSamplers() {
    if (static_specular_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, static_specular_sampler_, nullptr);
      static_specular_sampler_ = VK_NULL_HANDLE;
    }
    if (static_normal_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, static_normal_sampler_, nullptr);
      static_normal_sampler_ = VK_NULL_HANDLE;
    }
    if (static_albedo_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, static_albedo_sampler_, nullptr);
      static_albedo_sampler_ = VK_NULL_HANDLE;
    }
  }

void VulkanBackend::updateStaticTextureDescriptors(FrameSync &frame) {
    if (frame.static_descriptor_revision ==
        static_descriptor_revision_) {
      return;
    }
    std::array<VkDescriptorImageInfo, 3> image_infos{};
    const std::array<VkImageView, 3> views{
        static_texture_.view, static_normal_texture_.view,
        static_specular_texture_.view};
    const std::array<VkSampler, 3> samplers{
        static_albedo_sampler_, static_normal_sampler_,
        static_specular_sampler_};
    for (std::size_t i = 0; i < image_infos.size(); ++i) {
      image_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      image_infos[i].imageView = views[i];
      image_infos[i].sampler = samplers[i];
    }
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (std::size_t image = 0; image < image_infos.size(); ++image) {
      auto &write = writes[image];
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = frame.static_descriptor_set;
      write.dstBinding = static_cast<std::uint32_t>(1u + image);
      write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      write.descriptorCount = 1;
      write.pImageInfo = &image_infos[image];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      if (&frames_[i] != &frame) {
        continue;
      }
      if (static_rt_desc_layout_ && static_rt_descriptor_sets_[i]) {
        std::array<VkWriteDescriptorSet, 3> rt_writes{};
        constexpr std::array<std::uint32_t, 3> kRtBindings{1u, 3u, 4u};
        for (std::size_t image = 0; image < image_infos.size(); ++image) {
          auto &write = rt_writes[image];
          write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write.dstSet = static_rt_descriptor_sets_[i];
          write.dstBinding = kRtBindings[image];
          write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          write.descriptorCount = 1;
          write.pImageInfo = &image_infos[image];
        }
        vkUpdateDescriptorSets(device_,
                               static_cast<std::uint32_t>(rt_writes.size()),
                               rt_writes.data(), 0, nullptr);
      }
      break;
    }
    frame.static_descriptor_revision = static_descriptor_revision_;
  }

void VulkanBackend::updateStaticBoneDescriptor(FrameSync &frame) {
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = frame.bone_ssbo.buffer;
    buffer_info.offset = 0;
    buffer_info.range = frame.bone_ssbo.capacity;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = frame.static_descriptor_set;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount = 1;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    // Match frame index for RT descriptor set.
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      if (&frames_[i] != &frame) {
        continue;
      }
      if (static_rt_descriptor_sets_[i]) {
        write.dstSet = static_rt_descriptor_sets_[i];
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      }
      break;
    }
  }

bool VulkanBackend::resetRtSceneCandidate(std::size_t slot,
                                          const char *reason) noexcept {
  if (slot >= rt_scene_candidates_.size() ||
      !rt_scene_candidates_[slot]) {
    return false;
  }
  try {
    rt_scene_candidates_[slot]->shutdown();
    const bool initialized = rt_scene_candidates_[slot]->init(
        phys_, device_, graphics_family_, graphics_queue_);
    if (diagnostics_enabled_ && initialized) {
      xpbd::log::infof(
          "VKDIAG rt_candidate_discard slot=%zu initialized=%d reason=%s",
          slot, initialized ? 1 : 0,
          reason != nullptr ? reason : "unspecified");
    } else if (!initialized) {
      xpbd::log::warnf(
          "VKDIAG rt_candidate_discard slot=%zu initialized=0 reason=%s",
          slot, reason != nullptr ? reason : "unspecified");
    }
    return initialized;
  } catch (...) {
    writeLog("Vulkan RT Candidate discard/reinitialize failed");
    return false;
  }
}

bool VulkanBackend::prepareRtSceneCandidate(
    std::size_t slot, const RtSceneSynchronousUpdate &update,
    RtRestGeometry *replacement_rest, bool wait_for_completion,
    std::uint64_t additional_resident_bytes) {
  if (slot >= rt_scene_candidates_.size() ||
      !rt_scene_candidates_[slot]) {
    writeLog("Vulkan RT Candidate slot is unavailable");
    return false;
  }
  VulkanRtScene &candidate = *rt_scene_candidates_[slot];
  bool keep_candidate = false;
  auto discard_on_failure = [&] {
    if (!keep_candidate) {
      resetRtSceneCandidate(slot, "prepare-or-budget-failure");
    }
  };
  using DiscardCallback = decltype(discard_on_failure);
  struct DiscardGuard {
    DiscardCallback &callback;
    ~DiscardGuard() { callback(); }
  } discard_guard{discard_on_failure};
  if (replacement_rest != nullptr) {
    candidate.setRestGeometry(std::move(*replacement_rest));
  }
  const RtSceneStats before = candidate.stats();
  const auto transaction_begin = Clock::now();
  try {
    if (!candidate.updateGeometry(
            update.bone_transforms, update.bone_count, update.bone_tints,
            update.tint_count, update.colored_geometry,
            update.include_rest_model, update.previous_packed_positions,
            update.previous_bone_transforms, update.previous_bone_count,
            update.explicit_motion_history_valid, update.generations,
            update.generations_valid)) {
      xpbd::log::warnf("Vulkan RT Candidate update failed: slot=%zu %s",
                       slot, candidate.lastUpdateFailureReason());
      return false;
    }
    const bool build_started =
        wait_for_completion ? candidate.buildPendingAndWait()
                            : candidate.submitPendingBuild();
    if (!build_started) {
      xpbd::log::warnf("Vulkan RT Candidate build failed: slot=%zu %s",
                       slot, candidate.lastUpdateFailureReason());
      if (candidate.completionUnproven()) {
        keep_candidate = true;
        markGpuCompletionUnproven(
            "vkWaitForFences(static_rt_candidate)");
      }
      return false;
    }
  } catch (const std::bad_alloc &) {
    xpbd::log::warnf(
        "Vulkan RT Candidate CPU allocation failed: slot=%zu", slot);
    return false;
  } catch (const std::exception &exception) {
    xpbd::log::warnf("Vulkan RT Candidate construction failed: slot=%zu %s",
                     slot, exception.what());
    return false;
  } catch (...) {
    xpbd::log::warnf(
        "Vulkan RT Candidate construction failed: slot=%zu", slot);
    return false;
  }
  const RtSceneStats after = candidate.stats();
  std::uint64_t input_bytes = 0u;
  std::uint64_t static_cpu_bytes = 0u;
  std::uint64_t static_gpu_bytes = 0u;
  std::uint64_t scene_bytes = 0u;
  std::uint64_t component = 0u;
  bool resident_budget_valid =
      checkedMultiply(static_cast<std::uint64_t>(update.bone_count),
                      16u * sizeof(float), component) &&
      checkedAdd(component, input_bytes) &&
      checkedMultiply(static_cast<std::uint64_t>(update.tint_count),
                      4u * sizeof(float), component) &&
      checkedAdd(component, input_bytes) &&
      checkedMultiply(
          static_cast<std::uint64_t>(update.previous_packed_positions.size()),
          sizeof(float), component) &&
      checkedAdd(component, input_bytes) &&
      checkedMultiply(
          static_cast<std::uint64_t>(update.previous_bone_count),
          16u * sizeof(float), component) &&
      checkedAdd(component, input_bytes);
  for (const RtColoredGeometryView &view : update.colored_geometry) {
    resident_budget_valid =
        resident_budget_valid &&
        checkedMultiply(static_cast<std::uint64_t>(view.vertex_count),
                        sizeof(MeshVertex), component) &&
        checkedAdd(component, input_bytes);
  }
  resident_budget_valid =
      resident_budget_valid &&
      appendStaticDrawPlanBytes(static_draw_plan_, static_cpu_bytes) &&
      checkedAdd(static_vertex_bytes_, static_gpu_bytes) &&
      checkedAdd(static_index_bytes_, static_gpu_bytes) &&
      checkedAdd(static_texture_logical_bytes_, static_gpu_bytes);
  for (std::size_t i = 0u; i < rt_scenes_.size(); ++i) {
    if (rt_scenes_[i]) {
      const RtSceneStats current = rt_scenes_[i]->stats();
      resident_budget_valid =
          resident_budget_valid &&
          checkedAdd(current.allocated_bytes, scene_bytes) &&
          checkedAdd(current.cpu_allocated_bytes, scene_bytes);
    }
    if (rt_scene_candidates_[i]) {
      const RtSceneStats scratch = rt_scene_candidates_[i]->stats();
      resident_budget_valid =
          resident_budget_valid &&
          checkedAdd(scratch.allocated_bytes, scene_bytes) &&
          checkedAdd(scratch.cpu_allocated_bytes, scene_bytes);
    }
  }
  resident_budget_valid =
      resident_budget_valid &&
      checkedAdd(additional_resident_bytes, scene_bytes);
  std::uint64_t resident_total = 0u;
  resident_budget_valid =
      resident_budget_valid && checkedAdd(input_bytes, resident_total) &&
      checkedAdd(static_cpu_bytes, resident_total) &&
      checkedAdd(static_gpu_bytes, resident_total) &&
      checkedAdd(scene_bytes, resident_total) &&
      checkedAdd(kStaticRtTransactionSafetyBytes, resident_total);
  if (!resident_budget_valid) {
    resident_total = (std::numeric_limits<std::uint64_t>::max)();
  }
  const std::uint64_t resident_maximum = staticRtMaximumPeakBytes();
  const bool resident_budget_accepted =
      resident_budget_valid && resident_total <= resident_maximum;
  if (diagnostics_enabled_ || !resident_budget_accepted) {
    const char *result = resident_budget_accepted ? "accepted" : "rejected";
    if (resident_budget_accepted) {
      xpbd::log::infof(
          "VKDIAG rt_resident_budget frame=%llu slot=%zu result=%s "
          "input=%llu static_cpu=%llu static_gpu=%llu scenes=%llu "
          "safety=%llu total=%llu maximum=%llu",
          static_cast<unsigned long long>(diagnostic_context_.render_frame),
          slot, result, static_cast<unsigned long long>(input_bytes),
          static_cast<unsigned long long>(static_cpu_bytes),
          static_cast<unsigned long long>(static_gpu_bytes),
          static_cast<unsigned long long>(scene_bytes),
          static_cast<unsigned long long>(kStaticRtTransactionSafetyBytes),
          static_cast<unsigned long long>(resident_total),
          static_cast<unsigned long long>(resident_maximum));
    } else {
      xpbd::log::warnf(
          "VKDIAG rt_resident_budget frame=%llu slot=%zu result=%s "
          "input=%llu static_cpu=%llu static_gpu=%llu scenes=%llu "
          "safety=%llu total=%llu maximum=%llu",
          static_cast<unsigned long long>(diagnostic_context_.render_frame),
          slot, result, static_cast<unsigned long long>(input_bytes),
          static_cast<unsigned long long>(static_cpu_bytes),
          static_cast<unsigned long long>(static_gpu_bytes),
          static_cast<unsigned long long>(scene_bytes),
          static_cast<unsigned long long>(kStaticRtTransactionSafetyBytes),
          static_cast<unsigned long long>(resident_total),
          static_cast<unsigned long long>(resident_maximum));
    }
  }
  if (!resident_budget_accepted) {
    writeLog("Vulkan RT Candidate exceeds resident peak budget");
    return false;
  }
  const bool candidate_ready = candidate.ready();
  if (!candidate_ready) {
    writeLog("Vulkan RT Candidate is incomplete after build submission");
    return false;
  }
  if (diagnostics_enabled_) {
    xpbd::log::infof(
        "VKDIAG rt_candidate frame=%llu slot=%zu elapsed_ms=%.4f "
        "blas_full_delta=%llu blas_refit_delta=%llu "
        "tlas_full_delta=%llu tlas_update_delta=%llu "
        "allocated=%llu reason=%s",
        static_cast<unsigned long long>(diagnostic_context_.render_frame),
        slot,
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  transaction_begin)
            .count(),
        static_cast<unsigned long long>(after.full_builds >= before.full_builds
                                            ? after.full_builds -
                                                  before.full_builds
                                            : 0u),
        static_cast<unsigned long long>(after.refits >= before.refits
                                            ? after.refits - before.refits
                                            : 0u),
        static_cast<unsigned long long>(
            after.tlas_full_builds >= before.tlas_full_builds
                ? after.tlas_full_builds - before.tlas_full_builds
                : 0u),
        static_cast<unsigned long long>(
            after.tlas_updates >= before.tlas_updates
                ? after.tlas_updates - before.tlas_updates
                : 0u),
        static_cast<unsigned long long>(after.allocated_bytes),
        rtAccelerationBuildReasonName(after.last_build_reason));
  }
  keep_candidate = true;
  return true;
}

void VulkanBackend::publishRtSceneCandidate(
    std::size_t slot, std::uint64_t scene_hash) noexcept {
  if (slot >= rt_scenes_.size() || !rt_scenes_[slot] ||
      !rt_scene_candidates_[slot]) {
    return;
  }
  std::swap(rt_scenes_[slot], rt_scene_candidates_[slot]);
  rt_scene_built_[slot] = true;
  last_rt_scene_hash_[slot] = scene_hash;
}

bool VulkanBackend::prepareStaticPendingRtCandidates() {
  StaticAssetPending &pending = static_asset_pending_;
  if (!rt_capability_.device_extensions_enabled) {
    return true;
  }
  try {
    for (std::size_t i = 0u; i < pending.rt_candidates.size(); ++i) {
      if (pending.rt_candidates[i]) {
        continue;
      }
      auto replacement = std::make_unique<VulkanRtScene>();
      replacement->setWaitControl(render_thread_control_);
      if (!replacement->init(phys_, device_, graphics_family_,
                             graphics_queue_)) {
        writeLog("Vulkan static RT Candidate initialization failed");
        pending.rt_candidates = {};
        return false;
      }
      pending.rt_candidates[i] = std::move(replacement);
    }
  } catch (const std::bad_alloc &) {
    writeLog("Vulkan static RT Candidate allocation failed");
    pending.rt_candidates = {};
    return false;
  } catch (...) {
    writeLog("Vulkan static RT Candidate construction failed");
    pending.rt_candidates = {};
    return false;
  }
  return true;
}

void VulkanBackend::destroyStaticAssetRetired() noexcept {
  StaticAssetRetired &retired = static_asset_retired_;
  if (retired.completion_fence != VK_NULL_HANDLE) {
    return;
  }
  destroyBuffer(retired.vertex);
  destroyBuffer(retired.index);
  destroyImage(retired.albedo);
  destroyImage(retired.normal);
  destroyImage(retired.specular);
  if (retired.specular_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, retired.specular_sampler, nullptr);
    retired.specular_sampler = VK_NULL_HANDLE;
  }
  if (retired.normal_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, retired.normal_sampler, nullptr);
    retired.normal_sampler = VK_NULL_HANDLE;
  }
  if (retired.albedo_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, retired.albedo_sampler, nullptr);
    retired.albedo_sampler = VK_NULL_HANDLE;
  }
  for (auto &scene : retired.rt_scenes) {
    if (scene) {
      scene->shutdown();
      scene.reset();
    }
  }
  if (diagnostics_enabled_ &&
      (retired.model_generation != 0u ||
       retired.texture_generation != 0u)) {
    xpbd::log::infof(
        "VKDIAG static_retirement_reclaimed model_generation=%llu "
        "texture_generation=%llu elapsed_ms=%.4f",
        static_cast<unsigned long long>(retired.model_generation),
        static_cast<unsigned long long>(retired.texture_generation),
        std::chrono::duration<double, std::milli>(
            Clock::now() - retired.submitted_at)
            .count());
  }
  retired = {};
}

bool VulkanBackend::pollStaticAssetRetirement(
    bool wait_for_completion, bool &complete) {
  complete = !static_asset_retired_.active();
  if (complete) {
    return true;
  }
  if (gpu_completion_unproven_ || quarantine_required_) {
    return false;
  }
  VkResult status = VK_NOT_READY;
  if (wait_for_completion) {
    const ControlledWaitResult wait = waitForFenceControlled(
        static_asset_retired_.completion_fence,
        "vkWaitForFences.static_retirement", UINT32_MAX, VK_NULL_HANDLE,
        VK_NULL_HANDLE, true, true);
    if (!wait.completed()) {
      return false;
    }
    status = VK_SUCCESS;
  } else {
    status = vkGetFenceStatus(device_,
                              static_asset_retired_.completion_fence);
    if (status == VK_NOT_READY) {
      return true;
    }
  }
  if (status != VK_SUCCESS) {
    markGpuCompletionUnproven("vkGetFenceStatus.static_retirement");
    return false;
  }
  vkDestroyFence(device_, static_asset_retired_.completion_fence, nullptr);
  static_asset_retired_.completion_fence = VK_NULL_HANDLE;
  destroyStaticAssetRetired();
  complete = true;
  return true;
}

bool VulkanBackend::beginStaticAssetRetirement() {
  if (!static_model_ready_ &&
      !rt_capability_.device_extensions_enabled) {
    return true;
  }
  if (static_asset_retired_.active()) {
    writeLog("Vulkan static retirement slot is still active");
    return false;
  }
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  if (vkCreateFence(device_, &fence_info, nullptr, &fence) != VK_SUCCESS) {
    writeLog("Vulkan static retirement fence creation failed");
    return false;
  }
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  const VkResult result = vkQueueSubmit(graphics_queue_, 1, &submit, fence);
  if (result != VK_SUCCESS) {
    vkDestroyFence(device_, fence, nullptr);
    if (result == VK_ERROR_DEVICE_LOST) {
      recordFatalVulkanError("vkQueueSubmit.static_retirement", result);
    } else {
      writeLog("Vulkan static retirement marker submission failed");
    }
    return false;
  }

  StaticAssetRetired &retired = static_asset_retired_;
  retired.vertex = static_model_vbo_;
  retired.index = static_model_ibo_;
  retired.albedo = static_texture_;
  retired.normal = static_normal_texture_;
  retired.specular = static_specular_texture_;
  retired.albedo_sampler = static_albedo_sampler_;
  retired.normal_sampler = static_normal_sampler_;
  retired.specular_sampler = static_specular_sampler_;
  retired.completion_fence = fence;
  retired.model_generation = static_generations_.model;
  retired.texture_generation = static_generations_.texture;
  retired.submitted_at = Clock::now();
  static_model_vbo_ = {};
  static_model_ibo_ = {};
  static_texture_ = {};
  static_normal_texture_ = {};
  static_specular_texture_ = {};
  static_albedo_sampler_ = VK_NULL_HANDLE;
  static_normal_sampler_ = VK_NULL_HANDLE;
  static_specular_sampler_ = VK_NULL_HANDLE;
  if (rt_capability_.device_extensions_enabled) {
    for (std::size_t i = 0u; i < rt_scenes_.size(); ++i) {
      retired.rt_scenes[i] = std::move(rt_scenes_[i]);
    }
  }
  if (diagnostics_enabled_) {
    xpbd::log::infof(
        "VKDIAG static_retirement_submitted model_generation=%llu "
        "texture_generation=%llu fence=0x%llx",
        static_cast<unsigned long long>(retired.model_generation),
        static_cast<unsigned long long>(retired.texture_generation),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(fence)));
  }
  return true;
}

void VulkanBackend::discardStaticAssetPending(
    const char *reason) noexcept {
  StaticAssetPending &pending = static_asset_pending_;
  if (!pending.active || gpu_completion_unproven_) {
    return;
  }

  if (pending.upload_fence != VK_NULL_HANDLE) {
    const ControlledWaitResult wait = waitForFenceControlled(
        pending.upload_fence, "vkWaitForFences.static_pending_discard",
        UINT32_MAX, VK_NULL_HANDLE, pending.upload_command, true, true);
    if (!wait.completed()) {
      markGpuCompletionUnproven(
          "vkWaitForFences.static_pending_discard");
      return;
    }
    vkDestroyFence(device_, pending.upload_fence, nullptr);
    pending.upload_fence = VK_NULL_HANDLE;
    if (pending.upload_command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, cmd_pool_, 1,
                           &pending.upload_command);
      pending.upload_command = VK_NULL_HANDLE;
    }
    destroyBuffer(pending.staging);
  }

  for (std::size_t i = 0u; i < pending.rt_candidates.size(); ++i) {
    if (!pending.rt_candidate_submitted[i] ||
        !pending.rt_candidates[i]) {
      continue;
    }
    const RtScenePendingBuildState state =
        pending.rt_candidates[i]->pollPendingBuild(true);
    if (state == RtScenePendingBuildState::PendingFence ||
        state == RtScenePendingBuildState::CompletionUnproven) {
      markGpuCompletionUnproven(
          "vkWaitForFences(static_rt_pending_discard)");
      return;
    }
  }

  destroyBuffer(pending.staging);
  destroyBuffer(pending.vertex);
  destroyBuffer(pending.index);
  destroyImage(pending.albedo);
  destroyImage(pending.normal);
  destroyImage(pending.specular);
  if (pending.specular_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, pending.specular_sampler, nullptr);
    pending.specular_sampler = VK_NULL_HANDLE;
  }
  if (pending.normal_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, pending.normal_sampler, nullptr);
    pending.normal_sampler = VK_NULL_HANDLE;
  }
  if (pending.albedo_sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device_, pending.albedo_sampler, nullptr);
    pending.albedo_sampler = VK_NULL_HANDLE;
  }
  if (diagnostics_enabled_) {
    xpbd::log::infof(
        "VKDIAG static_pending_discard model_generation=%llu "
        "texture_generation=%llu reason=%s",
        static_cast<unsigned long long>(pending.model_generation),
        static_cast<unsigned long long>(pending.texture_generation),
        reason != nullptr ? reason : "unspecified");
  }
  pending = {};
}

bool VulkanBackend::commitStaticAssetPending(
    std::uint64_t &uploaded_bytes) {
  uploaded_bytes = 0u;
  StaticAssetPending &pending = static_asset_pending_;
  if (!pending.active || pending.upload_fence != VK_NULL_HANDLE) {
    return false;
  }
  const bool replacing_existing = static_asset_retired_.active();

  destroyBuffer(static_model_vbo_);
  destroyBuffer(static_model_ibo_);
  destroyImage(static_texture_);
  destroyImage(static_normal_texture_);
  destroyImage(static_specular_texture_);
  destroyStaticMaterialSamplers();
  static_model_vbo_ = pending.vertex;
  static_model_ibo_ = pending.index;
  static_texture_ = pending.albedo;
  static_normal_texture_ = pending.normal;
  static_specular_texture_ = pending.specular;
  static_albedo_sampler_ = pending.albedo_sampler;
  static_normal_sampler_ = pending.normal_sampler;
  static_specular_sampler_ = pending.specular_sampler;
  pending.vertex = {};
  pending.index = {};
  pending.albedo = {};
  pending.normal = {};
  pending.specular = {};
  pending.albedo_sampler = VK_NULL_HANDLE;
  pending.normal_sampler = VK_NULL_HANDLE;
  pending.specular_sampler = VK_NULL_HANDLE;
  static_draw_plan_ = std::move(pending.draw_plan);
  static_bone_count_ = pending.bone_count;
  static_vertex_bytes_ = pending.vertex_bytes;
  static_index_bytes_ = pending.index_bytes;
  static_texture_logical_bytes_ = pending.texture_logical_bytes;
  static_model_ready_ = true;
  static_mismatch_logged_ = false;
  static_generations_.accept(pending.model_generation,
                             pending.texture_generation);
  ++static_resource_rebuilds_;
  uploaded_bytes = pending.uploaded_bytes;
  if (static_descriptor_revision_ ==
      (std::numeric_limits<std::uint64_t>::max)()) {
    static_descriptor_revision_ = 1u;
    for (FrameSync &frame : frames_) {
      frame.static_descriptor_revision = 0u;
    }
  } else {
    ++static_descriptor_revision_;
  }

  if (rt_capability_.device_extensions_enabled) {
    const std::size_t scratch_base = rt_scenes_.size();
    if (pending.rt_candidates_submitted) {
      for (std::size_t i = 0u; i < rt_scenes_.size(); ++i) {
        rt_scenes_[i] = std::move(pending.rt_candidates[i]);
        rt_scene_built_[i] = true;
        last_rt_scene_hash_[i] = pending.scene_hash;
        rt_scene_candidates_[i]->setRestGeometry(
            std::move(pending.rt_rest_candidates[scratch_base + i]));
      }
    } else if (replacing_existing) {
      for (std::size_t i = 0u; i < rt_scenes_.size(); ++i) {
        rt_scenes_[i] = std::move(rt_scene_candidates_[i]);
        rt_scenes_[i]->setRestGeometry(
            std::move(pending.rt_rest_candidates[i]));
        rt_scene_candidates_[i] =
            std::move(pending.rt_candidates[i]);
        rt_scene_candidates_[i]->setRestGeometry(
            std::move(pending.rt_rest_candidates[scratch_base + i]));
      }
      rt_scene_built_.fill(false);
      last_rt_scene_hash_.fill(0);
    } else {
      for (std::size_t i = 0u; i < rt_scenes_.size(); ++i) {
        rt_scenes_[i]->setRestGeometry(
            std::move(pending.rt_rest_candidates[i]));
        rt_scene_candidates_[i]->setRestGeometry(
            std::move(pending.rt_rest_candidates[scratch_base + i]));
      }
      rt_scene_built_.fill(false);
      last_rt_scene_hash_.fill(0);
    }
  }
  if (diagnostics_enabled_) {
    xpbd::log::infof(
        "VKDIAG static_pending_commit model_generation=%llu "
        "texture_generation=%llu bytes=%llu elapsed_ms=%.4f",
        static_cast<unsigned long long>(pending.model_generation),
        static_cast<unsigned long long>(pending.texture_generation),
        static_cast<unsigned long long>(uploaded_bytes),
        std::chrono::duration<double, std::milli>(
            Clock::now() - pending.submitted_at)
            .count());
  }
  pending = {};
  return true;
}

bool VulkanBackend::pollStaticAssetPacket(
    std::uint64_t &uploaded_bytes, bool wait_for_completion,
    bool &complete, bool &superseded) {
  uploaded_bytes = 0u;
  complete = false;
  superseded = false;
  StaticAssetPending &pending = static_asset_pending_;
  if (!pending.active) {
    complete = true;
    return true;
  }
  if (gpu_completion_unproven_ || quarantine_required_) {
    return false;
  }

  if (pending.upload_fence != VK_NULL_HANDLE) {
    if (wait_for_completion) {
      const ControlledWaitResult wait = waitForFenceControlled(
          pending.upload_fence, "vkWaitForFences.static_pending",
          UINT32_MAX, VK_NULL_HANDLE, pending.upload_command, true, true);
      if (!wait.completed()) {
        return false;
      }
    } else {
      const VkResult status =
          vkGetFenceStatus(device_, pending.upload_fence);
      if (status == VK_NOT_READY) {
        return true;
      }
      if (status != VK_SUCCESS) {
        markGpuCompletionUnproven("vkGetFenceStatus.static_pending");
        return false;
      }
    }
    vkDestroyFence(device_, pending.upload_fence, nullptr);
    pending.upload_fence = VK_NULL_HANDLE;
    if (pending.upload_command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, cmd_pool_, 1,
                           &pending.upload_command);
      pending.upload_command = VK_NULL_HANDLE;
    }
    destroyBuffer(pending.staging);
  }

  for (std::size_t i = 0u; i < pending.rt_candidates.size(); ++i) {
    if (!pending.rt_candidate_submitted[i] ||
        !pending.rt_candidates[i]) {
      continue;
    }
    const RtScenePendingBuildState state =
        pending.rt_candidates[i]->pollPendingBuild(wait_for_completion);
    if (state == RtScenePendingBuildState::PendingFence) {
      return true;
    }
    if (state == RtScenePendingBuildState::CompletionUnproven) {
      markGpuCompletionUnproven(
          "vkGetFenceStatus(static_rt_pending)");
      return false;
    }
    if (state != RtScenePendingBuildState::ReadyToCommit) {
      discardStaticAssetPending("static-rt-pending-failed");
      return false;
    }
  }

  if (pending.superseded) {
    discardStaticAssetPending("static-candidate-superseded");
    if (gpu_completion_unproven_ || static_asset_pending_.active) {
      return false;
    }
    complete = true;
    superseded = true;
    return true;
  }

  if (pending.fail_before_commit) {
    discardStaticAssetPending("injected-static-publish-failure");
    return false;
  }
  bool retirement_complete = false;
  if (!pollStaticAssetRetirement(wait_for_completion,
                                 retirement_complete)) {
    return false;
  }
  if (!retirement_complete) {
    return true;
  }
  if (!prepareStaticPendingRtCandidates()) {
    discardStaticAssetPending("static-pending-rt-candidate-failure");
    return false;
  }
  if (!beginStaticAssetRetirement()) {
    discardStaticAssetPending("static-retirement-submit-failure");
    return false;
  }
  complete = commitStaticAssetPending(uploaded_bytes);
  if (!complete && static_asset_retired_.active()) {
    recordFatalError(
        "commitStaticAssetPending",
        "Vulkan static publication failed after retirement submission");
    return false;
  }
  return complete;
}

bool VulkanBackend::rebuildStaticModelResources(const StaticIndexedModelMesh &mesh,
                                   const TextureImage *texture,
                                   const ResolvedMaterialTable *material,
                                   std::uint64_t model_generation,
                                   std::uint64_t texture_generation,
                                   std::uint64_t &uploaded_bytes,
                                   const RtSceneSynchronousUpdate *rt_update,
                                   bool defer_commit) {
    uploaded_bytes = 0;
    if (static_asset_pending_.active) {
      writeLog("Vulkan static Candidate submission is already pending");
      return false;
    }
    try {
    StaticModelDrawPlan new_plan = makeStaticModelDrawPlan(mesh, texture);
    std::vector<RtRestGeometry> rt_rest_candidates;
    if (rt_capability_.device_extensions_enabled) {
      RtRestGeometry candidate;
      std::string candidate_error;
      if (!buildStaticRtRestCandidate(mesh, new_plan, material, candidate,
                                      candidate_error)) {
        writeLog(candidate_error.empty()
                     ? "Vulkan RT Candidate construction failed"
                     : candidate_error.c_str());
        return false;
      }
      try {
        // Two copies feed the unpublished RT Candidates and two synchronize
        // the old published scenes after they become the next scratch
        // Candidates. The two published scenes themselves own the current
        // immutable CPU rest state; no fifth canonical copy is needed.
        const std::size_t candidate_count =
            rt_scenes_.size() * 2u;
        rt_rest_candidates.reserve(candidate_count);
        for (std::size_t i = 0u; i < candidate_count; ++i) {
          if (i + 1u == candidate_count) {
            rt_rest_candidates.push_back(std::move(candidate));
          } else {
            rt_rest_candidates.push_back(candidate);
          }
        }
      } catch (const std::exception &exception) {
        const std::string message =
            std::string("Vulkan RT Candidate replication failed: ") +
            exception.what();
        writeLog(message.c_str());
        return false;
      } catch (...) {
        writeLog("Vulkan RT Candidate replication failed");
        return false;
      }
    }

    std::vector<StaticGpuVertex> gpu_vertices;
    if (!new_plan.indices.empty()) {
      std::vector<float> normal_sign(mesh.vertices.size(), 1.0f);
      if (texture == nullptr || !texture->valid()) {




        for (const auto &face : mesh.faces) {
          if (!face.textured || !detail::validFace(mesh, face) ||
              (face.direction != StaticModelFaceDirection::Down &&
               face.direction != StaticModelFaceDirection::Up)) {
            continue;
          }
          const std::size_t end =
              static_cast<std::size_t>(face.first_vertex) + face.vertex_count;
          std::fill(normal_sign.begin() + face.first_vertex,
                    normal_sign.begin() + end, -1.0f);
        }
      }
      gpu_vertices.resize(mesh.vertices.size());
      for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        const auto &source = mesh.vertices[i];
        auto &destination = gpu_vertices[i];
        destination.px = source.px;
        destination.py = source.py;
        destination.pz = source.pz;
        destination.nx = source.nx * normal_sign[i];
        destination.ny = source.ny * normal_sign[i];
        destination.nz = source.nz * normal_sign[i];
        destination.u = source.u;
        destination.v = source.v;
        destination.bone_index = source.bone_index;
        destination.flags = new_plan.textured_vertices[i] != 0u ? 1u : 0u;
        destination.tx = source.tx;
        destination.ty = source.ty;
        destination.tz = source.tz;
        destination.tangent_handedness = source.tangent_handedness;
      }
    }

    const VkDeviceSize vertex_bytes = static_cast<VkDeviceSize>(
        gpu_vertices.size() * sizeof(StaticGpuVertex));
    const VkDeviceSize index_bytes = static_cast<VkDeviceSize>(
        new_plan.indices.size() * sizeof(std::uint32_t));
    constexpr std::array<std::uint8_t, 4> kWhitePixel = {255, 255, 255, 255};
    constexpr std::array<std::uint8_t, 4> kFlatNormalPixel = {
        128, 128, 255, 255};
    constexpr std::array<std::uint8_t, 4> kFallbackSpecularPixel = {
        0, 10, 0, 255};
    const auto fallback_image = [](const std::array<std::uint8_t, 4> &pixel) {
      TextureImage image;
      image.width = 1;
      image.height = 1;
      image.source_channels = 4;
      image.rgba.assign(pixel.begin(), pixel.end());
      return image;
    };
    const bool has_texture = texture != nullptr && texture->valid();
    const bool has_normal =
        material != nullptr && material->normal_map_active &&
        material->normal_image.valid();
    const bool has_specular =
        material != nullptr && material->specular_map_active &&
        material->specular_image.valid();
    const TextureImage fallback_base = fallback_image(kWhitePixel);
    const TextureImage fallback_normal = fallback_image(kFlatNormalPixel);
    const TextureImage fallback_specular = fallback_image(kFallbackSpecularPixel);
    const TextureImage &base_source = has_texture ? *texture : fallback_base;
    const TextureImage &normal_source =
        has_normal ? material->normal_image : fallback_normal;
    const TextureImage &specular_source =
        has_specular ? material->specular_image : fallback_specular;

    std::vector<LabPbrAtlasIsland> atlas_islands;
    std::vector<LabPbrAtlasIsland> no_islands;
    std::string island_error;
    const bool islands_proven =
        has_texture &&
        buildLabPbrAtlasIslands(mesh, base_source, atlas_islands, &island_error);
    const auto &base_islands = islands_proven ? atlas_islands : no_islands;
    const bool normal_matches_base =
        has_normal && normal_source.width == base_source.width &&
        normal_source.height == base_source.height;
    const bool specular_matches_base =
        has_specular && specular_source.width == base_source.width &&
        specular_source.height == base_source.height;
    const auto &normal_islands =
        islands_proven && normal_matches_base ? atlas_islands : no_islands;
    const auto &specular_islands =
        islands_proven && specular_matches_base ? atlas_islands : no_islands;

    LabPbrMipChain albedo_chain;
    LabPbrMipChain normal_chain;
    LabPbrMipChain specular_chain;
    try {
      albedo_chain = buildLabPbrMipChain(
          base_source, base_islands,
          LabPbrMipSemantic::BaseColorCoverage);
      normal_chain = buildLabPbrMipChain(
          normal_source, normal_islands,
          LabPbrMipSemantic::IrisNormalAoHeight,
          normal_matches_base ? &albedo_chain : nullptr);
      specular_chain = buildLabPbrMipChain(
          specular_source, specular_islands,
          LabPbrMipSemantic::SpecularPacked,
          specular_matches_base ? &albedo_chain : nullptr);
    } catch (const std::exception &exception) {
      xpbd::log::warnf("LabPBR mip disabled: builder exception: %s",
                       exception.what());
      return false;
    } catch (...) {
      xpbd::log::warn("LabPBR mip disabled: unknown builder exception");
      return false;
    }
    const auto mark_inactive_fallback_full = [](LabPbrMipChain &chain) {
      chain.status = LabPbrMipBuildStatus::FullChain;
      chain.stop_reason.clear();
    };
    const auto force_base_only = [](LabPbrMipChain &chain,
                                    const char *reason) {
      if (chain.levels.size() > 1u) {
        chain.levels.resize(1u);
      }
      chain.safe_max_lod = 0u;
      chain.status = LabPbrMipBuildStatus::BaseOnlyFallback;
      chain.stop_reason = reason;
    };
    if (!has_texture) {
      mark_inactive_fallback_full(albedo_chain);
    } else if (!islands_proven && !island_error.empty()) {
      force_base_only(albedo_chain, island_error.c_str());
    }
    if (!has_normal) {
      mark_inactive_fallback_full(normal_chain);
    } else if (!normal_matches_base) {
      force_base_only(normal_chain,
                      "normal sidecar dimensions do not match the base atlas");
    } else if (!islands_proven && !island_error.empty()) {
      force_base_only(normal_chain, island_error.c_str());
    }
    if (!has_specular) {
      mark_inactive_fallback_full(specular_chain);
    } else if (!specular_matches_base) {
      force_base_only(
          specular_chain,
          "specular sidecar dimensions do not match the base atlas");
    } else if (!islands_proven && !island_error.empty()) {
      force_base_only(specular_chain, island_error.c_str());
    }
    if (!albedo_chain.valid() || !normal_chain.valid() ||
        !specular_chain.valid() ||
        albedo_chain.levels.size() >
            (std::numeric_limits<std::uint32_t>::max)() ||
        normal_chain.levels.size() >
            (std::numeric_limits<std::uint32_t>::max)() ||
        specular_chain.levels.size() >
            (std::numeric_limits<std::uint32_t>::max)()) {
      writeLog("Vulkan static semantic mip chain is invalid");
      return false;
    }

    const auto status_name = [](LabPbrMipBuildStatus status) {
      switch (status) {
      case LabPbrMipBuildStatus::FullChain:
        return "full";
      case LabPbrMipBuildStatus::SafelyTruncated:
        return "truncated";
      case LabPbrMipBuildStatus::BaseOnlyFallback:
        return "base-only";
      }
      return "unknown";
    };
    const auto log_chain = [&](const char *semantic, std::size_t island_count,
                               const LabPbrMipChain &chain) {
      const auto &base = chain.levels.front();
      const char *stop =
          chain.stop_reason.empty() ? "<none>" : chain.stop_reason.c_str();
      const char *status = status_name(chain.status);
      if (chain.status == LabPbrMipBuildStatus::BaseOnlyFallback) {
        xpbd::log::warnf(
            "LabPBR semantic mip: semantic=%s status=%s base=%ux%u "
            "islands=%zu levels=%zu safeMaxLod=%u stop=\"%s\"",
            semantic, status, base.width, base.height, island_count,
            chain.levels.size(), chain.safe_max_lod, stop);
      } else {
        xpbd::log::infof(
            "LabPBR semantic mip: semantic=%s status=%s base=%ux%u "
            "islands=%zu levels=%zu safeMaxLod=%u stop=\"%s\"",
            semantic, status, base.width, base.height, island_count,
            chain.levels.size(), chain.safe_max_lod, stop);
      }
    };
    log_chain("albedo", base_islands.size(), albedo_chain);
    log_chain("normal", normal_islands.size(), normal_chain);
    log_chain("specular", specular_islands.size(), specular_chain);
    xpbd::log::infof(
        "VKDIAG LabPBR GPU material normal=%d specular=%d flags=%u "
        "base=%ux%u normal=%ux%u specular=%ux%u",
        has_normal ? 1 : 0, has_specular ? 1 : 0,
        labPbrFeatureFlags(material), albedo_chain.levels.front().width,
        albedo_chain.levels.front().height, normal_chain.levels.front().width,
        normal_chain.levels.front().height,
        specular_chain.levels.front().width,
        specular_chain.levels.front().height);

    const std::uint32_t texture_width = albedo_chain.levels.front().width;
    const std::uint32_t texture_height = albedo_chain.levels.front().height;
    const std::uint32_t normal_width = normal_chain.levels.front().width;
    const std::uint32_t normal_height = normal_chain.levels.front().height;
    const std::uint32_t specular_width = specular_chain.levels.front().width;
    const std::uint32_t specular_height = specular_chain.levels.front().height;
    const std::uint32_t texture_mip_levels =
        static_cast<std::uint32_t>(albedo_chain.levels.size());
    const std::uint32_t normal_mip_levels =
        static_cast<std::uint32_t>(normal_chain.levels.size());
    const std::uint32_t specular_mip_levels =
        static_cast<std::uint32_t>(specular_chain.levels.size());

    VkDeviceSize staging_bytes = 0u;
    const auto append_size = [&](VkDeviceSize bytes) {
      if (bytes > (std::numeric_limits<VkDeviceSize>::max)() - staging_bytes) {
        return false;
      }
      staging_bytes += bytes;
      return true;
    };
    if (!append_size(vertex_bytes) || !append_size(index_bytes)) {
      writeLog("Vulkan static resource size overflow");
      return false;
    }
    std::vector<VkBufferImageCopy> texture_copies;
    std::vector<VkBufferImageCopy> normal_copies;
    std::vector<VkBufferImageCopy> specular_copies;
    const auto append_chain_layout =
        [&](const LabPbrMipChain &chain,
            std::vector<VkBufferImageCopy> &copies) {
          copies.reserve(chain.levels.size());
          for (std::size_t level_index = 0u;
               level_index < chain.levels.size(); ++level_index) {
            const auto &level = chain.levels[level_index];
            const std::size_t byte_count = level.rgba.size();
            if (byte_count >
                    static_cast<std::size_t>(
                        (std::numeric_limits<VkDeviceSize>::max)()) ||
                !append_size(static_cast<VkDeviceSize>(byte_count))) {
              return false;
            }
            VkBufferImageCopy copy{};
            copy.bufferOffset = staging_bytes - byte_count;
            copy.imageSubresource = {
                VK_IMAGE_ASPECT_COLOR_BIT,
                static_cast<std::uint32_t>(level_index), 0u, 1u};
            copy.imageExtent = {level.width, level.height, 1u};
            copies.push_back(copy);
          }
          return true;
        };
    bool copy_layouts_valid = false;
    try {
      copy_layouts_valid =
          append_chain_layout(albedo_chain, texture_copies) &&
          append_chain_layout(normal_chain, normal_copies) &&
          append_chain_layout(specular_chain, specular_copies);
    } catch (const std::exception &exception) {
      xpbd::log::warnf("LabPBR mip disabled: copy layout exception: %s",
                       exception.what());
      return false;
    } catch (...) {
      xpbd::log::warn("LabPBR mip disabled: unknown copy layout exception");
      return false;
    }
    if (!copy_layouts_valid) {
      writeLog("Vulkan static semantic mip staging size overflow");
      return false;
    }

    StaticRtTransactionBudget transaction_budget;
    transaction_budget.maximum_bytes = staticRtMaximumPeakBytes();
    bool budget_inputs_valid =
        appendStaticMeshBytes(mesh,
                              transaction_budget.cpu_persistent_bytes) &&
        appendTextureImageBytes(base_source,
                                transaction_budget.cpu_persistent_bytes) &&
        appendTextureImageBytes(normal_source,
                                transaction_budget.cpu_persistent_bytes) &&
        appendTextureImageBytes(specular_source,
                                transaction_budget.cpu_persistent_bytes) &&
        appendStaticDrawPlanBytes(
            static_draw_plan_, transaction_budget.cpu_persistent_bytes) &&
        appendStaticDrawPlanBytes(new_plan,
                                  transaction_budget.cpu_candidate_bytes) &&
        appendVectorBytes(gpu_vertices,
                          transaction_budget.cpu_candidate_bytes);
    for (const RtRestGeometry &candidate : rt_rest_candidates) {
      budget_inputs_valid =
          budget_inputs_valid &&
          appendRtRestBytes(candidate,
                            transaction_budget.cpu_candidate_bytes);
    }
    budget_inputs_valid =
        budget_inputs_valid &&
        appendVectorBytes(atlas_islands,
                          transaction_budget.cpu_temporary_bytes) &&
        appendVectorBytes(no_islands,
                          transaction_budget.cpu_temporary_bytes) &&
        appendLabPbrMipChainBytes(
            albedo_chain, transaction_budget.cpu_temporary_bytes) &&
        appendLabPbrMipChainBytes(
            normal_chain, transaction_budget.cpu_temporary_bytes) &&
        appendLabPbrMipChainBytes(
            specular_chain, transaction_budget.cpu_temporary_bytes) &&
        appendVectorBytes(texture_copies,
                          transaction_budget.cpu_temporary_bytes) &&
        appendVectorBytes(normal_copies,
                          transaction_budget.cpu_temporary_bytes) &&
        appendVectorBytes(specular_copies,
                          transaction_budget.cpu_temporary_bytes);
    if (!new_plan.indices.empty()) {
      std::uint64_t normal_sign_bytes = 0u;
      budget_inputs_valid =
          budget_inputs_valid &&
          checkedMultiply(static_cast<std::uint64_t>(mesh.vertices.size()),
                          sizeof(float), normal_sign_bytes) &&
          checkedAdd(normal_sign_bytes,
                     transaction_budget.cpu_temporary_bytes);
    }

    transaction_budget.staging_bytes =
        static_cast<std::uint64_t>(staging_bytes);
    transaction_budget.static_gpu_bytes =
        static_cast<std::uint64_t>(staging_bytes);
    budget_inputs_valid =
        budget_inputs_valid &&
        checkedAdd(static_vertex_bytes_,
                   transaction_budget.old_gpu_rt_bytes) &&
        checkedAdd(static_index_bytes_,
                   transaction_budget.old_gpu_rt_bytes) &&
        checkedAdd(static_texture_logical_bytes_,
                   transaction_budget.old_gpu_rt_bytes);

    for (std::size_t i = 0u; i < rt_scenes_.size(); ++i) {
      if (rt_scenes_[i]) {
        const RtSceneStats current = rt_scenes_[i]->stats();
        budget_inputs_valid =
            budget_inputs_valid &&
            checkedAdd(current.cpu_allocated_bytes,
                       transaction_budget.cpu_persistent_bytes) &&
            checkedAdd(current.allocated_bytes,
                       transaction_budget.old_gpu_rt_bytes);
      }
      if (rt_scene_candidates_[i]) {
        const RtSceneStats scratch = rt_scene_candidates_[i]->stats();
        budget_inputs_valid =
            budget_inputs_valid &&
            checkedAdd(scratch.cpu_allocated_bytes,
                       transaction_budget.cpu_persistent_bytes) &&
            checkedAdd(scratch.allocated_bytes,
                       transaction_budget.old_gpu_rt_bytes);
      }
    }

    RtSceneBudgetEstimate rt_estimate{};
    const bool build_rt_candidates =
        rt_capability_.device_extensions_enabled && rt_update != nullptr;
    if (build_rt_candidates) {
      if (rt_rest_candidates.size() < rt_scene_candidates_.size() ||
          !estimateRtSceneBudget(rt_rest_candidates.front(),
                                 rt_update->include_rest_model,
                                 rt_update->colored_geometry, rt_estimate)) {
        budget_inputs_valid = false;
      }
    }
    for (std::size_t i = 0u; i < rt_scene_candidates_.size(); ++i) {
      const std::uint64_t candidate_gpu_peak =
          build_rt_candidates ? rt_estimate.gpu_bytes : 0u;
      budget_inputs_valid =
          budget_inputs_valid &&
          checkedAdd(candidate_gpu_peak,
                     transaction_budget.new_rt_estimate_bytes);
      if (build_rt_candidates) {
        budget_inputs_valid =
            budget_inputs_valid &&
            checkedAdd(rt_estimate.cpu_workspace_bytes,
                       transaction_budget.rt_cpu_growth_bytes);
      }
    }

    const auto log_transaction_budget =
        [&](const char *phase, bool accepted) {
          const char *result = accepted ? "accepted" : "rejected";
          const auto log = [&](auto logger) {
            logger(
                "VKDIAG static_rt_budget phase=%s result=%s "
                "cpu_persistent=%llu cpu_candidate=%llu "
                "rt_cpu_growth=%llu cpu_temporary=%llu staging=%llu "
                "static_gpu=%llu new_rt=%llu old_gpu_rt=%llu "
                "safety=%llu total=%llu maximum=%llu",
                phase, result,
                static_cast<unsigned long long>(
                    transaction_budget.cpu_persistent_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.cpu_candidate_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.rt_cpu_growth_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.cpu_temporary_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.staging_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.static_gpu_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.new_rt_estimate_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.old_gpu_rt_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.safety_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.total_bytes),
                static_cast<unsigned long long>(
                    transaction_budget.maximum_bytes));
          };
          if (accepted) {
            log([](const char *format, auto... args) {
              xpbd::log::infof(format, args...);
            });
          } else {
            log([](const char *format, auto... args) {
              xpbd::log::warnf(format, args...);
            });
          }
        };
    const bool budget_finalized = finalizeStaticRtBudget(transaction_budget);
    if (!budget_inputs_valid) {
      transaction_budget.total_bytes =
          (std::numeric_limits<std::uint64_t>::max)();
    }
    const bool budget_accepted = budget_inputs_valid && budget_finalized;
    log_transaction_budget("preflight", budget_accepted);
    if (!budget_accepted) {
      writeLog("Vulkan static CPU/GPU/RT Candidate exceeds peak budget");
      return false;
    }

    Buffer staging{};
    Buffer new_vertex_buffer{};
    Buffer new_index_buffer{};
    ImageResource new_texture{};
    ImageResource new_normal_texture{};
    ImageResource new_specular_texture{};
    VkSampler new_albedo_sampler = VK_NULL_HANDLE;
    VkSampler new_normal_sampler = VK_NULL_HANDLE;
    VkSampler new_specular_sampler = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence upload_fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (gpu_completion_unproven_) {
        // At least one submitted command may still reference every object in
        // this transaction. The complete backend is retained by the worker's
        // quarantine registry, so intentionally abandon these local handles.
        return;
      }
      if (upload_fence) {
        vkDestroyFence(device_, upload_fence, nullptr);
        upload_fence = VK_NULL_HANDLE;
      }
      if (command) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(staging);
      destroyBuffer(new_vertex_buffer);
      destroyBuffer(new_index_buffer);
      destroyImage(new_texture);
      destroyImage(new_normal_texture);
      destroyImage(new_specular_texture);
      if (new_specular_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, new_specular_sampler, nullptr);
        new_specular_sampler = VK_NULL_HANDLE;
      }
      if (new_normal_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, new_normal_sampler, nullptr);
        new_normal_sampler = VK_NULL_HANDLE;
      }
      if (new_albedo_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, new_albedo_sampler, nullptr);
        new_albedo_sampler = VK_NULL_HANDLE;
      }
    };
    using CleanupCallback = decltype(cleanup);
    struct CleanupGuard {
      CleanupCallback &callback;
      ~CleanupGuard() { callback(); }
    } cleanup_guard{cleanup};

    if (!createBuffer(staging_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging) ||
        (vertex_bytes > 0 &&
         !createBuffer(vertex_bytes,
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                       new_vertex_buffer)) ||
        (index_bytes > 0 && !createBuffer(index_bytes,
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                                          new_index_buffer)) ||
        !createStaticTexture(texture_width, texture_height,
                             VK_FORMAT_R8G8B8A8_SRGB, new_texture,
                             texture_mip_levels) ||
        !createStaticTexture(normal_width, normal_height,
                             VK_FORMAT_R8G8B8A8_UNORM,
                             new_normal_texture, normal_mip_levels) ||
        !createStaticTexture(specular_width, specular_height,
                             VK_FORMAT_R8G8B8A8_UNORM,
                             new_specular_texture, specular_mip_levels) ||
        !createStaticMaterialSampler(
            VK_SAMPLER_MIPMAP_MODE_LINEAR,
            static_cast<float>(albedo_chain.safe_max_lod),
            new_albedo_sampler) ||
        !createStaticMaterialSampler(
            VK_SAMPLER_MIPMAP_MODE_LINEAR,
            static_cast<float>(normal_chain.safe_max_lod),
            new_normal_sampler) ||
        !createStaticMaterialSampler(
            VK_SAMPLER_MIPMAP_MODE_NEAREST,
            static_cast<float>(specular_chain.safe_max_lod),
            new_specular_sampler)) {
      cleanup();
      writeLog("Vulkan static device resource allocation failed");
      return false;
    }

    if (vertex_bytes > 0) {
      std::memcpy(staging.mapped, gpu_vertices.data(),
                  static_cast<std::size_t>(vertex_bytes));
    }
    if (index_bytes > 0) {
      std::memcpy(static_cast<std::byte *>(staging.mapped) + vertex_bytes,
                  new_plan.indices.data(),
                  static_cast<std::size_t>(index_bytes));
    }
    const auto stage_chain = [&](const LabPbrMipChain &chain,
                                 const std::vector<VkBufferImageCopy> &copies) {
      for (std::size_t level = 0u; level < chain.levels.size(); ++level) {
        std::memcpy(static_cast<std::byte *>(staging.mapped) +
                        copies[level].bufferOffset,
                    chain.levels[level].rgba.data(),
                    chain.levels[level].rgba.size());
      }
    };
    stage_chain(albedo_chain, texture_copies);
    stage_chain(normal_chain, normal_copies);
    stage_chain(specular_chain, specular_copies);

    VkCommandBufferAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = cmd_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate_info, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }

    if (vertex_bytes > 0) {
      VkBufferCopy copy{0, 0, vertex_bytes};
      vkCmdCopyBuffer(command, staging.buffer, new_vertex_buffer.buffer, 1,
                      &copy);
    }
    if (index_bytes > 0) {
      VkBufferCopy copy{vertex_bytes, 0, index_bytes};
      vkCmdCopyBuffer(command, staging.buffer, new_index_buffer.buffer, 1,
                      &copy);
    }

    std::array<VkBufferMemoryBarrier, 2> geometry_barriers{};
    std::uint32_t geometry_barrier_count = 0u;
    const auto append_geometry_barrier =
        [&](VkBuffer buffer, VkAccessFlags consumer_access) {
          auto &barrier = geometry_barriers[geometry_barrier_count++];
          barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask = consumer_access |
                                  VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.buffer = buffer;
          barrier.offset = 0u;
          barrier.size = VK_WHOLE_SIZE;
        };
    if (vertex_bytes > 0) {
      append_geometry_barrier(new_vertex_buffer.buffer,
                              VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
    }
    if (index_bytes > 0) {
      append_geometry_barrier(new_index_buffer.buffer,
                              VK_ACCESS_INDEX_READ_BIT);
    }
    if (geometry_barrier_count > 0u) {
      constexpr VkPipelineStageFlags kGeometryConsumerStages =
          VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
          VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
          VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR |
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           kGeometryConsumerStages, 0, 0, nullptr,
                           geometry_barrier_count, geometry_barriers.data(),
                           0, nullptr);
    }

    const auto upload_image =
        [&](const ImageResource &image,
            const std::vector<VkBufferImageCopy> &staged_copies) {
          if (staged_copies.empty() ||
              staged_copies.size() != image.mip_levels) {
            return false;
          }
          VkImageMemoryBarrier barrier{
              VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          barrier.image = image.image;
          barrier.subresourceRange = {
              VK_IMAGE_ASPECT_COLOR_BIT, 0, image.mip_levels, 0, 1};
          barrier.srcAccessMask = 0;
          barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &barrier);
          vkCmdCopyBufferToImage(command, staging.buffer, image.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 static_cast<std::uint32_t>(
                                     staged_copies.size()),
                                 staged_copies.data());
          constexpr VkPipelineStageFlags kTextureConsumerStages =
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
              VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
          barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               kTextureConsumerStages, 0, 0, nullptr, 0,
                               nullptr, 1, &barrier);
          return true;
        };
    if (!upload_image(new_texture, texture_copies) ||
        !upload_image(new_normal_texture, normal_copies) ||
        !upload_image(new_specular_texture, specular_copies)) {
      cleanup();
      writeLog("Vulkan static semantic mip copy layout is invalid");
      return false;
    }

    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fence_info, nullptr, &upload_fence) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    const auto submit_start = Clock::now();
    logDiagnosticApi("vkQueueSubmit.static_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, upload_fence, VK_NULL_HANDLE, command,
                     true, true);
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, upload_fence);
    logDiagnosticApi(
        "vkQueueSubmit.static_upload", "after", submit_result,
        std::chrono::duration<double, std::milli>(Clock::now() - submit_start)
            .count(),
        UINT32_MAX, upload_fence, VK_NULL_HANDLE, command, true, false);
    if (submit_result != VK_SUCCESS) {
      cleanup();
      return false;
    }
    StaticAssetPending &pending = static_asset_pending_;
    pending.staging = staging;
    pending.vertex = new_vertex_buffer;
    pending.index = new_index_buffer;
    pending.albedo = new_texture;
    pending.normal = new_normal_texture;
    pending.specular = new_specular_texture;
    pending.albedo_sampler = new_albedo_sampler;
    pending.normal_sampler = new_normal_sampler;
    pending.specular_sampler = new_specular_sampler;
    pending.upload_command = command;
    pending.upload_fence = upload_fence;
    pending.draw_plan = std::move(new_plan);
    pending.rt_rest_candidates = std::move(rt_rest_candidates);
    pending.model_generation = model_generation;
    pending.texture_generation = texture_generation;
    pending.uploaded_bytes = static_cast<std::uint64_t>(staging_bytes);
    pending.vertex_bytes = static_cast<std::uint64_t>(vertex_bytes);
    pending.index_bytes = static_cast<std::uint64_t>(index_bytes);
    pending.texture_logical_bytes =
        static_cast<std::uint64_t>(staging_bytes - vertex_bytes -
                                   index_bytes);
    pending.scene_hash = rt_update != nullptr ? rt_update->scene_hash : 0u;
    pending.bone_count = mesh.bone_names.size();
    pending.active = true;
    pending.submitted_at = submit_start;
    staging = {};
    new_vertex_buffer = {};
    new_index_buffer = {};
    new_texture = {};
    new_normal_texture = {};
    new_specular_texture = {};
    new_albedo_sampler = VK_NULL_HANDLE;
    new_normal_sampler = VK_NULL_HANDLE;
    new_specular_sampler = VK_NULL_HANDLE;
    command = VK_NULL_HANDLE;
    upload_fence = VK_NULL_HANDLE;

    // The static upload and RT Candidates remain unpublished. All submissions
    // share the graphics queue, so worker polling may overlap their fences
    // while the complete old CPU/GPU/RT set remains visible.
    if (rt_capability_.device_extensions_enabled && rt_update != nullptr) {
      if (!prepareStaticPendingRtCandidates()) {
        discardStaticAssetPending(
            "static-rt-candidate-initialization-failure");
        return false;
      }
      for (std::size_t i = 0u; i < rt_scene_candidates_.size(); ++i) {
        // Static upload Candidates own independent RT scene objects so the
        // normal published/scratch pair remains available to render the last
        // committed immutable packet throughout Pending.
        std::swap(rt_scene_candidates_[i], pending.rt_candidates[i]);
        std::uint64_t additional_resident_bytes = 0u;
        bool additional_resident_valid = true;
        for (const auto &resident : pending.rt_candidates) {
          if (!resident) {
            continue;
          }
          const RtSceneStats stats = resident->stats();
          additional_resident_valid =
              additional_resident_valid &&
              checkedAdd(stats.allocated_bytes,
                         additional_resident_bytes) &&
              checkedAdd(stats.cpu_allocated_bytes,
                         additional_resident_bytes);
        }
        const bool prepared =
            additional_resident_valid &&
            prepareRtSceneCandidate(
                i, *rt_update, &pending.rt_rest_candidates[i], false,
                additional_resident_bytes);
        std::swap(rt_scene_candidates_[i], pending.rt_candidates[i]);
        if (!prepared) {
          discardStaticAssetPending(
              "static-rt-candidate-submit-failure");
          writeLog("Vulkan static RT Candidate transaction failed");
          return false;
        }
        pending.rt_candidate_submitted[i] = true;
      }
      pending.rt_candidates_submitted = true;

      std::uint64_t actual_candidate_gpu_peak = 0u;
      std::uint64_t actual_candidate_cpu_growth = 0u;
      bool actual_budget_valid = true;
      for (std::size_t i = 0u; i < pending.rt_candidates.size(); ++i) {
        const RtSceneStats actual = pending.rt_candidates[i]->stats();
        actual_budget_valid =
            actual_budget_valid &&
            checkedAdd(actual.allocated_bytes,
                       actual_candidate_gpu_peak);
        actual_budget_valid =
            actual_budget_valid &&
            checkedAdd(actual.cpu_workspace_bytes,
                       actual_candidate_cpu_growth);
      }
      transaction_budget.new_rt_estimate_bytes =
          (std::max)(transaction_budget.new_rt_estimate_bytes,
                     actual_candidate_gpu_peak);
      transaction_budget.rt_cpu_growth_bytes =
          (std::max)(transaction_budget.rt_cpu_growth_bytes,
                     actual_candidate_cpu_growth);
      const bool actual_budget_finalized =
          finalizeStaticRtBudget(transaction_budget);
      if (!actual_budget_valid) {
        transaction_budget.total_bytes =
            (std::numeric_limits<std::uint64_t>::max)();
      }
      const bool actual_budget_accepted =
          actual_budget_valid && actual_budget_finalized;
      log_transaction_budget("post-build", actual_budget_accepted);
      if (!actual_budget_accepted) {
        discardStaticAssetPending(
            "static-rt-candidate-budget-failure");
        writeLog("Vulkan static RT Candidate actual peak exceeds budget");
        return false;
      }

      // Deterministic developer/CI failure injection for the atomic publish
      // seam.  The requested ordinal is based on successful publications, so
      // the one-shot latch lets the immediately following frame retry and
      // prove recovery without advancing the current generation on failure.
      if (!rt_candidate_failure_injected_) {
        const char *injected_ordinal = std::getenv(
            "XPBD_A3_FAIL_STATIC_RT_PUBLISH_ONCE_AT_REBUILD");
        if (injected_ordinal != nullptr && injected_ordinal[0] != '\0') {
          char *parse_end = nullptr;
          const unsigned long long ordinal =
              std::strtoull(injected_ordinal, &parse_end, 10);
          const std::uint64_t next_rebuild = static_resource_rebuilds_ + 1u;
          if (parse_end != injected_ordinal && *parse_end == '\0' &&
              ordinal == next_rebuild) {
            rt_candidate_failure_injected_ = true;
            const bool old_rt_complete =
                static_model_ready_ && rt_scenes_[0] && rt_scenes_[1] &&
                rt_scenes_[0]->ready() && rt_scenes_[1]->ready();
            xpbd::log::warnf(
                "A3_INJECT_STATIC_RT_PUBLISH_FAILURE rebuild=%llu "
                "old_set_retained=%d old_model_generation=%llu "
                "old_tlas0=0x%llx old_tlas1=0x%llx",
                static_cast<unsigned long long>(next_rebuild),
                old_rt_complete ? 1 : 0,
                static_cast<unsigned long long>(static_generations_.model),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(
                        rt_scenes_[0]->tlas())),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(
                        rt_scenes_[1]->tlas())));
            pending.fail_before_commit = true;
          }
        }
      }
    }

    if (defer_commit) {
      uploaded_bytes = pending.uploaded_bytes;
      return true;
    }
    bool complete = false;
    bool superseded = false;
    return pollStaticAssetPacket(uploaded_bytes, true, complete,
                                 superseded) &&
           complete && !superseded;
    } catch (const std::bad_alloc &) {
      discardStaticAssetPending("static-candidate-bad-alloc");
      writeLog("Vulkan static CPU Candidate allocation failed");
      return false;
    } catch (const std::exception &) {
      discardStaticAssetPending("static-candidate-exception");
      writeLog("Vulkan static CPU Candidate construction raised an exception");
      return false;
    } catch (...) {
      discardStaticAssetPending("static-candidate-unknown-exception");
      writeLog("Vulkan static CPU Candidate construction failed");
      return false;
    }
  }

} // namespace xpbd::gfx::detail
