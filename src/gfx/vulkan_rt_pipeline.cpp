#include "xpbd/gfx/vulkan_rt_pipeline.hpp"

#include "xpbd/log.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace xpbd::gfx {
namespace {

static const std::uint32_t kRtDebugRaygen[] = {
#include "spirv/rt_debug.rgen.spv.inc"
};
static const std::uint32_t kRtDebugMiss[] = {
#include "spirv/rt_debug.rmiss.spv.inc"
};
static const std::uint32_t kRtShadowMiss[] = {
#include "spirv/rt_shadow.rmiss.spv.inc"
};
static const std::uint32_t kRtDebugClosestHit[] = {
#include "spirv/rt_debug.rchit.spv.inc"
};
static const std::uint32_t kRtDebugAnyHit[] = {
#include "spirv/rt_debug.rahit.spv.inc"
};

constexpr std::uint32_t kShaderGroupCount = 4u;

struct RtPathPushConstants {
  float inverse_view_projection[16]{};
  float view_projection[16]{};
  // xyz camera position; w analytic environment strength.
  float camera_environment[4]{};
  std::uint32_t size_mode[4]{};
  std::uint32_t sampling[4]{};
  float light_direction_ambient[4]{};
  float light_color_intensity[4]{};
  std::uint32_t depth_limits[4]{};
  // x/y radiance clamps, z emissive multiplier, w light samples/path.
  float integrator[4]{};
  // xy: pixel-space camera jitter; z: temporal reconstruction input flag;
  // w: optional-output mask, bit-cast from uint32_t.
  float camera_jitter[4]{};
};
static_assert(sizeof(RtPathPushConstants) == 256u);

} // namespace

bool VulkanRtPipeline::init(VkPhysicalDevice physical_device,
                            VkDevice device) {
  shutdown();
  physical_device_ = physical_device;
  device_ = device;
  if (physical_device_ == VK_NULL_HANDLE || device_ == VK_NULL_HANDLE) {
    shutdown();
    return false;
  }
  VkPhysicalDeviceProperties device_properties{};
  vkGetPhysicalDeviceProperties(physical_device_, &device_properties);
  if (device_properties.limits.maxPushConstantsSize <
      sizeof(RtPathPushConstants)) {
    xpbd::log::warnf(
        "Vulkan RT Pipeline requires %zu push-constant bytes, device exposes "
        "%u",
        sizeof(RtPathPushConstants),
        device_properties.limits.maxPushConstantsSize);
    shutdown();
    return false;
  }
  if (!loadProcedures() || !createDescriptorResources() ||
      !createPipelineAndSbt()) {
    shutdown();
    return false;
  }
  xpbd::log::infof(
      "Vulkan RT Pipeline ready: groups=%u handle=%u align=%u base=%u "
      "stride=%u sbt=%llu",
      kShaderGroupCount, properties_.shaderGroupHandleSize,
      properties_.shaderGroupHandleAlignment,
      properties_.shaderGroupBaseAlignment, shader_group_stride_,
      static_cast<unsigned long long>(sbt_.size));
  return true;
}

void VulkanRtPipeline::shutdown() {
  if (device_ != VK_NULL_HANDLE) {
    destroyBuffer(sbt_);
    if (pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, pipeline_, nullptr);
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    }
    if (descriptor_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    }
    if (descriptor_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
    }
  }
  descriptor_layout_ = VK_NULL_HANDLE;
  descriptor_pool_ = VK_NULL_HANDLE;
  descriptor_set_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  pipeline_ = VK_NULL_HANDLE;
  raygen_region_ = {};
  miss_region_ = {};
  hit_region_ = {};
  callable_region_ = {};
  properties_ = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
  shader_group_stride_ = 0;
  dispatch_logged_ = false;
  bounds_failure_logged_ = false;
  descriptor_key_ = {};
  descriptor_key_valid_ = false;
  descriptor_write_calls_ = 0;
  descriptor_cache_hits_ = 0;
  descriptor_entries_written_ = 0;
  descriptor_update_ms_ = 0.0f;
  vkCreateRayTracingPipelinesKHR_ = nullptr;
  vkGetRayTracingShaderGroupHandlesKHR_ = nullptr;
  vkCmdTraceRaysKHR_ = nullptr;
  vkGetBufferDeviceAddress_ = nullptr;
  device_ = VK_NULL_HANDLE;
  physical_device_ = VK_NULL_HANDLE;
}

