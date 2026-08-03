#include "vulkan/vulkan_backend_internal.hpp"
#include "xpbd/log.hpp"

#include <cstddef>
#include <cstdint>

namespace xpbd::gfx::detail {

struct NkVertex {
  float pos[2];
  float uv[2];
  uint8_t col[4];
};

static const uint32_t kSpvUiVert[] = {
#include "spirv/ui.vert.spv.inc"
};

static const uint32_t kSpvUiFrag[] = {
#include "spirv/ui.frag.spv.inc"
};

static const uint32_t kSpvMeshVert[] = {
#include "spirv/mesh.vert.spv.inc"
};

static const uint32_t kSpvMeshFrag[] = {
#include "spirv/mesh.frag.spv.inc"
};

static const uint32_t kSpvStaticMeshVert[] = {
#include "spirv/static_mesh.vert.spv.inc"
};

static const uint32_t kSpvStaticMeshFrag[] = {
#include "spirv/static_mesh.frag.spv.inc"
};

static const uint32_t kSpvSkyboxVert[] = {
#include "spirv/skybox.vert.spv.inc"
};

static const uint32_t kSpvSkyboxFrag[] = {
#include "spirv/skybox.frag.spv.inc"
};

static const uint32_t kSpvMeshRtVert[] = {
#include "spirv/mesh_rt.vert.spv.inc"
};

static const uint32_t kSpvMeshRtFrag[] = {
#include "spirv/mesh_rt.frag.spv.inc"
};

static const uint32_t kSpvStaticMeshRtVert[] = {
#include "spirv/static_mesh_rt.vert.spv.inc"
};

static const uint32_t kSpvStaticMeshRtFrag[] = {
#include "spirv/static_mesh_rt.frag.spv.inc"
};

bool VulkanBackend::createPipelines() {
  VkShaderModule ui_vs = makeModule(kSpvUiVert, sizeof(kSpvUiVert) / 4);
  VkShaderModule ui_fs = makeModule(kSpvUiFrag, sizeof(kSpvUiFrag) / 4);
  VkShaderModule mesh_vs = makeModule(kSpvMeshVert, sizeof(kSpvMeshVert) / 4);
  VkShaderModule mesh_fs = makeModule(kSpvMeshFrag, sizeof(kSpvMeshFrag) / 4);
  VkShaderModule static_mesh_vs =
      makeModule(kSpvStaticMeshVert, sizeof(kSpvStaticMeshVert) / 4);
  VkShaderModule static_mesh_fs =
      makeModule(kSpvStaticMeshFrag, sizeof(kSpvStaticMeshFrag) / 4);
  VkShaderModule sky_vs =
      makeModule(kSpvSkyboxVert, sizeof(kSpvSkyboxVert) / 4);
  VkShaderModule sky_fs =
      makeModule(kSpvSkyboxFrag, sizeof(kSpvSkyboxFrag) / 4);
  auto destroy_modules = [&] {
    if (ui_vs) {
      vkDestroyShaderModule(device_, ui_vs, nullptr);
    }
    if (ui_fs) {
      vkDestroyShaderModule(device_, ui_fs, nullptr);
    }
    if (mesh_vs) {
      vkDestroyShaderModule(device_, mesh_vs, nullptr);
    }
    if (mesh_fs) {
      vkDestroyShaderModule(device_, mesh_fs, nullptr);
    }
    if (static_mesh_vs) {
      vkDestroyShaderModule(device_, static_mesh_vs, nullptr);
    }
    if (static_mesh_fs) {
      vkDestroyShaderModule(device_, static_mesh_fs, nullptr);
    }
    if (sky_vs) {
      vkDestroyShaderModule(device_, sky_vs, nullptr);
    }
    if (sky_fs) {
      vkDestroyShaderModule(device_, sky_fs, nullptr);
    }
  };
  auto fail = [&](const char *message) {
    if (message) {
      writeLog(message);
    }
    destroy_modules();
    destroyGraphicsPipelines();
    return false;
  };
  if (!ui_vs || !ui_fs || !mesh_vs || !mesh_fs || !static_mesh_vs ||
      !static_mesh_fs || !sky_vs || !sky_fs) {
    return fail("SPIR-V module create failed");
  }


  VkPushConstantRange ui_pc{};
  ui_pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  ui_pc.offset = 0;
  ui_pc.size = sizeof(float) * 4;

  VkPipelineLayoutCreateInfo ul{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ul.setLayoutCount = 1;
  ul.pSetLayouts = &desc_layout_;
  ul.pushConstantRangeCount = 1;
  ul.pPushConstantRanges = &ui_pc;
  if (vkCreatePipelineLayout(device_, &ul, nullptr, &ui_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan UI pipeline-layout creation failed");
  }

  VkPushConstantRange mesh_pc{};
  mesh_pc.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  mesh_pc.offset = 0;
  mesh_pc.size = sizeof(MeshScenePushConstants);
  VkPipelineLayoutCreateInfo ml{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ml.pushConstantRangeCount = 1;
  ml.pPushConstantRanges = &mesh_pc;
  if (vkCreatePipelineLayout(device_, &ml, nullptr, &mesh_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan mesh pipeline-layout creation failed");
  }

  ml.setLayoutCount = 1;
  ml.pSetLayouts = &static_desc_layout_;
  if (vkCreatePipelineLayout(device_, &ml, nullptr, &static_mesh_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan static-mesh pipeline-layout creation failed");
  }

  VkPushConstantRange sky_pc{};
  sky_pc.stageFlags =
      VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  sky_pc.offset = 0;
  sky_pc.size = sizeof(SkyboxPushConstants);
  VkPipelineLayoutCreateInfo sl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  sl.setLayoutCount = 1;
  sl.pSetLayouts = &skybox_desc_layout_;
  sl.pushConstantRangeCount = 1;
  sl.pPushConstantRanges = &sky_pc;
  if (vkCreatePipelineLayout(device_, &sl, nullptr, &skybox_layout_) !=
      VK_SUCCESS) {
    return fail("Vulkan skybox pipeline-layout creation failed");
  }

  auto makePipe = [&](VkShaderModule vs, VkShaderModule fs,
                      VkPipelineLayout layout, bool ui, bool static_mesh,
                      bool lines, bool mesh_trans, bool overlay_lines,
                      bool temporal_hud_lines,
                      VkPipeline *out) -> bool {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> attrs;
    if (ui) {
      bind.stride = sizeof(NkVertex);
      attrs = {
          {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NkVertex, pos)},
          {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(NkVertex, uv)},
          {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(NkVertex, col)},
      };
    } else if (static_mesh) {
      bind.stride = sizeof(StaticGpuVertex);
      attrs = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticGpuVertex, px)},
          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(StaticGpuVertex, nx)},
          {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(StaticGpuVertex, u)},
          {3, 0, VK_FORMAT_R32_UINT, offsetof(StaticGpuVertex, bone_index)},
          {4, 0, VK_FORMAT_R32_UINT, offsetof(StaticGpuVertex, flags)},
          {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
           offsetof(StaticGpuVertex, tx)},
      };
    } else {
      bind.stride = sizeof(MeshVertex);
      attrs = {
          {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, px)},
          {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MeshVertex, nx)},
          {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshVertex, r)},
      };
    }
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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
    if (overlay_lines) {
      // Selection outlines sit just outside their source cubes. Pull them a
      // minimal amount toward the camera to tolerate depth quantization while
      // retaining occlusion by genuinely foreground geometry.
      rs.depthBiasEnable = VK_TRUE;
      rs.depthBiasConstantFactor = -1.0f;
      rs.depthBiasSlopeFactor = -1.0f;
    }

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = (ui || temporal_hud_lines) ? VK_FALSE : VK_TRUE;
    ds.depthWriteEnable =
        (ui || mesh_trans || overlay_lines || temporal_hud_lines) ? VK_FALSE
                                                                  : VK_TRUE;
    ds.depthCompareOp = overlay_lines ? VK_COMPARE_OP_LESS_OR_EQUAL
                                      : VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    if (ui || mesh_trans || temporal_hud_lines) {
      blend.blendEnable = VK_TRUE;
      blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend.colorBlendOp = VK_BLEND_OP_ADD;
      blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      blend.alphaBlendOp = VK_BLEND_OP_ADD;
    } else {
      blend.blendEnable = VK_FALSE;
    }
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

    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dsi;
    pi.layout = layout;
    pi.renderPass = render_pass_;
    pi.subpass = 0;
    const VkResult result = vkCreateGraphicsPipelines(
        device_, VK_NULL_HANDLE, 1, &pi, nullptr, out);
    if (result != VK_SUCCESS) {
      SDL_Log("Vulkan graphics-pipeline creation failed: %d",
              static_cast<int>(result));
      return false;
    }
    return true;
  };

  if (!makePipe(ui_vs, ui_fs, ui_layout_, true, false, false, false, false,
                 false, &ui_pipeline_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, false, false,
                 false, false,
                 &mesh_pipeline_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, false, true,
                 false, false,
                 &mesh_pipeline_trans_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                 false, false,
                 &mesh_pipeline_lines_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                 true, false, &mesh_pipeline_overlay_lines_) ||
      !makePipe(mesh_vs, mesh_fs, mesh_layout_, false, false, true, false,
                 false, true, &mesh_pipeline_temporal_hud_lines_) ||
      !makePipe(static_mesh_vs, static_mesh_fs, static_mesh_layout_, false,
                 true, false, false, false, false,
                 &static_mesh_pipeline_) ||
      !makePipe(static_mesh_vs, static_mesh_fs, static_mesh_layout_, false,
                 true, false, true, false, false,
                 &static_mesh_pipeline_blend_)) {
    return fail("Vulkan graphics-pipeline bundle creation failed");
  }

  // Hybrid RT pipelines (ray-query shadows) when NVIDIA RT is armed.
  rt_pipelines_ready_ = false;
  if (rt_capability_.device_extensions_enabled && mesh_rt_desc_layout_ &&
      static_rt_desc_layout_) {
    VkShaderModule mesh_rt_vs =
        makeModule(kSpvMeshRtVert, sizeof(kSpvMeshRtVert) / 4);
    VkShaderModule mesh_rt_fs =
        makeModule(kSpvMeshRtFrag, sizeof(kSpvMeshRtFrag) / 4);
    VkShaderModule static_rt_vs =
        makeModule(kSpvStaticMeshRtVert, sizeof(kSpvStaticMeshRtVert) / 4);
    VkShaderModule static_rt_fs =
        makeModule(kSpvStaticMeshRtFrag, sizeof(kSpvStaticMeshRtFrag) / 4);
    if (!mesh_rt_vs || !mesh_rt_fs || !static_rt_vs || !static_rt_fs) {
      if (mesh_rt_vs)
        vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
      if (mesh_rt_fs)
        vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
      if (static_rt_vs)
        vkDestroyShaderModule(device_, static_rt_vs, nullptr);
      if (static_rt_fs)
        vkDestroyShaderModule(device_, static_rt_fs, nullptr);
      return fail("Vulkan RT shader module creation failed");
    }

    VkPushConstantRange mesh_pc_rt{};
    mesh_pc_rt.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    mesh_pc_rt.offset = 0;
    mesh_pc_rt.size = sizeof(MeshScenePushConstants);

    VkPipelineLayoutCreateInfo ml_rt{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ml_rt.setLayoutCount = 1;
    ml_rt.pSetLayouts = &mesh_rt_desc_layout_;
    ml_rt.pushConstantRangeCount = 1;
    ml_rt.pPushConstantRanges = &mesh_pc_rt;
    if (vkCreatePipelineLayout(device_, &ml_rt, nullptr, &mesh_rt_layout_) !=
        VK_SUCCESS) {
      vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
      vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
      vkDestroyShaderModule(device_, static_rt_vs, nullptr);
      vkDestroyShaderModule(device_, static_rt_fs, nullptr);
      return fail("Vulkan mesh RT pipeline-layout creation failed");
    }
    ml_rt.pSetLayouts = &static_rt_desc_layout_;
    if (vkCreatePipelineLayout(device_, &ml_rt, nullptr, &static_rt_layout_) !=
        VK_SUCCESS) {
      vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
      vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
      vkDestroyShaderModule(device_, static_rt_vs, nullptr);
      vkDestroyShaderModule(device_, static_rt_fs, nullptr);
      return fail("Vulkan static-mesh RT pipeline-layout creation failed");
    }

    const bool rt_ok =
        makePipe(mesh_rt_vs, mesh_rt_fs, mesh_rt_layout_, false, false, false,
                 false, false, false, &mesh_rt_pipeline_) &&
        makePipe(mesh_rt_vs, mesh_rt_fs, mesh_rt_layout_, false, false, false,
                 true, false, false, &mesh_rt_pipeline_trans_) &&
        makePipe(static_rt_vs, static_rt_fs, static_rt_layout_, false, true,
                 false, false, false, false, &static_rt_pipeline_) &&
        makePipe(static_rt_vs, static_rt_fs, static_rt_layout_, false, true,
                 false, true, false, false, &static_rt_pipeline_blend_);
    vkDestroyShaderModule(device_, mesh_rt_vs, nullptr);
    vkDestroyShaderModule(device_, mesh_rt_fs, nullptr);
    vkDestroyShaderModule(device_, static_rt_vs, nullptr);
    vkDestroyShaderModule(device_, static_rt_fs, nullptr);
    if (!rt_ok) {
      return fail("Vulkan RT graphics-pipeline bundle creation failed");
    }
    rt_pipelines_ready_ = true;
    writeLog("Vulkan hybrid RT shadow pipelines ready");
  }

  // Skybox pipeline: position-only verts, depth == far, no depth write.
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = sky_vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = sky_fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(float) * 3;
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr{};
    attr.location = 0;
    attr.binding = 0;
    attr.format = VK_FORMAT_R32G32B32_SFLOAT;
    attr.offset = 0;
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;

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
    // Both windings: skybox is a unit cube sampled from the inside.
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    blend.blendEnable = VK_FALSE;
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

    VkGraphicsPipelineCreateInfo pi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dsi;
    pi.layout = skybox_layout_;
    pi.renderPass = render_pass_;
    pi.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pi, nullptr,
                                  &skybox_pipeline_) != VK_SUCCESS) {
      return fail("Vulkan skybox pipeline creation failed");
    }
  }

  destroy_modules();
  return true;
}

} // namespace xpbd::gfx::detail
