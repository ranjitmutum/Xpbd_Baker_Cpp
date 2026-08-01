#include "xpbd/gfx/vulkan_rt_scene.hpp"

#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/gfx/world_environment.hpp"
#include "xpbd/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace xpbd::gfx {
namespace {

constexpr VkBuildAccelerationStructureFlagsKHR kDynamicBlasFlags =
    VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
constexpr VkBuildAccelerationStructureFlagsKHR kStaticBlasFlags =
    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

constexpr VkBuildAccelerationStructureFlagsKHR kTlasFlags =
    VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR |
    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;

struct alignas(16) RtInstanceMotionGpu {
  std::array<float, 16> current_transform{};
  std::array<float, 16> previous_transform{};
  // x history valid, y geometry kind, z BLAS policy, w source bone.
  std::array<std::uint32_t, 4> metadata{};
};
static_assert(sizeof(RtInstanceMotionGpu) == 144u);

} // namespace

bool VulkanRtScene::init(VkPhysicalDevice phys, VkDevice device,
                         std::uint32_t queue_family, VkQueue queue) {
  shutdown();
  phys_ = phys;
  device_ = device;
  queue_family_ = queue_family;
  queue_ = queue;
  if (!loadProcs()) {
    xpbd::log::warn("Vulkan RT scene: failed to load KHR function pointers");
    shutdown();
    return false;
  }
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.queueFamilyIndex = queue_family_;
  pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
             VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  if (vkCreateCommandPool(device_, &pi, nullptr, &cmd_pool_) != VK_SUCCESS) {
    shutdown();
    return false;
  }
  initialized_ = true;
  return true;
}

void VulkanRtScene::shutdown() {
  if (device_ != VK_NULL_HANDLE) {
    destroyAs(tlas_);
    destroyGeometryStates();
    destroyBuffer(scratch_);
    destroyBuffer(instance_buffer_);
    destroyBuffer(host_vertices_);
    destroyBuffer(host_previous_vertices_);
    destroyBuffer(host_indices_);
    destroyBuffer(host_normals_);
    destroyBuffer(host_uvs_);
    destroyBuffer(host_colors_);
    destroyBuffer(host_tangents_);
    destroyBuffer(host_primitive_flags_);
    destroyBuffer(host_primitive_metadata_);
    destroyBuffer(host_instance_metadata_);
    destroyBuffer(host_instance_motion_);
    destroyBuffer(host_emissive_triangles_);
    if (cmd_pool_) {
      vkDestroyCommandPool(device_, cmd_pool_, nullptr);
      cmd_pool_ = VK_NULL_HANDLE;
    }
  }
  rest_ = {};
  scratch_positions_ = {};
  previous_positions_ = {};
  scratch_normals_ = {};
  scratch_uvs_ = {};
  scratch_colors_ = {};
  scratch_tangents_ = {};
  scratch_indices_ = {};
  scratch_primitive_flags_ = {};
  last_vertex_count_ = 0;
  last_index_count_ = 0;
  visible_instance_mask_count_ = 0;
  hidden_instance_mask_count_ = 0;
  emissive_triangle_count_ = 0;
  motion_history_valid_ = false;
  tlas_build_scratch_ = 0;
  tlas_update_scratch_ = 0;
  tlas_built_once_ = false;
  tlas_requires_full_build_ = true;
  full_build_count_ = 0;
  refit_count_ = 0;
  tlas_full_build_count_ = 0;
  tlas_update_count_ = 0;
  upload_bytes_ = 0;
  emitter_distribution_ms_ = 0.0f;
  emitter_distribution_rebuild_count_ = 0;
  descriptor_write_count_ = 0;
  descriptor_cache_hits_ = 0;
  last_generations_ = {};
  generations_valid_ = false;
  uploaded_topology_generation_ = 0;
  uploaded_positions_generation_ = 0;
  uploaded_transforms_generation_ = 0;
  uploaded_material_generation_ = 0;
  uploaded_emission_generation_ = 0;
  uploaded_topology_valid_ = false;
  uploaded_positions_valid_ = false;
  uploaded_transforms_valid_ = false;
  uploaded_material_valid_ = false;
  uploaded_emission_valid_ = false;
  bone_transform_cache_ = {};
  emissive_records_cache_ = {};
  emissive_weights_cache_ = {};
  cached_emissive_count_ = 0;
  cached_hidden_source_emitter_triangle_count_ = 0;
  cached_hidden_positive_weight_triangle_count_ = 0;
  cached_positive_emission_source_ = false;
  emissive_distribution_valid_ = false;
  bone_cache_generation_ = 0;
  bone_cache_generation_valid_ = false;
  pending_build_reason_ = RtAccelerationBuildReason::None;
  last_build_reason_ = RtAccelerationBuildReason::None;
  last_tlas_reason_ = RtAccelerationBuildReason::None;
  geometry_prepared_ = false;
  pending_ = PendingBuild::None;
  initialized_ = false;
  procs_ok_ = false;
  phys_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
}

void VulkanRtScene::destroyGeometryStates() {
  for (GeometryState &state : geometry_states_) {
    destroyAs(state.blas);
  }
  geometry_states_.clear();
}

bool VulkanRtScene::loadProcs() {
  auto load = [&](const char *name) {
    return vkGetDeviceProcAddr(device_, name);
  };
  vkGetBufferDeviceAddressKHR_ =
      reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
          load("vkGetBufferDeviceAddressKHR"));
  if (!vkGetBufferDeviceAddressKHR_) {
    vkGetBufferDeviceAddressKHR_ =
        reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            load("vkGetBufferDeviceAddress"));
  }
  vkCreateAccelerationStructureKHR_ =
      reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
          load("vkCreateAccelerationStructureKHR"));
  vkDestroyAccelerationStructureKHR_ =
      reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
          load("vkDestroyAccelerationStructureKHR"));
  vkGetAccelerationStructureBuildSizesKHR_ =
      reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
          load("vkGetAccelerationStructureBuildSizesKHR"));
  vkGetAccelerationStructureDeviceAddressKHR_ =
      reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
          load("vkGetAccelerationStructureDeviceAddressKHR"));
  vkCmdBuildAccelerationStructuresKHR_ =
      reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
          load("vkCmdBuildAccelerationStructuresKHR"));

  procs_ok_ = vkGetBufferDeviceAddressKHR_ &&
              vkCreateAccelerationStructureKHR_ &&
              vkDestroyAccelerationStructureKHR_ &&
              vkGetAccelerationStructureBuildSizesKHR_ &&
              vkGetAccelerationStructureDeviceAddressKHR_ &&
              vkCmdBuildAccelerationStructuresKHR_;
  return procs_ok_;
}

std::uint32_t VulkanRtScene::findMemoryType(std::uint32_t type_bits,
                                            VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp{};
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (mp.memoryTypes[i].propertyFlags & props) == props) {
      return i;
    }
  }
  return std::numeric_limits<std::uint32_t>::max();
}

VkDeviceAddress VulkanRtScene::bufferDeviceAddress(VkBuffer buffer) const {
  VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  info.buffer = buffer;
  return vkGetBufferDeviceAddressKHR_(device_, &info);
}

bool VulkanRtScene::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                 VkMemoryPropertyFlags mem_props, Buffer &out,
                                 bool map_host) {
  destroyBuffer(out);
  if (size == 0) {
    return true;
  }
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &bi, nullptr, &out.buffer) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device_, out.buffer, &req);
  const std::uint32_t type = findMemoryType(req.memoryTypeBits, mem_props);
  if (type == std::numeric_limits<std::uint32_t>::max()) {
    destroyBuffer(out);
    return false;
  }
  VkMemoryAllocateFlagsInfo flags{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    ai.pNext = &flags;
  }
  if (vkAllocateMemory(device_, &ai, nullptr, &out.memory) != VK_SUCCESS) {
    destroyBuffer(out);
    return false;
  }
  if (vkBindBufferMemory(device_, out.buffer, out.memory, 0) != VK_SUCCESS) {
    destroyBuffer(out);
    return false;
  }
  out.capacity = size;
  if (map_host) {
    if (vkMapMemory(device_, out.memory, 0, size, 0, &out.mapped) !=
        VK_SUCCESS) {
      destroyBuffer(out);
      return false;
    }
  }
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    out.address = bufferDeviceAddress(out.buffer);
  }
  return true;
}

void VulkanRtScene::destroyBuffer(Buffer &b) {
  if (device_ == VK_NULL_HANDLE) {
    b = {};
    return;
  }
  if (b.mapped && b.memory) {
    vkUnmapMemory(device_, b.memory);
  }
  if (b.buffer) {
    vkDestroyBuffer(device_, b.buffer, nullptr);
  }
  if (b.memory) {
    vkFreeMemory(device_, b.memory, nullptr);
  }
  b = {};
}

void VulkanRtScene::destroyAs(AccelerationStructure &as) {
  if (device_ != VK_NULL_HANDLE && as.handle &&
      vkDestroyAccelerationStructureKHR_) {
    vkDestroyAccelerationStructureKHR_(device_, as.handle, nullptr);
  }
  destroyBuffer(as.buffer);
  as = {};
}