bool VulkanRtPipeline::loadProcedures() {
  vkCreateRayTracingPipelinesKHR_ =
      reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
          vkGetDeviceProcAddr(device_, "vkCreateRayTracingPipelinesKHR"));
  vkGetRayTracingShaderGroupHandlesKHR_ =
      reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
          vkGetDeviceProcAddr(device_,
                              "vkGetRayTracingShaderGroupHandlesKHR"));
  vkCmdTraceRaysKHR_ = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
      vkGetDeviceProcAddr(device_, "vkCmdTraceRaysKHR"));
  vkGetBufferDeviceAddress_ =
      reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
          vkGetDeviceProcAddr(device_, "vkGetBufferDeviceAddress"));
  if (vkGetBufferDeviceAddress_ == nullptr) {
    vkGetBufferDeviceAddress_ =
        reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
            vkGetDeviceProcAddr(device_, "vkGetBufferDeviceAddressKHR"));
  }
  return vkCreateRayTracingPipelinesKHR_ != nullptr &&
         vkGetRayTracingShaderGroupHandlesKHR_ != nullptr &&
         vkCmdTraceRaysKHR_ != nullptr &&
         vkGetBufferDeviceAddress_ != nullptr;
}

std::uint32_t VulkanRtPipeline::findMemoryType(
    std::uint32_t type_bits, VkMemoryPropertyFlags properties) const {
  VkPhysicalDeviceMemoryProperties memory_properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);
  for (std::uint32_t index = 0;
       index < memory_properties.memoryTypeCount; ++index) {
    if ((type_bits & (1u << index)) != 0u &&
        (memory_properties.memoryTypes[index].propertyFlags & properties) ==
            properties) {
      return index;
    }
  }
  return (std::numeric_limits<std::uint32_t>::max)();
}

bool VulkanRtPipeline::createBuffer(VkDeviceSize size,
                                    VkBufferUsageFlags usage,
                                    Buffer &buffer) {
  destroyBuffer(buffer);
  VkBufferCreateInfo create_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  create_info.size = size;
  create_info.usage = usage;
  create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &create_info, nullptr, &buffer.buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
  const std::uint32_t memory_type = findMemoryType(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memory_type == (std::numeric_limits<std::uint32_t>::max)()) {
    destroyBuffer(buffer);
    return false;
  }
  VkMemoryAllocateFlagsInfo flags{
      VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo allocate_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate_info.pNext = &flags;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &allocate_info, nullptr, &buffer.memory) !=
          VK_SUCCESS ||
      vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0) !=
          VK_SUCCESS ||
      vkMapMemory(device_, buffer.memory, 0, size, 0, &buffer.mapped) !=
          VK_SUCCESS) {
    destroyBuffer(buffer);
    return false;
  }
  VkBufferDeviceAddressInfo address_info{
      VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  address_info.buffer = buffer.buffer;
  buffer.address = vkGetBufferDeviceAddress_(device_, &address_info);
  buffer.size = size;
  if (buffer.address == 0u) {
    destroyBuffer(buffer);
    return false;
  }
  return true;
}

void VulkanRtPipeline::destroyBuffer(Buffer &buffer) {
  if (device_ != VK_NULL_HANDLE) {
    if (buffer.mapped != nullptr && buffer.memory != VK_NULL_HANDLE) {
      vkUnmapMemory(device_, buffer.memory);
    }
    if (buffer.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(device_, buffer.buffer, nullptr);
    }
    if (buffer.memory != VK_NULL_HANDLE) {
      vkFreeMemory(device_, buffer.memory, nullptr);
    }
  }
  buffer = {};
}

