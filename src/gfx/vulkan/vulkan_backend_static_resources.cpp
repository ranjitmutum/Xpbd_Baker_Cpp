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
#include <optional>
#include <utility>
#include <vector>

namespace xpbd::gfx::detail {
namespace {

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

} // namespace

void VulkanBackend::destroyStaticModelResources() {
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
    static_model_ready_ = false;
  }bool VulkanBackend::createStaticTexture(std::uint32_t width, std::uint32_t height,
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
  }void VulkanBackend::updateStaticTextureDescriptors() {
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
    std::array<VkWriteDescriptorSet, 6> writes{};
    for (std::size_t i = 0; i < frames_.size(); ++i) {
      for (std::size_t image = 0; image < image_infos.size(); ++image) {
        auto &write = writes[i * image_infos.size() + image];
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frames_[i].static_descriptor_set;
        write.dstBinding = static_cast<std::uint32_t>(1u + image);
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos[image];
      }
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    if (static_rt_desc_layout_ && static_rt_descriptor_sets_[0]) {
      std::array<VkWriteDescriptorSet, 6> rt_writes{};
      constexpr std::array<std::uint32_t, 3> kRtBindings{1u, 3u, 4u};
      for (std::size_t i = 0; i < frames_.size(); ++i) {
        for (std::size_t image = 0; image < image_infos.size(); ++image) {
          auto &write = rt_writes[i * image_infos.size() + image];
          write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write.dstSet = static_rt_descriptor_sets_[i];
          write.dstBinding = kRtBindings[image];
          write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          write.descriptorCount = 1;
          write.pImageInfo = &image_infos[image];
        }
      }
      vkUpdateDescriptorSets(device_,
                             static_cast<std::uint32_t>(rt_writes.size()),
                             rt_writes.data(), 0, nullptr);
    }
  }void VulkanBackend::updateStaticBoneDescriptor(FrameSync &frame) {
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
  }bool VulkanBackend::rebuildStaticModelResources(const StaticIndexedModelMesh &mesh,
                                   const TextureImage *texture,
                                   const ResolvedMaterialTable *material,
                                   std::uint64_t model_generation,
                                   std::uint64_t texture_generation,
                                   std::uint64_t &uploaded_bytes) {
    uploaded_bytes = 0;
    StaticModelDrawPlan new_plan = makeStaticModelDrawPlan(mesh, texture);

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
    const auto wait_start = Clock::now();
    logDiagnosticApi("vkWaitForFences.static_upload", "before", std::nullopt,
                     0.0, UINT32_MAX, upload_fence, VK_NULL_HANDLE, command,
                     true, true);
    const VkResult wait_result =
        vkWaitForFences(device_, 1, &upload_fence, VK_TRUE, UINT64_MAX);
    logDiagnosticApi(
        "vkWaitForFences.static_upload", "after", wait_result,
        std::chrono::duration<double, std::milli>(Clock::now() - wait_start)
            .count(),
        UINT32_MAX, upload_fence, VK_NULL_HANDLE, command, true, false);
    if (wait_result != VK_SUCCESS) {
      cleanup();
      return false;
    }
    vkDestroyFence(device_, upload_fence, nullptr);
    upload_fence = VK_NULL_HANDLE;
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
    command = VK_NULL_HANDLE;
    destroyBuffer(staging);

    destroyBuffer(static_model_vbo_);
    destroyBuffer(static_model_ibo_);
    destroyImage(static_texture_);
    destroyImage(static_normal_texture_);
    destroyImage(static_specular_texture_);
    destroyStaticMaterialSamplers();
    static_model_vbo_ = new_vertex_buffer;
    static_model_ibo_ = new_index_buffer;
    static_texture_ = new_texture;
    static_normal_texture_ = new_normal_texture;
    static_specular_texture_ = new_specular_texture;
    static_albedo_sampler_ = new_albedo_sampler;
    static_normal_sampler_ = new_normal_sampler;
    static_specular_sampler_ = new_specular_sampler;
    new_vertex_buffer = {};
    new_index_buffer = {};
    new_texture = {};
    new_normal_texture = {};
    new_specular_texture = {};
    new_albedo_sampler = VK_NULL_HANDLE;
    new_normal_sampler = VK_NULL_HANDLE;
    new_specular_sampler = VK_NULL_HANDLE;
    static_draw_plan_ = std::move(new_plan);
    static_bone_count_ = mesh.bone_names.size();
    static_vertex_bytes_ = vertex_bytes;
    static_index_bytes_ = index_bytes;
    static_model_ready_ = true;
    static_mismatch_logged_ = false;
    static_generations_.accept(model_generation, texture_generation);
    ++static_resource_rebuilds_;
    uploaded_bytes = static_cast<std::uint64_t>(staging_bytes);
    updateStaticTextureDescriptors();

    // Feed rest-pose triangles and exact per-primitive material metadata into
    // the unified RT scene (positions, normals, UVs, bones, alpha mode).
    if (rt_capability_.device_extensions_enabled) {
      RtRestGeometry rest;
      rest.positions.resize(mesh.vertices.size() * 3);
      rest.normals.resize(mesh.vertices.size() * 3);
      rest.uvs.resize(mesh.vertices.size() * 2);
      rest.tangents.resize(mesh.vertices.size() * 4);
      rest.bone_indices.resize(mesh.vertices.size());
      for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        rest.positions[i * 3 + 0] = mesh.vertices[i].px;
        rest.positions[i * 3 + 1] = mesh.vertices[i].py;
        rest.positions[i * 3 + 2] = mesh.vertices[i].pz;
        rest.normals[i * 3 + 0] = mesh.vertices[i].nx;
        rest.normals[i * 3 + 1] = mesh.vertices[i].ny;
        rest.normals[i * 3 + 2] = mesh.vertices[i].nz;
        rest.uvs[i * 2 + 0] = mesh.vertices[i].u;
        rest.uvs[i * 2 + 1] = mesh.vertices[i].v;
        rest.tangents[i * 4 + 0] = mesh.vertices[i].tx;
        rest.tangents[i * 4 + 1] = mesh.vertices[i].ty;
        rest.tangents[i * 4 + 2] = mesh.vertices[i].tz;
        rest.tangents[i * 4 + 3] = mesh.vertices[i].tangent_handedness;
        rest.bone_indices[i] = mesh.vertices[i].bone_index;
      }
      const std::optional<RtSurfaceOptics> optics_override =
          developerRtSurfaceOpticsOverride();
      const RtSceneRecords scene_records = buildRigidModelRtSceneRecords(
          mesh, static_draw_plan_, material,
          optics_override ? &*optics_override : nullptr);
      if (!scene_records.valid()) {
        writeLog("Vulkan RT scene-record construction failed");
        return false;
      }
      const RtPackedPrimitiveLayout packed_layout =
          buildRtPackedPrimitiveLayout(static_draw_plan_, scene_records);
      if (!packed_layout.valid()) {
        writeLog("Vulkan RT primitive packing failed");
        return false;
      }
      const auto primitive_flags =
          [&](std::size_t primitive) {
        std::uint32_t flags = 0u;
        if (primitive < static_draw_plan_.primitive_materials.size()) {
          const StaticModelPrimitiveMaterial &primitive_material =
              static_draw_plan_.primitive_materials[primitive];
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
      rest.indices = packed_layout.indices;
      rest.geometry_ranges = packed_layout.geometry_ranges;
      rest.primitive_flags.reserve(
          packed_layout.source_primitive_indices.size());
      rest.primitive_metadata.reserve(
          packed_layout.source_primitive_indices.size());
      rest.primitive_optics.reserve(
          packed_layout.source_primitive_indices.size());
      rest.primitive_emission.reserve(
          packed_layout.source_primitive_indices.size());
      for (std::size_t packed_primitive = 0;
           packed_primitive < packed_layout.source_primitive_indices.size();
           ++packed_primitive) {
        const std::uint32_t primitive =
            packed_layout.source_primitive_indices[packed_primitive];
        rest.primitive_flags.push_back(primitive_flags(primitive));
        if (primitive >= scene_records.primitives.size()) {
          writeLog("Vulkan RT primitive metadata packing failed");
          return false;
        }
        const RtPrimitiveRecord &record =
            scene_records.primitives[primitive];
        rest.primitive_metadata.push_back(
            {record.cube_index,
             static_cast<std::uint32_t>(record.face_direction),
             record.material_index, record.primitive_index});
        rest.primitive_optics.push_back(
            record.material_index < scene_records.materials.size()
                ? scene_records.materials[record.material_index]
                      .surface_optics
                : RtSurfaceOptics{});
        std::array<float, 3> average_emission{};
        if (material != nullptr && material->valid() &&
            material->specular_map_active &&
            packed_primitive * 3u + 2u < rest.indices.size()) {
          const std::uint32_t i0 = rest.indices[packed_primitive * 3u + 0u];
          const std::uint32_t i1 = rest.indices[packed_primitive * 3u + 1u];
          const std::uint32_t i2 = rest.indices[packed_primitive * 3u + 2u];
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
              for (std::size_t channel = 0; channel < 3u; ++channel) {
                average_emission[channel] +=
                    texel.emission_linear[channel] *
                    std::clamp(texel.opacity, 0.0f, 1.0f) * 0.25f;
              }
            }
          }
        }
        rest.primitive_emission.push_back(average_emission);
      }
      for (std::size_t i = 0; i < rt_scenes_.size(); ++i) {
        if (i + 1u == rt_scenes_.size()) {
          rt_scenes_[i].setRestGeometry(std::move(rest));
        } else {
          rt_scenes_[i].setRestGeometry(rest);
        }
      }
      rt_scene_built_.fill(false);
      last_rt_scene_hash_.fill(0);
    }
    return true;
  }

} // namespace xpbd::gfx::detail
