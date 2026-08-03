// VulkanBackend 的物理拆分片段。
// 本文件仍在 vulkan_backend.cpp 的同一编译单元中展开。
// 禁止独立编译、迁移资源所有权或在本任务中修改原逻辑。

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
    skybox_ready_ = false;
  }