VkShaderModule VulkanRtPipeline::createShaderModule(
    const std::uint32_t *words, std::size_t word_count) const {
  VkShaderModuleCreateInfo create_info{
      VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  create_info.codeSize = word_count * sizeof(std::uint32_t);
  create_info.pCode = words;
  VkShaderModule module = VK_NULL_HANDLE;
  return vkCreateShaderModule(device_, &create_info, nullptr, &module) ==
                 VK_SUCCESS
             ? module
             : VK_NULL_HANDLE;
}

bool VulkanRtPipeline::createDescriptorResources() {
  constexpr VkShaderStageFlags kAllRtStages =
      VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
      VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
  std::array<VkDescriptorSetLayoutBinding, 28> bindings{};
  for (std::uint32_t index = 0; index < bindings.size(); ++index) {
    bindings[index].binding = index;
    bindings[index].descriptorCount = 1;
    bindings[index].stageFlags = kAllRtStages;
    bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[5].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[12].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[13].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[14].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  bindings[21].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[22].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[26].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[27].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

  VkDescriptorSetLayoutCreateInfo layout_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount =
      static_cast<std::uint32_t>(bindings.size());
  layout_info.pBindings = bindings.data();
  if (vkCreateDescriptorSetLayout(device_, &layout_info, nullptr,
                                  &descriptor_layout_) != VK_SUCCESS) {
    return false;
  }

  const std::array<VkDescriptorPoolSize, 4> pool_sizes{{
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 9},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 14},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
  }};
  VkDescriptorPoolCreateInfo pool_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount =
      static_cast<std::uint32_t>(pool_sizes.size());
  pool_info.pPoolSizes = pool_sizes.data();
  if (vkCreateDescriptorPool(device_, &pool_info, nullptr,
                             &descriptor_pool_) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo allocate_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate_info.descriptorPool = descriptor_pool_;
  allocate_info.descriptorSetCount = 1;
  allocate_info.pSetLayouts = &descriptor_layout_;
  if (vkAllocateDescriptorSets(device_, &allocate_info,
                               &descriptor_set_) != VK_SUCCESS) {
    return false;
  }

  VkPushConstantRange push_range{};
  push_range.stageFlags = kAllRtStages;
  push_range.offset = 0;
  push_range.size = sizeof(RtPathPushConstants);
  VkPipelineLayoutCreateInfo pipeline_layout_info{
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 1;
  pipeline_layout_info.pSetLayouts = &descriptor_layout_;
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &push_range;
  return vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                                &pipeline_layout_) == VK_SUCCESS;
}

bool VulkanRtPipeline::createPipelineAndSbt() {
  VkPhysicalDeviceProperties2 properties2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  properties2.pNext = &properties_;
  vkGetPhysicalDeviceProperties2(physical_device_, &properties2);

  std::array<VkShaderModule, 5> modules{
      createShaderModule(kRtDebugRaygen, std::size(kRtDebugRaygen)),
      createShaderModule(kRtDebugMiss, std::size(kRtDebugMiss)),
      createShaderModule(kRtShadowMiss, std::size(kRtShadowMiss)),
      createShaderModule(kRtDebugClosestHit,
                         std::size(kRtDebugClosestHit)),
      createShaderModule(kRtDebugAnyHit, std::size(kRtDebugAnyHit))};
  if (std::find(modules.begin(), modules.end(), VK_NULL_HANDLE) !=
      modules.end()) {
    for (VkShaderModule module : modules) {
      if (module != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, module, nullptr);
      }
    }
    return false;
  }

  const std::array<VkShaderStageFlagBits, 5> stage_bits{
      VK_SHADER_STAGE_RAYGEN_BIT_KHR, VK_SHADER_STAGE_MISS_BIT_KHR,
      VK_SHADER_STAGE_MISS_BIT_KHR, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR,
      VK_SHADER_STAGE_ANY_HIT_BIT_KHR};
  std::array<VkPipelineShaderStageCreateInfo, 5> stages{};
  for (std::uint32_t index = 0; index < stages.size(); ++index) {
    stages[index].sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[index].stage = stage_bits[index];
    stages[index].module = modules[index];
    stages[index].pName = "main";
  }

  std::array<VkRayTracingShaderGroupCreateInfoKHR, kShaderGroupCount> groups{};
  for (auto &group : groups) {
    group.sType =
        VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
    group.generalShader = VK_SHADER_UNUSED_KHR;
    group.closestHitShader = VK_SHADER_UNUSED_KHR;
    group.anyHitShader = VK_SHADER_UNUSED_KHR;
    group.intersectionShader = VK_SHADER_UNUSED_KHR;
  }
  for (std::uint32_t group = 0; group < 3u; ++group) {
    groups[group].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
    groups[group].generalShader = group;
  }
  groups[3].type =
      VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
  groups[3].closestHitShader = 3u;
  groups[3].anyHitShader = 4u;

  VkRayTracingPipelineCreateInfoKHR pipeline_info{
      VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR};
  pipeline_info.stageCount =
      static_cast<std::uint32_t>(stages.size());
  pipeline_info.pStages = stages.data();
  pipeline_info.groupCount =
      static_cast<std::uint32_t>(groups.size());
  pipeline_info.pGroups = groups.data();
  pipeline_info.maxPipelineRayRecursionDepth = 1u;
  pipeline_info.layout = pipeline_layout_;
  const VkResult pipeline_result = vkCreateRayTracingPipelinesKHR_(
      device_, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
      &pipeline_);
  for (VkShaderModule module : modules) {
    vkDestroyShaderModule(device_, module, nullptr);
  }
  if (pipeline_result != VK_SUCCESS) {
    return false;
  }

  RtSbtLayoutRequest layout_request;
  layout_request.shader_group_handle_size =
      properties_.shaderGroupHandleSize;
  layout_request.shader_group_handle_alignment =
      properties_.shaderGroupHandleAlignment;
  layout_request.shader_group_base_alignment =
      properties_.shaderGroupBaseAlignment;
  layout_request.max_shader_group_stride =
      properties_.maxShaderGroupStride;
  layout_request.miss_group_count = 2u;
  layout_request.hit_group_count = 1u;
  layout_request.buffer_bytes =
      (std::numeric_limits<std::uint64_t>::max)();
  const auto provisional_layout = computeRtSbtLayout(layout_request);
  if (!provisional_layout) {
    return false;
  }
  shader_group_stride_ = provisional_layout->shader_group_stride;
  const VkDeviceSize allocation_bytes =
      provisional_layout->layout_bytes +
      properties_.shaderGroupBaseAlignment - 1u;
  if (!createBuffer(
          allocation_bytes,
          VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
          sbt_)) {
    return false;
  }
  layout_request.buffer_device_address = sbt_.address;
  layout_request.buffer_bytes = sbt_.size;
  const auto layout = computeRtSbtLayout(layout_request);
  if (!layout) {
    return false;
  }

  const std::size_t handle_size = properties_.shaderGroupHandleSize;
  std::vector<std::uint8_t> handles(handle_size * kShaderGroupCount);
  if (vkGetRayTracingShaderGroupHandlesKHR_(
          device_, pipeline_, 0, kShaderGroupCount, handles.size(),
          handles.data()) != VK_SUCCESS) {
    return false;
  }
  auto copy_handle = [&](VkDeviceSize offset, std::uint32_t group) {
    std::memcpy(static_cast<std::uint8_t *>(sbt_.mapped) +
                    layout->base_offset +
                    offset,
                handles.data() + static_cast<std::size_t>(group) *
                                     handle_size,
                handle_size);
  };
  copy_handle(layout->raygen_offset, 0u);
  copy_handle(layout->miss_offset, 1u);
  copy_handle(layout->miss_offset + shader_group_stride_, 2u);
  copy_handle(layout->hit_offset, 3u);

  const VkDeviceAddress base_address = sbt_.address + layout->base_offset;
  raygen_region_ = {base_address + layout->raygen_offset,
                    shader_group_stride_,
                    shader_group_stride_};
  miss_region_ = {base_address + layout->miss_offset, shader_group_stride_,
                  shader_group_stride_ * 2u};
  hit_region_ = {base_address + layout->hit_offset, shader_group_stride_,
                 shader_group_stride_};
  callable_region_ = {};
  return true;
}