void VulkanRtScene::setRestGeometry(RtRestGeometry geometry) {
  rest_ = std::move(geometry);
  // Topology changed — force full rebuild next updateGeometry().
  last_vertex_count_ = 0;
  last_index_count_ = 0;
  visible_instance_mask_count_ = 0;
  hidden_instance_mask_count_ = 0;
  emissive_triangle_count_ = 0;
  geometry_prepared_ = false;
  pending_ = PendingBuild::None;
  pending_build_reason_ = RtAccelerationBuildReason::None;
  last_tlas_reason_ = RtAccelerationBuildReason::None;
  tlas_built_once_ = false;
  tlas_requires_full_build_ = true;
  generations_valid_ = false;
  emitter_distribution_ms_ = 0.0f;
  emissive_records_cache_.clear();
  emissive_weights_cache_.clear();
  cached_emissive_count_ = 0;
  cached_hidden_source_emitter_triangle_count_ = 0;
  cached_hidden_positive_weight_triangle_count_ = 0;
  cached_positive_emission_source_ = false;
  emissive_distribution_valid_ = false;
  bone_transform_cache_.clear();
  bone_cache_generation_ = 0;
  bone_cache_generation_valid_ = false;
  uploaded_topology_valid_ = false;
  uploaded_positions_valid_ = false;
  uploaded_transforms_valid_ = false;
  uploaded_material_valid_ = false;
  uploaded_emission_valid_ = false;
  destroyAs(tlas_);
  destroyGeometryStates();
}

RtSceneStats VulkanRtScene::stats() const noexcept {
  RtSceneStats result;
  for (const GeometryState &state : geometry_states_) {
    if (state.blas.handle != VK_NULL_HANDLE) {
      ++result.blas_count;
      result.as_storage_bytes +=
          static_cast<std::uint64_t>(state.blas.buffer_size);
    }
  }
  result.tlas_count = tlas_.handle != VK_NULL_HANDLE ? 1u : 0u;
  result.instance_count =
      result.tlas_count != 0u
          ? static_cast<std::uint32_t>(geometry_states_.size())
          : 0u;
  result.visible_instance_mask_count =
      result.tlas_count != 0u ? visible_instance_mask_count_ : 0u;
  result.hidden_instance_mask_count =
      result.tlas_count != 0u ? hidden_instance_mask_count_ : 0u;
  result.positive_emitter_count =
      result.tlas_count != 0u ? emissive_triangle_count_ : 0u;
  result.hidden_source_emitter_triangle_count =
      result.tlas_count != 0u
          ? cached_hidden_source_emitter_triangle_count_
          : 0u;
  result.hidden_positive_weight_triangle_count =
      result.tlas_count != 0u
          ? cached_hidden_positive_weight_triangle_count_
          : 0u;
  result.primitive_count = last_index_count_ / 3u;
  result.as_storage_bytes +=
      static_cast<std::uint64_t>(tlas_.buffer_size);
  result.scratch_bytes = static_cast<std::uint64_t>(scratch_.capacity);
  result.attribute_bytes = static_cast<std::uint64_t>(
      host_vertices_.capacity + host_previous_vertices_.capacity +
      host_indices_.capacity +
      host_normals_.capacity + host_uvs_.capacity + host_colors_.capacity +
      host_tangents_.capacity + host_primitive_flags_.capacity +
      host_primitive_metadata_.capacity +
      host_instance_metadata_.capacity + host_instance_motion_.capacity +
      host_emissive_triangles_.capacity + instance_buffer_.capacity);
  result.allocated_bytes =
      result.as_storage_bytes + result.scratch_bytes + result.attribute_bytes;
  result.full_builds = full_build_count_;
  result.refits = refit_count_;
  result.tlas_full_builds = tlas_full_build_count_;
  result.tlas_updates = tlas_update_count_;
  result.upload_bytes = upload_bytes_;
  result.emitter_distribution_ms = emitter_distribution_ms_;
  result.emitter_distribution_rebuilds =
      emitter_distribution_rebuild_count_;
  result.descriptor_write_count = descriptor_write_count_;
  result.descriptor_cache_hits = descriptor_cache_hits_;
  result.last_build_reason = last_build_reason_;
  result.last_tlas_reason = last_tlas_reason_;
  return result;
}

VkCommandBuffer VulkanRtScene::beginOneShot() {
  VkCommandBufferAllocateInfo ai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = cmd_pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    return VK_NULL_HANDLE;
  }
  return cmd;
}

void VulkanRtScene::submitOneShot(VkCommandBuffer cmd) {
  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue_);
  vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
}

bool VulkanRtScene::ensureScratch(VkDeviceSize size) {
  if (scratch_.capacity >= size && scratch_.buffer) {
    return true;
  }
  return createBuffer(size,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratch_, false);
}

void VulkanRtScene::fillTriangleGeometry(
    const GeometryState &state,
    VkAccelerationStructureGeometryKHR &geometry,
    VkAccelerationStructureGeometryTrianglesDataKHR &tri) const {
  tri = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
  tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  tri.vertexData.deviceAddress = host_vertices_.address;
  tri.vertexStride = sizeof(float) * 3;
  tri.maxVertex = last_vertex_count_ > 0 ? last_vertex_count_ - 1 : 0;
  tri.indexType = VK_INDEX_TYPE_UINT32;
  tri.indexData.deviceAddress =
      host_indices_.address +
      static_cast<VkDeviceAddress>(state.first_index) *
          sizeof(std::uint32_t);

  geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  // Opaque ranges are explicitly marked so Vulkan can skip Any Hit.  Cutout
  // and blend ranges retain the alpha-aware path and keep flags clear.
  geometry.flags = state.opaque_geometry ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0u;
  geometry.geometry.triangles = tri;
}

