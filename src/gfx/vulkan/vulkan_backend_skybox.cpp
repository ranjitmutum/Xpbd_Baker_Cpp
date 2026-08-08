#include "vulkan/vulkan_backend_internal.hpp"
#include "xpbd/log.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

namespace xpbd::gfx::detail {

// Unit cube (triangle list, 36 verts) for skybox sampling.
static const float kSkyboxCubePositions[] = {
    // -Z
    -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, -1, -1, 1, 1, -1, -1, 1, -1,
    // +Z
    -1, -1, 1, -1, 1, 1, 1, 1, 1, -1, -1, 1, 1, 1, 1, 1, -1, 1,
    // -X
    -1, -1, -1, -1, 1, -1, -1, 1, 1, -1, -1, -1, -1, 1, 1, -1, -1, 1,
    // +X
    1, -1, -1, 1, -1, 1, 1, 1, 1, 1, -1, -1, 1, 1, 1, 1, 1, -1,
    // -Y
    -1, -1, -1, -1, -1, 1, 1, -1, 1, -1, -1, -1, 1, -1, 1, 1, -1, -1,
    // +Y
    -1, 1, -1, 1, 1, -1, 1, 1, 1, -1, 1, -1, 1, 1, 1, -1, 1, 1,
};

void VulkanBackend::destroySkyboxGpu() {
    destroyImage(skybox_cubemap_);
    destroyBuffer(skybox_vbo_);
    if (skybox_pipeline_) {
      vkDestroyPipeline(device_, skybox_pipeline_, nullptr);
      skybox_pipeline_ = VK_NULL_HANDLE;
    }
    if (skybox_layout_) {
      vkDestroyPipelineLayout(device_, skybox_layout_, nullptr);
      skybox_layout_ = VK_NULL_HANDLE;
    }
    if (skybox_desc_pool_) {
      vkDestroyDescriptorPool(device_, skybox_desc_pool_, nullptr);
      skybox_desc_pool_ = VK_NULL_HANDLE;
      skybox_desc_set_ = VK_NULL_HANDLE;
    }
    if (skybox_desc_layout_) {
      vkDestroyDescriptorSetLayout(device_, skybox_desc_layout_, nullptr);
      skybox_desc_layout_ = VK_NULL_HANDLE;
    }
    if (skybox_sampler_) {
      vkDestroySampler(device_, skybox_sampler_, nullptr);
      skybox_sampler_ = VK_NULL_HANDLE;
    }
    skybox_face_size_ = 0;
    skybox_generation_ = 0;
    skybox_source_identity_.clear();
    skybox_ready_ = false;
  }

bool VulkanBackend::uploadSkyboxCubemap(const PreviewSkybox &sky) {
    if (!sky.valid() || !device_ || !cmd_pool_ || !graphics_queue_) {
      return false;
    }
    if (skybox_ready_ && skybox_generation_ == sky.generation &&
        skybox_face_size_ == static_cast<std::uint32_t>(sky.face_size) &&
        skybox_source_identity_ == sky.source_identity &&
        skybox_cubemap_.view) {
      return true;
    }

    std::string candidate_source_identity;
    try {
      candidate_source_identity = sky.source_identity;
    } catch (...) {
      xpbd::log::warn(
          "Preview skybox identity Candidate allocation failed");
      return false;
    }

    const std::uint32_t face = static_cast<std::uint32_t>(sky.face_size);
    const VkDeviceSize face_bytes =
        static_cast<VkDeviceSize>(face) * face * 4u;
    const VkDeviceSize total_bytes = face_bytes * 6u;

    // Build a complete replacement without disturbing the published cubemap.
    ImageResource new_cubemap{};
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {face, face, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 6;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult create_result =
        vkCreateImage(device_, &image_info, nullptr, &new_cubemap.image);
    if (create_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImage", create_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage, total_bytes,
          (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, new_cubemap.image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    const auto memory_type = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      destroyImage(new_cubemap);
      return false;
    }
    allocation.memoryTypeIndex = *memory_type;
    const VkResult allocation_result = vkAllocateMemory(
        device_, &allocation, nullptr, &new_cubemap.memory);
    if (allocation_result != VK_SUCCESS) {
      logImageResourceError(
          "vkAllocateMemory", allocation_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage,
          requirements.size, *memory_type);
      destroyImage(new_cubemap);
      return false;
    }
    const VkResult bind_result = vkBindImageMemory(
        device_, new_cubemap.image, new_cubemap.memory, 0);
    if (bind_result != VK_SUCCESS) {
      logImageResourceError(
          "vkBindImageMemory", bind_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage,
          requirements.size, *memory_type);
      destroyImage(new_cubemap);
      return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = new_cubemap.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    const VkResult view_result = vkCreateImageView(
        device_, &view_info, nullptr, &new_cubemap.view);
    if (view_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImageView", view_result, "preview-skybox-cubemap",
          image_info.format, face, face, 1u, image_info.usage,
          requirements.size, *memory_type);
      destroyImage(new_cubemap);
      return false;
    }
    new_cubemap.width = face;
    new_cubemap.height = face;

    Buffer staging{};
    if (!createBuffer(total_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                       staging, "preview-skybox-staging")) {
      destroyImage(new_cubemap);
      return false;
    }
    std::memcpy(staging.mapped, sky.rgba.data(),
                static_cast<std::size_t>(total_bytes));

    if (!skybox_vbo_.buffer) {
      if (!createBuffer(sizeof(kSkyboxCubePositions),
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        skybox_vbo_) ||
          !uploadBuffer(skybox_vbo_, 0, sizeof(kSkyboxCubePositions),
                        kSkyboxCubePositions)) {
        destroyBuffer(staging);
        destroyImage(new_cubemap);
        return false;
      }
    }

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo ai{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &ai, &cmd) != VK_SUCCESS) {
      destroyBuffer(staging);
      destroyImage(new_cubemap);
      return false;
    }
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const VkResult begin_result = vkBeginCommandBuffer(cmd, &bi);
    if (begin_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Preview skybox command begin failed: API=vkBeginCommandBuffer "
          "VkResult=%s(%d) frame_slot=%u still_job_id=%llu",
          vkResultName(begin_result), static_cast<int>(begin_result),
          frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(new_cubemap);
      return false;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = new_cubemap.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);

    std::array<VkBufferImageCopy, 6> regions{};
    for (std::uint32_t layer = 0; layer < 6; ++layer) {
      regions[layer].bufferOffset = face_bytes * layer;
      regions[layer].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer,
                                         1};
      regions[layer].imageExtent = {face, face, 1};
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, new_cubemap.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6,
                           regions.data());

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);
    const VkResult end_result = vkEndCommandBuffer(cmd);
    if (end_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Preview skybox command end failed: API=vkEndCommandBuffer "
          "VkResult=%s(%d) frame_slot=%u still_job_id=%llu",
          vkResultName(end_result), static_cast<int>(end_result),
          frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(new_cubemap);
      return false;
    }

    if (!submitGraphicsTransactionAndWait(cmd, "preview-skybox")) {
      if (gpu_completion_unproven_) {
        return false;
      }
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
      destroyBuffer(staging);
      destroyImage(new_cubemap);
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
    destroyBuffer(staging);

    destroyImage(skybox_cubemap_);
    skybox_cubemap_ = new_cubemap;
    new_cubemap = {};

    VkDescriptorImageInfo desc_image{};
    desc_image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    desc_image.imageView = skybox_cubemap_.view;
    desc_image.sampler = skybox_sampler_;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = skybox_desc_set_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &desc_image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

    skybox_face_size_ = face;
    skybox_generation_ = sky.generation;
    skybox_source_identity_ = std::move(candidate_source_identity);
    skybox_ready_ = true;
    return true;
  }

} // namespace xpbd::gfx::detail