bool VulkanRtPipeline::record(
    VkCommandBuffer command_buffer, const VulkanRtScene &scene,
    const RtPipelineDispatchParams &params) {
  if (!ready() || command_buffer == VK_NULL_HANDLE || !scene.ready() ||
      params.output_view == VK_NULL_HANDLE ||
      params.depth_view == VK_NULL_HANDLE ||
      params.aov_array_view == VK_NULL_HANDLE ||
      params.statistics_view == VK_NULL_HANDLE ||
      params.rr_motion_view == VK_NULL_HANDLE ||
      params.rr_diffuse_albedo_view == VK_NULL_HANDLE ||
      params.rr_specular_albedo_view == VK_NULL_HANDLE ||
      params.rr_normal_roughness_view == VK_NULL_HANDLE ||
      params.rr_specular_hit_distance_view == VK_NULL_HANDLE ||
      params.albedo_view == VK_NULL_HANDLE ||
      params.normal_view == VK_NULL_HANDLE ||
      params.specular_view == VK_NULL_HANDLE ||
      params.albedo_sampler == VK_NULL_HANDLE ||
      params.motion_frame_buffer == VK_NULL_HANDLE ||
      params.motion_frame_buffer_bytes < 80u ||
      params.inverse_view_projection == nullptr ||
      params.view_projection == nullptr) {
    descriptor_write_calls_ = 0;
    descriptor_cache_hits_ = 0;
    descriptor_entries_written_ = 0;
    descriptor_update_ms_ = 0.0f;
    return false;
  }
  descriptor_write_calls_ = 0;
  descriptor_cache_hits_ = 0;
  descriptor_entries_written_ = 0;
  descriptor_update_ms_ = 0.0f;
  if (params.hdr_environment &&
      (params.environment_view == VK_NULL_HANDLE ||
       params.environment_sampler == VK_NULL_HANDLE ||
       params.environment_distribution == VK_NULL_HANDLE ||
       params.environment_distribution_bytes == 0u)) {
    return false;
  }
  if (params.debug_view == RtDebugView::Off &&
      params.sample_count == 0u) {
    return false;
  }
  const std::uint64_t primitive_count = scene.pathTraceIndexCount() / 3u;
  const RtSceneStats scene_stats = scene.stats();
  const RtDispatchBufferBounds bounds{
      scene.pathTraceVertexCount(),
      primitive_count,
      scene_stats.instance_count,
      scene.normalBufferBytes(),
      scene.tangentBufferBytes(),
      scene.indexAttribBufferBytes(),
      scene.uvBufferBytes(),
      scene.colorBufferBytes(),
      scene.primitiveFlagBufferBytes(),
      scene.primitiveMetadataBufferBytes(),
      scene.instanceMetadataBufferBytes()};
  const bool buffers_valid =
      scene.normalBuffer() != VK_NULL_HANDLE &&
      scene.tangentBuffer() != VK_NULL_HANDLE &&
      scene.indexAttribBuffer() != VK_NULL_HANDLE &&
      scene.uvBuffer() != VK_NULL_HANDLE &&
      scene.colorBuffer() != VK_NULL_HANDLE &&
      scene.primitiveFlagBuffer() != VK_NULL_HANDLE &&
      scene.primitiveMetadataBuffer() != VK_NULL_HANDLE &&
      scene.instanceMetadataBuffer() != VK_NULL_HANDLE &&
      scene.emissiveTriangleBuffer() != VK_NULL_HANDLE &&
      scene.positionBuffer() != VK_NULL_HANDLE &&
      scene.previousPositionBuffer() != VK_NULL_HANDLE &&
      scene.instanceMotionBuffer() != VK_NULL_HANDLE &&
      scene.positionBufferBytes() >=
          static_cast<VkDeviceSize>(scene.pathTraceVertexCount()) * 12u &&
      scene.previousPositionBufferBytes() >=
          static_cast<VkDeviceSize>(scene.pathTraceVertexCount()) * 12u &&
      scene.instanceMotionBufferBytes() >=
          static_cast<VkDeviceSize>(scene_stats.instance_count) * 144u &&
      rtDispatchBuffersInBounds(bounds);
  if (!buffers_valid) {
    if (!bounds_failure_logged_) {
      xpbd::log::warn("Vulkan RT Pipeline dispatch rejected invalid "
                      "attribute/identity buffer bounds");
      bounds_failure_logged_ = true;
    }
    return false;
  }

  VkWriteDescriptorSetAccelerationStructureKHR acceleration_info{
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
  VkAccelerationStructureKHR tlas = scene.tlas();
  acceleration_info.accelerationStructureCount = 1;
  acceleration_info.pAccelerationStructures = &tlas;
  std::array<VkDescriptorImageInfo, 13> images{};
  images[0].imageView = params.output_view;
  images[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[1].imageView = params.albedo_view;
  images[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[1].sampler = params.albedo_sampler;
  images[2].imageView = params.depth_view;
  images[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[3].imageView = params.normal_view;
  images[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[3].sampler = params.albedo_sampler;
  images[4].imageView = params.specular_view;
  images[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[4].sampler = params.albedo_sampler;
  images[5].imageView = params.environment_view;
  images[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  images[5].sampler = params.environment_sampler;
  images[6].imageView = params.aov_array_view;
  images[6].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[7].imageView = params.statistics_view;
  images[7].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[8].imageView = params.rr_motion_view;
  images[8].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[9].imageView = params.rr_specular_hit_distance_view;
  images[9].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[10].imageView = params.rr_diffuse_albedo_view;
  images[10].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[11].imageView = params.rr_specular_albedo_view;
  images[11].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  images[12].imageView = params.rr_normal_roughness_view;
  images[12].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

  std::array<VkDescriptorBufferInfo, 14> buffers{};
  const std::array<VkBuffer, 14> buffer_handles{
      scene.normalBuffer(), scene.indexAttribBuffer(), scene.uvBuffer(),
      scene.colorBuffer(), scene.primitiveFlagBuffer(),
      scene.primitiveMetadataBuffer(), scene.instanceMetadataBuffer(),
      scene.tangentBuffer(), params.environment_distribution,
      scene.emissiveTriangleBuffer(), scene.positionBuffer(),
      scene.previousPositionBuffer(), scene.instanceMotionBuffer(),
      params.motion_frame_buffer};
  const std::array<VkDeviceSize, 14> buffer_sizes{
      scene.normalBufferBytes(), scene.indexAttribBufferBytes(),
      scene.uvBufferBytes(), scene.colorBufferBytes(),
      scene.primitiveFlagBufferBytes(), scene.primitiveMetadataBufferBytes(),
      scene.instanceMetadataBufferBytes(), scene.tangentBufferBytes(),
      params.environment_distribution_bytes,
      scene.emissiveTriangleBufferBytes(), scene.positionBufferBytes(),
      scene.previousPositionBufferBytes(), scene.instanceMotionBufferBytes(),
      params.motion_frame_buffer_bytes};
  for (std::size_t index = 0; index < buffers.size(); ++index) {
    buffers[index].buffer = buffer_handles[index];
    buffers[index].range = buffer_sizes[index];
  }

  std::array<VkWriteDescriptorSet, 28> writes{};
  for (std::uint32_t binding = 0; binding < writes.size(); ++binding) {
    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[binding].dstSet = descriptor_set_;
    writes[binding].dstBinding = binding;
    writes[binding].descriptorCount = 1;
    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  }
  writes[0].pNext = &acceleration_info;
  writes[0].descriptorType =
      VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
  writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[1].pImageInfo = &images[0];
  writes[2].pBufferInfo = &buffers[0];
  writes[3].pBufferInfo = &buffers[1];
  writes[4].pBufferInfo = &buffers[2];
  writes[5].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[5].pImageInfo = &images[1];
  writes[6].pBufferInfo = &buffers[3];
  writes[7].pBufferInfo = &buffers[4];
  writes[8].pBufferInfo = &buffers[5];
  writes[9].pBufferInfo = &buffers[6];
  writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[10].pImageInfo = &images[2];
  writes[11].pBufferInfo = &buffers[7];
  writes[12].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[12].pImageInfo = &images[3];
  writes[13].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[13].pImageInfo = &images[4];
  writes[14].descriptorType =
      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  writes[14].pImageInfo = &images[5];
  writes[15].pBufferInfo = &buffers[8];
  writes[16].pBufferInfo = &buffers[9];
  writes[17].pBufferInfo = &buffers[10];
  writes[18].pBufferInfo = &buffers[11];
  writes[19].pBufferInfo = &buffers[12];
  writes[20].pBufferInfo = &buffers[13];
  writes[21].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[21].pImageInfo = &images[6];
  writes[22].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[22].pImageInfo = &images[7];
  writes[23].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[23].pImageInfo = &images[8];
  writes[24].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[24].pImageInfo = &images[9];
  writes[25].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[25].pImageInfo = &images[10];
  writes[26].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[26].pImageInfo = &images[11];
  writes[27].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[27].pImageInfo = &images[12];
  DescriptorKey descriptor_key{};
  descriptor_key.tlas = tlas;
  for (std::size_t index = 0; index < images.size(); ++index) {
    descriptor_key.image_views[index] = images[index].imageView;
    descriptor_key.image_samplers[index] = images[index].sampler;
  }
  descriptor_key.buffers = buffer_handles;
  descriptor_key.buffer_sizes = buffer_sizes;
  if (descriptor_key_valid_ && descriptor_key == descriptor_key_) {
    ++descriptor_cache_hits_;
  } else {
    const auto update_begin = std::chrono::steady_clock::now();
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
        nullptr);
    descriptor_update_ms_ =
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - update_begin)
            .count();
    descriptor_key_ = descriptor_key;
    descriptor_key_valid_ = true;
    descriptor_write_calls_ = 1;
    descriptor_entries_written_ = writes.size();
  }

  RtPathPushConstants push{};
  std::memcpy(push.inverse_view_projection,
              params.inverse_view_projection,
              sizeof(push.inverse_view_projection));
  std::memcpy(push.view_projection, params.view_projection,
              sizeof(push.view_projection));
  push.camera_environment[0] = params.camera_position[0];
  push.camera_environment[1] = params.camera_position[1];
  push.camera_environment[2] = params.camera_position[2];
  const PathTraceSettings settings =
      normalizePathTraceSettings(params.settings);
  push.camera_environment[3] =
      settings.analytic_environment_strength;
  push.size_mode[0] = (std::max)(params.width, 1u);
  push.size_mode[1] = (std::max)(params.height, 1u);
  push.size_mode[2] =
      static_cast<std::uint32_t>(params.debug_view);
  push.size_mode[3] = settings.max_bounces;
  push.sampling[0] = params.sample_base;
  push.sampling[1] = params.sample_count;
  push.sampling[2] =
      resolvedPathTraceSeed(settings) ^
      (params.temporal_reconstruction_input
           ? (params.frame_index * 0x9e3779b9u + 0x85ebca6bu)
           : 0u);
  push.sampling[3] =
      (settings.russian_roulette ? 1u : 0u) |
      ((settings.russian_roulette_start & 0x7fu) << 1u) |
      ((params.material_feature_flags & 0xffu) << 8u) |
      ((params.material_debug_view & 0xffu) << 16u) |
      (params.hdr_environment ? (1u << 24u) : 0u) |
      (settings.analytic_lights ? (1u << 25u) : 0u) |
      (settings.emissive_surfaces ? (1u << 26u) : 0u) |
      (settings.next_event_estimation ? (1u << 27u) : 0u) |
      (settings.multiple_importance_sampling ? (1u << 28u) : 0u) |
      (settings.environment_importance_sampling ? (1u << 29u) : 0u) |
      (settings.emissive_mesh_sampling ? (1u << 30u) : 0u) |
      (params.ray_reconstruction_guides ? (1u << 31u) : 0u);
  push.light_direction_ambient[0] = params.light_direction[0];
  push.light_direction_ambient[1] = params.light_direction[1];
  push.light_direction_ambient[2] = params.light_direction[2];
  push.light_direction_ambient[3] = params.ambient;
  push.light_color_intensity[0] = params.light_color[0];
  push.light_color_intensity[1] = params.light_color[1];
  push.light_color_intensity[2] = params.light_color[2];
  push.light_color_intensity[3] = params.light_intensity;
  push.depth_limits[0] = settings.max_diffuse_bounces;
  push.depth_limits[1] = settings.max_glossy_bounces;
  push.depth_limits[2] = settings.max_transmission_bounces;
  push.depth_limits[3] = settings.max_transparent_bounces;
  push.integrator[0] = settings.direct_clamp;
  push.integrator[1] = settings.indirect_clamp;
  push.integrator[2] = settings.emissive_multiplier;
  push.integrator[3] =
      static_cast<float>(settings.light_samples_per_path);
  push.camera_jitter[0] = params.camera_jitter[0];
  push.camera_jitter[1] = params.camera_jitter[1];
  push.camera_jitter[2] =
      params.temporal_reconstruction_input ? 1.0f : 0.0f;
  push.camera_jitter[3] =
      std::bit_cast<float>(params.output_write_mask);

  constexpr VkShaderStageFlags kAllRtStages =
      VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
      VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;
  vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                    pipeline_);
  vkCmdBindDescriptorSets(
      command_buffer, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
      pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
  vkCmdPushConstants(command_buffer, pipeline_layout_, kAllRtStages, 0,
                     sizeof(push), &push);
  vkCmdTraceRaysKHR_(command_buffer, &raygen_region_, &miss_region_,
                     &hit_region_, &callable_region_, push.size_mode[0],
                     push.size_mode[1], 1u);
  if (!dispatch_logged_) {
    xpbd::log::infof(
        "VKDIAG rt_pipeline dispatch mode=%s width=%u height=%u "
        "sample_base=%u samples=%u bounces=%u seed=%u material_flags=%u "
        "primitives=%llu emitters=%u instances=%u sbt_stride=%u "
        "sbt_bytes=%llu output_mask=0x%04x",
        rtDebugViewName(params.debug_view), push.size_mode[0],
        push.size_mode[1], push.sampling[0], push.sampling[1],
        push.size_mode[3], push.sampling[2], params.material_feature_flags,
        static_cast<unsigned long long>(primitive_count),
        scene.emissiveTriangleCount(), scene_stats.instance_count,
        shader_group_stride_,
        static_cast<unsigned long long>(sbt_.size),
        static_cast<unsigned>(params.output_write_mask));
    dispatch_logged_ = true;
  }
  return true;
}

RtPipelineStats VulkanRtPipeline::stats() const noexcept {
  RtPipelineStats result;
  result.ready = ready();
  result.shader_group_count = ready() ? kShaderGroupCount : 0u;
  result.shader_group_handle_size = properties_.shaderGroupHandleSize;
  result.shader_group_handle_alignment =
      properties_.shaderGroupHandleAlignment;
  result.shader_group_base_alignment =
      properties_.shaderGroupBaseAlignment;
  result.shader_group_stride = shader_group_stride_;
  result.sbt_bytes = static_cast<std::uint64_t>(sbt_.size);
  result.descriptor_write_calls = descriptor_write_calls_;
  result.descriptor_cache_hits = descriptor_cache_hits_;
  result.descriptor_entries_written = descriptor_entries_written_;
  result.descriptor_update_ms = descriptor_update_ms_;
  return result;
}

} // namespace xpbd::gfx