bool VulkanRtScene::ensureBlasStorage(GeometryState &state,
                                      std::uint32_t vertex_count) {
  if (vertex_count == 0 || state.index_count < 3) {
    return false;
  }

  VkAccelerationStructureGeometryTrianglesDataKHR tri{};
  VkAccelerationStructureGeometryKHR geometry{};
  const std::uint32_t prev_v = last_vertex_count_;
  last_vertex_count_ = vertex_count;
  fillTriangleGeometry(state, geometry, tri);
  last_vertex_count_ = prev_v;

  const std::uint32_t primitive_count = state.index_count / 3u;
  VkAccelerationStructureBuildGeometryInfoKHR build{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = state.blas_policy == RtBlasPolicy::DynamicRefit
                    ? kDynamicBlasFlags
                    : kStaticBlasFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  VkAccelerationStructureBuildSizesInfoKHR sizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR_(
      device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
      &primitive_count, &sizes);

  state.build_scratch = sizes.buildScratchSize;
  state.update_scratch = sizes.updateScratchSize;

  const bool need_realloc =
      !state.blas.handle ||
      state.blas.buffer_size < sizes.accelerationStructureSize;
  if (!need_realloc) {
    return ensureScratch(
        (std::max)(state.build_scratch, state.update_scratch));
  }

  destroyAs(state.blas);
  state.built_once = false;
  if (!createBuffer(sizes.accelerationStructureSize,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, state.blas.buffer,
                    false)) {
    return false;
  }
  state.blas.buffer_size = sizes.accelerationStructureSize;

  VkAccelerationStructureCreateInfoKHR ci{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = state.blas.buffer.buffer;
  ci.size = sizes.accelerationStructureSize;
  ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  if (vkCreateAccelerationStructureKHR_(device_, &ci, nullptr,
                                        &state.blas.handle) != VK_SUCCESS) {
    destroyAs(state.blas);
    return false;
  }
  VkAccelerationStructureDeviceAddressInfoKHR addr_info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  addr_info.accelerationStructure = state.blas.handle;
  state.blas.address =
      vkGetAccelerationStructureDeviceAddressKHR_(device_, &addr_info);

  return ensureScratch(
      (std::max)(state.build_scratch, state.update_scratch));
}

bool VulkanRtScene::ensureTlasStorage(std::uint32_t instance_count) {
  if (instance_count == 0u ||
      instance_count != geometry_states_.size()) {
    return false;
  }
  for (const GeometryState &state : geometry_states_) {
    if (state.blas.handle == VK_NULL_HANDLE || state.blas.address == 0) {
      return false;
    }
  }
  const VkDeviceSize instance_bytes =
      static_cast<VkDeviceSize>(instance_count) *
      sizeof(VkAccelerationStructureInstanceKHR);
  if (!instance_buffer_.buffer ||
      instance_buffer_.capacity < instance_bytes) {
    if (!createBuffer(instance_bytes,
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                          VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      instance_buffer_, true)) {
      return false;
    }
  }
  instance_buffer_.address = bufferDeviceAddress(instance_buffer_.buffer);
  const VkDeviceSize metadata_bytes =
      static_cast<VkDeviceSize>(instance_count) *
      sizeof(std::array<std::uint32_t, 4>);
  if (!host_instance_metadata_.buffer ||
      host_instance_metadata_.capacity < metadata_bytes) {
    if (!createBuffer(metadata_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      host_instance_metadata_, true)) {
      return false;
    }
  }

  VkAccelerationStructureGeometryInstancesDataKHR instances_data{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  instances_data.arrayOfPointers = VK_FALSE;
  instances_data.data.deviceAddress = instance_buffer_.address;

  VkAccelerationStructureGeometryKHR geometry{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = instances_data;

  VkAccelerationStructureBuildGeometryInfoKHR build{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = kTlasFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  const std::uint32_t primitive_count = instance_count;
  VkAccelerationStructureBuildSizesInfoKHR sizes{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  vkGetAccelerationStructureBuildSizesKHR_(
      device_, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
      &primitive_count, &sizes);
  tlas_build_scratch_ = sizes.buildScratchSize;
  tlas_update_scratch_ = sizes.updateScratchSize;

  const bool need_realloc =
      !tlas_.handle || tlas_.buffer_size < sizes.accelerationStructureSize;
  if (!need_realloc) {
    VkDeviceSize maximum_scratch =
        (std::max)(tlas_build_scratch_, tlas_update_scratch_);
    for (const GeometryState &state : geometry_states_) {
      maximum_scratch =
          (std::max)(maximum_scratch,
                     (std::max)(state.build_scratch,
                                state.update_scratch));
    }
    return ensureScratch(maximum_scratch);
  }

  destroyAs(tlas_);
  tlas_built_once_ = false;
  tlas_requires_full_build_ = true;
  if (!createBuffer(sizes.accelerationStructureSize,
                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, tlas_.buffer, false)) {
    return false;
  }
  tlas_.buffer_size = sizes.accelerationStructureSize;

  VkAccelerationStructureCreateInfoKHR ci{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = tlas_.buffer.buffer;
  ci.size = sizes.accelerationStructureSize;
  ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  if (vkCreateAccelerationStructureKHR_(device_, &ci, nullptr, &tlas_.handle) !=
      VK_SUCCESS) {
    destroyAs(tlas_);
    return false;
  }
  VkAccelerationStructureDeviceAddressInfoKHR addr_info{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  addr_info.accelerationStructure = tlas_.handle;
  tlas_.address =
      vkGetAccelerationStructureDeviceAddressKHR_(device_, &addr_info);

  VkDeviceSize maximum_scratch =
      (std::max)(tlas_build_scratch_, tlas_update_scratch_);
  for (const GeometryState &state : geometry_states_) {
    maximum_scratch =
        (std::max)(maximum_scratch,
                   (std::max)(state.build_scratch, state.update_scratch));
  }
  return ensureScratch(maximum_scratch);
}

void VulkanRtScene::writeInstanceData(
    const float *bone_transforms_column_major, std::size_t bone_count,
    const float *bone_tints_rgba, std::size_t tint_count,
    const float *previous_bone_transforms_column_major,
    std::size_t previous_bone_count, bool motion_history_valid) {
  auto *instances =
      static_cast<VkAccelerationStructureInstanceKHR *>(
          instance_buffer_.mapped);
  auto *metadata =
      static_cast<std::array<std::uint32_t, 4> *>(
          host_instance_metadata_.mapped);
  auto *motion = static_cast<RtInstanceMotionGpu *>(
      host_instance_motion_.mapped);
  std::fill_n(metadata, geometry_states_.size(),
              std::array<std::uint32_t, 4>{});
  visible_instance_mask_count_ = 0u;
  hidden_instance_mask_count_ = 0u;
  for (std::size_t index = 0; index < geometry_states_.size(); ++index) {
    GeometryState &state = geometry_states_[index];
    std::array<float, 16> next_transform{
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (state.blas_policy == RtBlasPolicy::RigidLocalSpace &&
        bone_transforms_column_major != nullptr &&
        state.source_bone_index < bone_count) {
      std::memcpy(
          next_transform.data(),
          bone_transforms_column_major +
              static_cast<std::size_t>(state.source_bone_index) * 16u,
          sizeof(next_transform));
    }
    std::array<float, 16> previous_transform = next_transform;
    bool instance_motion_valid = motion_history_valid;
    if (state.blas_policy == RtBlasPolicy::RigidLocalSpace) {
      instance_motion_valid =
          instance_motion_valid &&
          previous_bone_transforms_column_major != nullptr &&
          state.source_bone_index < previous_bone_count;
      if (instance_motion_valid) {
        std::memcpy(
            previous_transform.data(),
            previous_bone_transforms_column_major +
                static_cast<std::size_t>(state.source_bone_index) * 16u,
            sizeof(previous_transform));
      }
    }
    state.previous_transform = previous_transform;
    state.current_transform = next_transform;
    state.transform_history_valid = instance_motion_valid;

    VkTransformMatrixKHR transform{};
    for (std::size_t row = 0; row < 3u; ++row) {
      for (std::size_t column = 0; column < 4u; ++column) {
        transform.matrix[row][column] =
            state.current_transform[column * 4u + row];
      }
    }
    VkAccelerationStructureInstanceKHR instance{};
    instance.transform = transform;
    instance.instanceCustomIndex = state.instance_custom_index;
    instance.mask = 0xFFu;
    if (state.blas_policy == RtBlasPolicy::RigidLocalSpace &&
        bone_tints_rgba != nullptr && state.source_bone_index < tint_count) {
      instance.mask = rtInstanceVisibilityMask(
          bone_tints_rgba[static_cast<std::size_t>(state.source_bone_index) *
                              4u +
                          3u]);
    }
    if (instance.mask == 0u) {
      ++hidden_instance_mask_count_;
    } else {
      ++visible_instance_mask_count_;
    }
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags =
        VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = state.blas.address;
    instances[index] = instance;
    metadata[state.instance_custom_index] = {
        state.first_index / 3u, static_cast<std::uint32_t>(state.kind),
        state.source_bone_index,
        static_cast<std::uint32_t>(state.blas_policy)};
    RtInstanceMotionGpu &motion_record =
        motion[state.instance_custom_index];
    motion_record.current_transform = state.current_transform;
    motion_record.previous_transform = state.previous_transform;
    motion_record.metadata = {
        instance_motion_valid ? 1u : 0u,
        static_cast<std::uint32_t>(state.kind),
        static_cast<std::uint32_t>(state.blas_policy),
        state.source_bone_index};
  }
}

void VulkanRtScene::recordBlasBuild(VkCommandBuffer cmd,
                                    GeometryState &state, bool update) {
  VkAccelerationStructureGeometryTrianglesDataKHR tri{};
  VkAccelerationStructureGeometryKHR geometry{};
  fillTriangleGeometry(state, geometry, tri);

  const std::uint32_t primitive_count = state.index_count / 3u;
  VkAccelerationStructureBuildGeometryInfoKHR build{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = state.blas_policy == RtBlasPolicy::DynamicRefit
                    ? kDynamicBlasFlags
                    : kStaticBlasFlags;
  build.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                      : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  build.dstAccelerationStructure = state.blas.handle;
  if (update) {
    build.srcAccelerationStructure = state.blas.handle;
  }
  build.scratchData.deviceAddress = scratch_.address;

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount = primitive_count;
  const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {&range};
  vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &build, ranges);
}

void VulkanRtScene::recordTlasBuild(VkCommandBuffer cmd, bool update) {
  VkAccelerationStructureGeometryInstancesDataKHR instances_data{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
  instances_data.arrayOfPointers = VK_FALSE;
  instances_data.data.deviceAddress = instance_buffer_.address;

  VkAccelerationStructureGeometryKHR geometry{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geometry.geometry.instances = instances_data;

  VkAccelerationStructureBuildGeometryInfoKHR build{
      VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = kTlasFlags;
  build.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                      : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;
  build.dstAccelerationStructure = tlas_.handle;
  if (update) {
    build.srcAccelerationStructure = tlas_.handle;
    build.scratchData.deviceAddress = scratch_.address;
  } else {
    build.scratchData.deviceAddress = scratch_.address;
  }

  VkAccelerationStructureBuildRangeInfoKHR range{};
  range.primitiveCount =
      static_cast<std::uint32_t>(geometry_states_.size());
  const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {&range};
  vkCmdBuildAccelerationStructuresKHR_(cmd, 1, &build, ranges);
}

bool VulkanRtScene::updateGeometry(
    const float *bone_transforms_column_major, std::size_t bone_count,
    const float *bone_tints_rgba, std::size_t tint_count,
    std::span<const RtColoredGeometryView> colored_geometry,
    bool include_rest_model,
    std::span<const float> previous_packed_positions,
    const float *previous_bone_transforms_column_major,
    std::size_t previous_bone_count,
    bool explicit_motion_history_valid, RtSceneGenerations generations,
    bool generations_valid) {
  auto fail = [&](const char *reason) {
    last_update_failure_reason_ = reason;
    return false;
  };
  last_update_failure_reason_.clear();
  emitter_distribution_ms_ = 0.0f;
  if (!initialized_ || !procs_ok_) {
    return fail("scene not initialized or RT procedures unavailable");
  }
  if (generations_valid && generations_valid_ &&
      generations == last_generations_ && geometry_prepared_) {
    // No source domain changed since the last submission.  In particular,
    // camera-only frames do not rebuild CPU arrays or touch host buffers.
    // Mark scene-local motion unavailable so the shader reuses current
    // positions/transforms with the previous camera instead of replaying the
    // last object-motion vector indefinitely.
    motion_history_valid_ = false;
    return true;
  }
  const bool topology_generation_changed =
      !generations_valid || !generations_valid_ ||
      generations.topology != last_generations_.topology;
  const bool positions_generation_changed =
      !generations_valid || !generations_valid_ ||
      generations.positions != last_generations_.positions;
  const bool transforms_generation_changed =
      !generations_valid || !generations_valid_ ||
      generations.transforms != last_generations_.transforms;
  const bool materials_generation_changed =
      !generations_valid || !generations_valid_ ||
      generations.materials != last_generations_.materials;
  const bool emission_generation_changed =
      !generations_valid || !generations_valid_ ||
      generations.emission != last_generations_.emission;
  const bool visibility_generation_changed =
      !generations_valid || !generations_valid_ ||
      generations.visibility != last_generations_.visibility;
  // Instance transforms are the only source domain that requires a TLAS-only
  // build when BLAS content is otherwise unchanged.  Material/emission
  // updates still refresh their host buffers, but should not pay for a TLAS
  // UPDATE that cannot change instance visibility.
  const bool instance_transforms_changed =
      !generations_valid || !generations_valid_ ||
      generations.transforms != last_generations_.transforms ||
      visibility_generation_changed;

  const std::size_t stored_rest_vert_count = rest_.positions.size() / 3;
  const std::size_t rest_vert_count =
      include_rest_model ? stored_rest_vert_count : 0u;
  if ((rest_vert_count > 0 &&
       (rest_.indices.empty() ||
        rest_.bone_indices.size() != stored_rest_vert_count)) ||
      (include_rest_model && rest_.positions.size() % 3u != 0u)) {
    destroyAs(tlas_);
    destroyGeometryStates();
    last_vertex_count_ = 0;
    last_index_count_ = 0;
    geometry_prepared_ = false;
    pending_ = PendingBuild::None;
    pending_build_reason_ = RtAccelerationBuildReason::None;
    return fail("rest geometry has invalid positions, indices, or bone map");
  }

  std::size_t colored_vert_count = 0;
  for (const RtColoredGeometryView &view : colored_geometry) {
    if (view.vertices != nullptr) {
      colored_vert_count += view.vertex_count - (view.vertex_count % 3u);
    }
  }
  if (rest_vert_count == 0 && colored_vert_count == 0) {
    destroyAs(tlas_);
    destroyGeometryStates();
    last_vertex_count_ = 0;
    last_index_count_ = 0;
    geometry_prepared_ = false;
    pending_ = PendingBuild::None;
    pending_build_reason_ = RtAccelerationBuildReason::None;
    return fail("both rest and colored geometry are empty");
  }

  const std::uint32_t model_verts = static_cast<std::uint32_t>(rest_vert_count);
  const std::uint32_t model_indices =
      include_rest_model ? static_cast<std::uint32_t>(rest_.indices.size()) : 0u;
  if ((model_indices % 3u) != 0u ||
      (model_verts > 0 && model_indices < 3)) {
    return fail("rest index count is not a non-empty triangle list");
  }
  std::vector<GeometryState> desired_states;
  if (include_rest_model) {
    std::uint32_t expected_first_index = 0u;
    desired_states.reserve(rest_.geometry_ranges.size() +
                           colored_geometry.size());
    for (const RtRestGeometryRange &range : rest_.geometry_ranges) {
      if (range.first_index != expected_first_index ||
          range.index_count == 0u || range.index_count % 3u != 0u ||
          range.first_index > model_indices ||
          range.index_count > model_indices - range.first_index ||
          range.instance_custom_index != desired_states.size() ||
          (range.blas_policy == RtBlasPolicy::RigidLocalSpace &&
           range.source_bone_index >= bone_count)) {
        return fail("rest geometry range is not dense or references an invalid bone");
      }
      GeometryState state;
      state.kind = range.kind;
      state.blas_policy = range.blas_policy;
      state.instance_custom_index = range.instance_custom_index;
      state.source_bone_index = range.source_bone_index;
      state.first_index = range.first_index;
      state.index_count = range.index_count;
      desired_states.push_back(std::move(state));
      expected_first_index += range.index_count;
    }
    if (expected_first_index != model_indices ||
        (model_indices != 0u && desired_states.empty())) {
      return fail("rest geometry ranges do not cover the packed index buffer");
    }
  }
  if (rest_vert_count + colored_vert_count >
          (std::numeric_limits<std::uint32_t>::max)() ||
      static_cast<std::size_t>(model_indices) + colored_vert_count >
          (std::numeric_limits<std::uint32_t>::max)()) {
    return fail("geometry exceeds Vulkan 32-bit vertex/index capacity");
  }

  const std::uint32_t total_verts =
      static_cast<std::uint32_t>(rest_vert_count + colored_vert_count);
  const std::uint32_t total_indices = static_cast<std::uint32_t>(
      static_cast<std::size_t>(model_indices) + colored_vert_count);

  const bool topology_counts_changed =
      total_verts != last_vertex_count_ || total_indices != last_index_count_;
  const bool had_tlas = tlas_.handle != VK_NULL_HANDLE;
  bool topology_changed = topology_counts_changed || !had_tlas ||
                          topology_generation_changed;

  const std::size_t packed_position_count =
      static_cast<std::size_t>(total_verts) * 3u;
  const bool previous_positions_compatible =
      explicit_motion_history_valid &&
      previous_packed_positions.size() == packed_position_count;
  if (previous_positions_compatible) {
    previous_positions_.assign(previous_packed_positions.begin(),
                               previous_packed_positions.end());
  } else {
    previous_positions_.clear();
  }
  auto &world = scratch_positions_;
  auto &world_normals = scratch_normals_;
  auto &world_uvs = scratch_uvs_;
  auto &world_colors = scratch_colors_;
  auto &world_tangents = scratch_tangents_;
  auto &world_indices = scratch_indices_;
  auto &primitive_flags = scratch_primitive_flags_;
  auto &primitive_metadata = scratch_primitive_metadata_;
  world.resize(static_cast<std::size_t>(total_verts) * 3u);
  world_normals.assign(static_cast<std::size_t>(total_verts) * 4u, 0.0f);
  world_uvs.assign(static_cast<std::size_t>(total_verts) * 2u, 0.0f);
  world_colors.assign(static_cast<std::size_t>(total_verts) * 4u, 1.0f);
  world_tangents.assign(static_cast<std::size_t>(total_verts) * 4u, 0.0f);
  world_indices.clear();
  world_indices.reserve(total_indices);
  primitive_flags.clear();
  primitive_flags.reserve(total_indices / 3u);
  primitive_metadata.clear();
  primitive_metadata.reserve(total_indices / 3u);

  const bool have_rest_normals =
      rest_.normals.size() == static_cast<std::size_t>(model_verts) * 3u;
  const bool have_rest_tangents =
      rest_.tangents.size() == static_cast<std::size_t>(model_verts) * 4u;

  auto mul_bone_direction = [](const float *m, float x, float y, float z,
                               float &ox, float &oy, float &oz) {
    ox = m[0] * x + m[4] * y + m[8] * z;
    oy = m[1] * x + m[5] * y + m[9] * z;
    oz = m[2] * x + m[6] * y + m[10] * z;
  };

  // Build the inverse-transpose and tangent matrices once per bone.  The old
  // hot loop recomputed the 3x3 cofactors and determinant for every vertex.
  const bool can_cache_bones = bone_transforms_column_major != nullptr &&
                               bone_count > 0u;
  const bool rebuild_bone_cache =
      can_cache_bones &&
      (!bone_cache_generation_valid_ ||
       topology_generation_changed ||
       bone_cache_generation_ != generations.transforms ||
       bone_transform_cache_.size() != bone_count);
  if (!can_cache_bones) {
    bone_transform_cache_.clear();
    bone_cache_generation_valid_ = false;
  } else if (rebuild_bone_cache) {
    bone_transform_cache_.resize(bone_count);
    for (std::size_t bone = 0; bone < bone_count; ++bone) {
      BoneTransformCacheEntry &entry = bone_transform_cache_[bone];
      const float *m = bone_transforms_column_major + bone * 16u;
      std::memcpy(entry.transform.data(), m, sizeof(entry.transform));
      const float a = m[0], b = m[4], c = m[8];
      const float d = m[1], e = m[5], f = m[9];
      const float g = m[2], h = m[6], i = m[10];
      const float c00 = e * i - f * h;
      const float c01 = f * g - d * i;
      const float c02 = d * h - e * g;
      const float c10 = c * h - b * i;
      const float c11 = a * i - c * g;
      const float c12 = b * g - a * h;
      const float c20 = b * f - c * e;
      const float c21 = c * d - a * f;
      const float c22 = a * e - b * d;
      const float determinant = a * c00 + b * c01 + c * c02;
      entry.used_fallback =
          !std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12f;
      if (!entry.used_fallback) {
        const float inv_det = 1.0f / determinant;
        entry.normal_matrix = {c00 * inv_det, c01 * inv_det,
                               c02 * inv_det, c10 * inv_det,
                               c11 * inv_det, c12 * inv_det,
                               c20 * inv_det, c21 * inv_det,
                               c22 * inv_det};
      } else {
        entry.normal_matrix = {a, b, c, d, e, f, g, h, i};
      }
      entry.tangent_matrix = {a, b, c, d, e, f, g, h, i};
      entry.determinant_sign = determinant < 0.0f ? -1.0f : 1.0f;
      entry.valid = true;
    }
    bone_cache_generation_ = generations.transforms;
    bone_cache_generation_valid_ = generations_valid;
  }
  const auto apply_cached_normal = [](const BoneTransformCacheEntry &entry,
                                      float x, float y, float z) {
    float nx = entry.normal_matrix[0] * x + entry.normal_matrix[1] * y +
               entry.normal_matrix[2] * z;
    float ny = entry.normal_matrix[3] * x + entry.normal_matrix[4] * y +
               entry.normal_matrix[5] * z;
    float nz = entry.normal_matrix[6] * x + entry.normal_matrix[7] * y +
               entry.normal_matrix[8] * z;
    float length_squared = nx * nx + ny * ny + nz * nz;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f) {
      nx = x;
      ny = y;
      nz = z;
      length_squared = nx * nx + ny * ny + nz * nz;
    }
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f) {
      return std::array<float, 3>{0.0f, 1.0f, 0.0f};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return std::array<float, 3>{nx * inverse_length, ny * inverse_length,
                                nz * inverse_length};
  };
  const auto apply_cached_tangent = [](const BoneTransformCacheEntry &entry,
                                       float x, float y, float z) {
    float tx = entry.tangent_matrix[0] * x + entry.tangent_matrix[1] * y +
               entry.tangent_matrix[2] * z;
    float ty = entry.tangent_matrix[3] * x + entry.tangent_matrix[4] * y +
               entry.tangent_matrix[5] * z;
    float tz = entry.tangent_matrix[6] * x + entry.tangent_matrix[7] * y +
               entry.tangent_matrix[8] * z;
    const float length_squared = tx * tx + ty * ty + tz * tz;
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12f) {
      return std::array<float, 3>{1.0f, 0.0f, 0.0f};
    }
    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return std::array<float, 3>{tx * inverse_length, ty * inverse_length,
                                tz * inverse_length};
  };

  // Skin model only (positions + normals). UVs are rest-space invariant.
  for (std::uint32_t i = 0; i < model_verts; ++i) {
    const float lx = rest_.positions[i * 3 + 0];
    const float ly = rest_.positions[i * 3 + 1];
    const float lz = rest_.positions[i * 3 + 2];
    float rnx = 0.0f, rny = 1.0f, rnz = 0.0f;
    float rtx = 1.0f, rty = 0.0f, rtz = 0.0f, rtw = 1.0f;
    if (have_rest_normals) {
      rnx = rest_.normals[i * 3 + 0];
      rny = rest_.normals[i * 3 + 1];
      rnz = rest_.normals[i * 3 + 2];
    }
    if (have_rest_tangents) {
      rtx = rest_.tangents[i * 4 + 0];
      rty = rest_.tangents[i * 4 + 1];
      rtz = rest_.tangents[i * 4 + 2];
      rtw = rest_.tangents[i * 4 + 3];
    }
    // Rigid model vertices remain in canonical bone-local space. Their bone
    // transform is applied by the TLAS instance, not baked into the BLAS.
    world[i * 3 + 0] = lx;
    world[i * 3 + 1] = ly;
    world[i * 3 + 2] = lz;
    const std::uint32_t bi = rest_.bone_indices[i];
    if (bone_transforms_column_major && bi < bone_count) {
      const float *m = bone_transforms_column_major + bi * 16u;
      const BoneTransformCacheEntry *entry =
          bi < bone_transform_cache_.size()
              ? &bone_transform_cache_[bi]
              : nullptr;
      const auto transformed_normal =
          entry != nullptr && entry->valid
              ? apply_cached_normal(*entry, rnx, rny, rnz)
              : transformRtNormalInverseTranspose(
                    std::span<const float, 16>(m, 16u), {rnx, rny, rnz})
                    .value;
      world_normals[i * 4 + 0] = transformed_normal[0];
      world_normals[i * 4 + 1] = transformed_normal[1];
      world_normals[i * 4 + 2] = transformed_normal[2];
      const auto transformed_tangent =
          entry != nullptr && entry->valid
              ? apply_cached_tangent(*entry, rtx, rty, rtz)
              : [&] {
                  float tx = 1.0f, ty = 0.0f, tz = 0.0f;
                  mul_bone_direction(m, rtx, rty, rtz, tx, ty, tz);
                  const float tlen = std::sqrt(tx * tx + ty * ty + tz * tz);
                  if (tlen > 1e-6f) {
                    tx /= tlen;
                    ty /= tlen;
                    tz /= tlen;
                  } else {
                    tx = 1.0f;
                    ty = 0.0f;
                    tz = 0.0f;
                  }
                  return std::array<float, 3>{tx, ty, tz};
                }();
      world_tangents[i * 4 + 0] = transformed_tangent[0];
      world_tangents[i * 4 + 1] = transformed_tangent[1];
      world_tangents[i * 4 + 2] = transformed_tangent[2];
      // A mirrored bone reverses the tangent frame handedness.  Preserve the
      // source sign for ordinary transforms and flip it exactly once for a
      // negative determinant, matching the inverse-transpose normal path.
      world_tangents[i * 4 + 3] =
          rtw * (entry != nullptr && entry->valid ? entry->determinant_sign
                                                   : 1.0f);
    } else {
      world_normals[i * 4 + 0] = rnx;
      world_normals[i * 4 + 1] = rny;
      world_normals[i * 4 + 2] = rnz;
      world_tangents[i * 4 + 0] = rtx;
      world_tangents[i * 4 + 1] = rty;
      world_tangents[i * 4 + 2] = rtz;
      world_tangents[i * 4 + 3] = rtw;
    }
    if (rest_.uvs.size() == static_cast<std::size_t>(model_verts) * 2u) {
      world_uvs[i * 2 + 0] = rest_.uvs[i * 2 + 0];
      world_uvs[i * 2 + 1] = rest_.uvs[i * 2 + 1];
    }
    if (bone_tints_rgba != nullptr && bi < tint_count) {
      std::memcpy(world_colors.data() + i * 4u,
                  bone_tints_rgba + static_cast<std::size_t>(bi) * 4u,
                  4u * sizeof(float));
    }
  }

  if (include_rest_model) {
    world_indices.insert(world_indices.end(), rest_.indices.begin(),
                         rest_.indices.end());
  }
  const std::size_t model_primitive_count =
      static_cast<std::size_t>(model_indices) / 3u;
  if (rest_.primitive_flags.size() == model_primitive_count) {
    primitive_flags.insert(primitive_flags.end(),
                           rest_.primitive_flags.begin(),
                           rest_.primitive_flags.end());
  } else {
    primitive_flags.insert(primitive_flags.end(), model_primitive_count,
                           kRtPrimitiveTextured);
  }
  if (rest_.primitive_metadata.size() == model_primitive_count) {
    primitive_metadata.insert(primitive_metadata.end(),
                              rest_.primitive_metadata.begin(),
                              rest_.primitive_metadata.end());
  } else {
    for (std::size_t primitive = 0; primitive < model_primitive_count;
         ++primitive) {
      primitive_metadata.push_back(
          {UINT32_MAX, UINT32_MAX, 0u,
           static_cast<std::uint32_t>(primitive)});
    }
  }

  // Face normals when rest has none.
  if (!have_rest_normals) {
    std::fill(world_normals.begin(), world_normals.end(), 0.0f);
    const auto &indices = rest_.indices;
    for (std::uint32_t t = 0; t + 2 < model_indices; t += 3) {
      const std::uint32_t ia = indices[t + 0];
      const std::uint32_t ib = indices[t + 1];
      const std::uint32_t ic = indices[t + 2];
      if (ia >= model_verts || ib >= model_verts || ic >= model_verts) {
        continue;
      }
      const float ax = world[ia * 3 + 0], ay = world[ia * 3 + 1],
                  az = world[ia * 3 + 2];
      const float bx = world[ib * 3 + 0], by = world[ib * 3 + 1],
                  bz = world[ib * 3 + 2];
      const float cx = world[ic * 3 + 0], cy = world[ic * 3 + 1],
                  cz = world[ic * 3 + 2];
      float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
      float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
      float nx = e1y * e2z - e1z * e2y;
      float ny = e1z * e2x - e1x * e2z;
      float nz = e1x * e2y - e1y * e2x;
      const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (nlen < 1e-8f) {
        continue;
      }
      nx /= nlen;
      ny /= nlen;
      nz /= nlen;
      for (std::uint32_t v : {ia, ib, ic}) {
        world_normals[v * 4 + 0] += nx;
        world_normals[v * 4 + 1] += ny;
        world_normals[v * 4 + 2] += nz;
      }
    }
    for (std::uint32_t i = 0; i < model_verts; ++i) {
      float nx = world_normals[i * 4 + 0];
      float ny = world_normals[i * 4 + 1];
      float nz = world_normals[i * 4 + 2];
      float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
      if (nlen > 1e-6f) {
        nx /= nlen;
        ny /= nlen;
        nz /= nlen;
      } else {
        nx = 0.0f;
        ny = 1.0f;
        nz = 0.0f;
      }
      const std::uint32_t bi = rest_.bone_indices[i];
      if (bone_transforms_column_major != nullptr && bi < bone_count) {
        const float *m =
            bone_transforms_column_major +
            static_cast<std::size_t>(bi) * 16u;
        if (bi < bone_transform_cache_.size() &&
            bone_transform_cache_[bi].valid) {
          const auto transformed =
              apply_cached_normal(bone_transform_cache_[bi], nx, ny, nz);
          nx = transformed[0];
          ny = transformed[1];
          nz = transformed[2];
        } else {
          const auto transformed_normal = transformRtNormalInverseTranspose(
              std::span<const float, 16>(m, 16u), {nx, ny, nz});
          nx = transformed_normal.value[0];
          ny = transformed_normal.value[1];
          nz = transformed_normal.value[2];
        }
      }
      world_normals[i * 4 + 0] = nx;
      world_normals[i * 4 + 1] = ny;
      world_normals[i * 4 + 2] = nz;
    }
  }

  auto legacy_content_hash = [](const MeshVertex *vertices,
                                std::size_t vertex_count) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
      for (const float component :
           {vertices[vertex].px, vertices[vertex].py,
            vertices[vertex].pz}) {
        const auto *bytes =
            reinterpret_cast<const std::uint8_t *>(&component);
        for (std::size_t byte = 0; byte < sizeof(component); ++byte) {
          hash ^= bytes[byte];
          hash *= 1099511628211ull;
        }
      }
    }
    return hash;
  };

  std::uint32_t destination_vertex = model_verts;
  for (const RtColoredGeometryView &view : colored_geometry) {
    if (view.vertices == nullptr) {
      continue;
    }
    const std::size_t count = view.vertex_count - (view.vertex_count % 3u);
    if (count == 0u) {
      continue;
    }
    if (desired_states.size() >
        static_cast<std::size_t>(0x00ffffffu)) {
      return fail("colored geometry has too many instances");
    }
    GeometryState state;
    state.kind = view.kind;
    state.blas_policy = view.blas_policy;
    state.instance_custom_index =
        static_cast<std::uint32_t>(desired_states.size());
    state.first_index =
        static_cast<std::uint32_t>(world_indices.size());
    state.index_count = static_cast<std::uint32_t>(count);
    state.content_hash = view.content_generation != 0u
                             ? mixRtGeneration(view.content_generation,
                                               view.vertex_count)
                             : (generations_valid
                                    ? mixRtGeneration(generations.positions,
                                                      view.vertex_count)
                                    : legacy_content_hash(view.vertices, count));
    const std::uint32_t flags =
        kRtPrimitiveEnvironment |
        (view.alpha_blended ? kRtPrimitiveBlend : 0u);
    for (std::size_t i = 0; i < count; ++i, ++destination_vertex) {
      const MeshVertex &source = view.vertices[i];
      const std::size_t vertex = destination_vertex;
      world[vertex * 3u + 0u] = source.px;
      world[vertex * 3u + 1u] = source.py;
      world[vertex * 3u + 2u] = source.pz;
      world_normals[vertex * 4u + 0u] = source.nx;
      world_normals[vertex * 4u + 1u] = source.ny;
      world_normals[vertex * 4u + 2u] = source.nz;
      world_colors[vertex * 4u + 0u] = source.r;
      world_colors[vertex * 4u + 1u] = source.g;
      world_colors[vertex * 4u + 2u] = source.b;
      world_colors[vertex * 4u + 3u] = source.a;
      world_tangents[vertex * 4u + 0u] = 1.0f;
      world_tangents[vertex * 4u + 3u] = 1.0f;
      world_indices.push_back(destination_vertex);
      if ((i % 3u) == 2u) {
        primitive_flags.push_back(flags);
        primitive_metadata.push_back(
            {UINT32_MAX, UINT32_MAX, 0u,
             static_cast<std::uint32_t>(primitive_metadata.size())});
      }
    }
    desired_states.push_back(std::move(state));
  }

  if (destination_vertex != total_verts ||
      world_indices.size() != total_indices ||
      primitive_flags.size() != total_indices / 3u ||
      primitive_metadata.size() != total_indices / 3u ||
      desired_states.empty()) {
    return fail("assembled geometry buffers do not match their declared counts");
  }
  // A BLAS can carry the opaque optimization only when every primitive in its
  // range is opaque.  Classification is derived from the packed primitive
  // flags, so material alpha changes naturally force a BLAS rebuild.
  for (GeometryState &state : desired_states) {
    state.opaque_geometry = true;
    const std::size_t first_primitive = state.first_index / 3u;
    const std::size_t primitive_count = state.index_count / 3u;
    if (first_primitive + primitive_count > primitive_flags.size()) {
      return fail("geometry range exceeds packed primitive flags");
    }
    for (std::size_t primitive = first_primitive;
         primitive < first_primitive + primitive_count; ++primitive) {
      if ((primitive_flags[primitive] &
           (kRtPrimitiveCutout | kRtPrimitiveBlend)) != 0u) {
        state.opaque_geometry = false;
        break;
      }
    }
  }
  if (previous_positions_.empty()) {
    previous_positions_ = world;
  }

  // Build a dense per-primitive mesh-light table only when its inputs are
  // invalidated.  Once a scene is known to contain no positive emitters,
  // position-only water/animation updates cannot make one appear and reuse
  // the cached zero table without allocating vectors or rebuilding an alias
  // distribution.
  const bool rebuild_emissive_distribution =
      !emissive_distribution_valid_ || topology_changed ||
      materials_generation_changed || emission_generation_changed ||
      visibility_generation_changed ||
      (cached_positive_emission_source_ &&
       (positions_generation_changed || transforms_generation_changed));
  auto &emissive_records = emissive_records_cache_;
  auto &emissive_weights = emissive_weights_cache_;
  std::uint32_t emissive_count = cached_emissive_count_;
  std::uint32_t hidden_source_emitter_count =
      cached_hidden_source_emitter_triangle_count_;
  std::uint32_t hidden_positive_weight_count =
      cached_hidden_positive_weight_triangle_count_;
  if (rebuild_emissive_distribution) {
    const auto emitter_distribution_begin =
        std::chrono::steady_clock::now();
    emissive_records.assign(total_indices / 3u, {});
    emissive_weights.assign(emissive_records.size(), 0.0);
    auto transformed_model_position =
        [&](std::uint32_t vertex) -> std::array<float, 3> {
      std::array<float, 3> result{
          world[static_cast<std::size_t>(vertex) * 3u + 0u],
          world[static_cast<std::size_t>(vertex) * 3u + 1u],
          world[static_cast<std::size_t>(vertex) * 3u + 2u]};
      if (vertex >= model_verts || bone_transforms_column_major == nullptr) {
        return result;
      }
      const std::uint32_t bone = rest_.bone_indices[vertex];
      if (bone >= bone_count) {
        return result;
      }
      const float *matrix = bone_transforms_column_major +
                            static_cast<std::size_t>(bone) * 16u;
      const float x = result[0];
      const float y = result[1];
      const float z = result[2];
      result[0] =
          matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
      result[1] =
          matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
      result[2] =
          matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
      return result;
    };
    const bool have_primitive_emission =
        rest_.primitive_emission.size() == model_primitive_count;
    emissive_count = 0u;
    hidden_source_emitter_count = 0u;
    hidden_positive_weight_count = 0u;
    bool positive_emission_source = false;
    for (std::size_t primitive = 0; primitive < model_primitive_count;
         ++primitive) {
      const std::uint32_t i0 = world_indices[primitive * 3u + 0u];
      const std::uint32_t i1 = world_indices[primitive * 3u + 1u];
      const std::uint32_t i2 = world_indices[primitive * 3u + 2u];
      if (i0 >= model_verts || i1 >= model_verts || i2 >= model_verts) {
        continue;
      }
      const std::array<float, 3> p0 = transformed_model_position(i0);
      const std::array<float, 3> p1 = transformed_model_position(i1);
      const std::array<float, 3> p2 = transformed_model_position(i2);
      const std::array<double, 3> edge1{
          static_cast<double>(p1[0] - p0[0]),
          static_cast<double>(p1[1] - p0[1]),
          static_cast<double>(p1[2] - p0[2])};
      const std::array<double, 3> edge2{
          static_cast<double>(p2[0] - p0[0]),
          static_cast<double>(p2[1] - p0[1]),
          static_cast<double>(p2[2] - p0[2])};
      const std::array<double, 3> cross{
          edge1[1] * edge2[2] - edge1[2] * edge2[1],
          edge1[2] * edge2[0] - edge1[0] * edge2[2],
          edge1[0] * edge2[1] - edge1[1] * edge2[0]};
      const double area =
          0.5 * std::sqrt(cross[0] * cross[0] + cross[1] * cross[1] +
                           cross[2] * cross[2]);
      std::array<float, 3> emission{};
      float visibility = 1.0f;
      bool source_emission_positive = false;
      if (have_primitive_emission) {
        const auto &source = rest_.primitive_emission[primitive];
        const std::uint32_t source_bone = rest_.bone_indices[i0];
        if (bone_tints_rgba != nullptr && source_bone < tint_count) {
          visibility = rtEmitterVisibilityScale(
              bone_tints_rgba[static_cast<std::size_t>(source_bone) * 4u +
                              3u]);
        }
        for (std::size_t channel = 0; channel < 3u; ++channel) {
          source_emission_positive =
              source_emission_positive ||
              (std::isfinite(source[channel]) && source[channel] > 0.0f);
          const float tint =
              (world_colors[static_cast<std::size_t>(i0) * 4u + channel] +
               world_colors[static_cast<std::size_t>(i1) * 4u + channel] +
               world_colors[static_cast<std::size_t>(i2) * 4u + channel]) /
              3.0f;
          emission[channel] = std::max(
              source[channel] * std::max(tint, 0.0f) * visibility, 0.0f);
        }
      }
      const double luminance =
          0.2126 * static_cast<double>(emission[0]) +
          0.7152 * static_cast<double>(emission[1]) +
          0.0722 * static_cast<double>(emission[2]);
      positive_emission_source =
          positive_emission_source ||
          (std::isfinite(luminance) && luminance > 0.0);
      RtEmissiveTriangleGpu &record = emissive_records[primitive];
      record.p0_probability = {p0[0], p0[1], p0[2], 0.0f};
      record.p1_acceptance = {p1[0], p1[1], p1[2], 0.0f};
      record.p2_area = {p2[0], p2[1], p2[2],
                        static_cast<float>(std::max(area, 0.0))};
      record.emission_luminance = {
          emission[0], emission[1], emission[2],
          static_cast<float>(std::max(luminance, 0.0))};
      record.metadata[2] = static_cast<std::uint32_t>(primitive);
      double final_weight = 0.0;
      if (std::isfinite(area) && std::isfinite(luminance) &&
          area > 1.0e-12 && luminance > 0.0) {
        final_weight = area * luminance;
        emissive_weights[primitive] = final_weight;
        record.metadata[1] = 1u;
        ++emissive_count;
      }
      const RtEmitterVisibilityAudit visibility_audit =
          auditRtEmitterVisibility(visibility, source_emission_positive,
                                   final_weight);
      hidden_source_emitter_count +=
          visibility_audit.hidden_source_emitter ? 1u : 0u;
      hidden_positive_weight_count +=
          visibility_audit.hidden_positive_weight ? 1u : 0u;
    }
    AliasTable emissive_alias;
    const bool emissive_alias_valid =
        emissive_count > 0u && emissive_alias.build(emissive_weights);
    if (emissive_alias_valid) {
      for (std::size_t primitive = 0; primitive < emissive_records.size();
           ++primitive) {
        emissive_records[primitive].p0_probability[3] =
            static_cast<float>(emissive_alias.probability(primitive));
        emissive_records[primitive].p1_acceptance[3] =
            static_cast<float>(emissive_alias.acceptance(primitive));
        emissive_records[primitive].metadata[0] =
            emissive_alias.aliasIndex(primitive);
      }
    } else {
      emissive_count = 0u;
    }
    cached_emissive_count_ = emissive_count;
    cached_hidden_source_emitter_triangle_count_ =
        hidden_source_emitter_count;
    cached_hidden_positive_weight_triangle_count_ =
        hidden_positive_weight_count;
    cached_positive_emission_source_ = positive_emission_source;
    emissive_distribution_valid_ = true;
    ++emitter_distribution_rebuild_count_;
    emitter_distribution_ms_ =
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - emitter_distribution_begin)
            .count();
  }

  if (geometry_states_.size() != desired_states.size()) {
    topology_changed = true;
  } else {
    for (std::size_t i = 0; i < desired_states.size(); ++i) {
      const GeometryState &current = geometry_states_[i];
      const GeometryState &desired = desired_states[i];
      if (current.kind != desired.kind ||
          current.blas_policy != desired.blas_policy ||
          current.instance_custom_index != desired.instance_custom_index ||
          current.source_bone_index != desired.source_bone_index ||
          current.first_index != desired.first_index ||
          current.index_count != desired.index_count ||
          current.opaque_geometry != desired.opaque_geometry) {
        topology_changed = true;
        break;
      }
    }
  }
  if (topology_changed) {
    destroyAs(tlas_);
    destroyGeometryStates();
    geometry_states_ = std::move(desired_states);
    for (GeometryState &state : geometry_states_) {
      state.pending_full_build = true;
    }
  } else {
    for (std::size_t i = 0; i < geometry_states_.size(); ++i) {
      GeometryState &state = geometry_states_[i];
      const GeometryState &desired = desired_states[i];
      state.pending_full_build = false;
      state.pending_refit = false;
      switch (classifyRtGeometryUpdate(
          state.content_hash, desired.content_hash, state.blas_policy,
          state.built_once)) {
      case RtGeometryUpdateKind::Refit:
          state.pending_refit = true;
          break;
      case RtGeometryUpdateKind::FullBuild:
          state.pending_full_build = true;
          break;
      case RtGeometryUpdateKind::None:
          break;
      }
      state.content_hash = desired.content_hash;
    }
  }
  // The explicit snapshot is owned across frame slots by the backend. It may
  // remain compatible even when this slot performs its first local AS build.
  motion_history_valid_ = previous_positions_compatible;

  const VkDeviceSize vert_bytes =
      static_cast<VkDeviceSize>(world.size() * sizeof(float));
  const VkDeviceSize index_bytes =
      static_cast<VkDeviceSize>(world_indices.size() *
                                sizeof(std::uint32_t));
  const VkDeviceSize normal_bytes =
      static_cast<VkDeviceSize>(world_normals.size() * sizeof(float));
  const VkDeviceSize uv_bytes =
      static_cast<VkDeviceSize>(world_uvs.size() * sizeof(float));
  const VkDeviceSize color_bytes =
      static_cast<VkDeviceSize>(world_colors.size() * sizeof(float));
  const VkDeviceSize tangent_bytes =
      static_cast<VkDeviceSize>(world_tangents.size() * sizeof(float));
  const VkDeviceSize primitive_flag_bytes =
      static_cast<VkDeviceSize>(primitive_flags.size() *
                                sizeof(std::uint32_t));
  const VkDeviceSize primitive_metadata_bytes =
      static_cast<VkDeviceSize>(
          primitive_metadata.size() *
          sizeof(std::array<std::uint32_t, 4>));
  const VkDeviceSize emissive_triangle_bytes =
      sizeof(std::array<std::uint32_t, 4>) +
      static_cast<VkDeviceSize>(emissive_records.size()) *
          sizeof(RtEmissiveTriangleGpu);

  constexpr VkBufferUsageFlags kHostUsage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  constexpr VkBufferUsageFlags kIndexUsage =
      kHostUsage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  constexpr VkBufferUsageFlags kNormalUsage =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
      VK_BUFFER_USAGE_TRANSFER_DST_BIT;

  const VkBuffer previous_vertex_buffer = host_vertices_.buffer;
  const VkBuffer previous_index_buffer = host_indices_.buffer;
  bool any_buffer_reallocated = false;
  if (host_vertices_.capacity < vert_bytes || !host_vertices_.buffer) {
    if (!createBuffer(vert_bytes, kHostUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_vertices_, true)) {
      return fail("host vertex buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_previous_vertices_.capacity < vert_bytes ||
      !host_previous_vertices_.buffer) {
    if (!createBuffer(vert_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_previous_vertices_, true)) {
      return fail("previous vertex buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_indices_.capacity < index_bytes || !host_indices_.buffer) {
    if (!createBuffer(index_bytes, kIndexUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_indices_, true)) {
      return fail("host index buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_normals_.capacity < normal_bytes || !host_normals_.buffer) {
    if (!createBuffer(normal_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_normals_, true)) {
      return fail("host normal buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_uvs_.capacity < uv_bytes || !host_uvs_.buffer) {
    if (!createBuffer(uv_bytes, kNormalUsage,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, host_uvs_,
                      true)) {
      return fail("host UV buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_colors_.capacity < color_bytes || !host_colors_.buffer) {
    if (!createBuffer(color_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_colors_, true)) {
      return fail("host color buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_tangents_.capacity < tangent_bytes || !host_tangents_.buffer) {
    if (!createBuffer(tangent_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_tangents_, true)) {
      return fail("host tangent buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_primitive_flags_.capacity < primitive_flag_bytes ||
      !host_primitive_flags_.buffer) {
    if (!createBuffer(primitive_flag_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_primitive_flags_, true)) {
      return fail("host primitive-flag buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_primitive_metadata_.capacity < primitive_metadata_bytes ||
      !host_primitive_metadata_.buffer) {
    if (!createBuffer(primitive_metadata_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_primitive_metadata_, true)) {
      return fail("host primitive-metadata buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  if (host_emissive_triangles_.capacity < emissive_triangle_bytes ||
      !host_emissive_triangles_.buffer) {
    if (!createBuffer(emissive_triangle_bytes, kNormalUsage,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      host_emissive_triangles_, true)) {
      return fail("host emissive-triangle buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }
  const VkDeviceSize instance_motion_bytes =
      static_cast<VkDeviceSize>(geometry_states_.size()) *
      sizeof(RtInstanceMotionGpu);
  if (host_instance_motion_.capacity < instance_motion_bytes ||
      !host_instance_motion_.buffer) {
    if (!createBuffer(instance_motion_bytes, kNormalUsage,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       host_instance_motion_, true)) {
      return fail("instance motion buffer allocation failed");
    }
    any_buffer_reallocated = true;
  }

  const bool upload_topology =
      any_buffer_reallocated || topology_changed || topology_generation_changed ||
      !uploaded_topology_valid_;
  const bool upload_positions =
      any_buffer_reallocated || topology_changed ||
      positions_generation_changed ||
      !uploaded_positions_valid_;
  const bool upload_transforms =
      any_buffer_reallocated || topology_changed ||
      transforms_generation_changed ||
      !uploaded_transforms_valid_;
  const bool upload_materials =
      any_buffer_reallocated || topology_changed ||
      materials_generation_changed ||
      transforms_generation_changed || !uploaded_material_valid_;
  const bool upload_emission =
      any_buffer_reallocated || rebuild_emissive_distribution ||
      !uploaded_emission_valid_;
  auto copy_upload = [&](void *destination, const void *source,
                         VkDeviceSize bytes) {
    if (bytes == 0u) {
      return;
    }
    std::memcpy(destination, source, static_cast<std::size_t>(bytes));
    upload_bytes_ += static_cast<std::uint64_t>(bytes);
  };
  if (upload_positions) {
    copy_upload(host_vertices_.mapped, world.data(), vert_bytes);
    copy_upload(host_previous_vertices_.mapped, previous_positions_.data(),
                vert_bytes);
  }
  if (upload_transforms) {
    copy_upload(host_normals_.mapped, world_normals.data(), normal_bytes);
    copy_upload(host_tangents_.mapped, world_tangents.data(), tangent_bytes);
  }
  if (upload_materials) {
    copy_upload(host_colors_.mapped, world_colors.data(), color_bytes);
  }
  host_vertices_.address = bufferDeviceAddress(host_vertices_.buffer);
  host_indices_.address = bufferDeviceAddress(host_indices_.buffer);
  host_normals_.address = bufferDeviceAddress(host_normals_.buffer);
  host_uvs_.address = bufferDeviceAddress(host_uvs_.buffer);
  host_colors_.address = bufferDeviceAddress(host_colors_.buffer);
  host_tangents_.address = bufferDeviceAddress(host_tangents_.buffer);
  host_primitive_flags_.address =
      bufferDeviceAddress(host_primitive_flags_.buffer);
  host_primitive_metadata_.address =
      bufferDeviceAddress(host_primitive_metadata_.buffer);
  if (upload_topology || materials_generation_changed) {
    copy_upload(host_primitive_flags_.mapped, primitive_flags.data(),
                primitive_flag_bytes);
    copy_upload(host_primitive_metadata_.mapped, primitive_metadata.data(),
                primitive_metadata_bytes);
  }
  const std::array<std::uint32_t, 4> emissive_header{
      static_cast<std::uint32_t>(emissive_records.size()),
      emissive_count, 1u, 0u};
  if (upload_emission) {
    copy_upload(host_emissive_triangles_.mapped, emissive_header.data(),
                sizeof(emissive_header));
    if (!emissive_records.empty()) {
      copy_upload(
          static_cast<std::byte *>(host_emissive_triangles_.mapped) +
              sizeof(emissive_header),
          emissive_records.data(),
          emissive_records.size() * sizeof(RtEmissiveTriangleGpu));
    }
  }
  if (upload_topology) {
    copy_upload(host_indices_.mapped, world_indices.data(), index_bytes);
    copy_upload(host_uvs_.mapped, world_uvs.data(), uv_bytes);
  }
  if (generations_valid) {
    last_generations_ = generations;
    generations_valid_ = true;
    if (upload_topology) {
      uploaded_topology_generation_ = generations.topology;
      uploaded_topology_valid_ = true;
    }
    if (upload_positions) {
      uploaded_positions_generation_ = generations.positions;
      uploaded_positions_valid_ = true;
    }
    if (upload_transforms) {
      uploaded_transforms_generation_ = generations.transforms;
      uploaded_transforms_valid_ = true;
    }
    if (upload_materials) {
      uploaded_material_generation_ = generations.materials;
      uploaded_material_valid_ = true;
    }
    if (upload_emission) {
      uploaded_emission_generation_ = generations.emission;
      uploaded_emission_valid_ = true;
    }
  } else {
    generations_valid_ = false;
    uploaded_topology_valid_ = uploaded_positions_valid_ =
        uploaded_transforms_valid_ = uploaded_material_valid_ =
            uploaded_emission_valid_ = false;
  }

  const bool input_buffers_reallocated =
      previous_vertex_buffer != host_vertices_.buffer ||
      previous_index_buffer != host_indices_.buffer;
  last_vertex_count_ = total_verts;
  last_index_count_ = total_indices;
  emissive_triangle_count_ = emissive_count;
  bool any_full_build = false;
  bool any_refit = false;
  for (GeometryState &state : geometry_states_) {
    const VkDeviceAddress previous_address = state.blas.address;
    const bool had_built_blas = state.built_once;
    if (!ensureBlasStorage(state, total_verts)) {
      xpbd::log::warn("Vulkan RT: BLAS storage allocation failed");
      geometry_prepared_ = false;
      return fail("BLAS storage allocation failed");
    }
    if (input_buffers_reallocated || !had_built_blas ||
        state.blas.address != previous_address) {
      state.pending_full_build = true;
      state.pending_refit = false;
      if (state.blas.address != previous_address || input_buffers_reallocated) {
        tlas_requires_full_build_ = true;
      }
    }
    any_full_build = any_full_build || state.pending_full_build;
    any_refit = any_refit || state.pending_refit;
  }

  if (!ensureTlasStorage(
          static_cast<std::uint32_t>(geometry_states_.size()))) {
    xpbd::log::warn("Vulkan RT: TLAS storage allocation failed");
    geometry_prepared_ = false;
    return fail("TLAS storage allocation failed");
  }
  writeInstanceData(
      bone_transforms_column_major, bone_count,
      bone_tints_rgba, tint_count,
      previous_bone_transforms_column_major, previous_bone_count,
      motion_history_valid_);

  if (any_full_build) {
    pending_ = PendingBuild::Full;
    if (full_build_count_ == 0u) {
      pending_build_reason_ = RtAccelerationBuildReason::InitialBuild;
    } else if (topology_changed || topology_counts_changed) {
      pending_build_reason_ = RtAccelerationBuildReason::TopologyChanged;
    } else if (input_buffers_reallocated) {
      pending_build_reason_ = RtAccelerationBuildReason::StorageReallocated;
    } else if (!had_tlas) {
      pending_build_reason_ = RtAccelerationBuildReason::MissingTopLevel;
    } else {
      pending_build_reason_ = RtAccelerationBuildReason::OtherFullBuild;
    }
  } else if (any_refit) {
    pending_ = PendingBuild::Refit;
    pending_build_reason_ = RtAccelerationBuildReason::StableGeometryRefit;
  } else if (instance_transforms_changed) {
    pending_ = PendingBuild::TopLevel;
    pending_build_reason_ =
        RtAccelerationBuildReason::InstanceTransformsChanged;
  } else {
    pending_ = PendingBuild::None;
    pending_build_reason_ = RtAccelerationBuildReason::None;
  }
  geometry_prepared_ = true;
  return true;
}

void VulkanRtScene::recordBuilds(VkCommandBuffer cmd) {
  if (!initialized_ || !procs_ok_ || !cmd || pending_ == PendingBuild::None ||
      !geometry_prepared_ || geometry_states_.empty() || !tlas_.handle) {
    return;
  }

  // Host-visible vertex writes must be visible to the AS build.
  VkMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  host_barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
  host_barrier.dstAccessMask =
      VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
      VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                       VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                       0, 1, &host_barrier, 0, nullptr, 0, nullptr);

  const RtAccelerationBuildReason recorded_reason = pending_build_reason_;
  VkMemoryBarrier blas_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  blas_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  blas_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  bool recorded_full_blas = false;
  for (GeometryState &state : geometry_states_) {
    if (!state.pending_full_build && !state.pending_refit) {
      continue;
    }
    const bool update = state.pending_refit && !state.pending_full_build;
    recordBlasBuild(cmd, state, update);
    if (update) {
      ++refit_count_;
    } else {
      state.built_once = true;
      ++full_build_count_;
      recorded_full_blas = true;
    }
    state.pending_full_build = false;
    state.pending_refit = false;
    // Every BLAS uses the same scratch buffer, so serialize its reuse.
    vkCmdPipelineBarrier(
        cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
        &blas_barrier, 0, nullptr, 0, nullptr);
  }
  const bool tlas_update = tlas_built_once_ && !recorded_full_blas &&
                           !tlas_requires_full_build_ &&
                           pending_ != PendingBuild::Full;
  recordTlasBuild(cmd, tlas_update);
  if (tlas_update) {
    ++tlas_update_count_;
  } else {
    ++tlas_full_build_count_;
  }
  tlas_built_once_ = true;
  tlas_requires_full_build_ = false;
  last_build_reason_ = recorded_reason;
  last_tlas_reason_ = recorded_reason;

  // Make AS readable by path-tracer compute and hybrid ray-query fragments.
  VkMemoryBarrier use_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  use_barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
  use_barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                              VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(
      cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0, 1, &use_barrier, 0, nullptr, 0, nullptr);

  pending_ = PendingBuild::None;
  pending_build_reason_ = RtAccelerationBuildReason::None;
}

bool VulkanRtScene::updateAndBuild(const float *bone_transforms_column_major,
                                   std::size_t bone_count) {
  // Blocking fallback for callers that do not own a frame command buffer.
  if (!updateGeometry(bone_transforms_column_major, bone_count)) {
    return false;
  }
  if (pending_ == PendingBuild::None) {
    return ready();
  }
  VkCommandBuffer cmd = beginOneShot();
  if (!cmd) {
    return false;
  }
  recordBuilds(cmd);
  submitOneShot(cmd);
  return ready();
}

void VulkanRtScene::writeTlasDescriptor(VkDescriptorSet set,
                                        std::uint32_t binding) const {
  if (!ready() || set == VK_NULL_HANDLE) {
    return;
  }
  VkWriteDescriptorSetAccelerationStructureKHR as_info{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  as_info.accelerationStructureCount = 1;
  VkAccelerationStructureKHR tlas = tlas_.handle;
  as_info.pAccelerationStructures = &tlas;

  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.pNext = &as_info;
  write.dstSet = set;
  write.dstBinding = binding;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  ++descriptor_write_count_;
}

} // namespace xpbd::gfx
