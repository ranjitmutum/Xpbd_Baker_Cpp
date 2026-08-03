// Focused regression smoke tests for viewport mesh + texture helpers.
// Linked as xpbd_viewport_regression_tests (see CMakeLists.txt).

#include "xpbd/gfx/ray_tracing.hpp"
#include "xpbd/gfx/frame_generation_state.hpp"
#include "xpbd/gfx/gpu_backend.hpp"
#include "xpbd/gfx/labpbr_authoring.hpp"
#include "xpbd/gfx/labpbr_export.hpp"
#include "xpbd/gfx/labpbr_import.hpp"
#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/labpbr_memory.hpp"
#include "xpbd/gfx/path_trace_aov.hpp"
#include "xpbd/gfx/preview_scene.hpp"
#include "xpbd/gfx/rt_scene_records.hpp"
#include "xpbd/gfx/rt_scene_generations.hpp"
#include "xpbd/gfx/static_model_draw_plan.hpp"
#include "xpbd/gfx/texture_image.hpp"
#include "xpbd/gfx/uv_domain.hpp"
#include "xpbd/gfx/vulkan_queue_selection.hpp"
#include "xpbd/gfx/viewport_mesh.hpp"
#include "xpbd/gfx/world_environment.hpp"
#include "xpbd/baker/cube_geometry.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"
#include "xpbd/loader/model_loader.hpp"
#include "xpbd/render/skeleton_viewport.hpp"
#include "test_support/labpbr_synthetic_fixture.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

constexpr int kDefaultLabPbrStressSide = 2'048;
int g_labpbr_stress_side = kDefaultLabPbrStressSide;

bool configureLabPbrStressSide(int argc, char **argv) noexcept {
  if (argc == 1) {
    return true;
  }
  if (argc == 2 && argv[1] != nullptr) {
    const std::string_view option(argv[1]);
    if (option == "--labpbr-stress-side=2048") {
      g_labpbr_stress_side = 2'048;
      return true;
    }
    if (option == "--labpbr-stress-side=4096") {
      g_labpbr_stress_side = 4'096;
      return true;
    }
  }
  std::fprintf(
      stderr,
      "usage: xpbd_viewport_regression_tests "
      "[--labpbr-stress-side=2048|--labpbr-stress-side=4096]\n");
  return false;
}

int labPbrStressSide() noexcept {
  return g_labpbr_stress_side;
}

std::set<std::uint32_t> expandCoverageRuns(
    const xpbd::gfx::LabPbrUvCoverage &coverage,
    std::string_view group_name);
bool coverageContains(const xpbd::gfx::LabPbrUvCoverage &coverage,
                      std::string_view group_name,
                      std::uint32_t texel);

void expect(bool cond, const char *label) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", label);
    ++g_failures;
  } else {
    std::printf("ok: %s\n", label);
  }
}

void expectNear(float actual, float expected, float tolerance,
                 const char *label) {
  expect(std::abs(actual - expected) <= tolerance, label);
}

void expectNearDouble(double actual, double expected, double tolerance,
                      const char *label) {
  expect(std::abs(actual - expected) <= tolerance, label);
}

std::string readTestSource(const std::filesystem::path &relative_path) {
  const auto path =
      std::filesystem::path(XPBD_TEST_SOURCE_DIR) / relative_path;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::size_t countText(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t position = 0;
       (position = text.find(needle, position)) != std::string_view::npos;
       position += needle.size()) {
    ++count;
  }
  return count;
}

std::string compactTestSource(std::string_view text) {
  std::string compact;
  compact.reserve(text.size());
  for (const char character : text) {
    if (!std::isspace(static_cast<unsigned char>(character))) {
      compact.push_back(character);
    }
  }
  return compact;
}

void testPathTracePbrSourceContracts() {
  const std::string forward =
      readTestSource("src/gfx/spirv/static_mesh.frag");
  const std::string forward_rt =
      readTestSource("src/gfx/spirv/static_mesh_rt.frag");
  const std::string closest_hit =
      readTestSource("src/gfx/spirv/rt_debug.rchit");
  const std::string any_hit =
      readTestSource("src/gfx/spirv/rt_debug.rahit");
  const std::string raygen =
      readTestSource("src/gfx/spirv/rt_debug.rgen");
  const std::string compute =
      readTestSource("src/gfx/spirv/path_trace.comp");
  const std::string composite =
      readTestSource("src/gfx/spirv/pt_composite.frag");
  const std::string backend =
      readTestSource("src/gfx/vulkan_backend.cpp");
  const std::string backend_static = readTestSource(
      "src/gfx/vulkan/vulkan_backend_static_resources.cpp");
  const std::string backend_environment = readTestSource(
      "src/gfx/vulkan/vulkan_backend_environment.cpp");
  const std::string path_tracer =
      readTestSource("src/gfx/vulkan_path_tracer.cpp");
  const std::string rt_pipeline =
      readTestSource("src/gfx/vulkan_rt_pipeline.cpp");
  const std::string ray_tracing_header =
      readTestSource("include/xpbd/gfx/ray_tracing.hpp");
  const std::string ray_tracing_source =
      readTestSource("src/gfx/ray_tracing.cpp");
  const std::string rt_scene_header =
      readTestSource("include/xpbd/gfx/vulkan_rt_scene.hpp");
  const std::string rt_scene_source =
      readTestSource("src/gfx/vulkan_rt_scene.cpp");

  expect(!forward.empty() && !forward_rt.empty() && !closest_hit.empty() &&
             !any_hit.empty() && !raygen.empty() && !compute.empty() &&
             !composite.empty() && !backend.empty() &&
             !backend_static.empty() && !backend_environment.empty() &&
              !path_tracer.empty() &&
              !rt_pipeline.empty() && !ray_tracing_header.empty() &&
              !ray_tracing_source.empty() && !rt_scene_header.empty() &&
              !rt_scene_source.empty(),
          "PBR source-contract fixtures are readable");
  expect(closest_hit.find("sampleAlbedoRayCone") != std::string::npos &&
             closest_hit.find("sampleNormalRayCone") != std::string::npos &&
             closest_hit.find("sampleSpecularRayCone") !=
                 std::string::npos &&
             any_hit.find("sampleAlphaRayCone") != std::string::npos &&
             compute.find("sampleAlphaBaseLevel") != std::string::npos,
         "Full RT material shaders expose ray-cone LOD samplers while the "
         "compatibility compute path remains unchanged");
  expect(countText(closest_hit, "textureLod(") == 3u &&
             countText(any_hit, "textureLod(") == 1u &&
             countText(compute, "textureLod(uAlbedo") == 2u &&
             countText(compute, "textureLod(uNormalTexture") == 1u &&
             countText(compute, "textureLod(uSpecularTexture") == 1u &&
             closest_hit.find("rayConeTextureLod") != std::string::npos &&
             any_hit.find("rayConeTextureLod") != std::string::npos &&
             closest_hit.find(", uv, 0.0)") == std::string::npos &&
             any_hit.find(", uv, 0.0)") == std::string::npos &&
             compute.find(", uv, 0.0)") != std::string::npos,
         "Full RT material reads use explicit ray-cone LOD and compatibility "
         "material reads retain their frozen base level");
  expect(closest_hit.find("PositionBuffer") != std::string::npos &&
             closest_hit.find("objectPosition0") != std::string::npos &&
             closest_hit.find("vec4(objectPosition1 - objectPosition0, 0.0)") !=
                 std::string::npos &&
             closest_hit.find("cross(worldEdge1, worldEdge2)") !=
                 std::string::npos &&
             closest_hit.find("interpolatedNormal") != std::string::npos &&
             closest_hit.find("payload.geometricNormal = geometricNormal;") !=
                 std::string::npos &&
             closest_hit.find("payload.shadingNormal = materialNormal;") !=
                 std::string::npos,
         "Full RT closest hit separates true triangle Ng from interpolated "
         "and normal-mapped Ns");
  expect(raygen.find("vec3 offsetRayOrigin(") != std::string::npos &&
             raygen.find("offsetRayOrigin(hitPosition") !=
                 std::string::npos &&
             raygen.find("geometricNormal * 0.001") == std::string::npos &&
             raygen.find("direction * 0.001") == std::string::npos,
         "Full RT continuation, shadow, reflection, refraction, and "
         "transparent origins share the scale-safe Ng offset seam");
  expect(raygen.find("struct PathRngState") != std::string::npos &&
             raygen.find("PathRngState makePathRng(") != std::string::npos &&
             raygen.find("float nextRandom(inout PathRngState") !=
                 std::string::npos &&
             raygen.find("kRngDomainAlphaCoverage") != std::string::npos &&
             raygen.find("kRngDomainRussianRoulette") !=
                 std::string::npos,
         "Full RT paths use explicit state+dimension RNG domains");
  expect(raygen.find("struct RayCone") != std::string::npos &&
             raygen.find("initialRayCone(") != std::string::npos &&
             raygen.find("propagateRayCone(") != std::string::npos &&
             raygen.find("uv + vec2(inverseSize.x, 0.0)") !=
                 std::string::npos &&
             raygen.find("uv + vec2(0.0, inverseSize.y)") !=
                 std::string::npos &&
             raygen.find("clamp(uv + vec2") == std::string::npos &&
             closest_hit.find("payload.rayCone") != std::string::npos &&
             any_hit.find("primaryPayload.rayCone") != std::string::npos &&
             any_hit.find("shadowPayload.rayCone") != std::string::npos,
         "Full RT initializes and propagates ray-cone footprint state into "
         "material LOD selection");
  expect(backend_static.find("full_mip_levels") != std::string::npos &&
             backend_static.find("vkCmdBlitImage") != std::string::npos &&
             backend_static.find("mip_levels - 1u") != std::string::npos &&
             backend.find(
                 "static_sampler_info.maxLod = VK_LOD_CLAMP_NONE;") !=
                 std::string::npos &&
             backend.find(
                 "static_sampler_info.mipmapMode = "
                 "VK_SAMPLER_MIPMAP_MODE_LINEAR;") !=
                 std::string::npos &&
             backend_environment.find("environment_mip_levels") !=
                 std::string::npos &&
             backend_environment.find(
                 "sampler_info.maxLod = VK_LOD_CLAMP_NONE;") !=
                 std::string::npos &&
             path_tracer.find("si.maxLod = VK_LOD_CLAMP_NONE;") !=
                 std::string::npos &&
             path_tracer.find(
                 "si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;") !=
                 std::string::npos,
         "material images expose complete mip chains to explicit Full RT "
         "ray-cone LOD");
  expect(closest_hit.find("decodeLabPbrMicrofacetAlpha") !=
                 std::string::npos &&
             closest_hit.find("isnan") != std::string::npos &&
             closest_hit.find("isinf") != std::string::npos &&
             compute.find("decodeLabPbrMicrofacetAlpha") !=
                 std::string::npos,
         "RT normal/specular decoding retains finite roughness safeguards");
  expect(backend.find("static_albedo_sampler_") != std::string::npos &&
             backend.find("static_normal_sampler_") != std::string::npos &&
             backend.find("static_specular_sampler_") != std::string::npos &&
             rt_pipeline.find("images[3].sampler = params.normal_sampler") !=
                 std::string::npos &&
             rt_pipeline.find(
                 "images[4].sampler = params.specular_sampler") !=
                 std::string::npos,
         "Vulkan descriptors bind distinct pixel-atlas channel samplers");
  expect(countText(
             backend,
             "static_sampler_info.magFilter = VK_FILTER_NEAREST;") == 1u &&
             countText(
                 backend,
                 "static_sampler_info.minFilter = VK_FILTER_NEAREST;") == 1u &&
             countText(
                 backend,
                 "static_sampler_info.magFilter = VK_FILTER_LINEAR;") == 0u &&
             countText(
                 backend,
                 "static_sampler_info.minFilter = VK_FILTER_LINEAR;") == 0u &&
             countText(path_tracer,
                       "si.magFilter = VK_FILTER_NEAREST;") == 1u &&
             countText(path_tracer,
                       "si.minFilter = VK_FILTER_NEAREST;") == 1u &&
             countText(path_tracer,
                       "si.magFilter = VK_FILTER_LINEAR;") == 0u &&
             countText(path_tracer,
                       "si.minFilter = VK_FILTER_LINEAR;") == 0u,
         "Raster and path tracing keep every pixel-atlas channel "
         "nearest-filtered");
  expect(countText(
             backend,
             "static_sampler_info.addressModeU = "
             "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;") == 1u &&
             countText(
                 backend,
                 "static_sampler_info.addressModeV = "
                 "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;") == 1u &&
             countText(
                 backend,
                 "static_sampler_info.addressModeW = "
                 "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;") == 1u &&
             countText(path_tracer,
                       "si.addressModeU = "
                       "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;") == 1u &&
             countText(path_tracer,
                       "si.addressModeV = "
                       "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;") == 1u &&
             countText(path_tracer,
                       "si.addressModeW = "
                       "VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;") == 1u &&
             countText(path_tracer,
                       "si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;") ==
                 0u &&
             countText(path_tracer,
                       "si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;") ==
                 0u &&
             countText(path_tracer,
                       "si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;") ==
                 0u,
         "Raster, RT, and path tracing clamp model atlas channels at edges");

  const std::string compact_forward = compactTestSource(forward);
  const std::string compact_forward_rt = compactTestSource(forward_rt);
  const std::string compact_closest_hit = compactTestSource(closest_hit);
  const std::string compact_raygen = compactTestSource(raygen);
  const std::string compact_compute = compactTestSource(compute);
  const std::string compact_ray_tracing_header =
      compactTestSource(ray_tracing_header);
  const std::string compact_ray_tracing_source =
      compactTestSource(ray_tracing_source);
  const std::string compact_rt_scene_header =
      compactTestSource(rt_scene_header);
  const std::string compact_rt_scene_source =
      compactTestSource(rt_scene_source);
  expect(compact_closest_hit.find("floatggxAlpha;") != std::string::npos &&
             compact_closest_hit.find(
                 "returnperceptualRoughness*perceptualRoughness;") !=
                 std::string::npos &&
             compact_closest_hit.find(
                 "payload.ggxAlpha=clamp(ggxAlpha,0.0,1.0);") !=
                 std::string::npos &&
             compact_closest_hit.find("linearRoughness") ==
                 std::string::npos &&
             compact_raygen.find(
                 "constfloatkDeltaMirrorAlpha=1.0e-6;") !=
                 std::string::npos &&
             compact_raygen.find(
                 "constfloatkMinFiniteGgxAlpha=1.0e-4;") !=
                 std::string::npos &&
             compact_raygen.find(
                 "floatrrRoughness=sqrt(ggxAlpha);") !=
                 std::string::npos &&
             compact_raygen.find(
                 "if(ggxAlpha<=kDeltaMirrorAlpha)") !=
                 std::string::npos &&
             compact_raygen.find("linearRoughness") == std::string::npos,
         "full RT shaders preserve zero GGX alpha, branch exact mirrors, and "
         "submit RR perceptual roughness without ambiguous naming");
  const std::array<std::string_view, 2> forward_tint_contract{{
      "predefinedMetal=code>=230u&&code<=237u;",
      "reflection_tint*f0*specular*intensity*light",
  }};
  bool forward_tint_matches = true;
  for (const std::string_view contract : forward_tint_contract) {
    forward_tint_matches &=
        compact_forward.find(contract) != std::string::npos &&
        compact_forward_rt.find(contract) != std::string::npos;
  }
  expect(forward_tint_matches &&
             compact_closest_hit.find(
                 "(predefinedMetal?2u:0u)") != std::string::npos &&
             compact_raygen.find(
                 "reflectionTint*fresnel*distribution*geometry") !=
                 std::string::npos &&
             compact_raygen.find(
                 "reflectionTint*rrSpecularAlbedo") != std::string::npos &&
             compact_compute.find(
                 "predefinedMetal?baseColor.rgb:vec3(1.0)") !=
                 std::string::npos &&
             compact_compute.find(
                 "reflectionTint*rrSpecularAlbedo") != std::string::npos,
         "all Raster/PT paths tint predefined-metal reflection and RR guides");

  expect(compact_ray_tracing_header.find("structRtSurfaceOptics{") !=
                 std::string::npos &&
             compact_ray_tracing_header.find(
                 "attenuation_color{1.0f,1.0f,1.0f}") !=
                 std::string::npos &&
             compact_ray_tracing_header.find(
                 "floatattenuation_distance=0.0f;") != std::string::npos &&
             compact_ray_tracing_header.find("boolthin_walled=false;") !=
                 std::string::npos &&
             compact_closest_hit.find("PrimitiveOpticsBuffer") !=
                 std::string::npos &&
              compact_closest_hit.find(
                  "payload.transmission=clamp(surfaceOptics.x,0.0,1.0);") !=
                  std::string::npos &&
              backend_static.find("XPBD_RT_SURFACE_OPTICS") !=
                  std::string::npos &&
              compact_closest_hit.find("floattransmission=0.0;") ==
                  std::string::npos,
         "minimal source-independent optics seam is default-inert and separate from coverage alpha");
  expect(compact_raygen.find("sampleGgxVndf(") != std::string::npos &&
             compact_raygen.find("ggxVisibleNormalPdf(") !=
                 std::string::npos &&
             compact_raygen.find("vec3sampleGgx(") == std::string::npos &&
             compact_ray_tracing_source.find("sampleRtGgxVndf(") !=
                 std::string::npos &&
             compact_ray_tracing_source.find("rtGgxVisibleNormalPdf(") !=
                 std::string::npos,
         "finite GGX reflection uses matched Heitz visible-normal sampling on CPU and GPU");
  expect(compact_raygen.find("evaluateMicrofacetTransmission(") !=
                 std::string::npos &&
             compact_raygen.find("dwmDwi") != std::string::npos &&
             compact_raygen.find("etaScale") != std::string::npos &&
             compact_ray_tracing_source.find("evaluateRtMicrofacetTransmission") !=
                 std::string::npos,
         "rough dielectric transmission carries its BTDF Jacobian and radiance eta-squared factor");
  expect(compact_raygen.find("beerLambertTransmittance(") !=
                 std::string::npos &&
             compact_raygen.find("insideSolidMedium") !=
                 std::string::npos &&
             compact_raygen.find("thinWalled") != std::string::npos &&
             compact_ray_tracing_source.find("rtBeerLambertTransmittance(") !=
                 std::string::npos,
         "solid-only Beer-Lambert absorption and Thin-Walled medium bypass share CPU/GPU contracts");
  expect(compact_rt_scene_header.find("RtSurfaceOpticsGpu") !=
                 std::string::npos &&
             compact_rt_scene_header.find("primitiveOpticsBuffer()") !=
                 std::string::npos &&
             compact_rt_scene_source.find("host_primitive_optics_") !=
                 std::string::npos,
         "per-primitive optics has a material-generation GPU upload seam");

  const std::array<std::string_view, 19> raygen_descriptor_contract{{
      "layout(set=0,binding=0)uniformaccelerationStructureEXTtopLevelAS;",
      "layout(set=0,binding=1,rgba16f)uniformimage2DoutputImage;",
      "layout(set=0,binding=3,std430)readonlybufferIndexBuffer{",
      "layout(set=0,binding=10,r32f)uniformimage2DoutputDepth;",
      "layout(set=0,binding=14)uniformsampler2DenvironmentTexture;",
      "layout(set=0,binding=15,std430)readonlybufferEnvironmentBuffer{",
      "layout(set=0,binding=16,std430)readonlybufferEmissiveTriangleBuffer{",
      "layout(set=0,binding=17,std430)readonlybufferPositionBuffer{",
      "layout(set=0,binding=18,std430)readonlybufferPreviousPositionBuffer{",
      "layout(set=0,binding=19,std430)readonlybufferInstanceMotionBuffer{",
      "layout(set=0,binding=20,std430)readonlybufferMotionFrameBuffer{",
      "layout(set=0,binding=21,rgba16f)uniformimage2DArrayoutputAov;",
      "layout(set=0,binding=22,rgba32f)uniformimage2DoutputStatistics;",
      "layout(set=0,binding=23,rg32f)uniformimage2DoutputRrMotion;",
      "layout(set=0,binding=27,rgba16f)uniformimage2DoutputRrNormalRoughness;",
      "layout(set=0,binding=28,std430)readonlybufferPrimitiveOpticsBuffer{",
      "layout(set=0,binding=29,r8)uniformimage2DoutputTransparencyAndComposition;",
      "layout(set=0,binding=30,r8)uniformimage2DoutputReactiveMask;",
      "layout(set=0,binding=31,r8)uniformimage2DoutputGuideValidity;",
  }};
  bool raygen_bindings_match = true;
  for (const std::string_view descriptor : raygen_descriptor_contract) {
    raygen_bindings_match =
        raygen_bindings_match &&
        compact_raygen.find(descriptor) != std::string::npos;
  }

  const std::string compact_pipeline = compactTestSource(rt_pipeline);
  const std::array<std::string_view, 23> pipeline_descriptor_contract{{
      "std::array<VkDescriptorSetLayoutBinding,32>bindings{};",
      "bindings[index].binding=index;",
      "bindings[index].descriptorCount=1;",
      "bindings[index].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;",
      "bindings[0].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;",
      "bindings[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;",
      "bindings[14].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;",
      "bindings[21].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;",
      "bindings[27].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;",
      "bindings[29].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;",
      "bindings[31].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;",
      "{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1}",
      "{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,12}",
      "{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,15}",
      "{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,4}",
      "std::array<VkWriteDescriptorSet,32>writes{};",
      "writes[binding].dstBinding=binding;",
      "writes[14].pImageInfo=&images[5];",
      "writes[15].pBufferInfo=&buffers[8];",
      "writes[27].pImageInfo=&images[12];",
      "writes[28].pBufferInfo=&buffers[14];",
      "writes[29].pImageInfo=&images[13];",
      "writes[31].pImageInfo=&images[15];",
  }};
  bool pipeline_bindings_match = true;
  for (const std::string_view descriptor : pipeline_descriptor_contract) {
    pipeline_bindings_match =
        pipeline_bindings_match &&
        compact_pipeline.find(descriptor) != std::string::npos;
  }
  expect(raygen_bindings_match && pipeline_bindings_match,
           "raygen set-0 descriptor ABI matches the Vulkan 32-binding layout and writes");

  const auto binding_table_position = compact_pipeline.find(
      "std::array<VkDescriptorSetLayoutBinding,32>bindings{};");
  const auto descriptor_layout_position =
      compact_pipeline.find("vkCreateDescriptorSetLayout(");
  const auto descriptor_allocation_position =
      compact_pipeline.find("vkAllocateDescriptorSets(");
  const auto pipeline_layout_position =
      compact_pipeline.find("vkCreatePipelineLayout(");
  expect(binding_table_position != std::string::npos &&
             descriptor_layout_position != std::string::npos &&
             descriptor_allocation_position != std::string::npos &&
             pipeline_layout_position != std::string::npos &&
             binding_table_position < descriptor_layout_position &&
             descriptor_layout_position < descriptor_allocation_position &&
             descriptor_allocation_position < pipeline_layout_position,
         "RT descriptor table is created, allocated, then installed in pipeline order");

  const auto exposure_position =
      composite.find("color.rgb *= max(composite_push.display.x");
  const auto white_balance_position =
      composite.find("color.rgb *= whiteBalanceScale");
  const auto bloom_position = composite.find("if (bloom > 0.0)");
  const auto tone_map_position = composite.find("int toneMapping");
  const auto transfer_position = composite.find("linearToSrgb(color.rgb)");
  expect(exposure_position != std::string::npos &&
             white_balance_position != std::string::npos &&
             bloom_position != std::string::npos &&
             tone_map_position != std::string::npos &&
             transfer_position != std::string::npos &&
             exposure_position < white_balance_position &&
             white_balance_position < bloom_position &&
             bloom_position < tone_map_position &&
             tone_map_position < transfer_position,
         "preview display transform remains exposure-WB-bloom-tone-map-sRGB");
}

void testSelectionOutlineTemporalContract() {
  const std::string backend_internal = readTestSource(
      "src/gfx/vulkan/vulkan_backend_internal.hpp");
  const std::string backend_pipelines = readTestSource(
      "src/gfx/vulkan/vulkan_backend_pipelines.cpp");
  const std::string main_render_pass = readTestSource(
      "src/gfx/vulkan_render/render/70_main_render_pass.inc");
  const std::string frame_generation = readTestSource(
      "src/gfx/vulkan_render/render/80_frame_generation.inc");
  const std::string render_sequence = readTestSource(
      "src/gfx/vulkan_render/vulkan_backend_render.inc");
  const std::string path_tracer =
      readTestSource("src/gfx/vulkan_path_tracer.cpp");
  const std::string composite =
      readTestSource("src/gfx/spirv/pt_composite.frag");
  expect(!backend_internal.empty() && !backend_pipelines.empty() &&
             !main_render_pass.empty() && !frame_generation.empty() &&
             !render_sequence.empty() && !path_tracer.empty() &&
             !composite.empty(),
         "selection-outline temporal source fixtures are readable");

  const std::string compact_backend_internal =
      compactTestSource(backend_internal);
  const std::string compact_backend_pipelines =
      compactTestSource(backend_pipelines);
  const std::string compact_main_render_pass =
      compactTestSource(main_render_pass);
  const std::string compact_path_tracer = compactTestSource(path_tracer);
  const std::string compact_composite = compactTestSource(composite);
  expect(
      compact_path_tracer.find(
          "composite_push.flags[1]=std::bit_cast<std::uint32_t>("
          "use_reconstructed?params.camera_jitter[0]:0.0f);") !=
              std::string::npos &&
          compact_path_tracer.find(
              "composite_push.flags[2]=std::bit_cast<std::uint32_t>("
              "use_reconstructed?params.camera_jitter[1]:0.0f);") !=
              std::string::npos,
      "reconstructed composite receives the current pixel-space jitter");
  expect(
      compact_composite.find(
          "if((composite_push.flags.x&2u)!=0u){") != std::string::npos &&
          compact_composite.find(
              "vec2jitterPixels=uintBitsToFloat("
              "composite_push.flags.yz);") != std::string::npos &&
          compact_composite.find(
              "depthUv+=jitterPixels/"
              "vec2(textureSize(uPathDepth,0));") != std::string::npos &&
          compact_composite.find("texture(uPathDepth,depthUv)") !=
              std::string::npos,
      "post-DLSS depth lookup reverses primary-ray jitter in render pixels");

  expect(
      compact_backend_internal.find(
          "VkPipelinemesh_pipeline_overlay_lines_=VK_NULL_HANDLE;") !=
              std::string::npos &&
          compact_backend_pipelines.find("rs.depthBiasEnable=VK_TRUE;") !=
              std::string::npos &&
          compact_backend_pipelines.find(
              "ds.depthWriteEnable=(ui||mesh_trans||overlay_lines||"
              "temporal_hud_lines)?VK_FALSE:VK_TRUE;") !=
              std::string::npos &&
          compact_backend_pipelines.find(
              "ds.depthCompareOp=overlay_lines?"
              "VK_COMPARE_OP_LESS_OR_EQUAL:VK_COMPARE_OP_LESS;") !=
              std::string::npos,
      "selection overlays use tolerant non-writing depth without disabling "
      "foreground occlusion");

  expect(
      compact_backend_internal.find(
          "VkPipelinemesh_pipeline_temporal_hud_lines_=VK_NULL_HANDLE;") !=
              std::string::npos &&
          compact_backend_pipelines.find(
              "ds.depthTestEnable=(ui||temporal_hud_lines)?"
              "VK_FALSE:VK_TRUE;") != std::string::npos &&
          compact_backend_pipelines.find(
              "if(ui||mesh_trans||temporal_hud_lines){"
              "blend.blendEnable=VK_TRUE;") != std::string::npos,
      "temporal selection HUD pipeline blends without depth test or writes");

  expect(
      compact_main_render_pass.find(
          "constbooltemporal_selection_active="
          "pt_dlss_active||fg_frame_candidate;") != std::string::npos &&
          compact_main_render_pass.find(
              "draw_selection_lines(temporal_selection_active?"
              "mesh_pipeline_temporal_hud_lines_:"
              "mesh_pipeline_overlay_lines_);") != std::string::npos,
      "temporal selection uses the HUD pipeline while native selection keeps "
      "the original depth-aware pipeline");

  const auto composite_position =
      main_render_pass.find("path_tracer.recordComposite(");
  const auto overlay_position =
      main_render_pass.find("mesh_pipeline_overlay_lines_", composite_position);
  const auto grid_position =
      main_render_pass.find("mesh_pipeline_lines_", overlay_position);
  expect(composite_position != std::string::npos &&
             overlay_position != std::string::npos &&
             grid_position != std::string::npos &&
             composite_position < overlay_position &&
             overlay_position < grid_position,
         "selection overlay remains post-reconstruction and grid keeps its "
         "original depth pipeline");

  const auto fg_candidate_position =
      main_render_pass.find("const bool fg_frame_candidate");
  const auto hudless_copy_position =
      frame_generation.find("vkCmdCopyImage(");
  const auto fg_ui_pass_position = frame_generation.find(
      "fg_ui_rp.renderPass = fg_ui_render_pass_", hudless_copy_position);
  const auto fg_ui_selection_position = frame_generation.find(
      "draw_selection_lines(mesh_pipeline_temporal_hud_lines_);",
      fg_ui_pass_position);
  const auto fg_overlay_pass_position = frame_generation.find(
      "fg_overlay_rp.renderPass = fg_overlay_render_pass_",
      fg_ui_selection_position);
  const auto fg_overlay_selection_position = frame_generation.find(
      "draw_selection_lines(mesh_pipeline_temporal_hud_lines_);",
      fg_ui_selection_position + 1);
  const auto main_render_stage_position =
      render_sequence.find("render/70_main_render_pass.inc");
  const auto frame_generation_stage_position =
      render_sequence.find("render/80_frame_generation.inc");
  expect(main_render_stage_position != std::string::npos &&
             frame_generation_stage_position != std::string::npos &&
             main_render_stage_position < frame_generation_stage_position &&
             fg_candidate_position != std::string::npos &&
             hudless_copy_position != std::string::npos &&
             fg_ui_pass_position != std::string::npos &&
             fg_ui_selection_position != std::string::npos &&
             fg_overlay_pass_position != std::string::npos &&
             fg_overlay_selection_position != std::string::npos &&
             hudless_copy_position < fg_ui_pass_position &&
             fg_ui_pass_position < fg_ui_selection_position &&
             fg_ui_selection_position < fg_overlay_pass_position &&
             fg_overlay_pass_position < fg_overlay_selection_position,
         "FG copies HUDLess before routing selection to UI recomposition and "
         "the real-frame overlay");
}

void testLogicalFramebufferViewportContract() {
  using xpbd::gfx::logicalViewportToFramebuffer;
  const auto mixed_dpi = logicalViewportToFramebuffer(
      10.0f, 20.0f, 300.0f, 200.0f, 1.5f, 2.0f, 1920, 1080);
  expect(mixed_dpi.x == 15 && mixed_dpi.y == 40 &&
             mixed_dpi.w == 450 && mixed_dpi.h == 400,
         "logical preview viewport maps exactly across per-axis DPI");

  const auto clipped = logicalViewportToFramebuffer(
      900.0f, 500.0f, 200.0f, 100.0f, 2.0f, 2.0f, 1920, 1080);
  expect(clipped.x == 1800 && clipped.y == 1000 &&
             clipped.w == 120 && clipped.h == 80,
         "framebuffer viewport clips safely after resize");

  const auto minimum = logicalViewportToFramebuffer(
      4.0f, 5.0f, 0.0f, -2.0f,
      std::numeric_limits<float>::quiet_NaN(), 0.0f, 64, 64);
  expect(minimum.x == 4 && minimum.y == 5 &&
             minimum.w == 1 && minimum.h == 1,
         "invalid DPI and collapsed UI viewport retain a one-pixel target");

  using xpbd::gfx::choosePathTraceTargetExtent;
  expect(choosePathTraceTargetExtent(900u, 600u, 640u, 480u, true) ==
             xpbd::gfx::PathTraceTargetExtent{640u, 480u},
         "interactive preview resize reuses the complete current PT target");
  expect(choosePathTraceTargetExtent(900u, 600u, 0u, 0u, true) ==
             xpbd::gfx::PathTraceTargetExtent{900u, 600u},
         "first-frame resize allocates the requested PT target once");
  expect(choosePathTraceTargetExtent(900u, 600u, 640u, 480u, false) ==
             xpbd::gfx::PathTraceTargetExtent{900u, 600u},
         "released splitter adopts the final requested PT target");
}

void testVulkanQueueFamilySelection() {
  using xpbd::gfx::VulkanQueueFamilySupport;
  using xpbd::gfx::selectVulkanQueueFamilies;

  constexpr std::array laptop_queue_families{
      VulkanQueueFamilySupport{true, true},
      VulkanQueueFamilySupport{false, false},
      VulkanQueueFamilySupport{false, true},
  };
  constexpr auto shared =
      selectVulkanQueueFamilies(laptop_queue_families);
  static_assert(shared.valid() && shared.shared());
  static_assert(shared.graphics_family == 0u &&
                shared.present_family == 0u);
  expect(shared.shared() && shared.graphics_family == 0u,
         "Vulkan queue selection prefers the first shared graphics/present "
         "family");

  constexpr std::array split_queue_families{
      VulkanQueueFamilySupport{true, false},
      VulkanQueueFamilySupport{false, true},
  };
  constexpr auto split =
      selectVulkanQueueFamilies(split_queue_families);
  static_assert(split.valid() && !split.shared());
  expect(split.graphics_family == 0u && split.present_family == 1u,
         "Vulkan queue selection retains a valid split-family fallback");

  constexpr std::array unusable_queue_families{
      VulkanQueueFamilySupport{false, true},
  };
  constexpr auto unusable =
      selectVulkanQueueFamilies(unusable_queue_families);
  static_assert(!unusable.valid());
  expect(!unusable.valid(),
         "Vulkan queue selection rejects devices without graphics support");
}

// Minimal 1x1 opaque white PNG (generated: RGB 8-bit, single white pixel).
constexpr unsigned char kWhitePng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0xF8, 0xFF, 0xFF, 0x3F,
    0x00, 0x05, 0xFE, 0x02, 0xFE, 0x0D, 0xEF, 0x46, 0xB8, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

void testTextureFromMemory() {
  using xpbd::gfx::TextureDecodeLimits;
  using xpbd::gfx::TextureImage;
  using xpbd::gfx::TextureImageHeader;
  using xpbd::gfx::checkedTextureRgbaByteCount;
  using xpbd::gfx::kTextureDecodeMaximumPixels;

  xpbd::gfx::TextureImage img;
  std::string err;
  const bool ok = xpbd::gfx::loadTextureImageFromMemory(
      kWhitePng, static_cast<int>(sizeof(kWhitePng)), img, &err);
  expect(ok, "loadTextureImageFromMemory(1x1 png)");
  expect(img.valid(), "texture valid after load");
  expect(img.width == 1 && img.height == 1, "texture size 1x1");
  expect(img.source_channels == 3, "source RGB channel count retained");
  TextureImageHeader inspected_header;
  expect(xpbd::gfx::inspectTextureImageFromMemory(
             kWhitePng, static_cast<int>(sizeof(kWhitePng)),
             inspected_header, &err) &&
             inspected_header.width == 1 && inspected_header.height == 1 &&
             inspected_header.source_channels == 3,
         "texture header inspection validates without decoding pixels");
  if (img.valid()) {
    float r = 0, g = 0, b = 0, a = 0;
    img.sample(0.5f, 0.5f, r, g, b, a);
    expect(r > 0.9f && g > 0.9f && b > 0.9f, "sample near white");
    expect(a > 0.9f, "sample alpha opaque");
  }
  expect(!err.empty() || ok, "error string only on failure");

  std::size_t rgba_bytes = 99u;
  expect(checkedTextureRgbaByteCount(1u, 1u, rgba_bytes) &&
             rgba_bytes == 4u,
         "texture RGBA byte count uses checked multiplication");
  rgba_bytes = 99u;
  expect(!checkedTextureRgbaByteCount(
             (std::numeric_limits<std::size_t>::max)(), 2u,
             rgba_bytes) &&
             rgba_bytes == 0u,
         "texture RGBA byte count rejects size_t overflow");

  TextureImage preserved;
  preserved.width = 1;
  preserved.height = 1;
  preserved.source_channels = 4;
  preserved.rgba = {1u, 2u, 3u, 4u};
  preserved.path = "preserved.png";
  const auto outputWasPreserved = [&]() {
    return preserved.width == 1 && preserved.height == 1 &&
           preserved.source_channels == 4 &&
           preserved.rgba == std::vector<std::uint8_t>({1u, 2u, 3u, 4u}) &&
           preserved.path == "preserved.png";
  };
  const auto pngWithDimensions = [](std::uint32_t width,
                                    std::uint32_t height) {
    std::vector<unsigned char> png(std::begin(kWhitePng),
                                   std::end(kWhitePng));
    const auto writeBigEndian = [&](std::size_t offset,
                                    std::uint32_t value) {
      png[offset + 0u] = static_cast<unsigned char>(value >> 24u);
      png[offset + 1u] = static_cast<unsigned char>(value >> 16u);
      png[offset + 2u] = static_cast<unsigned char>(value >> 8u);
      png[offset + 3u] = static_cast<unsigned char>(value);
    };
    writeBigEndian(16u, width);
    writeBigEndian(20u, height);
    // stb_image's info preflight reads IHDR dimensions without decoding IDAT;
    // the original payload is intentionally left tiny to model a decode bomb.
    return png;
  };

  const std::array<unsigned char, 12> damaged_png{
      0x89u, 0x50u, 0x4eu, 0x47u, 0x0du, 0x0au,
      0x1au, 0x0au, 0x00u, 0x00u, 0x00u, 0x0du};
  inspected_header = {7, 9, 4};
  expect(!xpbd::gfx::inspectTextureImageFromMemory(
             damaged_png.data(), static_cast<int>(damaged_png.size()),
             inspected_header, &err) &&
             inspected_header.width == 7 && inspected_header.height == 9 &&
             inspected_header.source_channels == 4,
         "failed texture header inspection preserves its output");
  expect(!xpbd::gfx::loadTextureImageFromMemory(
             damaged_png.data(), static_cast<int>(damaged_png.size()),
             preserved, &err) &&
             err.find("stbi_info") != std::string::npos &&
             outputWasPreserved(),
         "damaged texture fails preflight without changing output");

  const auto oversized_side = pngWithDimensions(16'385u, 1u);
  expect(!xpbd::gfx::loadTextureImageFromMemory(
             oversized_side.data(), static_cast<int>(oversized_side.size()),
             preserved, &err) &&
             err.find("dimensions=16385x1") != std::string::npos &&
             err.find("width=16384") != std::string::npos &&
             outputWasPreserved(),
         "texture hard side limit rejects compressed decode bomb");

  const auto oversized_pixels = pngWithDimensions(16'384u, 8'193u);
  expect(static_cast<std::size_t>(16'384u) * 8'193u >
             kTextureDecodeMaximumPixels &&
             !xpbd::gfx::loadTextureImageFromMemory(
                 oversized_pixels.data(),
                 static_cast<int>(oversized_pixels.size()), preserved, &err) &&
             err.find("maximum_pixels=134217728") != std::string::npos &&
             outputWasPreserved(),
         "texture hard pixel limit rejects oversized IHDR before decode");

  TextureDecodeLimits decoded_budget;
  decoded_budget.maximum_decoded_bytes = 3u;
  expect(!xpbd::gfx::loadTextureImageFromMemory(
             kWhitePng, static_cast<int>(sizeof(kWhitePng)), preserved, &err,
             decoded_budget) &&
             err.find("decoded_rgba=4") != std::string::npos &&
             err.find("maximum_decoded_bytes=3") != std::string::npos &&
             outputWasPreserved(),
         "texture decoded-byte budget is enforced transactionally");

  TextureDecodeLimits peak_budget;
  peak_budget.maximum_peak_bytes = preserved.rgba.capacity() + 7u;
  expect(!xpbd::gfx::loadTextureImageFromMemory(
             kWhitePng, static_cast<int>(sizeof(kWhitePng)), preserved, &err,
             peak_budget) &&
             err.find("required_peak=") != std::string::npos &&
             err.find("maximum_peak_bytes=") != std::string::npos &&
             outputWasPreserved(),
         "texture decoder-plus-candidate peak budget is enforced");

  TextureDecodeLimits overflowed_peak;
  overflowed_peak.retained_resident_bytes =
      (std::numeric_limits<std::size_t>::max)();
  expect(!xpbd::gfx::loadTextureImageFromMemory(
             kWhitePng, static_cast<int>(sizeof(kWhitePng)), preserved, &err,
             overflowed_peak) &&
             err.find("peak byte arithmetic overflow") != std::string::npos &&
             outputWasPreserved(),
         "texture peak-budget addition rejects size_t overflow");
}

void testSyntheticLargeUvFixtureAndMemoryBaseline() {
  using xpbd::gfx::LabPbrMemoryEstimate;
  using xpbd::gfx::LabPbrMemoryEstimateRequest;
  using xpbd::gfx::ResolvedMaterialTexel;
  using xpbd::gfx::buildAuthoredResolvedMaterial;
  using xpbd::gfx::estimateLabPbrMemory;
  using xpbd::gfx::kLabPbrDefaultPeakBudgetBytes;
  using xpbd::gfx::kLabPbrResolvedTexelBytesPerPixel;
  using xpbd::gfx::preflightLabPbrMemory;
  using xpbd::test_support::SyntheticLabPbrSuitePaths;
  using xpbd::test_support::SyntheticLargeUvFixture;
  using xpbd::test_support::buildSyntheticLargeUvFixture;
  using xpbd::test_support::fixtureTexel;
  using xpbd::test_support::writeSyntheticLabPbrSuite;

  SyntheticLargeUvFixture fixture;
  std::string error;
  expect(buildSyntheticLargeUvFixture(fixture, &error) && error.empty(),
         "runtime synthetic large-UV fixture builds");
  expect(fixture.base.valid() && fixture.normal.valid() &&
             fixture.specular.valid() &&
             fixture.base.width == SyntheticLargeUvFixture::kAtlasWidth &&
             fixture.base.height == SyntheticLargeUvFixture::kAtlasHeight,
         "synthetic Base Normal and Specular atlases share 256x256 extent");
  expect(fixture.large_uv_geometry.description.texture_width == 16 &&
             fixture.large_uv_geometry.description.texture_height == 16 &&
             fixture.large_uv_geometry.bones.size() == 3u &&
             fixture.large_uv_geometry.bones[1].name == "eye_left" &&
             fixture.large_uv_geometry.bones[2].name == "eye_right",
         "synthetic eye model declares 16x16 and authors distinct large UVs");

  const auto body = fixtureTexel(fixture.base, fixture.body_texel);
  const auto left = fixtureTexel(fixture.base, fixture.left_eye_texel);
  const auto right = fixtureTexel(fixture.base, fixture.right_eye_texel);
  const auto repeat_trap =
      fixtureTexel(fixture.base, fixture.repeat_trap_texel);
  expect(body == std::array<std::uint8_t, 4>{220u, 40u, 20u, 255u} &&
             left ==
                 std::array<std::uint8_t, 4>{20u, 220u, 40u, 255u} &&
             right ==
                 std::array<std::uint8_t, 4>{20u, 40u, 220u, 255u} &&
             repeat_trap[3] == 0u,
         "synthetic body and eyes are distinct while Repeat trap is transparent");
  expect(fixtureTexel(fixture.normal, fixture.left_eye_texel) !=
                 fixtureTexel(fixture.normal, fixture.right_eye_texel) &&
             fixtureTexel(fixture.specular, fixture.left_eye_texel) !=
                 fixtureTexel(fixture.specular, fixture.right_eye_texel),
         "synthetic eye Normal and Specular texels have distinct sentinels");
  expect(fixture.high_resolution_geometry.bones.size() == 1u &&
             fixture.out_of_bounds_geometry.bones.size() == 1u &&
             fixture.uv_cases.size() >= 7u,
         "fixture includes high-resolution protection, out-of-bounds, and special UV cases");

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path suite_directory =
      std::filesystem::temp_directory_path() /
      std::filesystem::path(L"xpbd测试_希尔达_材质") /
      std::to_string(nonce);
  SyntheticLabPbrSuitePaths paths;
  expect(writeSyntheticLabPbrSuite(fixture, suite_directory,
                                   std::filesystem::path(L"默认材质.png"),
                                   paths, &error) &&
             error.empty(),
         "runtime fixture writes Unicode Base Normal Specular and properties");
  std::error_code filesystem_error;
  expect(std::filesystem::is_regular_file(paths.base, filesystem_error) &&
             std::filesystem::file_size(paths.base, filesystem_error) > 0u &&
             std::filesystem::is_regular_file(paths.normal,
                                              filesystem_error) &&
             std::filesystem::is_regular_file(paths.specular,
                                              filesystem_error) &&
             std::filesystem::is_regular_file(paths.properties,
                                              filesystem_error),
         "synthetic Unicode suite consists only of runtime-generated files");
  std::filesystem::remove_all(
      suite_directory.parent_path(), filesystem_error);
  expect(!filesystem_error, "remove runtime synthetic Unicode suite");

  for (const std::uint64_t side : {1'024u, 2'048u, 4'096u}) {
    const std::uint64_t pixels = side * side;
    const std::uint64_t rgba_bytes = pixels * 4u;
    LabPbrMemoryEstimateRequest legacy_request;
    legacy_request.width = side;
    legacy_request.height = side;
    legacy_request.resident_rgba_image_count = 3u;
    legacy_request.candidate_rgba_image_count = 3u;
    legacy_request.resolved_texel_bytes_per_pixel =
        sizeof(ResolvedMaterialTexel);
    legacy_request.encoded_snapshot_bytes = rgba_bytes * 3u;
    legacy_request.decoder_peak_bytes = rgba_bytes;
    legacy_request.coverage_peak_bytes =
        pixels * sizeof(std::uint32_t);
    legacy_request.cache_bytes = rgba_bytes * 3u;
    LabPbrMemoryEstimate legacy_estimate;
    LabPbrMemoryEstimate compact_estimate;
    auto compact_request = legacy_request;
    compact_request.resolved_texel_bytes_per_pixel =
        kLabPbrResolvedTexelBytesPerPixel;
    expect(estimateLabPbrMemory(legacy_request, legacy_estimate, &error) &&
               estimateLabPbrMemory(compact_request, compact_estimate,
                                    &error) &&
               legacy_estimate.resident_bytes ==
                   rgba_bytes * 3u +
                       pixels * sizeof(ResolvedMaterialTexel) &&
               compact_estimate.resident_bytes == rgba_bytes * 3u &&
               legacy_estimate.resident_bytes -
                       compact_estimate.resident_bytes ==
                   pixels * sizeof(ResolvedMaterialTexel) &&
               compact_estimate.coverage_peak_bytes ==
                   pixels * sizeof(std::uint32_t) &&
               compact_estimate.cache_bytes == rgba_bytes * 3u &&
               compact_estimate.peak_bytes ==
                   compact_estimate.resident_bytes + rgba_bytes * 10u +
                       compact_estimate.coverage_peak_bytes,
           "checked compact LabPBR estimate removes all resolved texel bytes");
    std::printf(
        "memory-compact: side=%llu resolved-texel-size=%llu legacy-resident=%llu "
        "resident=%llu peak=%llu coverage-peak=%llu cache=%llu\n",
        static_cast<unsigned long long>(side),
        static_cast<unsigned long long>(sizeof(ResolvedMaterialTexel)),
        static_cast<unsigned long long>(legacy_estimate.resident_bytes),
        static_cast<unsigned long long>(compact_estimate.resident_bytes),
        static_cast<unsigned long long>(compact_estimate.peak_bytes),
        static_cast<unsigned long long>(compact_estimate.coverage_peak_bytes),
        static_cast<unsigned long long>(compact_estimate.cache_bytes));
  }

  const LabPbrMemoryEstimate preserved{11u, 22u, 33u, 44u};
  LabPbrMemoryEstimate overflow_output = preserved;
  LabPbrMemoryEstimateRequest overflow_request;
  overflow_request.width =
      (std::numeric_limits<std::uint64_t>::max)();
  overflow_request.height = 2u;
  expect(!estimateLabPbrMemory(overflow_request, overflow_output, &error) &&
             error.find("overflow") != std::string::npos &&
             overflow_output == preserved,
         "LabPBR estimate rejects overflow without changing output");

  LabPbrMemoryEstimateRequest eight_k_request;
  eight_k_request.width = 8'192u;
  eight_k_request.height = 8'192u;
  eight_k_request.resident_rgba_image_count = 3u;
  eight_k_request.candidate_rgba_image_count = 3u;
  eight_k_request.resolved_texel_bytes_per_pixel =
      kLabPbrResolvedTexelBytesPerPixel;
  eight_k_request.encoded_snapshot_bytes = 3u * 8'192u * 8'192u * 4u;
  eight_k_request.decoder_peak_bytes = 8'192u * 8'192u * 4u;
  eight_k_request.coverage_peak_bytes = 8'192u * 8'192u * 4u;
  eight_k_request.candidate_fixed_bytes = 8'192u * 8'192u * 4u;
  eight_k_request.cache_bytes = 3u * 8'192u * 8'192u * 4u;
  LabPbrMemoryEstimate eight_k_output = preserved;
  expect(!preflightLabPbrMemory(eight_k_request,
                                kLabPbrDefaultPeakBudgetBytes,
                                eight_k_output, &error) &&
             error.find("budget preflight") != std::string::npos &&
             eight_k_output == preserved,
         "8K LabPBR budget rejects by arithmetic without allocating images");

  const int stress_side = labPbrStressSide();
  const std::size_t stress_bytes =
      static_cast<std::size_t>(stress_side) *
      static_cast<std::size_t>(stress_side) * 4u;
  xpbd::gfx::TextureImage stress_base;
  stress_base.width = stress_side;
  stress_base.height = stress_side;
  stress_base.source_channels = 4;
  stress_base.rgba.assign(stress_bytes, 192u);
  xpbd::gfx::TextureImage stress_normal = stress_base;
  stress_normal.rgba.assign(stress_bytes, 128u);
  xpbd::gfx::TextureImage stress_specular = stress_base;
  stress_specular.rgba.assign(stress_bytes, 64u);
  const auto stress_base_asset =
      std::make_shared<const xpbd::gfx::TextureImage>(std::move(stress_base));
  const auto stress_normal_asset =
      std::make_shared<const xpbd::gfx::TextureImage>(std::move(stress_normal));
  const auto stress_specular_asset =
      std::make_shared<const xpbd::gfx::TextureImage>(
          std::move(stress_specular));
  xpbd::gfx::ResolvedMaterialTable stress_material;
  expect(buildAuthoredResolvedMaterial(
             stress_base_asset, {}, stress_normal_asset, stress_specular_asset,
             stress_material, &error) &&
             stress_material.valid() &&
             stress_material.baseImageAsset() == stress_base_asset &&
             stress_material.normalImageAsset() == stress_normal_asset &&
             stress_material.specularImageAsset() == stress_specular_asset &&
             stress_material.base_image.rgba.size() == stress_bytes &&
             stress_material.normal_image.rgba.size() == stress_bytes &&
             stress_material.specular_image.rgba.size() == stress_bytes &&
             kLabPbrResolvedTexelBytesPerPixel == 0u,
         "runtime compact material stress retains three shared RGBA images and zero resolved texel bytes");

  const auto make_source_file = [](const char *name,
                                   std::uint8_t sentinel) {
    xpbd::gfx::LabPbrSourceFile source;
    source.path = std::filesystem::path(name);
    source.present = true;
    source.original_bytes =
        std::make_shared<const std::vector<std::uint8_t>>(
            std::vector<std::uint8_t>{sentinel});
    source.size = source.original_bytes->size();
    source.sha256 = std::string(64u, "0123456789abcdef"[sentinel & 0x0fu]);
    return source;
  };
  xpbd::gfx::ImportedLabPbrSuite stress_suite;
  stress_suite.base_image = stress_base_asset;
  stress_suite.material = stress_material;
  stress_suite.source.base = make_source_file("stress.png", 1u);
  stress_suite.source.specular = make_source_file("stress_s.png", 2u);
  stress_suite.source.normal = make_source_file("stress_n.png", 3u);
  stress_suite.source.confirmed_labpbr13_without_properties = true;
  stress_suite.source.cache_key =
      "runtime-labpbr-stress-" + std::to_string(stress_side);
  xpbd::gfx::LabPbrSuiteImportCache stress_cache;
  xpbd::gfx::ImportedLabPbrSuite cached_stress;
  expect(stress_suite.valid() && stress_cache.store(stress_suite) &&
             stress_cache.residentBytes() <= stress_cache.maximumBytes() &&
             stress_cache.find(stress_suite.source.cache_key, cached_stress) &&
             cached_stress.base_image == stress_base_asset &&
             cached_stress.material.baseImageAsset() == stress_base_asset &&
             cached_stress.material.normalImageAsset() == stress_normal_asset &&
             cached_stress.material.specularImageAsset() ==
                 stress_specular_asset &&
             cached_stress.source.normal.original_bytes ==
                 stress_suite.source.normal.original_bytes,
         "runtime shared material fits the byte-bounded cache without image or Iris snapshot copies");
  std::printf(
      "labpbr-material-cache-stress: side=%d image-bytes=%zu cache-bytes=%llu "
      "cache-budget=%llu\n",
      stress_side, stress_bytes,
      static_cast<unsigned long long>(stress_cache.residentBytes()),
      static_cast<unsigned long long>(stress_cache.maximumBytes()));
}

void testBedrockUvDomainResolution() {
  using xpbd::gfx::BedrockUvFace;
  using xpbd::gfx::ResolvedFaceUv;
  using xpbd::gfx::ResolvedUvDomain;
  using xpbd::gfx::UvBounds;
  using xpbd::gfx::UvDomainKind;
  using xpbd::gfx::bedrockFaceUvCorners;
  using xpbd::gfx::resolveBedrockFaceUv;
  using xpbd::gfx::resolveGeometryUvDomain;
  using xpbd::gfx::resolveUvDomain;
  using xpbd::gfx::scanGeometryUvBounds;
  using xpbd::loader::ModelLoader;
  using xpbd::test_support::SyntheticLargeUvFixture;
  using xpbd::test_support::buildSyntheticLargeUvFixture;

  SyntheticLargeUvFixture fixture;
  std::string error;
  expect(buildSyntheticLargeUvFixture(fixture, &error),
         "UV Domain table uses the shared synthetic fixture");

  UvBounds large_bounds;
  ResolvedUvDomain large_domain;
  expect(scanGeometryUvBounds(fixture.large_uv_geometry, large_bounds,
                              &error) &&
             large_bounds.face_count == 3u && large_bounds.min_u == 0.0 &&
             large_bounds.min_v == 0.0 && large_bounds.max_u == 224.0 &&
             large_bounds.max_v == 16.0 &&
             resolveUvDomain(fixture.large_uv_geometry.description,
                             large_bounds, 256, 256, large_domain, &error) &&
             large_domain.kind == UvDomainKind::Recovered &&
             large_domain.width == 256.0 &&
             large_domain.height == 256.0,
         "large eye UV bounds recover to the imported 256x256 atlas");

  ResolvedUvDomain protected_domain;
  expect(resolveGeometryUvDomain(fixture.high_resolution_geometry, 256, 256,
                                 protected_domain, &error) &&
             protected_domain.kind == UvDomainKind::Declared &&
             protected_domain.width == 16.0 &&
             protected_domain.height == 16.0,
         "high-resolution texture keeps reliable 16x16 declared UV domain");

  xpbd::loader::GeometryDescription no_declaration;
  ResolvedUvDomain imported_domain;
  expect(resolveUvDomain(no_declaration, protected_domain.bounds, 256, 256,
                         imported_domain, &error) &&
             imported_domain.kind == UvDomainKind::ImportedTexture &&
             imported_domain.width == 256.0,
         "missing declaration resolves against imported texture dimensions");
  auto width_only = no_declaration;
  width_only.texture_width = 16;
  width_only.has_texture_width = true;
  auto height_only = no_declaration;
  height_only.texture_height = 16;
  height_only.has_texture_height = true;
  ResolvedUvDomain width_only_domain;
  ResolvedUvDomain height_only_domain;
  expect(resolveUvDomain(width_only, protected_domain.bounds, 256, 256,
                         width_only_domain, &error) &&
             resolveUvDomain(height_only, protected_domain.bounds, 256, 256,
                             height_only_domain, &error) &&
             width_only_domain.kind == UvDomainKind::ImportedTexture &&
             height_only_domain.kind == UvDomainKind::ImportedTexture &&
             !width_only_domain.declaration_reliable &&
             !height_only_domain.declaration_reliable,
         "one-axis declarations remain recorded but are not reliable domains");

  ResolvedUvDomain preserved_domain;
  preserved_domain.kind = UvDomainKind::Declared;
  preserved_domain.width = 7.0;
  preserved_domain.height = 9.0;
  preserved_domain.imported_width = 7;
  preserved_domain.imported_height = 9;
  const ResolvedUvDomain domain_before_failure = preserved_domain;
  expect(!resolveGeometryUvDomain(fixture.out_of_bounds_geometry, 256, 256,
                                  preserved_domain, &error) &&
             error.find("exceed") != std::string::npos &&
             preserved_domain == domain_before_failure,
         "UV outside declared and imported domains is rejected transactionally");

  bool all_special_cases_match = true;
  for (const auto &test_case : fixture.uv_cases) {
    UvBounds bounds{1.0, 2.0, 3.0, 4.0, 5u};
    const UvBounds bounds_before = bounds;
    error.clear();
    const bool scanned =
        scanGeometryUvBounds(test_case.geometry, bounds, &error);
    if (test_case.name == "non_finite") {
      all_special_cases_match &= !scanned && bounds == bounds_before &&
                                 error.find("non-finite") !=
                                     std::string::npos;
      continue;
    }
    all_special_cases_match &= scanned && !bounds.empty();
    ResolvedUvDomain domain = domain_before_failure;
    error.clear();
    const bool resolved =
        scanned && resolveUvDomain(test_case.geometry.description, bounds,
                                   test_case.imported_width,
                                   test_case.imported_height, domain, &error);
    all_special_cases_match &=
        resolved == test_case.expected_domain_success;
    if (!resolved) {
      all_special_cases_match &= domain == domain_before_failure;
    }
  }
  expect(all_special_cases_match,
         "Box Per-Face mirror rotation negative-size Up/Down precision and invalid UV table matches");

  const auto find_case = [&](std::string_view name)
      -> const xpbd::test_support::SyntheticUvCase * {
    const auto found = std::find_if(
        fixture.uv_cases.begin(), fixture.uv_cases.end(),
        [&](const auto &test_case) { return test_case.name == name; });
    return found == fixture.uv_cases.end() ? nullptr : &*found;
  };
  const auto *rotation_case = find_case("per_face_rotation");
  const auto *negative_case = find_case("negative_uv_size");
  const auto *up_down_case = find_case("up_down_corners");
  ResolvedFaceUv rotation_face;
  ResolvedFaceUv negative_face;
  ResolvedFaceUv up_face;
  const bool exact_faces =
      rotation_case != nullptr && negative_case != nullptr &&
      up_down_case != nullptr &&
      resolveBedrockFaceUv(rotation_case->geometry.bones[0].cubes[0],
                           BedrockUvFace::North, rotation_face, &error) &&
      resolveBedrockFaceUv(negative_case->geometry.bones[0].cubes[0],
                           BedrockUvFace::North, negative_face, &error) &&
      resolveBedrockFaceUv(up_down_case->geometry.bones[0].cubes[0],
                           BedrockUvFace::Up, up_face, &error);
  const auto rotation_corners = bedrockFaceUvCorners(rotation_face);
  expect(exact_faces && rotation_face.rotation_quarter_turns == 1 &&
             rotation_corners[0] == std::array<double, 2>{32.0, 24.0} &&
             negative_face.u0 == 64.0 && negative_face.u1 == 48.0 &&
             up_face.u0 == 104.0 && up_face.v0 == 36.0 &&
             up_face.u1 == 96.0 && up_face.v1 == 32.0,
         "double Face UV parser preserves rotation negative size and opposite Up corner exactly");

  const auto geometry_json = [](std::string_view description_members) {
    return std::string(
               R"({"minecraft:geometry":[{"description":{"identifier":"geometry.uv_test")") +
           (description_members.empty()
                ? std::string{}
                : std::string(",") + std::string(description_members)) +
           R"(},"bones":[{"name":"root","pivot":[0,0,0],"cubes":[]}]}]})";
  };
  const auto full_declaration = ModelLoader::loadFromString(
      geometry_json(R"("texture_width":16,"texture_height":32)"));
  const auto parsed_width_only = ModelLoader::loadFromString(
      geometry_json(R"("texture_width":16)"));
  const auto parsed_height_only = ModelLoader::loadFromString(
      geometry_json(R"("texture_height":32)"));
  expect(full_declaration.description.hasCompleteTextureSize() &&
             full_declaration.description.texture_width == 16 &&
             full_declaration.description.texture_height == 32 &&
             parsed_width_only.description.has_texture_width &&
             !parsed_width_only.description.has_texture_height &&
             !parsed_width_only.description.hasCompleteTextureSize() &&
             !parsed_height_only.description.has_texture_width &&
             parsed_height_only.description.has_texture_height,
         "loader records width and height declaration presence independently");

  bool invalid_declarations_rejected = true;
  const std::array<std::string_view, 6> invalid_declarations{
      R"("texture_width":0)",
      R"("texture_width":-1)",
      R"("texture_width":16.5)",
      R"("texture_width":16385)",
      R"("texture_width":"16")",
      R"("texture_height":null)",
  };
  for (const std::string_view declaration : invalid_declarations) {
    try {
      (void)ModelLoader::loadFromString(geometry_json(declaration));
      invalid_declarations_rejected = false;
    } catch (const std::exception &) {
    }
  }
  expect(invalid_declarations_rejected,
         "loader rejects non-positive fractional oversized and nonnumeric texture declarations");
  const auto maximum_declaration = ModelLoader::loadFromString(
      geometry_json(
          R"("texture_width":16384,"texture_height":16384)"));
  expect(maximum_declaration.description.hasCompleteTextureSize() &&
             maximum_declaration.description.texture_width == 16'384 &&
             maximum_declaration.description.texture_height == 16'384,
         "loader accepts the 16384 precision boundary exactly");
}

void testNegativeInflatePlaneCompatibility() {
  using xpbd::baker::CubeGeometry;
  using xpbd::loader::ModelLoader;

  const auto geometry = ModelLoader::loadFromString(R"({
    "minecraft:geometry": [{
      "description": {"identifier": "geometry.negative_inflate_plane"},
      "bones": [{
        "name": "root",
        "cubes": [{
          "origin": [-0.6, 32.27232, -1.45091],
          "size": [1.2, 0.8, 0],
          "inflate": -0.001,
          "uv": {"south": {"uv": [32, 8], "uv_size": [1, 1]}}
        }]
      }]
    }]
  })");
  const auto &plane = geometry.bones.front().cubes.front();
  const auto effective_size = CubeGeometry::effectiveSize(plane);
  const auto effective_origin = CubeGeometry::effectiveOrigin(plane);
  bool bind_vertices_valid = true;
  try {
    (void)CubeGeometry::bindVertices(plane);
  } catch (const std::exception &) {
    bind_vertices_valid = false;
  }
  expect(geometry.bones.size() == 1u &&
             geometry.bones.front().cubes.size() == 1u &&
             bind_vertices_valid,
         "loader accepts a zero-thickness plane with negative inflate");
  expectNearDouble(effective_size[0], 1.198, 1.0e-12,
                   "negative inflate still contracts a nonzero plane axis");
  expectNearDouble(effective_size[1], 0.798, 1.0e-12,
                   "negative inflate contracts both nonzero plane axes");
  expectNearDouble(effective_size[2], 0.0, 0.0,
                   "negative inflate preserves zero plane thickness");
  expectNearDouble(effective_origin[2], plane.origin[2], 0.0,
                   "negative inflate keeps a zero-thickness plane centered");

  const auto over_deflated_geometry = ModelLoader::loadFromString(R"({
    "minecraft:geometry": [{
      "bones": [{
        "name": "root",
        "cubes": [{
          "origin": [10, -4, 6],
          "size": [3.4, 3.85, 1.8],
          "inflate": -1
        }, {
          "origin": [-3, 5, 7],
          "size": [2, 4, 6],
          "inflate": -1000000
        }]
      }]
    }]
  })");
  const auto &partially_collapsed =
      over_deflated_geometry.bones.front().cubes[0];
  const auto partial_size = CubeGeometry::effectiveSize(partially_collapsed);
  const auto partial_origin =
      CubeGeometry::effectiveOrigin(partially_collapsed);
  expectNearDouble(partially_collapsed.inflate, -1.0, 0.0,
                   "loader preserves authored over-contracting inflate");
  expectNearDouble(partial_size[0], 1.4, 1.0e-12,
                   "over-contracting inflate preserves unaffected X extent");
  expectNearDouble(partial_size[1], 1.85, 1.0e-12,
                   "over-contracting inflate preserves unaffected Y extent");
  expectNearDouble(partial_size[2], 0.0, 0.0,
                   "over-contracting inflate collapses affected Z extent");
  expectNearDouble(partial_origin[0], 11.0, 1.0e-12,
                   "unaffected X origin retains authored contraction");
  expectNearDouble(partial_origin[1], -3.0, 1.0e-12,
                   "unaffected Y origin retains authored contraction");
  expectNearDouble(partial_origin[2], 6.9, 1.0e-12,
                   "over-contracted axis remains centered");

  const auto &fully_collapsed =
      over_deflated_geometry.bones.front().cubes[1];
  const auto full_size = CubeGeometry::effectiveSize(fully_collapsed);
  const auto full_origin = CubeGeometry::effectiveOrigin(fully_collapsed);
  bool arbitrary_negative_bind_valid = true;
  try {
    (void)CubeGeometry::bindVertices(fully_collapsed);
  } catch (const std::exception &) {
    arbitrary_negative_bind_valid = false;
  }
  expect(arbitrary_negative_bind_valid &&
             full_size == std::array<double, 3>{0.0, 0.0, 0.0},
         "loader and canonical geometry accept arbitrary finite negative "
         "inflate");
  expect(full_origin == std::array<double, 3>{-2.0, 7.0, 10.0},
         "fully over-contracted cube collapses at its authored center");

  xpbd::loader::Cube positively_inflated_plane;
  positively_inflated_plane.size[2] = 0.0;
  positively_inflated_plane.inflate = 0.5;
  const auto positive_size =
      CubeGeometry::effectiveSize(positively_inflated_plane);
  expectNearDouble(positive_size[2], 1.0, 0.0,
                   "positive inflate still thickens a zero-thickness plane");
}

void testResolvedUvDomainMaterialConsumers() {
  using xpbd::gfx::LabPbrUvCoverage;
  using xpbd::gfx::ResolvedMaterialTable;
  using xpbd::gfx::StaticIndexedModelMesh;
  using xpbd::gfx::StaticModelFace;
  using xpbd::gfx::StaticModelMaterialClass;
  using xpbd::gfx::TextureImage;
  using xpbd::gfx::UvDomainKind;
  using xpbd::gfx::ViewportGpuScene;
  using xpbd::gfx::ViewportMeshBuilder;
  using xpbd::test_support::SyntheticLargeUvFixture;
  using xpbd::test_support::buildSyntheticLargeUvFixture;
  using xpbd::test_support::fixtureTexel;

  SyntheticLargeUvFixture fixture;
  std::string error;
  expect(buildSyntheticLargeUvFixture(fixture, &error),
         "material consumers use the shared large-UV fixture");

  ViewportMeshBuilder builder;
  builder.setGeometry(&fixture.large_uv_geometry);
  builder.setTexture(&fixture.base);
  StaticIndexedModelMesh mesh;
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.uv_domain.valid() &&
             mesh.uv_domain.kind == UvDomainKind::Recovered &&
             mesh.uv_domain.width == 256.0 &&
             mesh.uv_domain.height == 256.0,
         "static mesh owns the recovered imported-atlas Domain");

  const auto face_uv_range = [&](std::string_view group, double &raw_min,
                                 double &raw_max, double &normalized_min,
                                 double &normalized_max) {
    bool found = false;
    raw_min = normalized_min =
        (std::numeric_limits<double>::max)();
    raw_max = normalized_max =
        (std::numeric_limits<double>::lowest)();
    for (const auto &face : mesh.faces) {
      if (face.bone_index >= mesh.bone_names.size() ||
          mesh.bone_names[face.bone_index] != group) {
        continue;
      }
      for (std::uint32_t local = 0; local < face.vertex_count; ++local) {
        const auto &vertex = mesh.vertices[face.first_vertex + local];
        raw_min = std::min(raw_min, vertex.raw_u);
        raw_max = std::max(raw_max, vertex.raw_u);
        normalized_min =
            std::min(normalized_min, static_cast<double>(vertex.u));
        normalized_max =
            std::max(normalized_max, static_cast<double>(vertex.u));
        found = true;
      }
    }
    return found;
  };
  double left_raw_min = 0.0, left_raw_max = 0.0, left_min = 0.0,
         left_max = 0.0;
  double right_raw_min = 0.0, right_raw_max = 0.0, right_min = 0.0,
         right_max = 0.0;
  expect(face_uv_range("eye_left", left_raw_min, left_raw_max, left_min,
                       left_max) &&
             face_uv_range("eye_right", right_raw_min, right_raw_max,
                           right_min, right_max) &&
             left_raw_min == 192.0 && left_raw_max == 208.0 &&
             right_raw_min == 208.0 && right_raw_max == 224.0,
         "static mesh preserves exact double raw UVs for both eyes");
  expectNear(static_cast<float>(left_min), 192.0f / 256.0f, 1.0e-7f,
             "left eye normalizes through recovered Domain");
  expectNear(static_cast<float>(left_max), 208.0f / 256.0f, 1.0e-7f,
             "left eye normalized maximum remains in its atlas cell");
  expectNear(static_cast<float>(right_min), 208.0f / 256.0f, 1.0e-7f,
             "right eye normalizes through recovered Domain");
  expectNear(static_cast<float>(right_max), 224.0f / 256.0f, 1.0e-7f,
             "right eye normalized maximum remains in its atlas cell");

  LabPbrUvCoverage coverage;
  expect(xpbd::gfx::rasterizeLabPbrUvCoverage(
             mesh, fixture.base.width, fixture.base.height, coverage,
             &error) &&
             error.empty(),
         "Coverage accepts the mesh-owned Domain and imported atlas extent");
  const auto contains_fixture_texel = [&](std::string_view group,
                                          const std::array<int, 2> &pixel) {
    const auto index = static_cast<std::uint32_t>(
        pixel[1] * fixture.base.width + pixel[0]);
    return coverageContains(coverage, group, index);
  };
  expect(contains_fixture_texel("eye_left", fixture.left_eye_texel) &&
             contains_fixture_texel("eye_right", fixture.right_eye_texel) &&
             !contains_fixture_texel("eye_left", fixture.repeat_trap_texel),
         "Coverage aligns both eye groups and excludes the Repeat trap");
  LabPbrUvCoverage preserved_coverage{
      7, 9, {{"keep", {{0u, 3u, 3u}}}}};
  const auto coverage_before_failure = preserved_coverage;
  expect(!xpbd::gfx::rasterizeLabPbrUvCoverage(
             mesh, fixture.base.width - 1, fixture.base.height,
             preserved_coverage, &error) &&
             error.find("match") != std::string::npos &&
             preserved_coverage.width == coverage_before_failure.width &&
             preserved_coverage.height == coverage_before_failure.height &&
             preserved_coverage.group_runs ==
                 coverage_before_failure.group_runs,
         "Coverage mismatch rejects without replacing the caller candidate");

  builder.setShowGround(false);
  builder.setShowBones(false);
  ViewportGpuScene dynamic_scene;
  builder.buildRest(dynamic_scene);
  bool saw_left_green = false;
  bool saw_right_blue = false;
  for (const auto &vertex : dynamic_scene.solid) {
    saw_left_green |= vertex.g > 0.75f && vertex.r < 0.2f && vertex.b < 0.3f;
    saw_right_blue |= vertex.b > 0.75f && vertex.r < 0.2f && vertex.g < 0.3f;
  }
  expect(saw_left_green && saw_right_blue && dynamic_scene.transparent.empty(),
         "dynamic preview keeps both opaque eye cells visible without Repeat");

  ViewportMeshBuilder protected_builder;
  protected_builder.setGeometry(&fixture.high_resolution_geometry);
  protected_builder.setTexture(&fixture.base);
  StaticIndexedModelMesh protected_mesh;
  protected_builder.buildStaticIndexedModel(protected_mesh);
  expect(protected_mesh.uv_domain.kind == UvDomainKind::Declared &&
             protected_mesh.uv_domain.width == 16.0 &&
             protected_mesh.uv_domain.imported_width == 256,
         "high-resolution atlas preserves the reliable declared Domain");

  ViewportMeshBuilder rejected_builder;
  rejected_builder.setGeometry(&fixture.out_of_bounds_geometry);
  rejected_builder.setTexture(&fixture.base);
  StaticIndexedModelMesh rejected_mesh;
  rejected_mesh.bone_names = {"preserved only until candidate clear"};
  bool rejected = false;
  try {
    rejected_builder.buildStaticIndexedModel(rejected_mesh);
  } catch (const std::invalid_argument &exception) {
    rejected = std::string_view(exception.what()).find("exceed") !=
               std::string_view::npos;
  }
  expect(rejected && rejected_mesh.vertices.empty() &&
             rejected_mesh.faces.empty() && !rejected_mesh.uv_domain.valid(),
         "true out-of-domain UV fails before publishing a static mesh");

  TextureImage edge_texture;
  edge_texture.width = 2;
  edge_texture.height = 1;
  edge_texture.source_channels = 4;
  edge_texture.rgba = {255u, 0u, 0u, 0u, 0u, 0u, 255u, 255u};
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
  edge_texture.sampleModelAtlasClamp(-0.25, 0.5, r, g, b, a);
  const bool left_clamped = r > 0.9f && b < 0.1f && a < 0.1f;
  edge_texture.sampleModelAtlasClamp(1.25, 0.5, r, g, b, a);
  const bool right_clamped = b > 0.9f && r < 0.1f && a > 0.9f;
  edge_texture.sample(-0.25f, 0.5f, r, g, b, a);
  expect(left_clamped && right_clamped && b > 0.9f,
         "model atlas clamps at both edges while generic sampling stays Repeat");

  StaticIndexedModelMesh alpha_mesh;
  alpha_mesh.bone_names = {"edge"};
  alpha_mesh.vertices.resize(4u);
  for (auto &vertex : alpha_mesh.vertices) {
    vertex.u = 1.25f;
    vertex.v = 0.5f;
  }
  alpha_mesh.indices = {0u, 1u, 2u, 0u, 2u, 3u};
  StaticModelFace alpha_face;
  alpha_face.vertex_count = 4u;
  alpha_face.index_count = 6u;
  alpha_face.textured = true;
  alpha_mesh.faces.push_back(alpha_face);
  expect(xpbd::gfx::staticModelFaceMaterial(
             alpha_mesh, alpha_mesh.faces.front(), &edge_texture) ==
             StaticModelMaterialClass::Opaque,
         "static Alpha classification clamps instead of wrapping to cutout");

  ResolvedMaterialTable clamp_table;
  clamp_table.width = 2;
  clamp_table.height = 1;
  xpbd::gfx::TextureImage clamp_base;
  clamp_base.width = 2;
  clamp_base.height = 1;
  clamp_base.source_channels = 4;
  clamp_base.rgba =
      {255u, 255u, 255u, 64u, 255u, 255u, 255u, 191u};
  clamp_table.setImageAssets(
      std::make_shared<const xpbd::gfx::TextureImage>(std::move(clamp_base)));
  expectNear(clamp_table.sample(-0.25f, 0.5f).opacity, 64.0f / 255.0f,
              1.0e-6f,
              "resolved material clamps the lower atlas edge");
  expectNear(clamp_table.sample(1.25f, 0.5f).opacity, 191.0f / 255.0f,
              1.0e-6f,
              "resolved material clamps the upper atlas edge");

  ResolvedMaterialTable resolved;
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             fixture.base, ResolvedMaterialTable{}, &fixture.normal,
             &fixture.specular, resolved, &error),
         "Base Normal and Specular build one aligned resolved material");
  const auto base_texel = fixtureTexel(fixture.base, fixture.left_eye_texel);
  const auto normal_texel =
      fixtureTexel(fixture.normal, fixture.left_eye_texel);
  const auto specular_texel =
      fixtureTexel(fixture.specular, fixture.left_eye_texel);
  const auto expected = xpbd::gfx::decodeLabPbrTexel(
      base_texel, &normal_texel, &specular_texel);
  const auto &sampled = resolved.sample(
      (static_cast<float>(fixture.left_eye_texel[0]) + 0.5f) /
          static_cast<float>(fixture.base.width),
      (static_cast<float>(fixture.left_eye_texel[1]) + 0.5f) /
          static_cast<float>(fixture.base.height));
  expect(sampled == expected,
         "CPU material reference samples Base Normal Specular at one eye texel");

  bool all_compact_samples_match = true;
  for (int y = 0; y < fixture.base.height && all_compact_samples_match; ++y) {
    for (int x = 0; x < fixture.base.width; ++x) {
      const std::array<int, 2> coordinate{x, y};
      const auto base_reference = fixtureTexel(fixture.base, coordinate);
      const auto normal_reference = fixtureTexel(fixture.normal, coordinate);
      const auto specular_reference =
          fixtureTexel(fixture.specular, coordinate);
      const auto reference = xpbd::gfx::decodeLabPbrTexel(
          base_reference, &normal_reference, &specular_reference);
      const auto compact = resolved.sample(
          (static_cast<float>(x) + 0.5f) /
              static_cast<float>(fixture.base.width),
          (static_cast<float>(y) + 0.5f) /
              static_cast<float>(fixture.base.height));
      if (compact != reference) {
        all_compact_samples_match = false;
        break;
      }
    }
  }
  expect(all_compact_samples_match,
         "compact on-demand material matches the legacy decoder at every small-fixture texel");
}

void testCc0PreviewSceneAssets() {
  using xpbd::gfx::PreviewSceneId;
  using xpbd::gfx::ViewportRasterScene;
  using xpbd::gfx::canonicalPreviewSceneId;
  using xpbd::gfx::kPreviewSceneChoiceCount;
  using xpbd::gfx::loadPreviewSceneSkyboxAsset;
  using xpbd::gfx::previewSceneAssetFilename;
  using xpbd::gfx::previewSceneChoiceIndex;
  using xpbd::gfx::previewSceneIdFromChoiceIndex;

  expect(canonicalPreviewSceneId(PreviewSceneId::Dawn) ==
             PreviewSceneId::Sunset &&
             canonicalPreviewSceneId(PreviewSceneId::Space) ==
                 PreviewSceneId::Night &&
             canonicalPreviewSceneId(PreviewSceneId::End) ==
                 PreviewSceneId::Night &&
             canonicalPreviewSceneId(PreviewSceneId::Storm) ==
                 PreviewSceneId::Overcast,
         "retired preview presets map to curated stable replacements");
  for (int index = 0; index < kPreviewSceneChoiceCount; ++index) {
    const PreviewSceneId id = previewSceneIdFromChoiceIndex(index);
    expect(previewSceneChoiceIndex(id) == index,
           "curated preview scene index round-trips");
  }

  const std::filesystem::path asset_root =
      std::filesystem::path(XPBD_TEST_SOURCE_DIR) / "assets" /
      "preview_scenes";
  const std::array<PreviewSceneId, 5> asset_ids{
      PreviewSceneId::Studio, PreviewSceneId::Sky, PreviewSceneId::Night,
      PreviewSceneId::Sunset, PreviewSceneId::Overcast};
  for (const PreviewSceneId id : asset_ids) {
    const std::filesystem::path source =
        asset_root / previewSceneAssetFilename(id);
    expect(std::filesystem::is_regular_file(source),
           "bundled CC0 preview HDR exists");
    xpbd::gfx::PreviewSkybox skybox;
    std::string error;
    expect(loadPreviewSceneSkyboxAsset(id, asset_root, skybox, &error),
           "bundled CC0 preview HDR converts to cubemap");
    expect(skybox.valid() && skybox.face_size == 384 && skybox.cc0_asset &&
               skybox.source_identity == source.string(),
           "converted preview cubemap retains CC0 source identity");
    if (skybox.valid()) {
      std::uint8_t minimum = 255u;
      std::uint8_t maximum = 0u;
      std::uint8_t maximum_chroma = 0u;
      for (std::size_t pixel = 0; pixel + 3u < skybox.rgba.size();
           pixel += 4u) {
        const auto [lo, hi] =
            std::minmax({skybox.rgba[pixel + 0], skybox.rgba[pixel + 1],
                         skybox.rgba[pixel + 2]});
        minimum = std::min(minimum, lo);
        maximum = std::max(maximum, hi);
        maximum_chroma =
            std::max(maximum_chroma, static_cast<std::uint8_t>(hi - lo));
      }
      expect(minimum < maximum && maximum_chroma > 8u,
             "converted preview cubemap has finite visible range");
      if (id != PreviewSceneId::Studio) {
        const std::size_t face_pixels =
            static_cast<std::size_t>(skybox.face_size) * skybox.face_size;
        const std::size_t lower_offset = face_pixels * 4u * 3u;
        std::uint64_t lower_sum = 0u;
        for (std::size_t pixel = 0; pixel < face_pixels; ++pixel) {
          const std::size_t sample = lower_offset + pixel * 4u;
          lower_sum += skybox.rgba[sample + 0] +
                       skybox.rgba[sample + 1] +
                       skybox.rgba[sample + 2];
        }
        const double lower_average =
            static_cast<double>(lower_sum) /
            static_cast<double>(face_pixels * 3u);
        expect(lower_average > 12.0,
               "pure-sky cubemap synthesizes a non-black lower hemisphere");
      }
    }
  }

  ViewportRasterScene studio;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Studio, true, true,
                                      false, 0.0f, studio, asset_root);
  expect(studio.id == PreviewSceneId::Studio && studio.skybox.cc0_asset &&
             studio.environment.solid.empty() &&
             studio.environment.transparent.empty() && !studio.solid_ground &&
             !studio.show_environment,
         "CC0 Studio removes the old y=0 room geometry");

  ViewportRasterScene ocean;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, true,
                                      1.0f, ocean, asset_root);
  expect(ocean.skybox.cc0_asset && ocean.surface_dynamic_baked &&
             ocean.show_environment,
         "dynamic Ocean retains a static CC0 sky and animated surface");

  constexpr std::size_t kDesertVertexCount = 256u * 256u * 6u;
  constexpr std::size_t kOceanVertexCount = 176u * 176u * 6u;
  ViewportRasterScene desert;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Desert, true, true,
                                      false, 0.0f, desert, asset_root);
  expect(desert.environment.solid.size() == kDesertVertexCount &&
             desert.environment.transparent.empty(),
         "FastNoiseLite Desert emits the high-resolution 256x256 topology");
  float desert_min_height = std::numeric_limits<float>::max();
  float desert_max_height = std::numeric_limits<float>::lowest();
  float desert_min_normal_y = 1.0f;
  bool desert_finite = true;
  bool desert_origin_clear = true;
  for (const xpbd::gfx::MeshVertex &vertex : desert.environment.solid) {
    desert_finite =
        desert_finite && std::isfinite(vertex.px) &&
        std::isfinite(vertex.py) && std::isfinite(vertex.pz) &&
        std::isfinite(vertex.nx) && std::isfinite(vertex.ny) &&
        std::isfinite(vertex.nz) && std::isfinite(vertex.r) &&
        std::isfinite(vertex.g) && std::isfinite(vertex.b) &&
        std::isfinite(vertex.a);
    desert_min_height = std::min(desert_min_height, vertex.py);
    desert_max_height = std::max(desert_max_height, vertex.py);
    desert_min_normal_y = std::min(desert_min_normal_y, vertex.ny);
    if (std::abs(vertex.px) <= 10.0f && std::abs(vertex.pz) <= 10.0f) {
      desert_origin_clear =
          desert_origin_clear && std::abs(vertex.py) < 0.35f;
    }
  }
  expect(desert_finite, "FastNoiseLite Desert vertices remain finite");
  expect(desert_max_height - desert_min_height > 12.0f &&
             desert_min_normal_y < 0.98f,
         "FastNoiseLite Desert has non-flat dune relief and normals");
  expect(desert_origin_clear,
         "FastNoiseLite Desert preserves the y=0 inspection area");
  const std::uint64_t desert_generation = desert.geometry_generation;
  const std::vector<xpbd::gfx::MeshVertex> frozen_desert =
      desert.environment.solid;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Desert, true, true,
                                      false, 25.0f, desert, asset_root);
  expect(desert.geometry_generation == desert_generation &&
             desert.environment.solid.size() == frozen_desert.size() &&
             desert.environment.solid.front().py ==
                 frozen_desert.front().py,
         "static Desert does not rebuild when only time advances");

  expect(ocean.environment.solid.size() == 6u &&
             ocean.environment.transparent.size() == kOceanVertexCount,
         "osgw Ocean keeps a six-vertex deep body and high-resolution 176x176 "
         "wave topology");
  float ocean_min_alpha = 1.0f;
  float ocean_max_alpha = 0.0f;
  bool ocean_finite = true;
  bool ocean_normals_unit = true;
  bool ocean_origin_clear = true;
  bool ocean_horizontal_displacement = false;
  constexpr float kOceanHalf = 180.0f;
  constexpr float kOceanStep = 360.0f / 176.0f;
  for (const xpbd::gfx::MeshVertex &vertex : ocean.environment.transparent) {
    ocean_finite =
        ocean_finite && std::isfinite(vertex.px) &&
        std::isfinite(vertex.py) && std::isfinite(vertex.pz) &&
        std::isfinite(vertex.nx) && std::isfinite(vertex.ny) &&
        std::isfinite(vertex.nz) && std::isfinite(vertex.r) &&
        std::isfinite(vertex.g) && std::isfinite(vertex.b) &&
        std::isfinite(vertex.a);
    const float normal_length =
        std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny +
                  vertex.nz * vertex.nz);
    ocean_normals_unit =
        ocean_normals_unit && std::abs(normal_length - 1.0f) < 2.0e-3f;
    ocean_min_alpha = std::min(ocean_min_alpha, vertex.a);
    ocean_max_alpha = std::max(ocean_max_alpha, vertex.a);
    if (vertex.px * vertex.px + vertex.pz * vertex.pz < 100.0f) {
      ocean_origin_clear = ocean_origin_clear && vertex.py < -0.20f;
    }
    const float grid_x = (vertex.px + kOceanHalf) / kOceanStep;
    const float grid_z = (vertex.pz + kOceanHalf) / kOceanStep;
    ocean_horizontal_displacement =
        ocean_horizontal_displacement ||
        (std::abs(grid_x - std::round(grid_x)) > 0.02f &&
         std::abs(grid_z - std::round(grid_z)) > 0.02f);
  }
  expect(ocean_finite && ocean_normals_unit,
         "osgw Ocean positions and analytic normals remain finite/unit");
  expect(ocean_min_alpha >= 0.69f && ocean_max_alpha <= 0.99f &&
             ocean_max_alpha - ocean_min_alpha > 0.01f,
         "osgw Ocean remains on the varied-alpha transparent route");
  expect(ocean_horizontal_displacement,
         "osgw Ocean applies horizontal Gerstner displacement");
  expect(ocean_origin_clear,
         "osgw Ocean keeps the y=0 model inspection area uncovered");

  const std::vector<xpbd::gfx::MeshVertex> ocean_at_one =
      ocean.environment.transparent;
  const std::uint64_t ocean_generation = ocean.geometry_generation;
  const std::uint64_t ocean_topology_generation = ocean.topology_generation;
  const std::vector<std::uint8_t> ocean_sky = ocean.skybox.rgba;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, true,
                                      1.2f, ocean, asset_root);
  std::size_t changed_ocean_vertices = 0u;
  for (std::size_t index = 0; index < ocean_at_one.size(); ++index) {
    const auto &before = ocean_at_one[index];
    const auto &after = ocean.environment.transparent[index];
    if (std::abs(before.px - after.px) > 1.0e-4f ||
        std::abs(before.py - after.py) > 1.0e-4f ||
        std::abs(before.pz - after.pz) > 1.0e-4f) {
      ++changed_ocean_vertices;
    }
  }
  expect(ocean.geometry_generation == ocean_generation + 1u &&
             ocean.topology_generation == ocean_topology_generation &&
             ocean.environment.transparent.size() == ocean_at_one.size() &&
             changed_ocean_vertices > ocean_at_one.size() / 2u,
         "dynamic osgw Ocean advances positions with stable topology");
  expect(ocean.skybox.rgba == ocean_sky && ocean.skybox.cc0_asset,
         "dynamic osgw Ocean leaves the packaged CC0 sky unchanged");

  ViewportRasterScene static_ocean;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, false,
                                      0.0f, static_ocean, asset_root);
  const std::uint64_t static_ocean_generation =
      static_ocean.geometry_generation;
  const std::vector<xpbd::gfx::MeshVertex> frozen_ocean =
      static_ocean.environment.transparent;
  xpbd::gfx::buildViewportRasterScene(PreviewSceneId::Ocean, true, true, false,
                                      30.0f, static_ocean, asset_root);
  expect(static_ocean.geometry_generation == static_ocean_generation &&
             static_ocean.environment.transparent.size() ==
                 frozen_ocean.size() &&
             static_ocean.environment.transparent.front().px ==
                 frozen_ocean.front().px &&
             static_ocean.environment.transparent.front().py ==
                 frozen_ocean.front().py,
         "static osgw Ocean freezes its t=0 surface");
}

void testLabPbrDecode() {
  using xpbd::gfx::LabPbrMetalKind;
  using xpbd::gfx::LabPbrDebugView;
  using xpbd::gfx::decodeLabPbrTexel;
  using xpbd::gfx::labPbrDebugColor;
  using xpbd::gfx::labPbrDebugViewFromName;
  using xpbd::gfx::labPbrDebugViewName;
  using xpbd::gfx::labPbrFeatureFlags;
  using xpbd::gfx::rtRrRoughnessFromGgxAlpha;
  using xpbd::gfx::srgb8ToLinear;

  expectNear(srgb8ToLinear(0u), 0.0f, 1.0e-6f, "sRGB black -> linear zero");
  expectNear(srgb8ToLinear(255u), 1.0f, 1.0e-6f,
             "sRGB white -> linear one");
  expectNear(srgb8ToLinear(188u), 0.5029f, 2.0e-3f,
             "sRGB midpoint uses IEC transfer");

  const std::array<std::uint8_t, 4> base{188u, 128u, 0u, 64u};
  auto fallback = decodeLabPbrTexel(base, nullptr, nullptr);
  expectNear(fallback.base_color_linear[0], 0.5029f, 2.0e-3f,
             "base RGB resolves to linear");
  expectNear(fallback.opacity, 64.0f / 255.0f, 1.0e-6f,
             "base alpha remains opacity");
  expectNear(fallback.tangent_normal[2], 1.0f, 1.0e-6f,
             "missing normal uses flat tangent normal");
  expectNear(fallback.ambient_occlusion, 1.0f, 1.0e-6f,
             "missing normal uses unoccluded AO");
  expectNear(fallback.ggx_alpha, 1.0f, 1.0e-6f,
             "missing specular uses fully rough fallback");
  expectNear(fallback.dielectric_f0, 0.04f, 1.0e-6f,
             "missing specular uses dielectric F0");
  expectNear(fallback.emission_strength, 0.0f, 1.0e-6f,
             "missing specular has no emission");
  expect(labPbrFeatureFlags(nullptr) == 0u,
         "missing material exposes no GPU sidecar features");

  const std::array<std::uint8_t, 4> normal{255u, 128u, 64u, 0u};
  const std::array<std::uint8_t, 4> specular{128u, 229u, 64u, 254u};
  const auto resolved = decodeLabPbrTexel(base, &normal, &specular);
  expect(resolved.tangent_normal[0] > 0.99f,
         "normal red decodes toward tangent +X");
  expect(resolved.tangent_normal[1] < 0.0f,
         "DirectX normal green decodes Y-minus");
  expectNear(resolved.ambient_occlusion, 64.0f / 255.0f, 1.0e-6f,
             "normal blue decodes linear AO");
  expectNear(resolved.relative_depth, 0.25f, 1.0e-6f,
             "normal alpha zero decodes maximum relative depth");
  expectNear(resolved.perceptual_smoothness, 128.0f / 255.0f, 1.0e-6f,
             "specular red decodes perceptual smoothness");
  const float expected_roughness = 1.0f - 128.0f / 255.0f;
  expectNear(resolved.ggx_alpha,
             expected_roughness * expected_roughness, 1.0e-6f,
             "perceptual smoothness converts to GGX alpha");
  const std::array<std::uint8_t, 4> smoothness_codes{{255u, 254u, 128u,
                                                      0u}};
  for (const std::uint8_t smoothness_code : smoothness_codes) {
    const std::array<std::uint8_t, 4> roughness_specular{
        smoothness_code, 0u, 0u, 255u};
    const auto roughness_texel =
        decodeLabPbrTexel(base, nullptr, &roughness_specular);
    const float expected_perceptual =
        static_cast<float>(255u - smoothness_code) / 255.0f;
    expectNear(roughness_texel.perceptual_roughness, expected_perceptual,
               1.0e-7f,
               "LabPBR specular R decodes exact perceptual roughness");
    expectNear(roughness_texel.ggx_alpha,
               expected_perceptual * expected_perceptual, 1.0e-7f,
               "LabPBR specular R squares exactly once into GGX alpha");
    expectNear(rtRrRoughnessFromGgxAlpha(roughness_texel.ggx_alpha),
               expected_perceptual, 1.0e-7f,
               "RR guide recovers the exact perceptual roughness");
  }
  expectNear(resolved.dielectric_f0, 229.0f / 255.0f, 1.0e-6f,
             "dielectric F0 divides by 255");
  expectNear(resolved.porosity, 1.0f, 1.0e-6f,
             "specular blue 64 decodes full porosity");
  expectNear(resolved.emission_strength, 1.0f, 1.0e-6f,
             "specular alpha 254 decodes full emission");
  expectNear(resolved.opacity, 64.0f / 255.0f, 1.0e-6f,
             "emission does not replace base opacity");
  expect(labPbrDebugViewFromName("roughness") ==
             LabPbrDebugView::GgxAlpha,
         "material debug name selects GGX alpha");
  expect(std::string(labPbrDebugViewName(LabPbrDebugView::Emission)) ==
             "emission",
         "material debug view has stable diagnostic name");
  const auto opacity_debug =
      labPbrDebugColor(resolved, LabPbrDebugView::Opacity);
  expectNear(opacity_debug[0], resolved.opacity, 1.0e-6f,
             "CPU opacity debug color matches resolved opacity");
  const auto normal_debug =
      labPbrDebugColor(resolved, LabPbrDebugView::Normal);
  expectNear(normal_debug[0], resolved.tangent_normal[0] * 0.5f + 0.5f,
             1.0e-6f,
             "CPU normal debug color matches Raster/PT convention");

  auto rgb_specular = decodeLabPbrTexel(base, nullptr, &specular, 3);
  expectNear(rgb_specular.emission_strength, 0.0f, 1.0e-6f,
             "RGB-only specular forces emission off before filtering");
  const std::array<std::uint8_t, 4> ignored_emission{0u, 0u, 0u, 255u};
  auto ignored_alpha =
      decodeLabPbrTexel(base, nullptr, &ignored_emission);
  expectNear(ignored_alpha.emission_strength, 0.0f, 1.0e-6f,
             "specular alpha 255 is ignored/no emission");

  const std::array<std::uint8_t, 4> iron_specular{255u, 230u, 255u, 0u};
  const auto iron = decodeLabPbrTexel(base, nullptr, &iron_specular);
  expect(iron.metal_kind == LabPbrMetalKind::Predefined,
         "metal code 230 selects predefined metal");
  expect(iron.f0_color[0] > 0.5f && iron.f0_color[0] < 0.7f,
         "iron F0 is derived from official optical constants");
  for (std::size_t channel = 0; channel < 3u; ++channel) {
    expectNear(iron.metal_reflection_tint[channel],
               iron.base_color_linear[channel], 1.0e-6f,
               "predefined metal uses linear albedo as reflection tint");
  }
  expectNear(iron.subsurface_scattering, 1.0f, 1.0e-6f,
             "specular blue 255 decodes full SSS");

  const std::array<std::uint8_t, 4> custom_specular{0u, 255u, 0u, 255u};
  const auto custom = decodeLabPbrTexel(base, nullptr, &custom_specular);
  expect(custom.metal_kind == LabPbrMetalKind::Custom,
         "metal code 255 selects custom metal");
  expectNear(custom.f0_color[0], custom.base_color_linear[0], 1.0e-6f,
             "custom metal uses linear base color as F0");
  for (const float tint : custom.metal_reflection_tint) {
    expectNear(tint, 1.0f, 1.0e-6f,
               "custom metal does not apply albedo tint twice");
  }
}

void testLabPbrDiscoveryAndFallback() {
  namespace fs = std::filesystem;
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_regression_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error, "create isolated LabPBR regression directory");
  if (filesystem_error) {
    return;
  }

  const fs::path base_path = directory / "atlas.png";
  const fs::path normal_path = directory / "atlas_n.png";
  const fs::path specular_path = directory / "atlas_s.png";
  const fs::path properties_path = directory / "texture.properties";
  auto writeBytes = [](const fs::path &path, const void *data,
                       std::size_t size) {
    std::ofstream output(path, std::ios::binary);
    output.write(static_cast<const char *>(data),
                 static_cast<std::streamsize>(size));
    return output.good();
  };
  expect(writeBytes(base_path, kWhitePng, sizeof(kWhitePng)),
         "write temporary base atlas");
  expect(writeBytes(normal_path, kWhitePng, sizeof(kWhitePng)),
         "write temporary normal sidecar");
  expect(writeBytes(specular_path, kWhitePng, sizeof(kWhitePng)),
         "write temporary specular sidecar");
  const std::string properties = "format=lab-pbr/1.3\n";
  expect(writeBytes(properties_path, properties.data(), properties.size()),
         "write temporary LabPBR declaration");

  const auto paths = xpbd::gfx::discoverLabPbrAssets(base_path);
  expect(paths.normal == normal_path && paths.normal_exists,
         "discover sibling _n.png");
  expect(paths.specular == specular_path && paths.specular_exists,
         "discover sibling _s.png");
  expect(paths.properties == properties_path && paths.properties_exists,
         "discover texture.properties");

  xpbd::gfx::TextureImage base;
  std::string error;
  expect(xpbd::gfx::loadTextureImage(base_path, base, &error),
         "load temporary base atlas");
  xpbd::gfx::ResolvedMaterialTable material;
  expect(xpbd::gfx::resolveLabPbrMaterial(base, base_path, material, &error),
         "resolve declared LabPBR material");
  expect(material.valid(), "resolved material table valid");
  expect(material.format == xpbd::gfx::LabPbrFormat::LabPbr13,
         "declared LabPBR 1.3 accepted");
  expect(material.normal_map_active && material.specular_map_active,
         "compatible sidecars activated");
  expect(xpbd::gfx::labPbrFeatureFlags(&material) ==
             (xpbd::gfx::kLabPbrNormalMapActive |
              xpbd::gfx::kLabPbrSpecularMapActive),
         "resolved GPU feature bits match active sidecars");
  expectNear(material.sample(0.5f, 0.5f).emission_strength, 0.0f, 1.0e-6f,
              "RGB sidecar synthesized alpha does not emit");

  const std::string unsupported = "format=lab-pbr/1.2\n";
  expect(writeBytes(properties_path, unsupported.data(), unsupported.size()),
         "replace declaration with unsupported format");
  xpbd::gfx::ResolvedMaterialTable fallback;
  expect(xpbd::gfx::resolveLabPbrMaterial(base, base_path, fallback, &error),
         "unsupported format degrades to base material");
  expect(fallback.format == xpbd::gfx::LabPbrFormat::Unsupported,
         "unsupported format is explicit");
  expect(!fallback.normal_map_active && !fallback.specular_map_active,
         "unsupported sidecars are safely ignored");
  expect(!fallback.warnings.empty(), "unsupported format reports warning");

  fs::remove_all(directory, filesystem_error);
}

void testStrictLabPbrSuiteImport() {
  namespace fs = std::filesystem;
  using xpbd::gfx::LabPbrSuiteImportStatus;

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_strict_import_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated strict LabPBR import directory");
  if (filesystem_error) {
    return;
  }

  const auto writeBytes = [](const fs::path &path,
                             const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
  };
  const auto readBytes = [](const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
  };
  const auto encode = [](int width, int height,
                         const std::vector<std::uint8_t> &rgba) {
    std::vector<std::uint8_t> png;
    std::string error;
    expect(xpbd::gfx::encodePngRgba8(width, height, rgba, png, &error),
           "encode strict LabPBR fixture PNG");
    return png;
  };

  const std::vector<std::uint8_t> base_rgba{
      255u, 128u, 64u, 255u, 32u, 64u, 128u, 127u};
  const std::vector<std::uint8_t> specular_rgba{
      0u, 0u, 0u, 0u, 255u, 230u, 64u, 255u};
  const std::vector<std::uint8_t> normal_rgba{
      128u, 128u, 255u, 255u, 140u, 120u, 200u, 20u};
  const auto base_png = encode(2, 1, base_rgba);
  const auto specular_png = encode(2, 1, specular_rgba);
  const auto normal_png = encode(2, 1, normal_rgba);

  const fs::path base_path = directory / "texture.png";
  const fs::path specular_path = directory / "texture_s.png";
  const fs::path normal_path = directory / "texture_n.png";
  const fs::path properties_path = directory / "texture.properties";
  expect(writeBytes(base_path, base_png), "write strict base PNG");
  expect(writeBytes(specular_path, specular_png),
         "write strict specular PNG");
  expect(writeBytes(normal_path, normal_png), "write strict normal PNG");
  const std::string properties_text = "format=lab-pbr/1.3\n";
  const std::vector<std::uint8_t> properties_bytes(properties_text.begin(),
                                                    properties_text.end());
  expect(writeBytes(properties_path, properties_bytes),
         "write strict LabPBR properties");

  const fs::path second_base = directory / "stone.png";
  const fs::path second_specular = directory / "stone_s.png";
  expect(writeBytes(second_base, base_png), "write second candidate base");
  expect(writeBytes(second_specular, specular_png),
         "write second candidate specular");

  std::string error;
  const auto candidates =
      xpbd::gfx::discoverLabPbrSuiteCandidates(directory, &error);
  expect(error.empty() && candidates.size() == 2u,
         "folder discovery returns only paired base candidates");
  expect(std::find(candidates.begin(), candidates.end(),
                   fs::absolute(base_path).lexically_normal()) !=
             candidates.end() &&
             std::find(candidates.begin(), candidates.end(),
                       fs::absolute(second_base).lexically_normal()) !=
                 candidates.end(),
         "folder discovery retains both stems without guessing");

  xpbd::gfx::LabPbrSuiteImportCache cache;
  xpbd::gfx::LabPbrSuiteImportLimits predecode_limits;
  predecode_limits.maximum_peak_bytes =
      static_cast<std::uint64_t>(base_png.size() + specular_png.size() +
                                 normal_png.size() +
                                 properties_bytes.size() + 1u);
  predecode_limits.copy_normal_to_iris_asset = true;
  const auto predecode_rejected = xpbd::gfx::importLabPbrSuite(
      base_path, false, &cache, predecode_limits);
  expect(predecode_rejected.status == LabPbrSuiteImportStatus::Failed &&
             predecode_rejected.error.find("budget preflight") !=
                 std::string::npos &&
             predecode_rejected.suite.base_image == nullptr &&
             cache.size() == 0u,
         "strict Suite rejects the final-model budget after Header and before pixel decode");

  auto imported =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(imported.imported() && !imported.suite.cache_hit,
         "strict complete LabPBR suite imports");
  expect(imported.suite.material.specular_map_active &&
             imported.suite.material.normal_map_active &&
             imported.suite.material.format_declared &&
             imported.suite.base_image ==
                 imported.suite.material.baseImageAsset() &&
             imported.suite.material.normalImageAsset() != nullptr &&
             imported.suite.material.specularImageAsset() != nullptr,
         "strict import activates sidecars and shares decoded image assets");
  const auto imported_copy = imported.suite;
  expect(imported_copy.base_image == imported.suite.base_image &&
             imported_copy.material.baseImageAsset() ==
                 imported.suite.material.baseImageAsset() &&
             imported_copy.material.normalImageAsset() ==
                 imported.suite.material.normalImageAsset() &&
             imported_copy.material.specularImageAsset() ==
                 imported.suite.material.specularImageAsset(),
         "Imported Suite copies retain shared image identities");
  expect(imported.suite.source.base.valid() &&
             imported.suite.source.specular.valid() &&
             imported.suite.source.normal.valid() &&
             imported.suite.source.properties.valid(),
         "strict import retains complete source snapshots");
  expect(*imported.suite.source.base.original_bytes == base_png &&
             *imported.suite.source.specular.original_bytes == specular_png &&
             *imported.suite.source.normal.original_bytes == normal_png &&
             *imported.suite.source.properties.original_bytes ==
                 properties_bytes,
         "strict import preserves exact source bytes");
  expect(readBytes(base_path) == base_png &&
             readBytes(specular_path) == specular_png &&
             readBytes(normal_path) == normal_png &&
             readBytes(properties_path) == properties_bytes,
         "strict import never mutates source files");

  auto cached = xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(cached.imported() && cached.suite.cache_hit && cache.size() == 1u &&
             cached.suite.base_image == imported.suite.base_image &&
             cached.suite.material.normalImageAsset() ==
                 imported.suite.material.normalImageAsset() &&
             cached.suite.material.specularImageAsset() ==
                 imported.suite.material.specularImageAsset(),
         "same path and checksum reimport reuses shared cache assets");
  const auto unchanged =
      xpbd::gfx::checkLabPbrSuiteSourceChanges(cached.suite.source);
  expect(!unchanged.reloadRecommended() && !unchanged.metadata_changed &&
             unchanged.error.empty(),
         "unchanged strict source snapshot stays current");

  const xpbd::gfx::TextureImage *excluded_images[] = {
      imported.suite.base_image.get(),
      imported.suite.material.normalImageAsset().get(),
      imported.suite.material.specularImageAsset().get(),
  };
  const std::vector<std::uint8_t> *excluded_sources[] = {
      imported.suite.source.base.original_bytes.get(),
      imported.suite.source.normal.original_bytes.get(),
      imported.suite.source.specular.original_bytes.get(),
      imported.suite.source.properties.original_bytes.get(),
  };
  const auto full_cache_bytes = cache.residentBytes();
  const auto metadata_only_cache_bytes =
      cache.residentBytes(excluded_images, excluded_sources);
  expect(cache.maximumBytes() ==
             xpbd::gfx::kLabPbrDefaultImportCacheBudgetBytes &&
             full_cache_bytes <= cache.maximumBytes() &&
             metadata_only_cache_bytes < full_cache_bytes,
         "cache byte accounting excludes Session-shared image and source identities");

  const fs::path third_base = directory / "brick.png";
  const fs::path third_specular = directory / "brick_s.png";
  const fs::path fourth_base = directory / "grass.png";
  const fs::path fourth_specular = directory / "grass_s.png";
  expect(writeBytes(third_base, base_png) &&
             writeBytes(third_specular, specular_png) &&
             writeBytes(fourth_base, base_png) &&
             writeBytes(fourth_specular, specular_png),
         "write equal-charge LRU cache fixtures");

  xpbd::gfx::LabPbrSuiteImportCache lru_cache;
  auto lru_stone =
      xpbd::gfx::importLabPbrSuite(second_base, false, &lru_cache);
  auto lru_brick =
      xpbd::gfx::importLabPbrSuite(third_base, false, &lru_cache);
  const auto two_entry_budget = lru_cache.residentBytes();
  lru_cache.setMaximumBytes(two_entry_budget);
  xpbd::gfx::ImportedLabPbrSuite touched_stone;
  expect(lru_stone.imported() && lru_brick.imported() &&
             lru_cache.size() == 2u &&
             lru_cache.find(lru_stone.suite.source.cache_key,
                            touched_stone) &&
             touched_stone.base_image == lru_stone.suite.base_image,
         "cache Misses populate shared entries and find touches MRU identity");
  auto lru_grass =
      xpbd::gfx::importLabPbrSuite(fourth_base, false, &lru_cache);
  xpbd::gfx::ImportedLabPbrSuite evicted_brick;
  xpbd::gfx::ImportedLabPbrSuite retained_stone;
  xpbd::gfx::ImportedLabPbrSuite retained_grass;
  expect(lru_grass.imported() && lru_cache.size() == 2u &&
             lru_cache.residentBytes() <= lru_cache.maximumBytes() &&
             !lru_cache.find(lru_brick.suite.source.cache_key,
                             evicted_brick) &&
             lru_cache.find(lru_stone.suite.source.cache_key,
                            retained_stone) &&
             lru_cache.find(lru_grass.suite.source.cache_key,
                            retained_grass) &&
             retained_stone.base_image == lru_stone.suite.base_image &&
             retained_grass.base_image == lru_grass.suite.base_image,
         "LRU budget evicts the cold entry while retaining shared MRU assets");

  xpbd::gfx::LabPbrSuiteImportCache one_entry_cache;
  auto one_entry =
      xpbd::gfx::importLabPbrSuite(second_base, false, &one_entry_cache);
  const auto one_entry_bytes = one_entry_cache.residentBytes();
  const auto oversize_budget =
      one_entry_bytes > 0u ? one_entry_bytes - 1u : 0u;
  xpbd::gfx::LabPbrSuiteImportCache oversize_cache(oversize_budget);
  auto oversized =
      xpbd::gfx::importLabPbrSuite(second_base, false, &oversize_cache);
  expect(one_entry.imported() && one_entry_bytes > 0u &&
             oversized.imported() && !oversized.suite.cache_hit &&
             oversize_cache.size() == 0u &&
             oversize_cache.residentBytes() == 0u,
         "individually oversized Suite bypasses cache without failing import");

  xpbd::gfx::LabPbrSuiteImportCache release_cache;
  auto releasable =
      xpbd::gfx::importLabPbrSuite(fourth_base, false, &release_cache);
  std::weak_ptr<const xpbd::gfx::TextureImage> released_base =
      releasable.suite.base_image;
  releasable.suite = {};
  expect(releasable.imported() == false && !released_base.expired() &&
             release_cache.size() == 1u,
         "cache owns the shared asset after the import result releases it");
  release_cache.clear();
  expect(released_base.expired() && release_cache.size() == 0u &&
             release_cache.residentBytes() == 0u,
         "cache Clear releases its final shared asset ownership");

  auto changed_specular_rgba = specular_rgba;
  changed_specular_rgba[0] = 127u;
  const auto changed_specular_png = encode(2, 1, changed_specular_rgba);
  expect(writeBytes(specular_path, changed_specular_png),
         "replace strict specular source");
  const auto changed =
      xpbd::gfx::checkLabPbrSuiteSourceChanges(cached.suite.source);
  expect(changed.content_changed && changed.reloadRecommended() &&
             std::find(changed.changed_paths.begin(),
                       changed.changed_paths.end(),
                       fs::absolute(specular_path).lexically_normal()) !=
                 changed.changed_paths.end(),
         "checksum detects changed source content");
  auto reloaded =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(reloaded.imported() && !reloaded.suite.cache_hit &&
             cache.size() == 2u,
         "changed checksum creates a new cached import");

  fs::remove(specular_path, filesystem_error);
  expect(!filesystem_error, "remove mandatory specular fixture");
  const auto missing =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(missing.status == LabPbrSuiteImportStatus::Failed &&
             !missing.error.empty() && cache.size() == 2u,
         "missing mandatory _s is rejected without damaging cache");
  expect(writeBytes(specular_path, specular_png),
         "restore mandatory specular fixture");

  fs::remove(properties_path, filesystem_error);
  expect(!filesystem_error, "remove optional properties fixture");
  const auto needs_confirmation =
      xpbd::gfx::importLabPbrSuite(base_path, false, &cache);
  expect(needs_confirmation.status ==
             LabPbrSuiteImportStatus::NeedsLabPbr13Confirmation,
         "missing properties requires explicit confirmation");
  const auto confirmed =
      xpbd::gfx::importLabPbrSuite(base_path, true, &cache);
  expect(confirmed.imported() &&
             confirmed.suite.source
                 .confirmed_labpbr13_without_properties &&
             !confirmed.suite.material.format_declared,
         "explicit confirmation imports missing-properties suite");

  fs::remove(normal_path, filesystem_error);
  expect(!filesystem_error, "remove optional normal fixture");
  const auto without_normal =
      xpbd::gfx::importLabPbrSuite(base_path, true, nullptr);
  expect(without_normal.imported() &&
             !without_normal.suite.material.normal_map_active,
         "optional _n may be absent");
  expect(writeBytes(normal_path, normal_png),
         "add optional normal after import");
  const auto optional_appeared =
      xpbd::gfx::checkLabPbrSuiteSourceChanges(
          without_normal.suite.source);
  expect(optional_appeared.availability_changed &&
             optional_appeared.reloadRecommended(),
         "new optional sidecar is reported as a source change");

  const std::string wrong_properties = "format=lab-pbr/1.2\n";
  expect(writeBytes(properties_path,
                    std::vector<std::uint8_t>(wrong_properties.begin(),
                                              wrong_properties.end())),
         "write unsupported properties fixture");
  const auto unsupported =
      xpbd::gfx::importLabPbrSuite(base_path, true, nullptr);
  expect(unsupported.status == LabPbrSuiteImportStatus::Failed &&
             unsupported.error.find("unsupported") != std::string::npos,
         "unsupported properties format is rejected");

  expect(writeBytes(properties_path, properties_bytes),
         "restore valid properties fixture");
  const auto one_pixel_specular =
      encode(1, 1, {0u, 0u, 0u, 0u});
  expect(writeBytes(specular_path, one_pixel_specular),
         "write mismatched specular fixture");
  const auto mismatched =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(mismatched.status == LabPbrSuiteImportStatus::Failed &&
             mismatched.error.find("dimensions") != std::string::npos,
         "mismatched _s dimensions are rejected");

  expect(writeBytes(specular_path, {0u, 1u, 2u, 3u}),
         "write corrupt specular fixture");
  const auto corrupt =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(corrupt.status == LabPbrSuiteImportStatus::Failed &&
             corrupt.error.find("Specular Sidecar") != std::string::npos &&
             corrupt.error.find("Header stage") != std::string::npos,
         "corrupt _s is rejected");

  const auto wrong_selection =
      xpbd::gfx::importLabPbrSuite(specular_path, false, nullptr);
  expect(wrong_selection.status == LabPbrSuiteImportStatus::Failed &&
             wrong_selection.error.find("base") != std::string::npos,
         "selecting _s as base is rejected without stem guessing");

  fs::remove_all(directory, filesystem_error);
}

xpbd::gfx::StaticIndexedModelMesh makeOverlappingLabPbrMesh() {
  xpbd::gfx::StaticIndexedModelMesh mesh;
  mesh.uv_domain.width = 2.0;
  mesh.uv_domain.height = 2.0;
  mesh.uv_domain.imported_width = 2;
  mesh.uv_domain.imported_height = 2;
  mesh.bone_names = {"group_a", "group_b", "untextured"};
  const auto add_quad = [&mesh](std::uint32_t bone_index, bool mirrored,
                                bool textured) {
    const std::uint32_t first_vertex =
        static_cast<std::uint32_t>(mesh.vertices.size());
    const std::uint32_t first_index =
        static_cast<std::uint32_t>(mesh.indices.size());
    mesh.vertices.resize(mesh.vertices.size() + 4u);
    const std::array<std::array<float, 2>, 4> regular{{
        {0.0f, 0.0f},
        {1.0f, 0.0f},
        {1.0f, 1.0f},
        {0.0f, 1.0f},
    }};
    const std::array<std::array<float, 2>, 4> flipped{{
        {1.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 1.0f},
        {1.0f, 1.0f},
    }};
    const auto &uvs = mirrored ? flipped : regular;
    for (std::size_t i = 0; i < uvs.size(); ++i) {
      auto &vertex = mesh.vertices[first_vertex + i];
      vertex.u = uvs[i][0];
      vertex.v = uvs[i][1];
      vertex.raw_u = static_cast<double>(uvs[i][0]) * 2.0;
      vertex.raw_v = static_cast<double>(uvs[i][1]) * 2.0;
      vertex.bone_index = bone_index;
    }
    mesh.indices.insert(mesh.indices.end(),
                        {first_vertex, first_vertex + 1u,
                         first_vertex + 2u, first_vertex,
                         first_vertex + 2u, first_vertex + 3u});
    xpbd::gfx::StaticModelFace face;
    face.first_vertex = first_vertex;
    face.vertex_count = 4u;
    face.first_index = first_index;
    face.index_count = 6u;
    face.bone_index = bone_index;
    face.textured = textured;
    mesh.faces.push_back(face);
  };
  add_quad(0u, false, true);
  add_quad(1u, true, true);
  add_quad(2u, false, false);
  return mesh;
}

std::set<std::uint32_t> expandCoverageRuns(
    const xpbd::gfx::LabPbrUvCoverage &coverage,
    std::string_view group_name) {
  std::set<std::uint32_t> texels;
  const auto *runs = coverage.find(group_name);
  if (runs == nullptr || coverage.width <= 0) {
    return texels;
  }
  for (const xpbd::gfx::UvRun &run : *runs) {
    for (std::uint32_t x = run.x0;; ++x) {
      texels.insert(run.y * static_cast<std::uint32_t>(coverage.width) + x);
      if (x == run.x1) {
        break;
      }
    }
  }
  return texels;
}

bool coverageContains(const xpbd::gfx::LabPbrUvCoverage &coverage,
                      std::string_view group_name,
                      std::uint32_t texel) {
  const auto expanded = expandCoverageRuns(coverage, group_name);
  return expanded.contains(texel);
}

void testLabPbrAuthoringEncodingAndCoverage() {
  using xpbd::gfx::GroupLabPbrOverride;
  using xpbd::gfx::encodeLabPbrEmission;
  using xpbd::gfx::encodeLabPbrPorosity;
  using xpbd::gfx::encodeLabPbrRoughness;
  using xpbd::gfx::encodeLabPbrSubsurface;
  using xpbd::gfx::validGroupLabPbrOverride;
  std::string validation_error;

  const auto authoring_source =
      readTestSource("src/gfx/labpbr_authoring.cpp");
  expect(authoring_source.find("std::vector<std::uint8_t> marked") !=
                 std::string::npos &&
             authoring_source.find("std::vector<std::uint32_t> touched") !=
                 std::string::npos &&
             authoring_source.find("std::sort(touched.begin()") !=
                 std::string::npos &&
             authoring_source.find("std::set<std::uint32_t>") ==
                 std::string::npos &&
             authoring_source.find("std::unordered_set<std::uint32_t>") ==
                 std::string::npos,
         "production Coverage uses marked touched sort merge without texel Set nodes");
  const auto texture_header =
      readTestSource("include/xpbd/gfx/texture_image.hpp");
  const auto import_header =
      readTestSource("include/xpbd/gfx/labpbr_import.hpp");
  const auto import_source =
      readTestSource("src/gfx/labpbr_import.cpp");
  const auto app_session_source =
      readTestSource("src/app/app_session.cpp");
  const auto early_cache_find =
      import_source.find("cache->find(source.cache_key");
  const auto first_pixel_decode =
      import_source.find("TextureImage base;");
  expect(texture_header.find(
             "using SharedTextureImage = std::shared_ptr<const TextureImage>") !=
                 std::string::npos &&
             authoring_source.find(
                 "imported.original_file_bytes = snapshot.bytes;") !=
                 std::string::npos &&
             app_session_source.find(
                 "imported.suite.source.normal.original_bytes;") !=
                 std::string::npos &&
             app_session_source.find(
                 "metadata.base.original_bytes.reset();") !=
                 std::string::npos &&
             import_header.find(
                 "kLabPbrDefaultImportCacheBudgetBytes") !=
                 std::string::npos &&
             import_source.find(
                 "entries_.splice(entries_.begin(), entries_,") !=
                 std::string::npos &&
             early_cache_find != std::string::npos &&
             first_pixel_decode != std::string::npos &&
             early_cache_find < first_pixel_decode &&
             app_session_source.find(
                 "base_path, confirm_missing_properties, "
                 "&labpbr_import_cache_,") !=
                 std::string::npos,
         "shared-image Iris snapshot and byte-bounded LRU source contracts are explicit");

  expect(encodeLabPbrEmission(0.0f) == 0u,
         "LabPBR emission zero encodes to zero");
  expect(encodeLabPbrEmission(0.5f) == 127u,
         "LabPBR emission midpoint rounds against 254");
  expect(encodeLabPbrEmission(1.0f) == 254u,
         "LabPBR emission maximum never encodes reserved 255");
  expect(encodeLabPbrEmission(2.0f) == 254u,
         "LabPBR emission clamps above one");
  expect(encodeLabPbrRoughness(0.0f) == 255u,
         "zero roughness encodes full smoothness");
  expect(encodeLabPbrRoughness(0.5f) == 128u,
         "roughness midpoint uses inverse rounded smoothness");
  expect(encodeLabPbrRoughness(1.0f) == 0u,
         "full roughness encodes zero smoothness");
  expect(encodeLabPbrRoughness(
             (std::numeric_limits<float>::quiet_NaN)()) == 0u,
         "non-finite roughness uses safe fully rough fallback");
  expect(encodeLabPbrPorosity(0.5f) == 32u,
         "porosity midpoint encodes against 64");
  expect(encodeLabPbrPorosity(1.0f) == 64u,
         "porosity maximum remains below SSS range");
  expect(encodeLabPbrSubsurface(0.0f) == 65u,
         "subsurface starts at the LabPBR SSS range");
  expect(encodeLabPbrSubsurface(0.5f) == 160u,
         "subsurface midpoint encodes against 65..255");
  expect(encodeLabPbrSubsurface(1.0f) == 255u,
         "subsurface maximum reaches 255");

  GroupLabPbrOverride subsurface;
  subsurface.group_name = "sss";
  subsurface.porosity_enabled = true;
  subsurface.subsurface_scattering = true;
  subsurface.subsurface = 0.5f;
  expect(validGroupLabPbrOverride(subsurface, &validation_error),
         "subsurface override validates as a unit interval");
  const auto sss_composition = xpbd::gfx::composeLabPbrSpecular(
      1, 1, xpbd::gfx::SharedTextureImage{},
      xpbd::gfx::LabPbrUvCoverage{1, 1, {{"sss", {{0u, 0u, 0u}}}}},
      {{"sss", subsurface}});
  expect(sss_composition.exportable() &&
             sss_composition.specular != nullptr &&
             sss_composition.specular->rgba[2] == 160u,
         "subsurface override writes the LabPBR SSS B range");

  GroupLabPbrOverride semantic;
  semantic.group_name = "group_a";
  semantic.metal_enabled = true;
  semantic.metal = false;
  semantic.dielectric_f0 = 229u;
  expect(validGroupLabPbrOverride(semantic, &validation_error),
         "dielectric F0 229 is valid");
  semantic.dielectric_f0 = 230u;
  expect(!validGroupLabPbrOverride(semantic, &validation_error),
         "dielectric F0 230 is rejected");
  semantic.metal = true;
  semantic.metal_code = 229u;
  expect(!validGroupLabPbrOverride(semantic, &validation_error),
         "metal code below 230 is rejected");
  semantic.metal_code = 255u;
  expect(validGroupLabPbrOverride(semantic, &validation_error),
         "custom metal code 255 is valid");

  const auto mesh = makeOverlappingLabPbrMesh();
  const auto coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(mesh, 2, 2);
  expect(coverage.valid(), "LabPBR UV coverage dimensions are valid");
  const auto *group_a = coverage.find("group_a");
  const auto *group_b = coverage.find("group_b");
  const std::set<std::uint32_t> reference_texels{0u, 1u, 2u, 3u};
  expect(group_a != nullptr && coverage.texelCount("group_a") == 4u &&
             expandCoverageRuns(coverage, "group_a") == reference_texels,
         "selected group rasterizes all covered atlas texels");
  expect(group_b != nullptr && coverage.texelCount("group_b") == 4u &&
             expandCoverageRuns(coverage, "group_b") == reference_texels,
         "mirrored group coverage deduplicates triangle texels");
  expect(coverage.find("untextured") == nullptr,
         "untextured faces do not enter LabPBR coverage");
  xpbd::gfx::StaticIndexedModelMesh empty_mesh;
  empty_mesh.uv_domain.width = 2.0;
  empty_mesh.uv_domain.height = 2.0;
  empty_mesh.uv_domain.imported_width = 2;
  empty_mesh.uv_domain.imported_height = 2;
  const auto empty_coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(empty_mesh, 2, 2);
  expect(empty_coverage.valid() && empty_coverage.group_runs.empty(),
         "empty model produces valid empty LabPBR coverage");

  auto stress_mesh = makeOverlappingLabPbrMesh();
  stress_mesh.faces.resize(1u);
  const auto stress_side =
      static_cast<std::uint32_t>(labPbrStressSide());
  stress_mesh.uv_domain.width = static_cast<double>(stress_side);
  stress_mesh.uv_domain.height = static_cast<double>(stress_side);
  stress_mesh.uv_domain.imported_width =
      static_cast<int>(stress_side);
  stress_mesh.uv_domain.imported_height =
      static_cast<int>(stress_side);
  for (std::size_t i = 0; i < 4u; ++i) {
    stress_mesh.vertices[i].raw_u =
        static_cast<double>(stress_mesh.vertices[i].u) * stress_side;
    stress_mesh.vertices[i].raw_v =
        static_cast<double>(stress_mesh.vertices[i].v) * stress_side;
  }
  const auto stress_coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(
          stress_mesh, static_cast<int>(stress_side),
          static_cast<int>(stress_side));
  const auto *stress_runs = stress_coverage.find("group_a");
  bool exact_stress_runs = stress_runs != nullptr &&
                           stress_runs->size() == stress_side;
  if (exact_stress_runs) {
    for (std::uint32_t y = 0; y < stress_side; ++y) {
      const auto &run = (*stress_runs)[y];
      exact_stress_runs &=
          run.y == y && run.x0 == 0u && run.x1 == stress_side - 1u;
    }
  }
  expect(stress_coverage.valid() && exact_stress_runs &&
             stress_coverage.texelCount("group_a") ==
                 static_cast<std::uint64_t>(stress_side) * stress_side,
         "runtime full-atlas Coverage compacts to one run per row");
  std::printf("labpbr-coverage-stress: side=%u runs=%zu texels=%llu\n",
              stress_side, stress_runs != nullptr ? stress_runs->size() : 0u,
              static_cast<unsigned long long>(
                  stress_coverage.texelCount("group_a")));

  std::uint64_t eight_k_coverage_peak = 0u;
  xpbd::gfx::LabPbrMemoryEstimateRequest eight_k_request;
  eight_k_request.width = 8192u;
  eight_k_request.height = 8192u;
  xpbd::gfx::LabPbrMemoryEstimate preserved_estimate{11u, 22u, 33u, 44u};
  auto rejected_estimate = preserved_estimate;
  expect(xpbd::gfx::estimateLabPbrUvRunCoveragePeakBytes(
             8192u, 8192u, eight_k_coverage_peak, &validation_error) &&
             eight_k_coverage_peak == 8192u * 8192u * 17u,
         "8K run Coverage peak uses checked arithmetic without allocation");
  eight_k_request.coverage_peak_bytes = eight_k_coverage_peak;
  expect(!xpbd::gfx::preflightLabPbrMemory(
             eight_k_request, xpbd::gfx::kLabPbrDefaultPeakBudgetBytes,
             rejected_estimate, &validation_error) &&
             rejected_estimate == preserved_estimate,
         "8K run Coverage rejects before allocation and preserves estimate output");

  const auto lazy_default = xpbd::gfx::composeLabPbrSpecular(
      2, 2, xpbd::gfx::SharedTextureImage{}, {}, {});
  expect(lazy_default.exportable() &&
             lazy_default.specular_materialization_deferred &&
             lazy_default.deferred_width == 2 &&
             lazy_default.deferred_height == 2 &&
             lazy_default.specular == nullptr,
         "no Override keeps default Specular and Coverage nonresident");
  xpbd::gfx::TextureImage materialized_default;
  expect(xpbd::gfx::materializeLabPbrSpecular(
             2, 2, nullptr, materialized_default, &validation_error) &&
             materialized_default.valid() &&
             materialized_default.rgba[0] == 0u &&
             materialized_default.rgba[1] == 10u,
         "default Specular materializes transactionally only on demand");
  const auto preserved_materialized = materialized_default;
  xpbd::gfx::TextureImage mismatched_source;
  mismatched_source.width = 1;
  mismatched_source.height = 1;
  mismatched_source.source_channels = 4;
  mismatched_source.rgba = {0u, 10u, 0u, 0u};
  expect(!xpbd::gfx::materializeLabPbrSpecular(
             2, 2, &mismatched_source, materialized_default,
             &validation_error) &&
             materialized_default.width == preserved_materialized.width &&
             materialized_default.height == preserved_materialized.height &&
             materialized_default.source_channels ==
                 preserved_materialized.source_channels &&
             materialized_default.rgba == preserved_materialized.rgba,
         "Composition materialization failure preserves caller output");
  GroupLabPbrOverride missing_group;
  missing_group.group_name = "missing";
  missing_group.emission_enabled = true;
  missing_group.emission = 1.0f;
  const auto empty_composition = xpbd::gfx::composeLabPbrSpecular(
      2, 2, xpbd::gfx::SharedTextureImage{}, empty_coverage,
      {{"missing", missing_group}});
  expect(empty_composition.exportable() &&
             empty_composition.warnings.size() == 1u,
         "selected group without textured UVs is a safe warned no-op");

  xpbd::gfx::TextureImage base;
  base.width = 1;
  base.height = 1;
  base.source_channels = 4;
  base.rgba = {255u, 255u, 255u, 255u};
  xpbd::gfx::ResolvedMaterialTable fallback_source;
  fallback_source.width = 1;
  fallback_source.height = 1;
  xpbd::gfx::ResolvedMaterialTable authored;
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             base, fallback_source, nullptr, nullptr, authored,
             &validation_error),
         "rebuild resolved material without authored sidecars");
  expect(!authored.specular_map_active &&
             xpbd::gfx::labPbrFeatureFlags(&authored) == 0u,
         "no override preserves exact missing-specular feature state");
  expectNear(authored.sample(0.5f, 0.5f).dielectric_f0, 0.04f, 1.0e-6f,
              "no override preserves exact dielectric fallback");
  xpbd::gfx::TextureImage authored_specular;
  authored_specular.width = 1;
  authored_specular.height = 1;
  authored_specular.source_channels = 4;
  authored_specular.rgba = {255u, 230u, 64u, 254u};
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             base, fallback_source, nullptr, &authored_specular, authored,
             &validation_error),
         "rebuild resolved material with authored specular");
  const auto authored_sample = authored.sample(0.5f, 0.5f);
  expect(authored.specular_map_active &&
             authored_sample.metal_kind ==
                 xpbd::gfx::LabPbrMetalKind::Predefined &&
             authored_sample.emission_strength > 0.99f,
         "applied authored specular reaches resolved preview semantics");
  xpbd::gfx::TextureImage rgb_normal;
  rgb_normal.width = 1;
  rgb_normal.height = 1;
  rgb_normal.source_channels = 3;
  rgb_normal.rgba = {128u, 128u, 255u, 255u};
  expect(xpbd::gfx::buildAuthoredResolvedMaterial(
             base, fallback_source, &rgb_normal, nullptr, authored,
             &validation_error),
         "RGB LabPBR normal sidecar remains importable");
  expect(authored.normal_map_active && authored.normal_image.source_channels == 3,
         "RGB LabPBR normal sidecar reaches the resolved material");
}

void testLabPbrCompositionAndConflicts() {
  using xpbd::gfx::GroupLabPbrOverride;
  using xpbd::gfx::LabPbrOverrideChannel;

  const auto coverage =
      xpbd::gfx::rasterizeLabPbrUvCoverage(
          makeOverlappingLabPbrMesh(), 2, 2);
  xpbd::gfx::TextureImage imported;
  imported.width = 2;
  imported.height = 2;
  imported.source_channels = 4;
  imported.rgba = {
      1u, 2u, 3u, 4u,       5u, 6u, 7u, 8u,
      9u, 10u, 11u, 12u,    13u, 14u, 15u, 16u,
  };
  const auto imported_asset =
      std::make_shared<const xpbd::gfx::TextureImage>(imported);

  const auto lazy_source = xpbd::gfx::composeLabPbrSpecular(
      2, 2, imported_asset, {}, {});
  xpbd::gfx::TextureImage materialized_source;
  std::string materialize_error;
  expect(lazy_source.exportable() &&
             !lazy_source.specular_materialization_deferred &&
             lazy_source.specular == imported_asset &&
             xpbd::gfx::materializeLabPbrSpecular(
                 2, 2, &imported, materialized_source,
                 &materialize_error) &&
             materialized_source.rgba == imported.rgba,
         "no Override shares Source Specular until explicit materialization");

  GroupLabPbrOverride group_a;
  group_a.group_name = "group_a";
  group_a.roughness_enabled = true;
  group_a.roughness = 0.25f;
  std::map<std::string, GroupLabPbrOverride> overrides{
      {"group_a", group_a}};
  auto composed = xpbd::gfx::composeLabPbrSpecular(
      2, 2, imported_asset, coverage, overrides);
  expect(composed.exportable() && composed.specular != nullptr &&
             composed.specular != imported_asset,
         "single selected-group override composes without conflict");
  expect(composed.specular->rgba[0] == 191u &&
             composed.specular->rgba[4] == 191u &&
             composed.specular->rgba[8] == 191u &&
             composed.specular->rgba[12] == 191u,
         "roughness override changes only covered R texels");
  expect(composed.specular->rgba[1] == imported.rgba[1] &&
             composed.specular->rgba[2] == imported.rgba[2] &&
             composed.specular->rgba[3] == imported.rgba[3],
         "disabled channels preserve imported texture bytes");

  group_a = {};
  group_a.group_name = "group_a";
  group_a.emission_enabled = true;
  group_a.emission = 0.5f;
  GroupLabPbrOverride group_b = group_a;
  group_b.group_name = "group_b";
  overrides = {{"group_a", group_a}, {"group_b", group_b}};
  composed = xpbd::gfx::composeLabPbrSpecular(
      2, 2, imported_asset, coverage, overrides);
  expect(composed.exportable() && composed.conflicts.empty(),
         "identical overlapping group values are exportable");

  group_b.emission = 1.0f;
  overrides["group_b"] = group_b;
  composed = xpbd::gfx::composeLabPbrSpecular(
      2, 2, imported_asset, coverage, overrides);
  expect(!composed.exportable() && composed.conflicts.size() == 4u,
         "different overlapping group values block all conflict texels");
  if (!composed.conflicts.empty()) {
    const auto &conflict = composed.conflicts.front();
    expect(conflict.channel == LabPbrOverrideChannel::Emission,
           "conflict identifies the LabPBR channel");
    expect(conflict.groups.size() == 2u &&
               conflict.groups[0] == "group_a" &&
               conflict.groups[1] == "group_b",
           "conflict reports both groups deterministically");
    expect(conflict.encoded_values.size() == 2u &&
               conflict.encoded_values[0] == 127u &&
               conflict.encoded_values[1] == 254u,
           "conflict reports both encoded channel values");
  }
}

void testLabPbrPngChecksumAndNormalImport() {
  namespace fs = std::filesystem;
  const std::array<std::uint8_t, 3> abc{'a', 'b', 'c'};
  expect(xpbd::gfx::sha256Hex(abc) ==
             "ba7816bf8f01cfea414140de5dae2223"
             "b00361a396177a9cb410ff61f20015ad",
         "SHA-256 matches the abc known vector");

  const std::vector<std::uint8_t> rgba{
      255u, 128u, 64u, 32u, 0u, 1u, 2u, 3u};
  std::vector<std::uint8_t> png;
  std::string error;
  expect(xpbd::gfx::encodePngRgba8(2, 1, rgba, png, &error),
         "encode deterministic RGBA8 PNG");
  xpbd::gfx::TextureImage decoded;
  expect(xpbd::gfx::loadTextureImageFromMemory(
             png.data(), static_cast<int>(png.size()), decoded, &error),
         "decode authored RGBA8 PNG");
  expect(decoded.width == 2 && decoded.height == 1 &&
             decoded.source_channels == 4 && decoded.rgba == rgba,
         "authored PNG round-trips dimensions, channels, and bytes");

  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_authoring_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated LabPBR authoring regression directory");
  if (filesystem_error) {
    return;
  }
  const auto write_bytes = [](const fs::path &path,
                              const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
  };

  const fs::path normal_path = directory / "atlas_n.png";
  expect(write_bytes(normal_path, png), "write authored RGBA normal asset");
  xpbd::gfx::ReadOnlyIrisNormalAsset normal;
  expect(xpbd::gfx::importReadOnlyIrisNormal(
             normal_path, 2, 1, normal, &error),
         "import matching RGBA Iris normal asset");
  expect(normal.valid() && normal.original_file_bytes != nullptr &&
             *normal.original_file_bytes == png &&
             normal.sha256 == xpbd::gfx::sha256Hex(png),
         "Iris normal import preserves exact bytes and checksum");

  const auto preserved_bytes = normal.original_file_bytes;
  const auto preserved_hash = normal.sha256;
  const auto preserved_decoded = normal.decoded;
  xpbd::gfx::TextureDecodeLimits iris_shared_budget;
  iris_shared_budget.maximum_peak_bytes =
      normal.original_file_bytes->capacity() +
      normal.decoded->rgba.capacity() + png.size() +
      normal.decoded->rgba.size() * 2u;
  expect(xpbd::gfx::importReadOnlyIrisNormal(
             normal_path, 2, 1, normal, &error, iris_shared_budget) &&
             normal.original_file_bytes != nullptr &&
             *normal.original_file_bytes == png &&
             normal.decoded != nullptr && normal.decoded->rgba == rgba,
         "Iris sharing fits the peak that previously required an encoded-byte copy");
  const auto committed_bytes = normal.original_file_bytes;
  const auto committed_decoded = normal.decoded;

  const std::vector<std::uint8_t> rgb_png(
      std::begin(kWhitePng), std::end(kWhitePng));
  expect(write_bytes(normal_path, rgb_png),
         "replace temporary source with RGB PNG");
  expect(!xpbd::gfx::importReadOnlyIrisNormal(
             normal_path, 2, 1, normal, &error),
         "RGB Iris normal import is rejected");
  expect(normal.original_file_bytes == committed_bytes &&
             normal.decoded == committed_decoded &&
             normal.sha256 == preserved_hash &&
             preserved_bytes != nullptr && preserved_decoded != nullptr,
         "failed normal replacement preserves active shared Iris assets");

  fs::remove_all(directory, filesystem_error);
}

void testLabPbrBundleExport() {
  namespace fs = std::filesystem;
  const auto nonce =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const fs::path directory =
      fs::temp_directory_path() /
      ("xpbd_labpbr_export_" + std::to_string(nonce));
  std::error_code filesystem_error;
  fs::create_directories(directory, filesystem_error);
  expect(!filesystem_error,
         "create isolated LabPBR export regression directory");
  if (filesystem_error) {
    return;
  }
  const auto read_bytes = [](const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
  };
  const auto write_bytes = [](const fs::path &path,
                              const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
  };

  xpbd::gfx::LabPbrCompositionResult composition;
  xpbd::gfx::TextureImage composition_image;
  composition_image.width = 2;
  composition_image.height = 1;
  composition_image.source_channels = 4;
  composition_image.rgba = {
      255u, 229u, 64u, 254u, 0u, 255u, 0u, 0u};
  composition.specular =
      std::make_shared<const xpbd::gfx::TextureImage>(
          std::move(composition_image));
  std::vector<std::uint8_t> base_png;
  const std::vector<std::uint8_t> base_rgba{
      240u, 220u, 200u, 255u, 120u, 140u, 160u, 255u};
  std::vector<std::uint8_t> normal_png;
  const std::vector<std::uint8_t> normal_rgba{
      128u, 128u, 255u, 255u, 255u, 0u, 64u, 32u};
  std::string error;
  expect(xpbd::gfx::encodePngRgba8(
             2, 1, base_rgba, base_png, &error),
         "encode bundle base PNG fixture");
  const fs::path base_path = directory / "skin.png";
  expect(write_bytes(base_path, base_png),
         "write bundle base PNG fixture");
  expect(xpbd::gfx::encodePngRgba8(
             2, 1, normal_rgba, normal_png, &error),
         "encode bundle normal PNG fixture");
  const fs::path normal_source = directory / "source_n.png";
  expect(write_bytes(normal_source, normal_png),
         "write bundle normal PNG fixture");
  xpbd::gfx::ReadOnlyIrisNormalAsset normal;
  expect(xpbd::gfx::importReadOnlyIrisNormal(
             normal_source, 2, 1, normal, &error),
         "import bundle normal PNG fixture");

  auto exported = xpbd::gfx::exportLabPbrBundle(
      directory / "skin.png", composition, &normal, false);
  expect(exported.success && !exported.overwrite_required,
         "new LabPBR bundle exports transactionally");
  expect(exported.specular_path.filename() == "skin_s.png" &&
             exported.normal_path.filename() == "skin_n.png" &&
             exported.properties_path.filename() == "texture.properties",
         "LabPBR bundle uses standard output names");
  xpbd::gfx::TextureImage exported_specular;
  expect(xpbd::gfx::loadTextureImage(
             exported.specular_path, exported_specular, &error) &&
             exported_specular.source_channels == 4 &&
             exported_specular.rgba == composition.specular->rgba,
         "exported specular PNG round-trips exact RGBA bytes");
  expect(read_bytes(exported.normal_path) == normal_png,
         "exported Iris normal preserves exact original file bytes");
  const auto properties = read_bytes(exported.properties_path);
  expect(std::string(properties.begin(), properties.end()) ==
             "format=lab-pbr/1.3\n",
         "exported texture.properties declares LabPBR 1.3");

  const auto imported_before_edit =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(imported_before_edit.imported() &&
             imported_before_edit.suite.material.specular_image.rgba ==
                 composition.specular->rgba &&
             imported_before_edit.suite.source.normal.original_bytes &&
             *imported_before_edit.suite.source.normal.original_bytes ==
                 normal_png,
         "exported complete LabPBR suite imports before editing");

  const auto original_specular = exported_specular.rgba;
  auto edited_composition_image = *composition.specular;
  edited_composition_image.rgba[0] = 7u;
  composition.specular =
      std::make_shared<const xpbd::gfx::TextureImage>(
          std::move(edited_composition_image));
  auto overwrite = xpbd::gfx::exportLabPbrBundle(
      directory / "skin_s.png", composition, &normal, false);
  expect(!overwrite.success && overwrite.overwrite_required &&
             overwrite.existing_paths.size() == 3u,
         "existing LabPBR bundle requires explicit overwrite approval");
  expect(xpbd::gfx::loadTextureImage(
             exported.specular_path, exported_specular, &error) &&
             exported_specular.rgba == original_specular,
         "declined overwrite leaves existing bundle unchanged");

  overwrite = xpbd::gfx::exportLabPbrBundle(
      directory / "skin_s.png", composition, &normal, true);
  expect(overwrite.success,
         "approved LabPBR bundle overwrite completes");
  expect(xpbd::gfx::loadTextureImage(
             overwrite.specular_path, exported_specular, &error) &&
             exported_specular.rgba == composition.specular->rgba,
         "approved overwrite installs newly validated specular bytes");
  const auto reimported_after_edit =
      xpbd::gfx::importLabPbrSuite(base_path, false, nullptr);
  expect(reimported_after_edit.imported() &&
             reimported_after_edit.suite.material.specular_image.rgba ==
                 composition.specular->rgba &&
             reimported_after_edit.suite.source.normal.original_bytes &&
             *reimported_after_edit.suite.source.normal.original_bytes ==
                 normal_png &&
             reimported_after_edit.suite.material.format_declared,
         "import-edit-export-reimport round-trip preserves authored RGBA, exact _n bytes, and properties");

  xpbd::gfx::LabPbrCompositionResult deferred;
  deferred.specular_materialization_deferred = true;
  deferred.deferred_width = 2;
  deferred.deferred_height = 1;
  const auto deferred_prompt = xpbd::gfx::exportLabPbrBundle(
      directory / "skin_s.png", deferred, nullptr, false,
      composition.specular.get());
  expect(deferred_prompt.overwrite_required &&
             deferred.specular == nullptr,
         "deferred overwrite prompt does not materialize Source Specular");

  const auto deferred_source_export = xpbd::gfx::exportLabPbrBundle(
      directory / "lazy_source.png", deferred, nullptr, true,
      composition.specular.get());
  xpbd::gfx::TextureImage lazy_source_image;
  expect(deferred_source_export.success &&
             deferred.specular == nullptr &&
             xpbd::gfx::loadTextureImage(
                 deferred_source_export.specular_path, lazy_source_image,
                 &error) &&
             lazy_source_image.rgba == composition.specular->rgba,
         "actual deferred export materializes exact Source Specular locally");

  const auto deferred_default_export = xpbd::gfx::exportLabPbrBundle(
      directory / "lazy_default.png", deferred, nullptr, true, nullptr);
  xpbd::gfx::TextureImage lazy_default_image;
  expect(deferred_default_export.success &&
             deferred.specular == nullptr &&
             xpbd::gfx::loadTextureImage(
                 deferred_default_export.specular_path, lazy_default_image,
                 &error) &&
             lazy_default_image.rgba ==
                 std::vector<std::uint8_t>{0u, 10u, 0u, 0u,
                                           0u, 10u, 0u, 0u},
         "missing Source Specular materializes the default map only for export");

  xpbd::gfx::LabPbrCompositionResult conflicting = composition;
  conflicting.conflicts.push_back({});
  const auto blocked = xpbd::gfx::exportLabPbrBundle(
      directory / "blocked.png", conflicting, nullptr, true);
  expect(!blocked.success &&
             !fs::exists(directory / "blocked_s.png"),
         "UV conflicts block export before any target is created");

  fs::remove_all(directory, filesystem_error);
}

void testTangentFrames() {
  const std::array<float, 3> p0{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> p1{1.0f, 0.0f, 0.0f};
  const std::array<float, 3> p2{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
  const std::array<float, 2> uv0{0.0f, 0.0f};
  const std::array<float, 2> uv1{1.0f, 0.0f};
  const std::array<float, 2> uv2{0.0f, 1.0f};

  auto regular =
      xpbd::gfx::computeTangentFrame(p0, p1, p2, normal, uv0, uv1, uv2);
  expectNear(regular.tangent[0], 1.0f, 1.0e-6f,
             "regular UV tangent follows +X");
  expectNear(regular.handedness, 1.0f, 1.0e-6f,
             "regular UV has positive handedness");
  expect(!regular.used_fallback, "regular UV does not use tangent fallback");

  auto mirrored =
      xpbd::gfx::computeTangentFrame(p0, p1, p2, normal, uv0, uv2, uv1);
  expectNear(mirrored.handedness, -1.0f, 1.0e-6f,
             "mirrored UV flips tangent handedness");

  auto degenerate =
      xpbd::gfx::computeTangentFrame(p0, p1, p2, normal, uv0, uv0, uv0);
  expect(degenerate.used_fallback,
         "degenerate UV uses deterministic tangent fallback");
  expectNear(degenerate.tangent[0], 1.0f, 1.0e-6f,
             "degenerate +Z normal falls back to +X tangent");
}

void testRtNormalTransformAndUpdatePolicy() {
  using xpbd::gfx::RtBlasPolicy;
  using xpbd::gfx::RtGeometryUpdateKind;
  using xpbd::gfx::classifyRtGeometryUpdate;
  using xpbd::gfx::transformRtNormalInverseTranspose;

  const std::array<float, 16> non_uniform{
      2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const float inv_sqrt2 = 1.0f / std::sqrt(2.0f);
  const auto transformed = transformRtNormalInverseTranspose(
      non_uniform, {inv_sqrt2, inv_sqrt2, 0.0f});
  expect(!transformed.used_fallback,
         "non-uniform normal uses inverse-transpose path");
  expectNear(transformed.value[0], 0.4472136f, 1.0e-5f,
             "non-uniform normal inverse-transpose X");
  expectNear(transformed.value[1], 0.8944272f, 1.0e-5f,
             "non-uniform normal inverse-transpose Y");
  const std::array<float, 3> transformed_tangent{
      2.0f * inv_sqrt2, -inv_sqrt2, 0.0f};
  const float normal_tangent_dot =
      transformed.value[0] * transformed_tangent[0] +
      transformed.value[1] * transformed_tangent[1] +
      transformed.value[2] * transformed_tangent[2];
  expectNear(normal_tangent_dot, 0.0f, 1.0e-5f,
             "inverse-transpose normal stays orthogonal to transformed tangent");

  const std::array<float, 16> mirrored{
      -2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const auto mirrored_normal =
      transformRtNormalInverseTranspose(mirrored, {1.0f, 0.0f, 0.0f});
  expectNear(mirrored_normal.value[0], -1.0f, 1.0e-6f,
             "mirrored transform preserves inverse determinant sign");

  const std::array<float, 16> singular{
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  const auto singular_normal =
      transformRtNormalInverseTranspose(singular, {1.0f, 0.0f, 0.0f});
  expect(singular_normal.used_fallback,
         "singular normal transform reports deterministic fallback");
  expectNear(singular_normal.value[0], 1.0f, 1.0e-6f,
             "singular normal transform remains finite and normalized");

  expect(classifyRtGeometryUpdate(7u, 7u, RtBlasPolicy::RigidLocalSpace,
                                  true) == RtGeometryUpdateKind::None,
         "stable rigid-local content requires no BLAS work");
  expect(classifyRtGeometryUpdate(7u, 8u, RtBlasPolicy::RigidLocalSpace,
                                  true) == RtGeometryUpdateKind::FullBuild,
         "changed rigid-local content fails safe to full BLAS build");
  expect(classifyRtGeometryUpdate(7u, 8u, RtBlasPolicy::DynamicRefit,
                                  false) == RtGeometryUpdateKind::FullBuild,
         "first dynamic geometry update requires full BLAS build");
  expect(classifyRtGeometryUpdate(7u, 8u, RtBlasPolicy::DynamicRefit,
                                  true) == RtGeometryUpdateKind::Refit,
         "built dynamic geometry position change selects BLAS refit");
}

void testRtSceneGenerationContract() {
  using xpbd::gfx::RtSceneGenerations;
  using xpbd::gfx::rtSceneGenerationKey;

  const RtSceneGenerations stable{7u, 11u, 13u, 17u, 19u, 23u};
  const RtSceneGenerations same = stable;
  expect(stable == same, "RT generation records compare by domain");
  expect(rtSceneGenerationKey(stable) == rtSceneGenerationKey(same),
         "equal RT generations produce a stable scene key");

  for (std::size_t index = 0; index < 6u; ++index) {
    RtSceneGenerations changed = stable;
    switch (index) {
    case 0:
      ++changed.topology;
      break;
    case 1:
      ++changed.positions;
      break;
    case 2:
      ++changed.transforms;
      break;
    case 3:
      ++changed.materials;
      break;
    case 4:
      ++changed.emission;
      break;
    default:
      ++changed.visibility;
      break;
    }
    expect(changed != stable &&
               rtSceneGenerationKey(changed) != rtSceneGenerationKey(stable),
           "each RT invalidation domain changes the scene key");
  }
}

void testPathTraceOptionalOutputMaskContract() {
  using xpbd::gfx::kPathTraceAllAovOutputMask;
  using xpbd::gfx::kPathTraceAllOptionalOutputMask;
  using xpbd::gfx::kPathTraceAllRrGuideOutputMask;
  using xpbd::gfx::kPathTraceRrMotionOutputMask;
  using xpbd::gfx::kPathTraceStatisticsOutputMask;

  expect(kPathTraceAllAovOutputMask == 0x03ffu,
         "path-trace AOV layers occupy bits 0 through 9");
  expect(kPathTraceRrMotionOutputMask == 0x0400u,
         "SR/FG motion guide occupies bit 10");
  expect(kPathTraceAllRrGuideOutputMask == 0x7c00u,
         "all five RR guides occupy bits 10 through 14");
  expect(kPathTraceStatisticsOutputMask == 0x8000u,
         "path statistics occupy bit 15");
  expect(kPathTraceAllOptionalOutputMask == 0x7ffffu,
         "optional output ABI includes three standalone temporal transparency masks");
}

void testRtNearestHitReference() {
  using xpbd::gfx::RtAlphaMode;
  using xpbd::gfx::RtHitCandidate;
  using xpbd::gfx::intersectRtTriangleTwoSided;
  using xpbd::gfx::selectRtNearestValidHit;

  expect(xpbd::gfx::rtDebugViewFromName("instance") ==
                 xpbd::gfx::RtDebugView::Instance &&
             std::string(xpbd::gfx::rtDebugViewName(
                 xpbd::gfx::RtDebugView::Normal)) == "normal" &&
             xpbd::gfx::rtDebugViewFromName("unknown") ==
             xpbd::gfx::RtDebugView::Off,
         "RT pipeline debug-view names are stable and unknown-safe");

  xpbd::gfx::RtSbtLayoutRequest sbt_request;
  sbt_request.shader_group_handle_size = 32u;
  sbt_request.shader_group_handle_alignment = 32u;
  sbt_request.shader_group_base_alignment = 64u;
  sbt_request.max_shader_group_stride = 4096u;
  sbt_request.miss_group_count = 2u;
  sbt_request.hit_group_count = 1u;
  sbt_request.buffer_device_address = 0x1003u;
  sbt_request.buffer_bytes = 224u;
  const auto sbt_layout = xpbd::gfx::computeRtSbtLayout(sbt_request);
  expect(sbt_layout && sbt_layout->shader_group_stride == 32u &&
             sbt_layout->base_offset == 61u &&
             sbt_layout->miss_offset == 64u &&
             sbt_layout->hit_offset == 128u &&
             sbt_layout->layout_bytes == 160u,
         "RT SBT layout aligns stride, base, miss, and hit records");
  sbt_request.shader_group_handle_alignment = 24u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects non-power-of-two handle alignment");
  sbt_request.shader_group_handle_alignment = 32u;
  sbt_request.max_shader_group_stride = 16u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects stride above device maximum");
  sbt_request.max_shader_group_stride = 4096u;
  sbt_request.buffer_bytes = 220u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects undersized allocation after base alignment");
  sbt_request.buffer_bytes = 224u;
  sbt_request.buffer_device_address =
      (std::numeric_limits<std::uint64_t>::max)() - 31u;
  expect(!xpbd::gfx::computeRtSbtLayout(sbt_request),
         "RT SBT layout rejects device-address alignment overflow");

  xpbd::gfx::RtDispatchBufferBounds dispatch_bounds{
      24u, 12u, 2u, 24u * 16u, 24u * 16u, 12u * 3u * 4u,
      24u * 8u, 24u * 16u, 12u * 4u, 12u * 16u, 12u * 32u,
      2u * 16u};
  expect(xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch accepts complete vertex/primitive/instance buffers");
  dispatch_bounds.normal_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized normal buffer");
  dispatch_bounds.normal_bytes += 1u;
  dispatch_bounds.tangent_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized tangent buffer");
  dispatch_bounds.tangent_bytes += 1u;
  dispatch_bounds.index_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized index buffer");
  dispatch_bounds.index_bytes += 1u;
  dispatch_bounds.primitive_metadata_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized primitive identity buffer");
  dispatch_bounds.primitive_metadata_bytes += 1u;
  dispatch_bounds.primitive_optics_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized primitive optics buffer");
  dispatch_bounds.primitive_optics_bytes += 1u;
  dispatch_bounds.instance_metadata_bytes -= 1u;
  expect(!xpbd::gfx::rtDispatchBuffersInBounds(dispatch_bounds),
         "RT dispatch rejects undersized instance identity buffer");

  const std::array<float, 3> origin{0.25f, 0.25f, 1.0f};
  const std::array<float, 3> grazing_direction{
      0.9999995f, 0.0f, -0.001f};
  const std::array<float, 3> vertex0{0.0f, 0.0f, 0.0f};
  const std::array<float, 3> vertex1{2000.0f, 0.0f, 0.0f};
  const std::array<float, 3> vertex2{0.0f, 2.0f, 0.0f};
  const auto grazing = intersectRtTriangleTwoSided(
      origin, grazing_direction, vertex0, vertex1, vertex2, 0.001f,
      2000.0f);
  expect(grazing.has_value() && std::isfinite(grazing->distance) &&
             grazing->distance > 999.0f && grazing->distance < 1001.0f,
         "grazing-angle two-sided triangle hit remains finite");
  expect(grazing.has_value() &&
             grazing->barycentrics[0] >= 0.0f &&
             grazing->barycentrics[1] >= 0.0f &&
             grazing->barycentrics[0] +
                     grazing->barycentrics[1] <=
                 1.0f,
         "grazing-angle hit retains valid barycentrics");
  expect(intersectRtTriangleTwoSided(
             origin, grazing_direction, vertex0, vertex2, vertex1,
             0.001f, 2000.0f)
             .has_value(),
         "grazing-angle reference matches two-sided Vulkan instance policy");
  expect(!intersectRtTriangleTwoSided(
              origin, {1.0f, 0.0f, 0.0f}, vertex0, vertex1, vertex2,
              0.001f, 2000.0f)
              .has_value(),
         "parallel ray does not fabricate a triangle hit");

  const float grazing_distance =
      grazing ? grazing->distance : 1000.0f;
  const std::array<RtHitCandidate, 4> candidates{{
      {grazing_distance + 2.0f, 202u, RtAlphaMode::Opaque, 1.0f},
      {grazing_distance - 1.0f, 101u, RtAlphaMode::Cutout, 0.0f},
      {grazing_distance, 303u, RtAlphaMode::Opaque, 1.0f},
      {std::numeric_limits<float>::quiet_NaN(), 404u,
       RtAlphaMode::Opaque, 1.0f},
  }};
  const auto nearest =
      selectRtNearestValidHit(candidates, 0.001f, 2000.0f);
  expect(nearest.has_value() &&
             nearest->primitive_identity == 303u &&
             nearest->distance == grazing_distance,
         "nearest valid hit skips cutout and non-finite candidates");

  const std::array<RtHitCandidate, 2> blend_candidates{{
      {5.0f, 11u, RtAlphaMode::Opaque, 1.0f},
      {4.0f, 12u, RtAlphaMode::Blend, 0.25f},
  }};
  const auto nearest_blend =
      selectRtNearestValidHit(blend_candidates, 0.001f, 100.0f);
  expect(nearest_blend.has_value() &&
             nearest_blend->primitive_identity == 12u &&
             std::abs(nearest_blend->accepted_opacity - 0.25f) <
                 1.0e-6f,
         "nearest valid blended layer preserves fractional opacity");
}

void testPathTraceSamplingAndAccumulation() {
  using xpbd::gfx::PathTraceAccumulationRequest;
  using xpbd::gfx::PathTraceFrameGeneration;
  using xpbd::gfx::PathTraceLightSamplingMode;
  using xpbd::gfx::PathTraceLobe;
  using xpbd::gfx::PathTraceRngDomain;
  using xpbd::gfx::PathTraceSettings;
  using xpbd::gfx::PathTraceUpscale;
  using xpbd::gfx::advancePathTraceAccumulation;
  using xpbd::gfx::pathTraceLightEndpointWeight;
  using xpbd::gfx::normalizePathTraceSettings;
  using xpbd::gfx::kDefaultPathTraceExposureEv;
  using xpbd::gfx::pathTraceRandom01;
  using xpbd::gfx::pathTraceRandomBits;
  using xpbd::gfx::makePathTraceRngState;
  using xpbd::gfx::offsetPathTraceRayOrigin;
  using xpbd::gfx::pathTraceNextRandomBits;
  using xpbd::gfx::pathTraceNextRandom01;
  using xpbd::gfx::pathTraceRayConeTextureLod;
  using xpbd::gfx::pathTraceTriangleGeometricNormal;
  using xpbd::gfx::initializePathTraceRayCone;
  using xpbd::gfx::propagatePathTraceRayCone;
  using xpbd::gfx::pathTraceTemporalJitter;
  using xpbd::gfx::samplePathTraceCosineHemisphere;
  using xpbd::gfx::shouldResetTemporalReconstructionHistory;

  PathTraceSettings invalid;
  invalid.samples_per_frame = 0u;
  invalid.maximum_samples = (std::numeric_limits<std::uint32_t>::max)();
  invalid.max_bounces = 99u;
  invalid.analytic_environment_strength =
      (std::numeric_limits<float>::quiet_NaN)();
  invalid.display_exposure_ev =
      (std::numeric_limits<float>::quiet_NaN)();
  const auto normalized = normalizePathTraceSettings(invalid);
  expect(normalized.samples_per_frame == 1u &&
             normalized.maximum_samples == 65'536u &&
             normalized.max_bounces == 64u &&
             normalized.analytic_environment_strength == 0.0f &&
             normalized.display_exposure_ev == 0.0f,
         "path settings clamp invalid and non-finite values");
  invalid.max_diffuse_bounces = 99u;
  invalid.max_glossy_bounces = 99u;
  invalid.max_transmission_bounces = 99u;
  invalid.max_transparent_bounces = 99u;
  invalid.russian_roulette_start = 99u;
  const auto normalized_depth = normalizePathTraceSettings(invalid);
  expect(normalized_depth.max_diffuse_bounces == 16u &&
             normalized_depth.max_glossy_bounces == 16u &&
             normalized_depth.max_transmission_bounces == 32u &&
             normalized_depth.max_transparent_bounces == 64u &&
             normalized_depth.russian_roulette_start == 64u,
         "path settings clamp Phase 5 per-lobe and RR depth ranges");
  PathTraceSettings low_bounces;
  low_bounces.max_bounces = 0u;
  low_bounces.analytic_environment_strength = 99.0f;
  low_bounces.display_exposure_ev = 99.0f;
  const auto normalized_low = normalizePathTraceSettings(low_bounces);
  expect(normalized_low.max_bounces == 1u &&
             normalized_low.analytic_environment_strength == 16.0f &&
             normalized_low.display_exposure_ev == 16.0f,
         "path settings preserve Phase 5 total/environment limits");
  expect(PathTraceSettings{}.display_exposure_ev ==
             kDefaultPathTraceExposureEv &&
             kDefaultPathTraceExposureEv == 0.0f,
         "default PT display exposure is neutral");
  expect(PathTraceSettings{}.requested_frame_generation ==
             PathTraceFrameGeneration::Off,
         "DLSS Frame Generation is opt-in and defaults to Off");

  PathTraceSettings bsdf_only;
  bsdf_only.analytic_lights = false;
  bsdf_only.next_event_estimation = false;
  bsdf_only.multiple_importance_sampling = false;
  const auto normalized_bsdf_only =
      normalizePathTraceSettings(bsdf_only);
  expect(!normalized_bsdf_only.next_event_estimation &&
             xpbd::gfx::resolvedPathTraceLightSamplingMode(
                 normalized_bsdf_only) ==
                 PathTraceLightSamplingMode::BsdfOnly,
         "disabled NEE resolves to the BSDF-only light mode");

  PathTraceSettings light_only = bsdf_only;
  light_only.next_event_estimation = true;
  expect(xpbd::gfx::resolvedPathTraceLightSamplingMode(
             normalizePathTraceSettings(light_only)) ==
             PathTraceLightSamplingMode::LightOnly,
         "NEE without MIS resolves to the light-only mode");

  PathTraceSettings mis_requires_nee = bsdf_only;
  mis_requires_nee.multiple_importance_sampling = true;
  const auto normalized_mis =
      normalizePathTraceSettings(mis_requires_nee);
  expect(normalized_mis.next_event_estimation &&
             xpbd::gfx::resolvedPathTraceLightSamplingMode(normalized_mis) ==
                 PathTraceLightSamplingMode::Combined,
         "MIS normalization enables NEE and resolves to combined sampling");

  PathTraceSettings analytic_requires_nee = bsdf_only;
  analytic_requires_nee.analytic_lights = true;
  const auto normalized_analytic =
      normalizePathTraceSettings(analytic_requires_nee);
  expect(normalized_analytic.next_event_estimation &&
             xpbd::gfx::resolvedPathTraceLightSamplingMode(
                 normalized_analytic) ==
                 PathTraceLightSamplingMode::LightOnly,
         "analytic-light normalization enables explicit light sampling");

  expect(pathTraceLightEndpointWeight(
             PathTraceLightSamplingMode::BsdfOnly, false, 1.0f, 3.0f) ==
             1.0f &&
             pathTraceLightEndpointWeight(
                 PathTraceLightSamplingMode::LightOnly, false, 1.0f,
                 3.0f) == 0.0f &&
             pathTraceLightEndpointWeight(
                 PathTraceLightSamplingMode::LightOnly, true, 1.0f,
                 3.0f) == 1.0f &&
             pathTraceLightEndpointWeight(
                 PathTraceLightSamplingMode::LightOnly, false, 1.0f,
                 0.0f) == 1.0f &&
             std::abs(pathTraceLightEndpointWeight(
                          PathTraceLightSamplingMode::Combined, false,
                          1.0f, 3.0f) -
                      0.1f) < 1.0e-6f,
         "endpoint weighting preserves primary/delta and unsampled lights while "
         "LightOnly suppresses sampleable endpoints and Combined uses MIS");

  PathTraceSettings legacy_upscale;
  legacy_upscale.requested_upscale = PathTraceUpscale::Auto;
  expect(normalizePathTraceSettings(legacy_upscale).requested_upscale ==
             PathTraceUpscale::Quality,
         "legacy Auto DLSS selection migrates to Quality");
  legacy_upscale.requested_upscale = PathTraceUpscale::UltraQuality;
  expect(normalizePathTraceSettings(legacy_upscale).requested_upscale ==
             PathTraceUpscale::Quality,
         "legacy Ultra Quality DLSS selection migrates to Quality");

  const std::uint32_t random_a =
      pathTraceRandomBits(17u, 29u, 5u, 3u, 12345u);
  const std::uint32_t random_repeat =
      pathTraceRandomBits(17u, 29u, 5u, 3u, 12345u);
  const std::uint32_t random_next_dimension =
      pathTraceRandomBits(17u, 29u, 5u, 4u, 12345u);
  const float random_unit =
      pathTraceRandom01(17u, 29u, 5u, 3u, 12345u);
  expect(random_a == random_repeat && random_a != random_next_dimension,
         "fixed path seed is exact and dimensions decorrelate");
  expect(std::isfinite(random_unit) && random_unit >= 0.0f &&
             random_unit < 1.0f,
         "path random float remains finite in [0,1)");
  auto lobe_rng = makePathTraceRngState(
      17u, 29u, 5u, 2u, PathTraceRngDomain::LobeSelection, 0u, 12345u);
  auto lobe_rng_repeat = makePathTraceRngState(
      17u, 29u, 5u, 2u, PathTraceRngDomain::LobeSelection, 0u, 12345u);
  auto shadow_rng = makePathTraceRngState(
      17u, 29u, 5u, 2u, PathTraceRngDomain::ShadowTransparency, 0u,
      12345u);
  const std::uint32_t lobe_word = pathTraceNextRandomBits(lobe_rng);
  for (std::uint32_t light_sample = 0u; light_sample < 16u;
       ++light_sample) {
    (void)pathTraceNextRandom01(shadow_rng);
  }
  expect(lobe_word == 0x8e5e8ff8u &&
             lobe_word == pathTraceNextRandomBits(lobe_rng_repeat) &&
             lobe_rng.dimension == 1u && shadow_rng.dimension == 16u &&
             lobe_word != pathTraceNextRandomBits(shadow_rng),
         "state+dimension RNG domains are reproducible and Light Samples "
         "cannot perturb the BSDF sequence");

  const auto geometric = pathTraceTriangleGeometricNormal(
      {1.0e6f, 0.0f, 0.0f}, {1.0e6f, 2.0f, 0.0f},
      {1.0e6f, 0.0f, -3.0f});
  expect(geometric[0] < -0.9999f && std::abs(geometric[1]) < 1.0e-6f &&
             std::abs(geometric[2]) < 1.0e-6f,
         "true triangle Ng preserves winding under far-origin nonuniform "
         "geometry");
  const auto tiny_offset = offsetPathTraceRayOrigin(
      {1.0e-9f, 0.0f, -1.0e-9f}, {0.0f, 1.0f, 0.0f},
      {1.0f, 1.0e-4f, 0.0f});
  const std::array<float, 3> far_position{1.0e8f, -2.0e8f, 3.0e8f};
  const auto far_offset = offsetPathTraceRayOrigin(
      far_position, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
  const auto reverse_offset = offsetPathTraceRayOrigin(
      {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
      {0.0f, -1.0f, 0.0f});
  expect(std::isfinite(tiny_offset[1]) && tiny_offset[1] > 0.0f &&
             std::isfinite(far_offset[1]) &&
             far_offset[1] > far_position[1] &&
             reverse_offset[1] < 0.0f,
         "ULP-first Ng ray offset remains finite at tiny/far scales and "
         "selects the outgoing thin-sheet side");

  const auto primary_cone = initializePathTraceRayCone(
      590u, 579u, 1.0471975511965976f);
  const float close_lod = pathTraceRayConeTextureLod(
      primary_cone, 1.0f, 2.0f, 1.0f, 2048u, 2048u);
  const float far_lod = pathTraceRayConeTextureLod(
      primary_cone, 100.0f, 2.0f, 1.0f, 2048u, 2048u);
  const auto mirror_cone = propagatePathTraceRayCone(
      primary_cone, 5.0f, PathTraceLobe::Glossy, 0.0f);
  const auto diffuse_cone = propagatePathTraceRayCone(
      primary_cone, 5.0f, PathTraceLobe::Diffuse, 1.0f);
  expect(primary_cone.width == 0.0f && primary_cone.spread_angle > 0.0f &&
             close_lod >= 0.0f && far_lod > close_lod &&
             mirror_cone.spread_angle == primary_cone.spread_angle &&
             diffuse_cone.width > 0.0f &&
             diffuse_cone.spread_angle > mirror_cone.spread_angle,
         "ray cone initializes from pixel projection and propagates "
         "continuous distance/lobe/UV-area LOD");
  const auto temporal_jitter_0 =
      pathTraceTemporalJitter(0u, 393u, 590u);
  const auto temporal_jitter_1 =
      pathTraceTemporalJitter(1u, 393u, 590u);
  expect(temporal_jitter_0[0] >= -0.5f &&
             temporal_jitter_0[0] <= 0.5f &&
             temporal_jitter_0[1] >= -0.5f &&
             temporal_jitter_0[1] <= 0.5f,
         "temporal reconstruction jitter stays in pixel bounds");
  expect(temporal_jitter_0 != temporal_jitter_1 &&
             temporal_jitter_0 ==
                 pathTraceTemporalJitter(0u, 393u, 590u),
         "temporal reconstruction jitter is stable and advances");
  expect(temporal_jitter_0 ==
             pathTraceTemporalJitter(18u, 393u, 590u),
         "Quality jitter repeats at NVIDIA's scale-dependent phase count");
  expect(pathTraceTemporalJitter(0u, 590u, 590u) ==
             pathTraceTemporalJitter(8u, 590u, 590u),
         "DLAA jitter repeats after eight phases");
  expect(shouldResetTemporalReconstructionHistory(
             false, 0u, 7u, false) &&
             !shouldResetTemporalReconstructionHistory(
                 true, 7u, 7u, true) &&
             shouldResetTemporalReconstructionHistory(
                 true, 7u, 8u, true) &&
             shouldResetTemporalReconstructionHistory(
                 true, 7u, 7u, false),
         "Streamline history resets only for first/incompatible/invalid-motion "
         "frames, not ordinary dense-motion camera frames");

  const auto hemisphere = samplePathTraceCosineHemisphere(
      {0.0f, 1.0f, 0.0f}, random_unit,
      pathTraceRandom01(17u, 29u, 5u, 4u, 12345u));
  const float hemisphere_length = std::sqrt(
      hemisphere.direction[0] * hemisphere.direction[0] +
      hemisphere.direction[1] * hemisphere.direction[1] +
      hemisphere.direction[2] * hemisphere.direction[2]);
  expect(!hemisphere.used_fallback &&
             std::isfinite(hemisphere_length) &&
             std::abs(hemisphere_length - 1.0f) < 1.0e-5f &&
             hemisphere.direction[1] >= 0.0f,
         "cosine sample is finite, normalized, and above the hemisphere");
  const auto invalid_hemisphere = samplePathTraceCosineHemisphere(
      {0.0f, 0.0f, 0.0f}, 0.5f, 0.5f);
  expect(invalid_hemisphere.used_fallback &&
             invalid_hemisphere.direction ==
                 std::array<float, 3>{0.0f, 1.0f, 0.0f},
         "invalid cosine-sampling normal uses finite deterministic fallback");

  PathTraceAccumulationRequest request;
  request.history_key = 101u;
  request.settings.samples_per_frame = 3u;
  request.settings.maximum_samples = 40u;
  auto first = advancePathTraceAccumulation(request);
  expect(first.history_reset && first.sample_base == 0u &&
             first.dispatch_samples == 3u &&
             first.accumulated_samples_after_dispatch == 3u,
         "new history starts at sample zero and dispatches spp");

  request.history_valid = true;
  request.previous_history_key = 101u;
  request.accumulated_samples = 29u;
  request.settings.samples_per_frame = 4u;
  request.settings.maximum_samples = 32u;
  const auto continued = advancePathTraceAccumulation(request);
  expect(!continued.history_reset && continued.sample_base == 29u &&
             continued.dispatch_samples == 3u &&
             continued.accumulated_samples_after_dispatch == 32u &&
             continued.maximum_reached,
         "spp/max changes preserve history and stop exactly at maximum");

  request.accumulated_samples =
      continued.accumulated_samples_after_dispatch;
  request.settings.maximum_samples = 32u;
  const auto stopped = advancePathTraceAccumulation(request);
  expect(!stopped.history_reset && stopped.sample_base == 32u &&
             stopped.dispatch_samples == 0u &&
             stopped.accumulated_samples_after_dispatch == 32u &&
             stopped.maximum_reached,
         "lowered maximum stops without discarding compatible samples");

  request.history_key = 202u;
  request.settings.maximum_samples = 40u;
  const auto reset = advancePathTraceAccumulation(request);
  expect(reset.history_reset && reset.sample_base == 0u &&
             reset.dispatch_samples == 4u &&
             reset.accumulated_samples_after_dispatch == 4u,
         "radiance-affecting history-key change resets slot accumulation");
}

void testPathTraceAdjustableSettingsContract() {
  using namespace xpbd::gfx;

  const auto realtime =
      pathTraceSettingsForPreset(PathTracePreset::Realtime);
  const auto reference =
      pathTraceSettingsForPreset(PathTracePreset::Reference);
  expect(realtime.preset == PathTracePreset::Realtime &&
             reference.preset == PathTracePreset::Reference &&
             reference.samples_per_frame > realtime.samples_per_frame &&
             reference.max_bounces > realtime.max_bounces,
         "path tracing presets provide distinct normalized quality tiers");

  auto custom = applyPathTracePreset(realtime,
                                     PathTracePreset::HighQuality);
  custom.preset = PathTracePreset::Custom;
  custom.samples_per_frame = 3u;
  const auto restored = restorePathTraceSourcePreset(custom);
  expect(restored.preset == PathTracePreset::HighQuality &&
             restored.source_preset == PathTracePreset::HighQuality &&
             restored.samples_per_frame == 4u,
         "Custom retains and restores its source preset");

  auto schedule = realtime;
  schedule.samples_per_frame = 8u;
  schedule.maximum_samples = 2048u;
  const auto schedule_change =
      classifyPathTraceSettingsChange(realtime, schedule);
  expect(hasPathTraceChange(
             schedule_change,
             PathTraceChangeClass::SamplingSchedule) &&
             !hasPathTraceChange(
                 schedule_change,
                 PathTraceChangeClass::ResetAccumulation),
         "SPP and maximum samples preserve compatible accumulation");

  auto physical = schedule;
  physical.multiple_importance_sampling = false;
  physical.direct_clamp = 2.0f;
  const auto physical_change =
      classifyPathTraceSettingsChange(schedule, physical);
  expect(hasPathTraceChange(
             physical_change,
             PathTraceChangeClass::ResetAccumulation),
         "integrator edits reset accumulated radiance");

  auto film = physical;
  film.display_exposure_ev = -1.0f;
  film.tone_mapping = PathTraceToneMapping::Reinhard;
  const auto film_change =
      classifyPathTraceSettingsChange(physical, film);
  expect(hasPathTraceChange(
             film_change, PathTraceChangeClass::DisplayOnly) &&
             !hasPathTraceChange(
                 film_change,
                 PathTraceChangeClass::ResetAccumulation),
         "film edits are classified as display-only");

  auto target = film;
  target.preview_resolution_scale = 0.5f;
  expect(hasPathTraceChange(
             classifyPathTraceSettingsChange(film, target),
             PathTraceChangeClass::RecreateTarget),
         "preview scale is classified as target recreation");
  PathTraceSettings automatic_seed;
  automatic_seed.reset_generation = 11u;
  const std::uint32_t generated_seed =
      resolvedPathTraceSeed(automatic_seed);
  expect(generated_seed == resolvedPathTraceSeed(automatic_seed) &&
             generated_seed != automatic_seed.seed,
         "automatic seed is stable for one accumulation generation");
  automatic_seed.automatic_seed = false;
  automatic_seed.seed = 42u;
  expect(resolvedPathTraceSeed(automatic_seed) == 42u,
         "fixed seed is passed through exactly");

  PathTraceSettings post;
  post.requested_denoiser =
      PathTraceDenoiser::DlssRayReconstruction;
  post.requested_upscale = PathTraceUpscale::Quality;
  PathTracePostProcessCapabilities capabilities;
  auto unresolved = resolvePathTracePostProcess(post, capabilities);
  expect(!unresolved.denoiser_supported &&
             unresolved.active_denoiser ==
                 PathTraceDenoiser::Raw &&
             !unresolved.upscale_supported &&
             unresolved.active_upscale == PathTraceUpscale::Off,
         "unsupported denoise/upscale requests resolve visibly to raw/off");
  capabilities.dlss_ray_reconstruction = true;
  capabilities.dlss_super_resolution = true;
  const auto rr = resolvePathTracePostProcess(post, capabilities);
  expect(rr.active_denoiser ==
             PathTraceDenoiser::DlssRayReconstruction &&
             rr.active_upscale == PathTraceUpscale::Off &&
             rr.reconstruction_mode == PathTraceUpscale::Quality &&
             rr.conflict_resolved,
         "Ray Reconstruction owns reconstruction and excludes separate SR");
  post.requested_upscale = PathTraceUpscale::Off;
  const auto rr_without_quality =
      resolvePathTracePostProcess(post, capabilities);
  expect(rr_without_quality.active_denoiser ==
                 PathTraceDenoiser::Raw &&
             rr_without_quality.reconstruction_mode ==
                 PathTraceUpscale::Off &&
             rr_without_quality.rr_mode_required,
         "Ray Reconstruction requires a low-resolution quality tier");
  auto retired_reblur = post;
  retired_reblur.requested_denoiser =
      static_cast<PathTraceDenoiser>(2);
  retired_reblur.requested_upscale = PathTraceUpscale::Off;
  expect(normalizePathTraceSettings(retired_reblur).requested_denoiser ==
             PathTraceDenoiser::Raw,
         "retired NRD REBLUR setting migrates safely to Raw");
  auto retired_relax = post;
  retired_relax.requested_denoiser =
      static_cast<PathTraceDenoiser>(3);
  retired_relax.requested_upscale = PathTraceUpscale::Off;
  expect(normalizePathTraceSettings(retired_relax).requested_denoiser ==
             PathTraceDenoiser::Raw,
         "retired NRD RELAX setting migrates safely to Raw");

  const auto snapshot = makePathTraceRenderSnapshot(physical);
  physical.max_bounces = 1u;
  expect(snapshot.settings.max_bounces != physical.max_bounces &&
             snapshot.source_generation ==
                 snapshot.settings.reset_generation,
         "still render snapshot freezes normalized path settings");
}

void testPathTraceBsdfAndDepth() {
  using xpbd::gfx::PathTraceDepthState;
  using xpbd::gfx::PathTraceLobe;
  using xpbd::gfx::PathTraceSettings;
  using xpbd::gfx::RtBsdfMaterial;
  using xpbd::gfx::RtSurfaceOptics;
  using xpbd::gfx::advancePathTraceDepth;
  using xpbd::gfx::evaluatePathTraceRussianRoulette;
  using xpbd::gfx::evaluateRtBsdf;
  using xpbd::gfx::pathTraceBounceAllowed;
  using xpbd::gfx::pathTraceRandom01;
  using xpbd::gfx::rtBsdfLobeProbabilities;
  using xpbd::gfx::rtDielectricF0FromIor;
  using xpbd::gfx::rtDielectricIorFromF0;
  using xpbd::gfx::rtFresnelDielectric;
  using xpbd::gfx::rtFresnelSchlick;
  using xpbd::gfx::rtGgxDistribution;
  using xpbd::gfx::rtGgxVisibleNormalPdf;
  using xpbd::gfx::rtBeerLambertTransmittance;
  using xpbd::gfx::rtShadingNormalCorrection;
  using xpbd::gfx::rtSmithGgxG1;
  using xpbd::gfx::kDeltaMirrorAlpha;
  using xpbd::gfx::kMinFiniteGgxAlpha;
  using xpbd::gfx::kPathTraceShadingNormalCorrectionLimit;
  using xpbd::gfx::samplePathTraceCosineHemisphere;
  using xpbd::gfx::sampleRtGgxVndf;
  using xpbd::gfx::sampleRtBsdf;

  const float glass_f0 = rtDielectricF0FromIor(1.5f);
  expect(kDeltaMirrorAlpha == 1.0e-6f &&
             kMinFiniteGgxAlpha == 1.0e-4f,
         "CPU mirror and finite-GGX thresholds match the shader contract");
  const float glass_ior = rtDielectricIorFromF0(glass_f0);
  expect(std::abs(glass_f0 - 0.04f) < 1.0e-5f &&
             std::abs(glass_ior - 1.5f) < 1.0e-4f,
         "dielectric F0 and IOR round-trip at glass reference");
  expect(std::abs(rtFresnelDielectric(1.0f, 1.0f, 1.5f) -
                  glass_f0) < 1.0e-5f &&
             rtFresnelDielectric(0.2f, 1.5f, 1.0f) == 1.0f,
         "exact dielectric Fresnel matches normal incidence and TIR");
  const auto schlick_normal =
      rtFresnelSchlick({0.04f, 0.2f, 0.8f}, 1.0f);
  const auto schlick_grazing =
      rtFresnelSchlick({0.04f, 0.2f, 0.8f}, 0.0f);
  expect(schlick_normal ==
             std::array<float, 3>{0.04f, 0.2f, 0.8f} &&
             schlick_grazing ==
                 std::array<float, 3>{1.0f, 1.0f, 1.0f},
         "Schlick Fresnel preserves F0 and reaches one at grazing");
  expect(std::isfinite(rtGgxDistribution(1.0f, 0.02f)) &&
             rtGgxDistribution(1.0f, 0.02f) >
                 rtGgxDistribution(1.0f, 0.8f) &&
             rtSmithGgxG1(1.0f, 0.5f) == 1.0f &&
             rtSmithGgxG1(0.0f, 0.5f) == 0.0f,
         "GGX distribution and Smith masking retain finite limits");
  expect(rtGgxDistribution(1.0f, 0.0f) == 0.0f &&
             rtGgxDistribution(1.0f, 1.0e-5f) >
                 rtGgxDistribution(1.0f, 0.02f) * 39'000.0f,
         "GGX keeps zero as a delta atom and uses only the 1e-4 finite floor");

  const RtSurfaceOptics default_optics;
  expect(default_optics.transmission == 0.0f &&
             default_optics.ior == 1.5f &&
             default_optics.attenuation_color ==
                 std::array<float, 3>{1.0f, 1.0f, 1.0f} &&
             default_optics.attenuation_distance == 0.0f &&
             !default_optics.thin_walled,
         "surface-optics defaults are opaque and absorption-inert");
  RtSurfaceOptics absorbing_optics;
  absorbing_optics.transmission = 1.0f;
  absorbing_optics.attenuation_color = {0.25f, 0.5f, 1.0f};
  absorbing_optics.attenuation_distance = 2.0f;
  const auto one_absorption_distance =
      rtBeerLambertTransmittance(absorbing_optics, 2.0f);
  const auto two_absorption_distances =
      rtBeerLambertTransmittance(absorbing_optics, 4.0f);
  expect(std::abs(one_absorption_distance[0] - 0.25f) < 1.0e-6f &&
             std::abs(one_absorption_distance[1] - 0.5f) < 1.0e-6f &&
             std::abs(one_absorption_distance[2] - 1.0f) < 1.0e-6f &&
             std::abs(two_absorption_distances[0] - 0.0625f) < 1.0e-6f &&
             std::abs(two_absorption_distances[1] - 0.25f) < 1.0e-6f,
         "Beer-Lambert matches attenuation color at its reference distance");
  absorbing_optics.thin_walled = true;
  expect(rtBeerLambertTransmittance(absorbing_optics, 100.0f) ==
             std::array<float, 3>{1.0f, 1.0f, 1.0f} &&
             rtBeerLambertTransmittance(default_optics, 100.0f) ==
                 std::array<float, 3>{1.0f, 1.0f, 1.0f},
         "Thin-Walled and disabled attenuation never accumulate interior distance");

  RtBsdfMaterial dielectric;
  dielectric.base_color = {0.8f, 0.6f, 0.3f};
  dielectric.f0 = {glass_f0, glass_f0, glass_f0};
  dielectric.ggx_alpha = 0.3f;
  dielectric.ior = 1.5f;
  const auto dielectric_probabilities =
      rtBsdfLobeProbabilities(dielectric);
  expect(dielectric_probabilities.diffuse > 0.0f &&
             dielectric_probabilities.glossy > 0.0f &&
             dielectric_probabilities.transmission == 0.0f,
         "opaque dielectric selects diffuse and glossy only");

  RtBsdfMaterial metal = dielectric;
  metal.base_color = {0.1f, 0.9f, 0.25f};
  metal.f0 = {0.9f, 0.55f, 0.15f};
  metal.transmission = 1.0f;
  metal.metal = true;
  const auto metal_probabilities = rtBsdfLobeProbabilities(metal);
  expect(metal_probabilities.diffuse == 0.0f &&
             metal_probabilities.glossy == 1.0f &&
             metal_probabilities.transmission == 0.0f,
         "metal suppresses diffuse and transmission lobes");

  const std::array<float, 3> normal{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> view{0.0f, 1.0f, 0.0f};
  const std::array<std::array<float, 3>, 2> vndf_views{{
      view, {0.98480775f, 0.17364818f, 0.0f}}};
  const std::array<float, 4> vndf_alphas{{
      kMinFiniteGgxAlpha, 0.03f, 0.35f, 1.0f}};
  for (std::size_t view_index = 0u; view_index < vndf_views.size();
       ++view_index) {
    for (std::size_t alpha_index = 0u; alpha_index < vndf_alphas.size();
         ++alpha_index) {
      std::uint32_t valid_vndf = 0u;
      double pdf_integral = 0.0;
      constexpr std::uint32_t kVndfSamples = 4096u;
      for (std::uint32_t sample_index = 0u;
           sample_index < kVndfSamples; ++sample_index) {
        const float u = pathTraceRandom01(
            41u, 43u, sample_index, 0u,
            901u + static_cast<std::uint32_t>(
                       view_index * vndf_alphas.size() + alpha_index));
        const float v = pathTraceRandom01(
            41u, 43u, sample_index, 1u,
            901u + static_cast<std::uint32_t>(
                       view_index * vndf_alphas.size() + alpha_index));
        const auto sampled = sampleRtGgxVndf(
            normal, vndf_views[view_index], vndf_alphas[alpha_index], u, v);
        valid_vndf += sampled.valid ? 1u : 0u;
        const float uniform_y = u;
        const float radial =
            std::sqrt(std::max(0.0f, 1.0f - uniform_y * uniform_y));
        const float phi = 6.28318530717958647692f * v;
        const std::array<float, 3> uniform_half{
            radial * std::cos(phi), uniform_y, radial * std::sin(phi)};
        pdf_integral += rtGgxVisibleNormalPdf(
                            normal, vndf_views[view_index], uniform_half,
                            vndf_alphas[alpha_index]) *
                        6.28318530717958647692;
      }
      pdf_integral /= kVndfSamples;
      // A uniform-hemisphere estimator cannot resolve the 1e-4 lobe's
      // vanishingly small support. Its sampler validity is still covered here;
      // normalization is integrated for the remaining finite-alpha range.
      const bool integral_resolved =
          vndf_alphas[alpha_index] <= kMinFiniteGgxAlpha * 1.001f ||
          (std::isfinite(pdf_integral) &&
           std::abs(pdf_integral - 1.0) < 0.10);
      expect(valid_vndf == kVndfSamples &&
                 integral_resolved,
             "VNDF remains valid and its visible-normal PDF integrates near one");
    }
  }
  RtBsdfMaterial delta_metal = metal;
  delta_metal.ggx_alpha = 0.0f;
  const auto delta_eval =
      evaluateRtBsdf(delta_metal, normal, view, normal);
  const auto delta_sample =
      sampleRtBsdf(delta_metal, normal, view, true, 0.5f, 0.37f, 0.61f);
  expect(!delta_eval.valid && delta_sample.valid && delta_sample.delta &&
             delta_sample.lobe == PathTraceLobe::Glossy &&
             std::abs(delta_sample.direction[0]) < 1.0e-7f &&
             std::abs(delta_sample.direction[1] - 1.0f) < 1.0e-7f &&
             std::abs(delta_sample.direction[2]) < 1.0e-7f &&
             std::abs(delta_sample.pdf - 1.0f) < 1.0e-7f &&
             std::abs(delta_sample.weight[0] - delta_metal.f0[0]) <
                 1.0e-6f &&
             std::abs(delta_sample.weight[1] - delta_metal.f0[1]) <
                 1.0e-6f &&
             std::abs(delta_sample.weight[2] - delta_metal.f0[2]) <
                 1.0e-6f,
         "zero-alpha metal is an exact glossy delta and has no continuous "
         "GGX density");
  std::uint32_t valid_samples = 0u;
  bool sample_eval_consistent = true;
  for (std::uint32_t sample_index = 0u; sample_index < 4096u;
       ++sample_index) {
    const auto sampled = sampleRtBsdf(
        dielectric, normal, view, true,
        pathTraceRandom01(3u, 7u, sample_index, 0u, 91u),
        pathTraceRandom01(3u, 7u, sample_index, 1u, 91u),
        pathTraceRandom01(3u, 7u, sample_index, 2u, 91u));
    if (!sampled.valid) {
      continue;
    }
    ++valid_samples;
    const auto evaluated =
        evaluateRtBsdf(dielectric, normal, view, sampled.direction);
    sample_eval_consistent =
        sample_eval_consistent && evaluated.valid &&
        std::abs(evaluated.pdf - sampled.pdf) < 1.0e-6f;
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      sample_eval_consistent =
          sample_eval_consistent &&
          std::abs(evaluated.value[channel] -
                   sampled.value[channel]) < 1.0e-6f;
    }
  }
  expect(sample_eval_consistent,
         "BSDF sample/eval values and mixture PDFs are consistent");
  expect(valid_samples > 3500u,
         "deterministic BSDF sampling yields sufficient valid reflections");

  const std::array<RtBsdfMaterial, 4> furnace_materials{{
      {{1.0f, 1.0f, 1.0f}, {0.04f, 0.04f, 0.04f},
       0.05f, 0.0f, 1.5f, false},
      {{1.0f, 1.0f, 1.0f}, {0.04f, 0.04f, 0.04f},
       0.8f, 0.0f, 1.5f, false},
      {{1.0f, 1.0f, 1.0f}, {0.95f, 0.7f, 0.2f},
       0.08f, 0.0f, 1.5f, true},
      {{1.0f, 1.0f, 1.0f}, {0.95f, 0.7f, 0.2f},
       0.7f, 0.0f, 1.5f, true},
  }};
  for (std::size_t material_index = 0u;
       material_index < furnace_materials.size(); ++material_index) {
    std::array<double, 3> furnace{};
    constexpr std::uint32_t kFurnaceSamples = 32768u;
    for (std::uint32_t sample_index = 0u;
         sample_index < kFurnaceSamples; ++sample_index) {
      const auto direction = samplePathTraceCosineHemisphere(
          normal,
          pathTraceRandom01(11u, 19u, sample_index, 0u,
                            501u + static_cast<std::uint32_t>(material_index)),
          pathTraceRandom01(11u, 19u, sample_index, 1u,
                            501u + static_cast<std::uint32_t>(material_index)));
      const auto evaluated = evaluateRtBsdf(
          furnace_materials[material_index], normal, view,
          direction.direction);
      if (!evaluated.valid) {
        continue;
      }
      constexpr double kPi = 3.14159265358979323846;
      for (std::size_t channel = 0u; channel < 3u; ++channel) {
        furnace[channel] += evaluated.value[channel] * kPi;
      }
    }
    for (double &channel : furnace) {
      channel /= kFurnaceSamples;
      expect(std::isfinite(channel) && channel >= 0.0 &&
                 channel <= 1.02,
             "white-furnace reflection remains finite and energy bounded");
    }
  }

  RtBsdfMaterial glass;
  glass.base_color = {1.0f, 1.0f, 1.0f};
  glass.f0 = {glass_f0, glass_f0, glass_f0};
  glass.ggx_alpha = 0.2f;
  glass.transmission = 1.0f;
  glass.ior = 1.5f;
  RtBsdfMaterial smooth_glass = glass;
  smooth_glass.ggx_alpha = 0.0f;
  const auto refraction =
      sampleRtBsdf(smooth_glass, normal, view, true, 0.99f, 0.4f, 0.7f);
  expect(refraction.valid && refraction.delta &&
             refraction.lobe == PathTraceLobe::Transmission &&
             !refraction.total_internal_reflection &&
             refraction.direction[1] < 0.0f &&
             std::abs(refraction.weight[0] - (1.0f / 1.5f) *
                                                   (1.0f / 1.5f)) <
                 1.0e-5f,
         "smooth glass uses exact Fresnel selection and radiance eta-squared refraction");
  const std::array<float, 3> inside_grazing{
      0.9f, 0.4358899f, 0.0f};
  const auto tir = sampleRtBsdf(
      smooth_glass, normal, inside_grazing, false, 0.99f, 0.4f, 0.7f);
  expect(tir.valid && tir.delta &&
             tir.total_internal_reflection &&
             tir.lobe == PathTraceLobe::Glossy &&
             std::abs(tir.pdf - 1.0f) < 1.0e-6f &&
             tir.direction[1] > 0.0f &&
             std::abs(tir.weight[0] - 1.0f) < 1.0e-6f &&
             std::abs(tir.weight[1] - 1.0f) < 1.0e-6f &&
             std::abs(tir.weight[2] - 1.0f) < 1.0e-6f,
         "neutral glass TIR is a unit glossy delta reflection");

  RtBsdfMaterial colored_glass = smooth_glass;
  colored_glass.base_color = {0.1f, 0.4f, 0.9f};
  const auto colored_tir = sampleRtBsdf(
      colored_glass, normal, inside_grazing, false, 0.01f, 0.2f, 0.8f);
  expect(colored_tir.valid && colored_tir.delta &&
             colored_tir.total_internal_reflection &&
             colored_tir.lobe == PathTraceLobe::Glossy &&
             std::abs(colored_tir.pdf - 1.0f) < 1.0e-6f &&
             std::abs(colored_tir.weight[0] - 1.0f) < 1.0e-6f &&
             std::abs(colored_tir.weight[1] - 1.0f) < 1.0e-6f &&
             std::abs(colored_tir.weight[2] - 1.0f) < 1.0e-6f,
         "colored glass TIR is not attenuated or tinted");

  const std::array<float, 3> inside_below_critical{
      0.6f, 0.8f, 0.0f};
  const auto below_critical = sampleRtBsdf(
      smooth_glass, normal, inside_below_critical, false, 0.99f, 0.4f, 0.7f);
  expect(below_critical.valid && below_critical.delta &&
             !below_critical.total_internal_reflection &&
             below_critical.lobe == PathTraceLobe::Transmission &&
             below_critical.direction[1] < 0.0f,
         "inside-glass incidence below the critical angle still refracts");
  RtBsdfMaterial thin_glass = glass;
  thin_glass.thin_walled = true;
  const auto thin_transmission = sampleRtBsdf(
      thin_glass, normal, view, true, 0.99f, 0.2f, 0.8f);
  const auto thin_continuous =
      evaluateRtBsdf(thin_glass, normal, view, normal, true);
  expect(thin_transmission.valid && thin_transmission.delta &&
              thin_transmission.lobe == PathTraceLobe::Transmission &&
              !thin_continuous.valid &&
              std::abs(thin_transmission.direction[0] + view[0]) < 1.0e-6f &&
             std::abs(thin_transmission.direction[1] + view[1]) < 1.0e-6f &&
             std::abs(thin_transmission.direction[2] + view[2]) < 1.0e-6f,
         "Thin-Walled transmission combines two interfaces and stays outside the medium stack");

  std::uint32_t rough_reflections = 0u;
  std::uint32_t rough_transmissions = 0u;
  std::uint32_t rough_invalid = 0u;
  bool rough_sample_eval_consistent = true;
  constexpr std::uint32_t kRoughGlassSamples = 8192u;
  for (std::uint32_t sample_index = 0u;
       sample_index < kRoughGlassSamples; ++sample_index) {
    const auto sampled = sampleRtBsdf(
        glass, normal, view, true,
        pathTraceRandom01(53u, 59u, sample_index, 0u, 1103u),
        pathTraceRandom01(53u, 59u, sample_index, 1u, 1103u),
        pathTraceRandom01(53u, 59u, sample_index, 2u, 1103u));
    if (!sampled.valid) {
      ++rough_invalid;
      continue;
    }
    rough_reflections += sampled.lobe == PathTraceLobe::Glossy ? 1u : 0u;
    rough_transmissions +=
        sampled.lobe == PathTraceLobe::Transmission ? 1u : 0u;
    const auto evaluated =
        evaluateRtBsdf(glass, normal, view, sampled.direction, true);
    const float pdf_tolerance =
        std::max(2.0e-5f, evaluated.pdf * 2.0e-4f);
    rough_sample_eval_consistent =
        rough_sample_eval_consistent && !sampled.delta && evaluated.valid &&
        std::abs(evaluated.pdf - sampled.pdf) <= pdf_tolerance;
    for (std::size_t channel = 0u; channel < 3u; ++channel) {
      const float value_tolerance =
          std::max(2.0e-5f, evaluated.value[channel] * 2.0e-4f);
      rough_sample_eval_consistent =
          rough_sample_eval_consistent &&
          std::abs(evaluated.value[channel] - sampled.value[channel]) <=
              value_tolerance;
    }
  }
  expect(rough_sample_eval_consistent && rough_reflections > 100u &&
             rough_transmissions > 6'000u && rough_invalid < 800u,
         "rough glass VNDF reflection/refraction sample, BSDF, Jacobian, and PDF stay matched");

  const std::array<std::array<float, 3>, 2> glass_views{{
      view, inside_grazing}};
  const std::array<bool, 2> glass_front_faces{{true, false}};
  for (std::size_t view_index = 0u;
       view_index < glass_views.size(); ++view_index) {
    std::array<double, 3> energy{};
    constexpr std::uint32_t kGlassEnergySamples = 16384u;
    for (std::uint32_t sample_index = 0u;
         sample_index < kGlassEnergySamples; ++sample_index) {
      const auto sampled = sampleRtBsdf(
          smooth_glass, normal, glass_views[view_index],
          glass_front_faces[view_index],
          pathTraceRandom01(23u, 31u, sample_index, 0u,
                            701u + static_cast<std::uint32_t>(view_index)),
          pathTraceRandom01(23u, 31u, sample_index, 1u,
                            701u + static_cast<std::uint32_t>(view_index)),
          pathTraceRandom01(23u, 31u, sample_index, 2u,
                            701u + static_cast<std::uint32_t>(view_index)));
      if (!sampled.valid) {
        continue;
      }
      for (std::size_t channel = 0u; channel < 3u; ++channel) {
        energy[channel] += sampled.weight[channel];
      }
    }
    const double energy_limit = view_index == 0u ? 1.02 : 1.001;
    for (double &channel : energy) {
      channel /= kGlassEnergySamples;
      expect(std::isfinite(channel) && channel >= 0.0 &&
                  channel <= energy_limit,
              "glass/TIR white-furnace sampling remains energy bounded");
      if (view_index == 1u) {
        expect(channel >= 0.999 && channel <= 1.001,
               "TIR white-furnace sampling conserves unit energy");
      }
    }
  }

  expect(rtShadingNormalCorrection(normal, normal, view, normal) == 1.0f,
         "identical normals have unit shading-normal correction");
  const float tilted_correction = rtShadingNormalCorrection(
      normal, {0.0f, 0.8f, 0.6f}, view, {0.3f, 0.9f, 0.1f});
  expect(std::isfinite(tilted_correction) &&
             tilted_correction > 0.0f &&
             tilted_correction <=
                 kPathTraceShadingNormalCorrectionLimit,
         "tilted shading-normal correction stays finite and bounded");
  const float extreme_correction = rtShadingNormalCorrection(
      normal, {0.0f, 0.01f, 0.99995f}, view,
      {0.0f, 0.01f, 0.99995f});
  expect(std::abs(extreme_correction -
                  kPathTraceShadingNormalCorrectionLimit) < 1.0e-6f &&
             kPathTraceShadingNormalCorrectionLimit == 4.0f,
         "extreme shading-normal energy is hard-capped at four");
  expect(rtShadingNormalCorrection(
             normal, {0.0f, 0.8f, 0.6f}, view,
             {0.0f, -0.2f, 1.0f}) == 0.0f,
         "shading-normal correction rejects hemisphere disagreement");

  PathTraceSettings depth_settings;
  depth_settings.max_bounces = 3u;
  depth_settings.max_diffuse_bounces = 1u;
  depth_settings.max_glossy_bounces = 2u;
  depth_settings.max_transmission_bounces = 1u;
  depth_settings.russian_roulette_start = 2u;
  PathTraceDepthState depth;
  const auto first_diffuse =
      advancePathTraceDepth(depth_settings, depth, PathTraceLobe::Diffuse);
  expect(first_diffuse.has_value() &&
             !pathTraceBounceAllowed(depth_settings, *first_diffuse,
                                     PathTraceLobe::Diffuse),
         "per-lobe diffuse boundary stops exactly at the configured count");
  const auto second_glossy = advancePathTraceDepth(
      depth_settings, *first_diffuse, PathTraceLobe::Glossy);
  const auto third_transmission = advancePathTraceDepth(
      depth_settings, *second_glossy, PathTraceLobe::Transmission);
  expect(third_transmission.has_value() &&
             !pathTraceBounceAllowed(depth_settings, *third_transmission,
                                     PathTraceLobe::Glossy),
         "total bounce boundary stops every lobe exactly");

  const auto rr_before = evaluatePathTraceRussianRoulette(
      depth_settings, *first_diffuse, 0.2f, 0.9f);
  const auto rr_kill = evaluatePathTraceRussianRoulette(
      depth_settings, *second_glossy, 0.2f, 0.9f);
  const auto rr_survive = evaluatePathTraceRussianRoulette(
      depth_settings, *second_glossy, 0.2f, 0.1f);
  expect(!rr_before.applied && rr_before.survives &&
             rr_kill.applied && !rr_kill.survives &&
             rr_survive.applied && rr_survive.survives &&
             std::abs(rr_survive.throughput_scale - 5.0f) < 1.0e-6f,
         "Russian roulette starts at its boundary and reweights survivors");
}

void testEmptyTextureSample() {
  xpbd::gfx::TextureImage empty;
  float r = 0, g = 0, b = 0, a = 0;
  empty.sample(0.0f, 0.0f, r, g, b, a);
  expect(r == 1.0f && g == 1.0f && b == 1.0f && a == 1.0f,
         "empty texture samples white");
}

void testViewportMeshEmptyGeometry() {
  xpbd::loader::Geometry geo;
  xpbd::baker::BoneMapper mapper;
  xpbd::gfx::ViewportMeshBuilder builder;
  builder.setGeometry(&geo);
  builder.setBoneMapper(&mapper);
  builder.setShowBones(false);
  builder.setShowGround(true);

  xpbd::gfx::ViewportGpuScene scene;
  builder.buildRest(scene);
  // Ground grid alone should produce some line geometry when enabled.
  expect(scene.line_segment_count >= 0, "buildRest completes");
  expect(scene.cube_count == 0, "empty geometry has zero cubes");

  xpbd::gfx::StaticIndexedModelMesh static_mesh;
  builder.buildStaticIndexedModel(static_mesh);
  expect(static_mesh.cube_count == 0, "static indexed model empty");
  expect(static_mesh.vertices.empty(), "static model has no vertices");
}

void testFrontFacingFlatCubePicking() {
  xpbd::loader::Geometry geometry;
  xpbd::loader::Bone bone;
  bone.name = "front_facing_flat";
  xpbd::loader::Cube cube;
  cube.origin[0] = -5.0;
  cube.origin[1] = -5.0;
  cube.origin[2] = 0.0;
  cube.size[0] = 10.0;
  cube.size[1] = 10.0;
  cube.size[2] = 0.0;
  bone.cubes.push_back(cube);
  geometry.bones.push_back(bone);

  xpbd::render::SkeletonViewport viewport;
  viewport.setGeometry(&geometry);
  viewport.setShowBones(false);
  xpbd::render::ViewportCamera camera;
  camera.yaw_deg = 0.0f;
  camera.pitch_deg = 0.0f;
  camera.distance = 50.0f;
  const auto draw_list = viewport.buildRest(camera, 200.0f, 200.0f, false);

  expect(!draw_list.faces.empty(),
         "front-facing zero-thickness cube contributes a pickable face");
  expect(xpbd::render::pickBone(draw_list, 100.0f, 100.0f, 0.0f) ==
             "front_facing_flat",
         "front-facing zero-thickness cube is selectable from its broad side");

  xpbd::render::SkeletonDrawList overlap;
  xpbd::render::ProjectedFace tolerance_only;
  tolerance_only.bone_name = "near_tolerance_halo";
  tolerance_only.depth = 1.0f;
  tolerance_only.depths = {1.0f, 1.0f, 1.0f, 1.0f};
  tolerance_only.xy = {102.0f, 95.0f, 112.0f, 95.0f,
                       112.0f, 105.0f, 102.0f, 105.0f};
  xpbd::render::ProjectedFace exact_visible;
  exact_visible.bone_name = "exact_visible_face";
  exact_visible.depth = 10.0f;
  exact_visible.depths = {10.0f, 10.0f, 10.0f, 10.0f};
  exact_visible.xy = {90.0f, 90.0f, 101.0f, 90.0f,
                      101.0f, 110.0f, 90.0f, 110.0f};
  overlap.faces = {tolerance_only, exact_visible};
  expect(xpbd::render::pickBone(overlap, 100.0f, 100.0f, 6.0f) ==
             "exact_visible_face",
         "visible face hit wins over a nearer neighbour's tolerance halo");

  xpbd::render::SkeletonDrawList edge_fallback;
  auto cursor_far = tolerance_only;
  cursor_far.bone_name = "near_depth_far_cursor";
  cursor_far.xy = {103.0f, 95.0f, 113.0f, 95.0f,
                   113.0f, 105.0f, 103.0f, 105.0f};
  auto cursor_close = tolerance_only;
  cursor_close.bone_name = "far_depth_close_cursor";
  cursor_close.depth = 10.0f;
  cursor_close.depths = {10.0f, 10.0f, 10.0f, 10.0f};
  cursor_close.xy = {101.0f, 95.0f, 111.0f, 95.0f,
                     111.0f, 105.0f, 101.0f, 105.0f};
  edge_fallback.faces = {cursor_far, cursor_close};
  expect(xpbd::render::pickBone(edge_fallback, 100.0f, 100.0f, 6.0f) ==
             "far_depth_close_cursor",
         "edge fallback follows cursor distance before depth");
}

void testCanonicalCubeAndRtSceneRecords() {
  xpbd::loader::Geometry geometry;
  geometry.description.has_texture_width = true;
  geometry.description.has_texture_height = true;
  geometry.description.texture_width = 16;
  geometry.description.texture_height = 16;
  xpbd::loader::Bone root;
  root.name = "root";
  xpbd::loader::Cube root_cube;
  root_cube.uv_mode = xpbd::loader::CubeUVMode::Box;
  root_cube.uv_box[0] = 0.0;
  root_cube.uv_box[1] = 0.0;
  root.cubes.push_back(root_cube);
  geometry.bones.push_back(root);

  xpbd::baker::BoneMapper mapper;
  mapper.replaceModelBones(geometry.bones);
  xpbd::gfx::ViewportMeshBuilder builder;
  builder.setGeometry(&geometry);
  builder.setBoneMapper(&mapper);
  xpbd::gfx::StaticIndexedModelMesh mesh;
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.cube_count == 1u && mesh.faces.size() == 6u,
         "canonical non-degenerate cube emits six faces");
  expect(mesh.vertices.size() == 24u && mesh.indices.size() == 36u,
         "canonical cube emits 24 face-split vertices and 12 triangles");
  bool flat_normals = true;
  for (const auto &face : mesh.faces) {
    for (std::uint32_t vertex = 1u; vertex < face.vertex_count; ++vertex) {
      const auto &first = mesh.vertices[face.first_vertex];
      const auto &next = mesh.vertices[face.first_vertex + vertex];
      flat_normals =
          flat_normals &&
          std::abs(first.nx - next.nx) < 1.0e-6f &&
          std::abs(first.ny - next.ny) < 1.0e-6f &&
          std::abs(first.nz - next.nz) < 1.0e-6f;
    }
  }
  expect(flat_normals, "canonical cube preserves one flat normal per face");

  xpbd::loader::Geometry transformed_geometry;
  transformed_geometry.description.has_texture_width = true;
  transformed_geometry.description.has_texture_height = true;
  transformed_geometry.description.texture_width = 16;
  transformed_geometry.description.texture_height = 16;
  xpbd::loader::Bone transformed_bone;
  transformed_bone.name = "transformed";
  xpbd::loader::Cube transformed_cube;
  transformed_cube.origin[0] = 1.0;
  transformed_cube.origin[1] = 2.0;
  transformed_cube.origin[2] = 3.0;
  transformed_cube.size[0] = 2.0;
  transformed_cube.size[1] = 4.0;
  transformed_cube.size[2] = 6.0;
  transformed_cube.inflate = 0.5;
  transformed_cube.has_pivot = true;
  transformed_cube.pivot[0] = 2.0;
  transformed_cube.pivot[1] = 4.0;
  transformed_cube.pivot[2] = 6.0;
  transformed_cube.has_rotation = true;
  transformed_cube.rotation[0] = 15.0;
  transformed_cube.rotation[1] = 30.0;
  transformed_cube.rotation[2] = 45.0;
  transformed_cube.uv_mode = xpbd::loader::CubeUVMode::Box;
  transformed_cube.uv_box[0] = 1.0;
  transformed_cube.uv_box[1] = 2.0;
  transformed_cube.mirror = true;
  transformed_bone.cubes.push_back(transformed_cube);
  transformed_geometry.bones.push_back(transformed_bone);
  xpbd::baker::BoneMapper transformed_mapper;
  transformed_mapper.replaceModelBones(transformed_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder transformed_builder;
  transformed_builder.setGeometry(&transformed_geometry);
  transformed_builder.setBoneMapper(&transformed_mapper);
  xpbd::gfx::StaticIndexedModelMesh transformed_mesh;
  transformed_builder.buildStaticIndexedModel(transformed_mesh);
  const auto transformed_bind =
      xpbd::baker::CubeGeometry::bindVertices(transformed_cube);
  bool vertices_match_bind = transformed_mesh.faces.size() == 6u;
  for (const auto &vertex : transformed_mesh.vertices) {
    bool matched = false;
    for (std::size_t source = 0; source < 8u; ++source) {
      matched = matched ||
                (std::abs(vertex.px -
                          transformed_bind[source * 3u + 0u]) < 1.0e-5 &&
                 std::abs(vertex.py -
                          transformed_bind[source * 3u + 1u]) < 1.0e-5 &&
                 std::abs(vertex.pz -
                          transformed_bind[source * 3u + 2u]) < 1.0e-5);
    }
    vertices_match_bind = vertices_match_bind && matched;
  }
  expect(vertices_match_bind,
         "canonical tessellator preserves pivot/rotation/inflate vertices");
  expect(transformed_mesh.vertices[0].u >
             transformed_mesh.vertices[1].u &&
             transformed_mesh.vertices[0].tangent_handedness < 0.0f,
         "mirrored Box UV reverses U and tangent handedness");

  xpbd::loader::Geometry per_face_geometry;
  per_face_geometry.description.has_texture_width = true;
  per_face_geometry.description.has_texture_height = true;
  per_face_geometry.description.texture_width = 16;
  per_face_geometry.description.texture_height = 16;
  xpbd::loader::Bone per_face_bone;
  per_face_bone.name = "per_face";
  xpbd::loader::Cube per_face_cube;
  per_face_cube.uv_mode = xpbd::loader::CubeUVMode::PerFace;
  per_face_cube.uv_north = {4.0, 5.0, 2.0, 3.0, true};
  per_face_cube.uv_north.rotation_degrees = 90;
  per_face_bone.cubes.push_back(per_face_cube);
  per_face_geometry.bones.push_back(per_face_bone);
  xpbd::baker::BoneMapper per_face_mapper;
  per_face_mapper.replaceModelBones(per_face_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder per_face_builder;
  per_face_builder.setGeometry(&per_face_geometry);
  per_face_builder.setBoneMapper(&per_face_mapper);
  xpbd::gfx::StaticIndexedModelMesh per_face_mesh;
  per_face_builder.buildStaticIndexedModel(per_face_mesh);
  std::size_t textured_face_count = 0u;
  for (const auto &face : per_face_mesh.faces) {
    textured_face_count += face.textured ? 1u : 0u;
  }
  const auto north_it = std::find_if(
      per_face_mesh.faces.begin(), per_face_mesh.faces.end(),
      [](const xpbd::gfx::StaticModelFace &face) {
        return face.direction ==
               xpbd::gfx::StaticModelFaceDirection::North;
      });
  const xpbd::gfx::StaticModelFace *north_face =
      north_it != per_face_mesh.faces.end() ? &*north_it : nullptr;
  expect(per_face_mesh.faces.size() == 1u &&
             textured_face_count == 1u &&
             north_face != nullptr && north_face->textured,
         "per-face UV omits unauthored faces and keeps authored face");
  if (north_face != nullptr) {
    expectNear(per_face_mesh.vertices[north_face->first_vertex].u,
               4.0f / 16.0f, 1.0e-6f,
               "per-face UV rotation preserves authored U edge");
    expectNear(per_face_mesh.vertices[north_face->first_vertex].v,
               8.0f / 16.0f, 1.0e-6f,
               "per-face UV rotation maps the clockwise source corner");
  }

  xpbd::loader::Geometry flat_geometry = per_face_geometry;
  flat_geometry.bones[0].name = "flat_face";
  flat_geometry.bones[0].cubes[0].size[1] = 0.0;
  flat_geometry.bones[0].cubes[0].uv_north = {};
  flat_geometry.bones[0].cubes[0].uv_south = {};
  flat_geometry.bones[0].cubes[0].uv_up = {4.0, 5.0, 2.0, 3.0, true};
  flat_geometry.bones[0].cubes[0].uv_down = {4.0, 5.0, 2.0, 3.0, true};
  xpbd::baker::BoneMapper flat_mapper;
  flat_mapper.replaceModelBones(flat_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder flat_builder;
  flat_builder.setGeometry(&flat_geometry);
  flat_builder.setBoneMapper(&flat_mapper);
  xpbd::gfx::StaticIndexedModelMesh flat_mesh;
  flat_builder.buildStaticIndexedModel(flat_mesh);
  expect(flat_mesh.faces.size() == 1u &&
             flat_mesh.faces.front().direction ==
                 xpbd::gfx::StaticModelFaceDirection::Up,
         "zero-thickness UV cube removes coincident opposite face");
  if (!flat_mesh.faces.empty()) {
    const auto first_vertex = flat_mesh.faces.front().first_vertex;
    expectNear(flat_mesh.vertices[first_vertex].u, 6.0f / 16.0f, 1.0e-6f,
               "Bedrock Up UV starts at the opposite U corner");
    expectNear(flat_mesh.vertices[first_vertex].v, 8.0f / 16.0f, 1.0e-6f,
               "Bedrock Up UV starts at the opposite V corner");
  }

  xpbd::loader::Geometry eye_plane_geometry;
  eye_plane_geometry.description.has_texture_width = true;
  eye_plane_geometry.description.has_texture_height = true;
  eye_plane_geometry.description.texture_width = 256;
  eye_plane_geometry.description.texture_height = 256;
  xpbd::loader::Bone eye_plane_bone;
  eye_plane_bone.name = "eye_plane";
  xpbd::loader::Cube eye_plane_cube;
  eye_plane_cube.size[0] = 1.25;
  eye_plane_cube.size[1] = 0.7;
  eye_plane_cube.size[2] = 0.0;
  eye_plane_cube.uv_mode = xpbd::loader::CubeUVMode::PerFace;
  eye_plane_cube.uv_north = {9.0, 128.0, 1.0, 1.0, true};
  eye_plane_cube.uv_south = {128.0, 9.0, 1.0, 1.0, true};
  eye_plane_bone.cubes.push_back(eye_plane_cube);
  eye_plane_geometry.bones.push_back(eye_plane_bone);
  xpbd::baker::BoneMapper eye_plane_mapper;
  eye_plane_mapper.replaceModelBones(eye_plane_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder eye_plane_builder;
  eye_plane_builder.setGeometry(&eye_plane_geometry);
  eye_plane_builder.setBoneMapper(&eye_plane_mapper);
  xpbd::gfx::StaticIndexedModelMesh eye_plane_mesh;
  eye_plane_builder.buildStaticIndexedModel(eye_plane_mesh);
  expect(eye_plane_mesh.faces.size() == 1u &&
             eye_plane_mesh.faces.front().direction ==
                 xpbd::gfx::StaticModelFaceDirection::North,
         "zero-Z eye plane retains its authored North/front UV face");
  if (!eye_plane_mesh.faces.empty()) {
    const auto first_vertex = eye_plane_mesh.faces.front().first_vertex;
    expectNear(eye_plane_mesh.vertices[first_vertex].u, 9.0f / 256.0f,
               1.0e-6f,
               "zero-Z eye plane retains the North/front U coordinate");
    expectNear(eye_plane_mesh.vertices[first_vertex].v, 128.0f / 256.0f,
               1.0e-6f,
               "zero-Z eye plane retains the North/front V coordinate");
  }

  xpbd::gfx::TextureImage base_atlas;
  base_atlas.width = 1;
  base_atlas.height = 1;
  base_atlas.source_channels = 4;
  base_atlas.rgba = {255u, 255u, 255u, 255u};
  const auto draw_plan =
      xpbd::gfx::makeStaticModelDrawPlan(mesh, &base_atlas);
  expect(draw_plan.primitive_materials.size() == 12u,
         "draw plan keeps metadata for every canonical cube triangle");
  bool source_identity = true;
  for (std::size_t primitive = 0;
       primitive < draw_plan.primitive_materials.size(); ++primitive) {
    const auto &source = draw_plan.primitive_materials[primitive];
    source_identity =
        source_identity && source.source_face_index == primitive / 2u &&
        source.source_triangle_index == primitive % 2u &&
        source.bone_index == 0u && source.cube_index == 0u &&
        source.face_direction ==
            static_cast<xpbd::gfx::StaticModelFaceDirection>(
                primitive / 2u);
  }
  expect(source_identity,
         "draw-plan reordering metadata preserves primitive source identity");

  xpbd::gfx::ResolvedMaterialTable emissive_material;
  emissive_material.specular_map_active = true;
  auto records = xpbd::gfx::buildRigidModelRtSceneRecords(
      mesh, draw_plan, &emissive_material);
  expect(records.valid() && records.geometries.size() == 1u &&
             records.instances.size() == 1u &&
             records.primitives.size() == 12u,
         "one rigid bone produces one geometry, instance, and 12 records");
  expect(records.geometries[0].blas_policy ==
             xpbd::gfx::RtBlasPolicy::RigidLocalSpace &&
             records.geometries[0].local_space &&
             !records.geometries[0].dynamic_vertices,
         "rigid cube group selects immutable local-space BLAS policy");
  expect(records.instances[0].instance_custom_index == 0u &&
             records.primitives[0].uses_emission_texture,
         "stable instance id and read-only emission metadata are retained");
  expect(records.materials.size() == 1u &&
             records.materials[0].read_only &&
             records.materials[0].feature_flags ==
                  xpbd::gfx::kLabPbrSpecularMapActive &&
             records.materials[0].surface_optics.transmission == 0.0f,
         "RT scene exposes one read-only, optics-inert resolved-material record");
  xpbd::gfx::RtSurfaceOptics optics_override;
  optics_override.transmission = 0.75f;
  optics_override.ior = 1.33f;
  optics_override.attenuation_color = {0.8f, 0.9f, 1.0f};
  optics_override.attenuation_distance = 4.0f;
  optics_override.thin_walled = true;
  const auto optical_records = xpbd::gfx::buildRigidModelRtSceneRecords(
      mesh, draw_plan, &emissive_material, &optics_override);
  expect(optical_records.valid() && optical_records.materials.size() == 1u &&
             std::abs(optical_records.materials[0]
                          .surface_optics.transmission -
                      0.75f) < 1.0e-6f &&
             std::abs(optical_records.materials[0].surface_optics.ior -
                      1.33f) < 1.0e-6f &&
             optical_records.materials[0].surface_optics.thin_walled,
         "explicit fixture override can populate optics without deriving it from Base Alpha");
  const auto packed_layout =
      xpbd::gfx::buildRtPackedPrimitiveLayout(draw_plan, records);
  expect(packed_layout.valid() &&
             packed_layout.geometry_ranges.size() == 1u &&
             packed_layout.geometry_ranges[0].first_index == 0u &&
             packed_layout.geometry_ranges[0].index_count == 36u &&
             packed_layout.source_primitive_indices.size() == 12u,
         "single-bone RT primitive packing is dense and complete");

  std::vector<xpbd::gfx::StaticModelBoneState> transforms(1);
  transforms[0].transform[12] = 2.0f;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             records, transforms, true),
         "rigid instance transform initializes with reset history");
  expectNear(records.instances[0].current_transform[12], 2.0f, 1.0e-6f,
              "rigid instance current transform uses bone transform");
  expectNear(records.instances[0].previous_transform[12], 2.0f, 1.0e-6f,
              "reset rigid history copies current into previous");
  expect(records.instances[0].visibility_mask == 0xFFu,
         "visible rigid bone resolves to full TLAS instance mask");
  transforms[0].tint[3] = 0.0f;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             records, transforms, false) &&
             records.instances[0].visibility_mask == 0u,
         "hidden rigid bone resolves to zero TLAS instance mask");
  expectNear(xpbd::gfx::rtEmitterVisibilityScale(transforms[0].tint[3]),
             0.0f, 0.0f,
             "hidden rigid bone contributes no mesh-light weight");
  const auto hidden_emitter_audit = xpbd::gfx::auditRtEmitterVisibility(
      xpbd::gfx::rtEmitterVisibilityScale(transforms[0].tint[3]), true, 0.0);
  expect(hidden_emitter_audit.hidden_source_emitter &&
             !hidden_emitter_audit.hidden_positive_weight,
         "hidden authored emitter is observed with zero final weight");
  const auto hidden_weight_violation =
      xpbd::gfx::auditRtEmitterVisibility(0.0f, true, 0.25);
  expect(hidden_weight_violation.hidden_source_emitter &&
             hidden_weight_violation.hidden_positive_weight,
         "visibility audit exposes a hidden positive-weight violation");
  const auto visible_emitter_audit =
      xpbd::gfx::auditRtEmitterVisibility(1.0f, true, 0.25);
  expect(!visible_emitter_audit.hidden_source_emitter &&
             !visible_emitter_audit.hidden_positive_weight,
         "visible emitter is excluded from hidden-only diagnostics");
  transforms[0].tint[3] = 1.0f;
  transforms[0].transform[12] = 4.0f;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             records, transforms, false),
         "rigid instance transform advances without BLAS mutation");
  expectNear(records.instances[0].previous_transform[12], 2.0f, 1.0e-6f,
             "rigid instance keeps previous transform for motion");
  expectNear(records.instances[0].current_transform[12], 4.0f, 1.0e-6f,
              "rigid instance installs next current transform");
  expect(records.instances[0].visibility_mask == 0xFFu &&
             xpbd::gfx::rtInstanceVisibilityMask(
                 std::numeric_limits<float>::quiet_NaN()) == 0u,
         "unhide restores full mask and invalid alpha remains hidden");

  xpbd::loader::Bone child;
  child.name = "child";
  xpbd::loader::Cube child_cube;
  child_cube.uv_mode = xpbd::loader::CubeUVMode::Box;
  child_cube.uv_box[0] = 0.0;
  child_cube.uv_box[1] = 0.0;
  child.cubes.push_back(child_cube);
  auto two_bone_geometry = geometry;
  two_bone_geometry.bones[0].rotation[1] = 90.0;
  child.has_parent = true;
  child.parent = "root";
  two_bone_geometry.bones.push_back(child);
  xpbd::baker::BoneMapper two_bone_mapper;
  two_bone_mapper.replaceModelBones(two_bone_geometry.bones);
  xpbd::gfx::ViewportMeshBuilder two_bone_builder;
  two_bone_builder.setGeometry(&two_bone_geometry);
  two_bone_builder.setBoneMapper(&two_bone_mapper);
  xpbd::gfx::StaticIndexedModelMesh two_bone_mesh;
  two_bone_builder.buildStaticIndexedModel(two_bone_mesh);
  xpbd::gfx::StaticModelFrameData hierarchy_frame;
  two_bone_builder.buildStaticRestFrame(hierarchy_frame);
  bool inherited_parent_rotation = hierarchy_frame.bones.size() == 2u;
  for (const std::size_t element :
       {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u}) {
    inherited_parent_rotation =
        inherited_parent_rotation &&
        std::abs(hierarchy_frame.bones[0].transform[element] -
                 hierarchy_frame.bones[1].transform[element]) < 1.0e-5f;
  }
  inherited_parent_rotation =
      inherited_parent_rotation &&
      std::abs(hierarchy_frame.bones[1].transform[0] - 1.0f) > 0.5f;
  expect(inherited_parent_rotation,
         "child TLAS transform inherits parent rest rotation");
  const auto two_bone_plan =
      xpbd::gfx::makeStaticModelDrawPlan(two_bone_mesh, &base_atlas);
  auto two_bone_records =
      xpbd::gfx::buildRigidModelRtSceneRecords(
          two_bone_mesh, two_bone_plan, &emissive_material);
  const auto two_bone_packed =
      xpbd::gfx::buildRtPackedPrimitiveLayout(two_bone_plan,
                                               two_bone_records);
  expect(two_bone_records.valid() &&
             two_bone_records.geometries.size() == 2u &&
             two_bone_records.instances[0].instance_custom_index == 0u &&
             two_bone_records.instances[1].instance_custom_index == 1u,
         "two rigid bones produce stable dense TLAS instance ids");
  expect(two_bone_packed.valid() &&
             two_bone_packed.geometry_ranges.size() == 2u &&
             two_bone_packed.geometry_ranges[0].first_index == 0u &&
             two_bone_packed.geometry_ranges[0].index_count == 36u &&
             two_bone_packed.geometry_ranges[1].first_index == 36u &&
             two_bone_packed.geometry_ranges[1].index_count == 36u &&
             two_bone_packed.indices.size() == 72u,
         "per-bone BLAS ranges repack all primitives without omission");
  const auto child_first_identity =
      xpbd::gfx::resolveRtPackedPrimitiveIdentity(
          two_bone_packed, two_bone_records, 1u, 0u);
  const auto child_last_identity =
      xpbd::gfx::resolveRtPackedPrimitiveIdentity(
          two_bone_packed, two_bone_records, 1u, 11u);
  expect(child_first_identity.has_value() &&
             child_first_identity->packed_primitive_index == 12u &&
             child_first_identity->source_primitive_index == 12u &&
             child_first_identity->geometry_index == 1u &&
             child_first_identity->bone_index == 1u &&
             child_first_identity->cube_index == 0u,
         "GPU-style second-instance first hit resolves source cube identity");
  expect(child_last_identity.has_value() &&
             child_last_identity->packed_primitive_index == 23u &&
             child_last_identity->source_primitive_index == 23u &&
             child_last_identity->geometry_index == 1u &&
             child_last_identity->bone_index == 1u,
         "GPU-style second-instance last hit stays within packed range");
  expect(!xpbd::gfx::resolveRtPackedPrimitiveIdentity(
              two_bone_packed, two_bone_records, 2u, 0u)
              .has_value() &&
             !xpbd::gfx::resolveRtPackedPrimitiveIdentity(
                 two_bone_packed, two_bone_records, 1u, 12u)
                 .has_value(),
         "GPU-style identity resolver rejects invalid instance/local indices");

  xpbd::loader::Animation rigid_animation;
  rigid_animation.loop = true;
  rigid_animation.loop_behavior =
      xpbd::loader::Animation::LoopBehavior::Loop;
  rigid_animation.animation_length = 2.0;
  xpbd::loader::BoneAnimation root_motion;
  root_motion.has_rotation = true;
  root_motion.rotation.put(0.0, {0.0, 0.0, 0.0});
  root_motion.rotation.put(1.0, {0.0, 45.0, 0.0});
  root_motion.rotation.put(2.0, {0.0, 0.0, 0.0});
  root_motion.setLooping(true);
  rigid_animation.bones.emplace("root", std::move(root_motion));

  xpbd::gfx::StaticModelFrameData rigid_frame_zero;
  xpbd::gfx::StaticModelFrameData rigid_frame_one;
  two_bone_builder.buildStaticAnimationFrame(&rigid_animation, 0.0,
                                             rigid_frame_zero);
  two_bone_builder.buildStaticAnimationFrame(&rigid_animation, 1.0,
                                             rigid_frame_one);
  expect(rigid_frame_zero.bones.size() == 2u &&
             rigid_frame_one.bones.size() == 2u &&
             rigid_frame_zero.bones[0].transform !=
                 rigid_frame_one.bones[0].transform &&
             rigid_frame_zero.bones[1].transform !=
                 rigid_frame_one.bones[1].transform,
         "numeric live animation changes parent and inherited child transforms");
  const auto rigid_local_vertices = two_bone_mesh.vertices;
  expect(xpbd::gfx::updateRigidRtInstanceTransforms(
             two_bone_records, rigid_frame_zero.bones, true) &&
             xpbd::gfx::updateRigidRtInstanceTransforms(
                 two_bone_records, rigid_frame_one.bones, false),
         "live rigid animation advances TLAS transform history");
  expect(two_bone_records.instances[0].previous_transform !=
                 two_bone_records.instances[0].current_transform &&
             two_bone_records.instances[1].previous_transform !=
                 two_bone_records.instances[1].current_transform,
         "live rigid animation keeps distinct current and previous transforms");
  bool rigid_local_positions_unchanged =
      two_bone_mesh.vertices.size() == rigid_local_vertices.size();
  for (std::size_t vertex = 0;
       rigid_local_positions_unchanged &&
       vertex < two_bone_mesh.vertices.size();
       ++vertex) {
    rigid_local_positions_unchanged =
        two_bone_mesh.vertices[vertex].px ==
            rigid_local_vertices[vertex].px &&
        two_bone_mesh.vertices[vertex].py ==
            rigid_local_vertices[vertex].py &&
        two_bone_mesh.vertices[vertex].pz ==
            rigid_local_vertices[vertex].pz;
  }
  expect(rigid_local_positions_unchanged,
         "live rigid animation leaves canonical BLAS-local vertices unchanged");

  geometry.bones[0].cubes.push_back(root_cube);
  mapper.replaceModelBones(geometry.bones);
  builder.setGeometry(&geometry);
  builder.setBoneMapper(&mapper);
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.faces.size() == 12u && mesh.indices.size() == 72u,
         "overlapping opaque source cubes retain all internal faces");

  xpbd::loader::Geometry degenerate_geometry;
  xpbd::loader::Bone degenerate_bone;
  degenerate_bone.name = "flat";
  xpbd::loader::Cube degenerate_cube;
  degenerate_cube.size[0] = 0.0;
  degenerate_bone.cubes.push_back(degenerate_cube);
  degenerate_geometry.bones.push_back(degenerate_bone);
  mapper.replaceModelBones(degenerate_geometry.bones);
  builder.setGeometry(&degenerate_geometry);
  builder.setBoneMapper(&mapper);
  builder.buildStaticIndexedModel(mesh);
  expect(mesh.cube_count == 1u && mesh.faces.size() == 1u &&
             mesh.indices.size() == 6u,
         "degenerate flat cube counts source but emits one two-sided face");
}

void testRayTracingCapability() {
  using xpbd::gfx::evaluateRayTracingCapability;
  using xpbd::gfx::isNvidiaVendorId;
  using xpbd::gfx::isNvidiaRtx20OrNewer;
  using xpbd::gfx::kVendorIdNvidia;
  using xpbd::gfx::RenderPath;
  using xpbd::gfx::resolveRenderPath;
  using xpbd::gfx::clampRayTracingPreference;

  expect(isNvidiaVendorId(kVendorIdNvidia), "NVIDIA vendor id match");
  expect(!isNvidiaVendorId(0x1002u), "AMD vendor is not NVIDIA");
  expect(!isNvidiaVendorId(0x8086u), "Intel vendor is not NVIDIA");
  expect(isNvidiaRtx20OrNewer(0x1E04u, "NVIDIA GeForce RTX 2080"),
         "RTX 20-series is eligible for path tracing");
  expect(isNvidiaRtx20OrNewer(0x2684u, "NVIDIA GeForce RTX 4090"),
         "RTX 40-series is eligible for path tracing");
  expect(!isNvidiaRtx20OrNewer(0x2184u, "NVIDIA GeForce GTX 1660 Ti"),
         "GTX 16-series is not eligible for path tracing");

  auto amd = evaluateRayTracingCapability(0x1002u, 1, "Radeon", true, true, 1);
  expect(!amd.supported, "AMD with RT exts still unsupported (NVIDIA-only)");
  expect(!amd.unsupported_reason.empty(), "AMD has reason string");

  auto nv_no_ext =
      evaluateRayTracingCapability(kVendorIdNvidia, 1, "RTX", false, true, 1);
  expect(!nv_no_ext.supported, "NVIDIA without RT extensions unsupported");

  auto nv_no_feat =
      evaluateRayTracingCapability(kVendorIdNvidia, 1, "RTX", true, false, 1);
  expect(!nv_no_feat.supported, "NVIDIA without RT features unsupported");

  auto nv_ok =
      evaluateRayTracingCapability(kVendorIdNvidia, 1, "RTX 3080", true, true,
                                   31, 0, 0);
  expect(nv_ok.supported, "NVIDIA with RT extensions+features supported");
  expect(nv_ok.is_nvidia, "flag is_nvidia");
  expect(nv_ok.max_ray_recursion_depth == 31u, "recursion depth stored");
  auto nv_old_generation = evaluateRayTracingCapability(
      kVendorIdNvidia, 0x2184u, "NVIDIA GeForce GTX 1660 Ti", true, true, 31,
      0, 0);
  expect(!nv_old_generation.supported,
         "pre-RTX-20 NVIDIA hardware cannot enable path tracing");
  expect(nv_old_generation.unsupported_reason.find("RTX 20-series") !=
             std::string::npos,
         "pre-RTX-20 reason names the minimum generation");

  expect(resolveRenderPath(false, nv_ok) == RenderPath::Raster,
         "default path is raster");
  expect(resolveRenderPath(true, nv_ok) == RenderPath::Raster,
         "RT preference without device_extensions_enabled stays raster");
  nv_ok.device_extensions_enabled = true;
  expect(resolveRenderPath(true, nv_ok) == RenderPath::RayTracing,
         "RT preference with armed device uses RT path");
  expect(resolveRenderPath(true, amd) == RenderPath::Raster,
         "unsupported GPU falls back to raster");

  expect(!clampRayTracingPreference(true, false),
         "clamp clears preference when HW unsupported");
  expect(clampRayTracingPreference(true, true),
         "clamp keeps preference when HW supported");
  expect(!clampRayTracingPreference(false, true),
         "clamp keeps default-off preference");
}

void testFrameGenerationStateLegality() {
  using xpbd::gfx::frameGenerationAcquireMustPrecedeFrame;
  using xpbd::gfx::frameGenerationCurrentFrameInputsReady;
  using xpbd::gfx::frameGenerationDisableAttemptAllowed;
  using xpbd::gfx::frameGenerationDisableMayDestroy;
  using xpbd::gfx::frameGenerationInputClearMayArmProxy;
  using xpbd::gfx::frameGenerationRecordDisableAttempt;
  using xpbd::gfx::frameGenerationRecordDisableCleanupFailure;
  using xpbd::gfx::frameGenerationRecordDisableDrain;
  using xpbd::gfx::frameGenerationRuntimeCombinationIsLegal;
  using xpbd::gfx::frameGenerationStateAfterValidInputTagging;
  using xpbd::gfx::frameGenerationTemporalInputIsReady;
  using xpbd::gfx::frameGenerationViewportIsValid;
  using xpbd::gfx::makeFrameGenerationTagExtent;
  using xpbd::gfx::FrameGenerationDisableProgress;
  using xpbd::gfx::FrameGenerationRuntimeState;
  using xpbd::gfx::SwapchainOwnership;
  constexpr SwapchainOwnership native = SwapchainOwnership::Native;
  constexpr SwapchainOwnership proxy =
      SwapchainOwnership::StreamlineFrameGenerationProxy;

  expect(!frameGenerationAcquireMustPrecedeFrame(native),
         "native acquire may wait at first swapchain image use");
  expect(frameGenerationAcquireMustPrecedeFrame(proxy),
         "DLSS-G acquire handoff blocks all work at frame start");

  constexpr auto asymmetric_extent =
      makeFrameGenerationTagExtent(991u, 37u, 289u, 763u);
  expect(asymmetric_extent.top == 37u &&
             asymmetric_extent.left == 991u &&
             asymmetric_extent.width == 289u &&
             asymmetric_extent.height == 763u,
         "DLSS-G maps viewport y/x to Streamline top/left order");
  expect(frameGenerationViewportIsValid(
             1280u, 800u, 991u, 37u, 289u, 763u),
         "DLSS-G accepts an asymmetric subrect touching right/bottom edges");
  expect(!frameGenerationViewportIsValid(
             1280u, 800u, 992u, 37u, 289u, 763u),
         "DLSS-G rejects a subrect beyond the right edge");
  expect(!frameGenerationViewportIsValid(
             (std::numeric_limits<std::uint32_t>::max)(), 800u,
             (std::numeric_limits<std::uint32_t>::max)() - 3u,
             37u, 8u, 100u),
         "DLSS-G viewport validation rejects unsigned overflow");
  expect(frameGenerationTemporalInputIsReady(false, false),
         "DLSS-G accepts native full-resolution input without reconstruction");
  expect(frameGenerationTemporalInputIsReady(true, true),
         "DLSS-G accepts a successful temporal reconstruction output");
  expect(!frameGenerationTemporalInputIsReady(true, false),
         "DLSS-G rejects raw fallback after temporal reconstruction failure");

  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::Unsupported, false, native, false,
             false),
         "DLSS-G unsupported state owns only a Native swapchain");
  expect(!frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::Unsupported, true, native, false,
             false),
         "DLSS-G unsupported state rejects a loaded plugin");
  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::EnablingCreateProxySwapchain, true,
             native, false, false),
         "DLSS-G enable creates proxy only after plugin load");
  expect(!frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::EnablingCreateProxySwapchain,
             false, native, false, false),
         "DLSS-G enable rejects proxy creation before plugin load");
  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::ProxyArmed, true, proxy, false,
             false),
         "DLSS-G proxy may arm before the first valid tagged frame");
  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::Active, true, proxy, true, true),
         "DLSS-G Active requires proxy, options, and valid tags");
  expect(!frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::Active, true, native, true, true),
         "DLSS-G Active rejects Native swapchain ownership");
  expect(!frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::Active, true, proxy, false, true),
         "DLSS-G Active rejects disabled options");
  expect(!frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::Active, true, proxy, true, false),
         "DLSS-G Active rejects null resource tags");
  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::DisablingOptions, false, native,
             false, false),
         "DLSS-G Native resize uses an idempotent disable transaction");
  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::DisablingDrain, true, proxy, false,
             false),
         "DLSS-G proxy drain begins only after options and tags are cleared");
  expect(!frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::DisablingDrain, true, proxy, true,
             false),
         "DLSS-G drain rejects enabled options");
  expect(frameGenerationRuntimeCombinationIsLegal(
             FrameGenerationRuntimeState::FaultedRecoveringNative, true,
             proxy, true, true),
         "DLSS-G failed disable preserves proxy, options, and tagged inputs");

  FrameGenerationDisableProgress retry_progress{};
  expect(frameGenerationDisableAttemptAllowed(retry_progress),
         "DLSS-G fresh disable transaction permits its first SDK attempt");
  expect(frameGenerationRecordDisableAttempt(retry_progress, false) &&
             retry_progress.attempts == 1u &&
             retry_progress.failure_latched &&
             retry_progress.recovery_required &&
             !retry_progress.confirmed_off,
         "DLSS-G first eOff failure latches recovery without confirmation");
  expect(!frameGenerationDisableAttemptAllowed(retry_progress),
         "DLSS-G cannot busy-retry before a GPU and Present drain");
  expect(!frameGenerationDisableMayDestroy(retry_progress, true, true, true),
         "DLSS-G failed eOff preserves state and blocks proxy destruction");
  expect(frameGenerationRecordDisableDrain(retry_progress) &&
             frameGenerationDisableAttemptAllowed(retry_progress),
         "DLSS-G completed drain permits exactly one retry");
  expect(frameGenerationRecordDisableAttempt(retry_progress, true) &&
             retry_progress.attempts == 2u &&
             retry_progress.confirmed_off &&
             retry_progress.failure_latched,
         "DLSS-G retry may confirm Off while preserving the failure latch");
  expect(!frameGenerationDisableMayDestroy(retry_progress, false, false,
                                           false),
         "DLSS-G successful retry still requires its final drain");
  expect(frameGenerationRecordDisableDrain(retry_progress) &&
             frameGenerationDisableMayDestroy(retry_progress, false, false,
                                               false),
         "DLSS-G confirmed Off with cleared state and final drain may destroy");

  FrameGenerationDisableProgress cleanup_failure = retry_progress;
  frameGenerationRecordDisableCleanupFailure(cleanup_failure);
  expect(cleanup_failure.exhausted && cleanup_failure.failure_latched &&
             cleanup_failure.recovery_required &&
             !frameGenerationDisableMayDestroy(cleanup_failure, false, false,
                                               true),
         "DLSS-G null-tag failure remains a diagnosable destruction blocker");

  FrameGenerationDisableProgress exhausted{};
  expect(frameGenerationRecordDisableAttempt(exhausted, false) &&
             frameGenerationRecordDisableDrain(exhausted) &&
             frameGenerationRecordDisableAttempt(exhausted, false) &&
             frameGenerationRecordDisableDrain(exhausted) &&
             frameGenerationRecordDisableAttempt(exhausted, false),
         "DLSS-G continuous failure consumes three drain-gated attempts");
  expect(exhausted.attempts == 3u && exhausted.exhausted &&
             exhausted.failure_latched && exhausted.recovery_required &&
             !frameGenerationDisableAttemptAllowed(exhausted) &&
             !frameGenerationDisableMayDestroy(exhausted, true, true, true),
         "DLSS-G exhausted transaction stays recoverable but non-destructive");

  expect(frameGenerationCurrentFrameInputsReady(true, true, true, 42u, 42u,
                                                42u),
         "DLSS-G current frame accepts matching options/constants/tags");
  expect(!frameGenerationCurrentFrameInputsReady(false, true, true, 42u, 42u,
                                                 42u),
         "DLSS-G current frame rejects disabled options");
  expect(!frameGenerationCurrentFrameInputsReady(true, false, true, 42u, 42u,
                                                 42u),
         "DLSS-G current frame rejects an invalid options key");
  expect(!frameGenerationCurrentFrameInputsReady(true, true, false, 42u, 42u,
                                                 42u),
         "DLSS-G current frame rejects cleared tags");
  expect(!frameGenerationCurrentFrameInputsReady(true, true, true, 42u, 41u,
                                                 42u),
         "DLSS-G current frame rejects stale constants");
  expect(!frameGenerationCurrentFrameInputsReady(true, true, true, 42u, 42u,
                                                 41u),
         "DLSS-G current frame rejects stale tags");

  expect(frameGenerationStateAfterValidInputTagging(
             FrameGenerationRuntimeState::Active) ==
             FrameGenerationRuntimeState::Active,
         "DLSS-G valid input tagging preserves confirmed Active state");
  expect(frameGenerationStateAfterValidInputTagging(
             FrameGenerationRuntimeState::ProxyArmed) ==
             FrameGenerationRuntimeState::ProxyArmed,
         "DLSS-G valid input tagging keeps an unconfirmed proxy armed");
  expect(frameGenerationInputClearMayArmProxy(
             FrameGenerationRuntimeState::Active),
         "DLSS-G invalid input pauses an Active proxy back to armed");
  expect(frameGenerationInputClearMayArmProxy(
             FrameGenerationRuntimeState::ProxyArmed),
         "DLSS-G invalid input may keep an armed proxy armed");
  expect(!frameGenerationInputClearMayArmProxy(
             FrameGenerationRuntimeState::DisablingDrain),
         "DLSS-G input cleanup cannot overwrite a disabling transaction");
  expect(!frameGenerationInputClearMayArmProxy(
             FrameGenerationRuntimeState::ShuttingDown),
         "DLSS-G input cleanup cannot overwrite shutdown state");
}

void testEnvironmentLightAdoptionSourceContracts() {
  const std::string world_header =
      readTestSource("include/xpbd/gfx/world_environment.hpp");
  const std::string ray_header =
      readTestSource("include/xpbd/gfx/ray_tracing.hpp");
  const std::string stage20 = readTestSource(
      "src/gfx/vulkan_render/render/20_environment_and_rt_scene.inc");
  const std::string stage50 = readTestSource(
      "src/gfx/vulkan_render/render/50_path_trace_and_dlss.inc");
  const std::string stage60 = readTestSource(
      "src/gfx/vulkan_render/render/60_still_render.inc");
  const std::string stage70 = readTestSource(
      "src/gfx/vulkan_render/render/70_main_render_pass.inc");
  const std::string backend_environment = readTestSource(
      "src/gfx/vulkan/vulkan_backend_environment.cpp");
  const std::string rt_scene_header =
      readTestSource("include/xpbd/gfx/vulkan_rt_scene.hpp");
  const std::string rt_scene_source =
      readTestSource("src/gfx/vulkan_rt_scene.cpp");
  const std::string raygen =
      readTestSource("src/gfx/spirv/rt_debug.rgen");
  expect(!world_header.empty() && !ray_header.empty() && !stage20.empty() &&
             !stage50.empty() && !stage60.empty() && !stage70.empty() &&
             !backend_environment.empty() && !rt_scene_header.empty() &&
             !rt_scene_source.empty() && !raygen.empty(),
         "G06 Environment/Light source-contract fixtures are readable");

  const std::string compact_world = compactTestSource(world_header);
  expect(compact_world.find("structResolvedSunLight{") != std::string::npos &&
             compact_world.find("structResolvedEnvironmentView{") !=
                 std::string::npos &&
             compact_world.find("sampleEnvironment(") != std::string::npos &&
             compact_world.find("evaluateEnvironment(") !=
                 std::string::npos &&
             compact_world.find("environmentPdf(") != std::string::npos &&
             compact_world.find("environmentGeneration(") !=
                 std::string::npos,
         "World authoring resolves one read-only Sun and Environment sampling seam");

  expect(ray_header.find("enum class RtLightType") != std::string::npos &&
             ray_header.find("Environment") != std::string::npos &&
             ray_header.find("SunDisk") != std::string::npos &&
             ray_header.find("EmissiveTriangle") != std::string::npos &&
             ray_header.find("struct RtLightRegistry") != std::string::npos &&
             ray_header.find("RtStableLightId") != std::string::npos,
         "minimal Light Registry freezes current families, stable IDs, and reserved ABI");

  const std::string compact_stage20 = compactTestSource(stage20);
  expect(compact_stage20.find("resolveSunLight(resolved_world_environment)") !=
                  std::string::npos &&
             compact_stage20.find(
                 "resolveEnvironmentView(resolved_world_environment,") !=
                  std::string::npos &&
             compact_stage20.find("buildRtLightRegistry(") !=
                 std::string::npos,
         "render stage 20 resolves the sole Sun/Environment and Registry projection once");

  expect(stage50.find("resolved_sun_light") != std::string::npos &&
             stage60.find("resolved_sun_light") != std::string::npos &&
             stage70.find("resolved_sun_light") != std::string::npos &&
             stage50.find("pt_params.sun_radiance") != std::string::npos &&
             stage50.find("pt_params.sun_angular_radius") !=
                 std::string::npos &&
             stage60.find("still_params.sun_radiance") !=
                 std::string::npos &&
             stage60.find("still_params.sun_angular_radius") !=
                 std::string::npos,
         "Preview, Full RT, and Still consume the same resolved finite Sun while the compatibility fields remain separate");

  expect(raygen.find("struct RtLightSample") != std::string::npos &&
             raygen.find("sampleLightRegistry(") != std::string::npos &&
             raygen.find("sampleSunDisk(") != std::string::npos &&
             raygen.find("lightRegistryProbabilities(") !=
                 std::string::npos,
         "RayGen samples Environment, finite SunDisk, and Emissive through one family selector");

  const std::string compact_backend_environment =
      compactTestSource(backend_environment);
  expect(compact_backend_environment.find(
             "static_cast<float>(sun_coverage)*analytic_sun_radiance[channel]") ==
                 std::string::npos &&
             raygen.find("background ? proceduralSunRadiance") !=
                 std::string::npos,
         "Procedural Sun remains background-visible but is not duplicated in the lighting environment distribution");

  const std::string compact_raygen = compactTestSource(raygen);
  expect(rt_scene_header.find("stable_light_id") != std::string::npos &&
             rt_scene_source.find("makeRtEmissiveTriangleStableId") !=
                 std::string::npos &&
             raygen.find("emissiveTriangleTwoSided") != std::string::npos &&
             compact_raygen.find(
                 "abs(dot(emissiveTriangleNormal(entry),-lightDirection))") ==
                 std::string::npos,
         "emissive triangles carry stable identity and explicit sidedness instead of unconditional absolute cosine");
}

void testTransparentGuidePolicySourceContracts() {
  const std::string aov_header =
      readTestSource("include/xpbd/gfx/path_trace_aov.hpp");
  const std::string ray_header =
      readTestSource("include/xpbd/gfx/ray_tracing.hpp");
  const std::string path_tracer_header =
      readTestSource("include/xpbd/gfx/vulkan_path_tracer.hpp");
  const std::string path_tracer_source =
      readTestSource("src/gfx/vulkan_path_tracer.cpp");
  const std::string rt_pipeline_header =
      readTestSource("include/xpbd/gfx/vulkan_rt_pipeline.hpp");
  const std::string rt_pipeline_source =
      readTestSource("src/gfx/vulkan_rt_pipeline.cpp");
  const std::string stage50 = readTestSource(
      "src/gfx/vulkan_render/render/50_path_trace_and_dlss.inc");
  const std::string streamline_header = readTestSource(
      "include/xpbd/gfx/streamline_vulkan_runtime.hpp");
  const std::string streamline_source =
      readTestSource("src/gfx/streamline_vulkan_runtime.cpp");
  const std::string raygen =
      readTestSource("src/gfx/spirv/rt_debug.rgen");
  const std::string any_hit =
      readTestSource("src/gfx/spirv/rt_debug.rahit");
  const std::string shadow_miss =
      readTestSource("src/gfx/spirv/rt_shadow.rmiss");
  expect(!aov_header.empty() && !ray_header.empty() &&
             !path_tracer_header.empty() && !path_tracer_source.empty() &&
             !rt_pipeline_header.empty() && !rt_pipeline_source.empty() &&
             !stage50.empty() && !streamline_header.empty() &&
             !streamline_source.empty() && !raygen.empty() &&
             !any_hit.empty() && !shadow_miss.empty(),
         "G07 transparent guide/mask source-contract fixtures are readable");

  const std::string compact_aov = compactTestSource(aov_header);
  const std::string compact_ray = compactTestSource(ray_header);
  expect(compact_ray.find("enumclassTransparentGuidePolicyV1") !=
                 std::string::npos &&
             compact_ray.find("OpaqueBehind") != std::string::npos &&
             compact_ray.find("FrontCoverage") != std::string::npos &&
             compact_ray.find("ConservativeInvalid") != std::string::npos &&
             compact_ray.find("kTransparentGuidePolicyV1=") !=
                 std::string::npos &&
             compact_ray.find("TransparentGuidePolicyV1::FrontCoverage") !=
                 std::string::npos,
         "TransparentGuidePolicyV1 compares all three policies and freezes FrontCoverage");
  expect(compact_aov.find("TransparencyAndComposition") !=
                 std::string::npos &&
             compact_aov.find("ReactiveMask") != std::string::npos &&
             compact_aov.find("GuideValidity") != std::string::npos &&
             compact_aov.find("kPathTraceTransparencyGuideOutputMask") !=
                 std::string::npos &&
             compact_aov.find("kPathTraceAllOptionalOutputMask==0x7ffffu") !=
                 std::string::npos,
         "three independent single-channel temporal outputs extend the shared mask ABI");

  const std::string compact_path_header =
      compactTestSource(path_tracer_header);
  const std::string compact_path_source =
      compactTestSource(path_tracer_source);
  expect(compact_path_header.find("transparencyAndCompositionImage()") !=
                 std::string::npos &&
             compact_path_header.find("reactiveMaskImage()") !=
                 std::string::npos &&
             compact_path_header.find("guideValidityImage()") !=
                 std::string::npos &&
             compact_path_source.find("VK_FORMAT_R8_UNORM") !=
                 std::string::npos &&
             compact_path_source.find("transparency-and-composition") !=
                 std::string::npos &&
             compact_path_source.find("guide-validity") !=
                 std::string::npos,
         "R8 UNORM masks are lazy exact-extent target-bundle resources with public real-image accessors");

  const std::string compact_raygen = compactTestSource(raygen);
  const std::string compact_pipeline = compactTestSource(rt_pipeline_source);
  expect(compact_raygen.find(
             "layout(set=0,binding=29,r8)uniformimage2DoutputTransparencyAndComposition;") !=
                 std::string::npos &&
             compact_raygen.find(
             "layout(set=0,binding=30,r8)uniformimage2DoutputReactiveMask;") !=
                 std::string::npos &&
             compact_raygen.find(
             "layout(set=0,binding=31,r8)uniformimage2DoutputGuideValidity;") !=
                 std::string::npos &&
             compact_pipeline.find(
                 "std::array<VkDescriptorSetLayoutBinding,32>bindings{};") !=
                 std::string::npos &&
             compact_pipeline.find(
                 "{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,12}") !=
                 std::string::npos,
         "Full RT mask bindings match the Vulkan 32-binding descriptor ABI");
  expect(compact_raygen.find("kTransparentGuidePolicyFrontCoverage") !=
                 std::string::npos &&
             compact_raygen.find("guideValidity") != std::string::npos &&
             compact_raygen.find("transparencyAndComposition") !=
                 std::string::npos &&
             compact_raygen.find("reactiveMask") != std::string::npos &&
             compact_raygen.find("neutralizeRrGuides") !=
                 std::string::npos,
         "the deterministic primary probe owns one guide surface, masks, validity, and neutral RR fallback");

  const std::string compact_stage50 = compactTestSource(stage50);
  expect(compact_stage50.find("kPathTraceSrRequiredOutputMask") !=
                 std::string::npos &&
             compact_stage50.find("kPathTraceRrRequiredOutputMask") !=
                 std::string::npos &&
             compact_stage50.find("pt_temporal_inputs_ready_for_frame") !=
                 std::string::npos &&
             compact_stage50.find("lastOutputWriteMask()") !=
                 std::string::npos &&
             compact_stage50.find("transparency_and_composition_image") !=
                 std::string::npos &&
             compact_stage50.find("reactive_mask_image") !=
                 std::string::npos,
         "SR/RR submit only complete current-frame Depth/Motion/Guide/Mask resources or fall back structurally");

  const std::string compact_streamline_header =
      compactTestSource(streamline_header);
  const std::string compact_streamline_source =
      compactTestSource(streamline_source);
  expect(compact_streamline_header.find(
             "VkImagetransparency_and_composition_image") !=
                 std::string::npos &&
             compact_streamline_header.find("VkImagereactive_mask_image") !=
                 std::string::npos &&
             compact_streamline_source.find(
                 "sl::kBufferTypeTransparencyAndCompositionMaskHint") !=
                 std::string::npos &&
             compact_streamline_source.find(
                 "sl::kBufferTypeReactiveMaskHint") !=
                 std::string::npos &&
             compact_streamline_source.find("VK_FORMAT_R8_UNORM") !=
                 std::string::npos,
         "Streamline receives real current-frame R8 masks through supported SDK tags");

  const std::string compact_any_hit = compactTestSource(any_hit);
  const std::string compact_shadow_miss = compactTestSource(shadow_miss);
  expect(compact_any_hit.find("hashAlphaWord") == std::string::npos &&
             compact_any_hit.find("PrimitiveOpticsBuffer") !=
                 std::string::npos &&
             compact_any_hit.find("coverageVisibility") !=
                 std::string::npos &&
             compact_any_hit.find("physicalTransmission") !=
                 std::string::npos &&
             compact_any_hit.find("shadowPayload.visibility*=") !=
                 std::string::npos &&
             compact_shadow_miss.find("shadowPayload.visibility=1.0;") ==
                 std::string::npos &&
             compact_raygen.find("kRngDomainAlphaCoverage") !=
                 std::string::npos,
         "Cutout stays binary while deterministic coverage shadow visibility remains separate from physical transmission and beauty RNG");
}

void testFrameGenerationDisableSourceContracts() {
  const std::string header = readTestSource(
      "include/xpbd/gfx/streamline_vulkan_runtime.hpp");
  const std::string state = readTestSource(
      "include/xpbd/gfx/frame_generation_state.hpp");
  const std::string runtime = readTestSource(
      "src/gfx/streamline_vulkan_runtime.cpp");
  const std::string backend = readTestSource("src/gfx/vulkan_backend.cpp");
  const std::string backend_internal = readTestSource(
      "src/gfx/vulkan/vulkan_backend_internal.hpp");
  const std::string frame_generation = readTestSource(
      "src/gfx/vulkan_render/render/80_frame_generation.inc");
  const std::string frame_entry = readTestSource(
      "src/gfx/vulkan_render/render/00_frame_entry.inc");
  const std::string swapchain_wait = readTestSource(
      "src/gfx/vulkan_render/render/10_swapchain_and_frame_wait.inc");
  const std::string present = readTestSource(
      "src/gfx/vulkan_render/render/90_submit_present_and_finalize.inc");
  const std::string app_main = readTestSource("src/app/main_sdl3.cpp");
  expect(!header.empty() && !state.empty() && !runtime.empty() &&
             !backend.empty() && !backend_internal.empty() &&
             !frame_generation.empty() && !frame_entry.empty() &&
             !swapchain_wait.empty() && !present.empty() &&
             !app_main.empty(),
         "DLSS-G disable-transaction source fixtures are readable");

  const std::string compact_header = compactTestSource(header);
  const std::string compact_state = compactTestSource(state);
  const std::string compact_runtime = compactTestSource(runtime);
  const std::string compact_backend = compactTestSource(backend);
  const std::string compact_backend_internal =
      compactTestSource(backend_internal);
  const std::string compact_frame_generation =
      compactTestSource(frame_generation);
  const std::string compact_frame_entry = compactTestSource(frame_entry);
  const std::string compact_swapchain_wait =
      compactTestSource(swapchain_wait);
  const std::string compact_present = compactTestSource(present);
  const std::string compact_app_main = compactTestSource(app_main);

  expect(
      compact_header.find(
          "[[nodiscard]]boolbeginFrameGenerationShutdown(") !=
              std::string::npos &&
          compact_header.find(
              "[[nodiscard]]boolretryFrameGenerationDisableAfterDrain(") !=
              std::string::npos &&
          compact_header.find(
              "[[nodiscard]]boolnotifyFrameGenerationSwapchainDestroyed(") !=
              std::string::npos &&
          compact_header.find(
              "[[nodiscard]]boolclearFrameGenerationInputs(") !=
              std::string::npos &&
          compact_header.find(
              "[[nodiscard]]booldisableFrameGeneration()noexcept;") !=
              std::string::npos,
      "DLSS-G Options-Off and destruction APIs return checked results");
  expect(
      compact_state.find("kFrameGenerationDisableMaxAttempts=3u") !=
              std::string::npos &&
          compact_state.find("frameGenerationRecordDisableAttempt(") !=
              std::string::npos &&
          compact_state.find("frameGenerationRecordDisableDrain(") !=
              std::string::npos &&
          compact_state.find("frameGenerationDisableMayDestroy(") !=
              std::string::npos,
      "DLSS-G disable policy is bounded, drain-gated, and destruction-safe");
  expect(
      compact_runtime.find(
          "boolStreamlineVulkanRuntime::disableFrameGeneration()noexcept") !=
              std::string::npos &&
          compact_runtime.find("frameGenerationRecordDisableAttempt(") !=
              std::string::npos &&
          compact_runtime.find("frameGenerationRecordDisableDrain(") !=
              std::string::npos &&
          compact_runtime.find("frameGenerationDisableMayDestroy(") !=
              std::string::npos &&
          compact_runtime.find("if(!previous&&enabled){") !=
              std::string::npos,
      "Streamline runtime implements the bounded Options-Off transaction");
  expect(
      compact_backend.find("retryFrameGenerationDisableAfterDrain(") !=
              std::string::npos &&
          compact_backend.find(
              "shutdownblockedbyFGdisablefailure") != std::string::npos &&
          compact_backend_internal.find(
              "[[nodiscard]]booldestroySwapchainObjects();") !=
              std::string::npos,
      "Vulkan recreate and shutdown drain, retry, and block destruction");
  expect(
      compact_frame_generation.find(
          "frameGenerationRecoveryRequired()") != std::string::npos &&
          compact_frame_generation.find("fg_force_native_recovery_=true;") !=
              std::string::npos,
      "FG input-clear failures propagate into Native recovery");
  expect(
      compact_runtime.find("if(result==VK_ERROR_DEVICE_LOST){") !=
              std::string::npos &&
          compact_runtime.find(
              "returnFrameGenerationTransitionResult::FatalDeviceLost;") !=
              std::string::npos &&
          compact_runtime.find(
              "returnFrameGenerationTransitionResult::RecoverNative;") !=
              std::string::npos,
      "DLSS-G classifies only device loss as fatal and keeps SDK errors recoverable");
  expect(
      compact_frame_entry.find("!fg_diagnostic.recovery_required") !=
              std::string::npos &&
          compact_swapchain_wait.find("if(!recreateSwapchain()){") !=
              std::string::npos &&
          compact_swapchain_wait.find("if(!fatal_error_){") !=
              std::string::npos &&
          compact_present.find(
              "fg_present_transition==FrameGenerationTransitionResult::RecoverNative") !=
              std::string::npos &&
          compact_present.find(
              "fg_present_transition==FrameGenerationTransitionResult::FatalDeviceLost") !=
              std::string::npos,
      "frame entry, resize retry, and Present recovery preserve fatal routing");
  expect(
      compact_app_main.find("XPBD_R0F_F11_GATE") != std::string::npos &&
          compact_app_main.find("R0F_G03_F11_GATE_TRIGGER") !=
              std::string::npos &&
          compact_app_main.find("toggle_borderless_fullscreen();") !=
              std::string::npos,
      "developer-only F11 fixture invokes the production fullscreen transition");
}

void testRtAlphaSemantics() {
  using xpbd::gfx::RtAlphaMode;
  using xpbd::gfx::RtFrontToBackAccumulator;
  using xpbd::gfx::RtTransparencyClass;
  using xpbd::gfx::RtTransparentGuideProbeInputV1;
  using xpbd::gfx::TransparentGuidePolicyV1;
  using xpbd::gfx::resolveRtTransparentGuideProbeV1;
  using xpbd::gfx::rtAcceptedOpacity;
  using xpbd::gfx::rtDeterministicShadowVisibilityAfter;
  using xpbd::gfx::rtShadowVisibilityAfter;

  expectNear(rtAcceptedOpacity(RtAlphaMode::Cutout, 0.0f), 0.0f, 1.0e-6f,
             "RT cutout rejects transparent texel");
  expectNear(rtAcceptedOpacity(RtAlphaMode::Cutout, 0.5f), 1.0f, 1.0e-6f,
             "RT cutout accepts surviving texel as opaque");
  expectNear(rtAcceptedOpacity(RtAlphaMode::Blend, 0.25f), 0.25f, 1.0e-6f,
             "RT blend preserves fractional alpha");

  RtFrontToBackAccumulator accumulated;
  expect(!accumulated.add(1.0f, 0.0f, 0.0f, 0.25f,
                          RtAlphaMode::Blend),
         "front blend keeps traversal open");
  expectNear(accumulated.premultiplied_r, 0.25f, 1.0e-6f,
             "front blend stores premultiplied red");
  expectNear(accumulated.alpha(), 0.25f, 1.0e-6f,
             "front blend output alpha");
  expect(accumulated.add(0.0f, 0.0f, 1.0f, 1.0f,
                         RtAlphaMode::Opaque),
         "opaque surface terminates traversal");
  expectNear(accumulated.premultiplied_b, 0.75f, 1.0e-6f,
             "opaque surface fills remaining transmittance");
  expectNear(accumulated.alpha(), 1.0f, 1.0e-6f,
             "front-to-back stack becomes opaque");

  float shadow = rtShadowVisibilityAfter(1.0f, 0.5f, RtAlphaMode::Blend);
  shadow = rtShadowVisibilityAfter(shadow, 0.5f, RtAlphaMode::Blend);
  expectNear(shadow, 0.25f, 1.0e-6f,
             "two half-alpha layers transmit one quarter shadow light");

  expectNear(rtDeterministicShadowVisibilityAfter(
                 1.0f, 0.5f, RtAlphaMode::Blend, 0.25f),
             0.625f, 1.0e-6f,
             "coverage and physical transmission combine independently");
  expectNear(rtDeterministicShadowVisibilityAfter(
                 1.0f, 0.5f, RtAlphaMode::Cutout, 0.0f),
             0.0f, 1.0e-6f,
             "accepted cutout remains a binary shadow occluder");
  expectNear(rtDeterministicShadowVisibilityAfter(
                 1.0f, 0.0f, RtAlphaMode::Cutout, 0.75f),
             1.0f, 1.0e-6f,
             "discarded cutout does not apply physical transmission");

  RtTransparentGuideProbeInputV1 guide_input;
  auto guide = resolveRtTransparentGuideProbeV1(guide_input);
  expect(guide.policy == TransparentGuidePolicyV1::FrontCoverage &&
             guide.use_front_surface && guide.guide_validity == 1.0f &&
             !guide.neutralize_rr_guides,
         "opaque primary guide remains valid on the front surface");

  guide_input.classification = RtTransparencyClass::Cutout;
  guide_input.coverage = 0.0f;
  guide_input.opaque_behind_available = true;
  guide = resolveRtTransparentGuideProbeV1(guide_input);
  expect(guide.policy == TransparentGuidePolicyV1::OpaqueBehind &&
             !guide.use_front_surface && guide.use_opaque_behind &&
             guide.guide_validity == 1.0f,
         "discarded cutout deterministically selects the opaque surface behind");

  guide_input = {};
  guide_input.classification = RtTransparencyClass::CoverageBlend;
  guide_input.coverage = 0.5f;
  guide_input.coverage_layer_count = 1u;
  guide = resolveRtTransparentGuideProbeV1(guide_input);
  expect(guide.policy == TransparentGuidePolicyV1::FrontCoverage &&
             guide.use_front_surface && guide.guide_validity == 1.0f &&
             !guide.neutralize_rr_guides,
         "single coverage layer freezes FrontCoverage guide policy");
  expectNear(guide.transparency_and_composition, 1.0f, 1.0e-6f,
             "half coverage produces maximal composition mask");
  expectNear(guide.reactive, 1.0f, 1.0e-6f,
             "half coverage produces maximal reactive mask");

  guide_input.coverage_layer_count = 2u;
  guide = resolveRtTransparentGuideProbeV1(guide_input);
  expect(guide.policy == TransparentGuidePolicyV1::ConservativeInvalid &&
             guide.guide_validity == 0.0f &&
             guide.disocclusion == 1.0f &&
             guide.neutralize_rr_guides,
         "ambiguous coverage stack rejects history with neutral RR guides");

  guide_input = {};
  guide_input.classification = RtTransparencyClass::PhysicalTransmission;
  guide_input.physical_transmission = 0.8f;
  guide = resolveRtTransparentGuideProbeV1(guide_input);
  expect(guide.policy == TransparentGuidePolicyV1::FrontCoverage &&
             guide.use_front_surface && guide.guide_validity == 0.0f &&
             guide.neutralize_rr_guides,
         "physical transmission keeps front Depth/Motion and neutralizes unreliable RR guides");
  expectNear(guide.transparency_and_composition, 0.8f, 1.0e-6f,
             "physical transmission drives the composition mask");

  guide_input = {};
  guide_input.classification = RtTransparencyClass::CoverageBlend;
  guide_input.coverage = std::numeric_limits<float>::quiet_NaN();
  guide_input.coverage_layer_count = 1u;
  guide_input.surface_identity_matches_history = false;
  guide = resolveRtTransparentGuideProbeV1(guide_input);
  expectNear(guide.transparency_and_composition, 0.0f, 1.0e-6f,
             "non-finite coverage sanitizes to a finite zero mask");
  expect(guide.disocclusion == 1.0f,
         "guide surface identity change forces disocclusion");
}

void testRtMotionProjection() {
  using xpbd::gfx::evaluateRtMotionProjection;
  using xpbd::gfx::RtMotionProjectionInput;

  RtMotionProjectionInput input;
  input.current_uv = {0.5f, 0.5f};
  input.previous_clip = {0.0f, 0.0f, 0.5f, 1.0f};
  input.viewport_width = 1920u;
  input.viewport_height = 1080u;

  auto projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion rejects absent camera/geometry history");

  input.camera_history_valid = true;
  input.geometry_history_valid = true;
  projected = evaluateRtMotionProjection(input);
  expect(projected.valid && projected.disocclusion == 0.0f,
         "motion accepts finite in-viewport history");
  expectNear(projected.current_to_previous_pixels[0], 0.0f, 1.0e-6f,
             "static motion x is zero");
  expectNear(projected.current_to_previous_pixels[1], 0.0f, 1.0e-6f,
             "static motion y is zero");

  // The uploaded previous clip matrix is already GL-to-Vulkan converted. A
  // top-left UV therefore has the same numeric Y as the Vulkan clip mapping.
  input.current_uv = {0.25f, 0.20f};
  input.previous_clip = {-0.50f, -0.60f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(projected.valid, "off-centre static motion remains valid");
  expectNear(projected.current_to_previous_pixels[0], 0.0f, 1.0e-4f,
             "off-centre static motion x is zero");
  expectNear(projected.current_to_previous_pixels[1], 0.0f, 1.0e-4f,
             "Vulkan clip y maps directly to top-left framebuffer motion");

  input.current_uv = {0.5f, 0.5f};
  input.previous_clip = {0.25f, -0.5f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(projected.valid, "translated motion remains valid");
  expectNear(projected.current_to_previous_pixels[0], 240.0f, 1.0e-4f,
             "motion x uses current-to-previous pixel convention");
  expectNear(projected.current_to_previous_pixels[1], -270.0f, 1.0e-4f,
             "motion y uses current-to-previous pixel convention");

  input.previous_clip = {0.0f, 0.0f, 0.5f, -1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion marks behind-camera history disoccluded");

  input.previous_clip = {3.0f, 0.0f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion marks previous sample outside viewport disoccluded");

  input.previous_clip = {
      std::numeric_limits<float>::infinity(), 0.0f, 0.5f, 1.0f};
  projected = evaluateRtMotionProjection(input);
  expect(!projected.valid && projected.disocclusion == 1.0f,
         "motion rejects non-finite previous clip values");
}

void testStaticMaterialClassification() {
  xpbd::gfx::StaticIndexedModelMesh mesh;
  mesh.bone_names.emplace_back("root");
  mesh.vertices.resize(3);
  mesh.vertices[0].u = 0.0f;
  mesh.vertices[0].v = 0.0f;
  mesh.vertices[1].u = 1.0f;
  mesh.vertices[1].v = 0.0f;
  mesh.vertices[2].u = 0.0f;
  mesh.vertices[2].v = 1.0f;
  mesh.indices = {0u, 1u, 2u};
  xpbd::gfx::StaticModelFace face;
  face.first_vertex = 0;
  face.vertex_count = 3;
  face.first_index = 0;
  face.index_count = 3;
  face.bone_index = 0;
  face.textured = true;
  mesh.faces.push_back(face);

  xpbd::gfx::TextureImage texture;
  texture.width = 1;
  texture.height = 1;
  texture.rgba = {255u, 255u, 255u, 128u};
  expect(xpbd::gfx::staticModelFaceMaterial(mesh, mesh.faces[0], &texture) ==
             xpbd::gfx::StaticModelMaterialClass::Blend,
         "fractional texture alpha classifies as RT/raster blend");
  auto blend_plan = xpbd::gfx::makeStaticModelDrawPlan(mesh, &texture);
  expect(blend_plan.blend.index_count == 3u,
         "blend face reaches blend index range");
  expect(blend_plan.primitive_materials.size() == 1u &&
             blend_plan.primitive_materials[0].textured &&
             blend_plan.primitive_materials[0].material ==
                 xpbd::gfx::StaticModelMaterialClass::Blend,
         "ordered primitive keeps exact textured blend metadata");

  texture.rgba[3] = 0u;
  expect(xpbd::gfx::staticModelFaceMaterial(mesh, mesh.faces[0], &texture) ==
             xpbd::gfx::StaticModelMaterialClass::Cutout,
         "zero-alpha texture classifies as cutout");
  texture.rgba[3] = 255u;
  expect(xpbd::gfx::staticModelFaceMaterial(mesh, mesh.faces[0], &texture) ==
             xpbd::gfx::StaticModelMaterialClass::Opaque,
         "opaque texture classifies as opaque");
}

std::vector<std::uint8_t> makeRadianceHdr(
    int width, int height,
    const std::vector<std::array<std::uint8_t, 4>> &rgbe) {
  const std::string header =
      "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
      std::to_string(height) + " +X " + std::to_string(width) + "\n";
  std::vector<std::uint8_t> encoded(header.begin(), header.end());
  for (const auto &pixel : rgbe) {
    encoded.insert(encoded.end(), pixel.begin(), pixel.end());
  }
  return encoded;
}

void testWorldEnvironmentFoundation() {
  using namespace xpbd::gfx;
  constexpr double pi = 3.14159265358979323846;

  const UtcDateTime utc{2024, 1, 1, 0, 0, 0.0};
  const ObserverLocation shanghai{31.2304, 121.4737, 5.0, 0.0};
  CelestialState celestial;
  std::string error;
  expect(computeCelestialState(utc, shanghai, celestial, &error),
         "fixed UTC/observer produces a celestial state");
  expect(celestial.valid && error.empty(),
         "valid celestial state has no error");
  expectNearDouble(celestial.sun.azimuth_degrees, 126.176, 0.002,
                   "Sun azimuth matches frozen Astronomy reference");
  expectNearDouble(celestial.sun.apparent_altitude_degrees, 11.5352, 0.002,
                   "Sun altitude matches frozen Astronomy reference");
  expectNearDouble(celestial.moon.azimuth_degrees, 266.919, 0.002,
                   "Moon azimuth matches frozen Astronomy reference");
  expectNearDouble(celestial.moon.apparent_altitude_degrees, 29.1462, 0.002,
                   "Moon altitude matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_illuminated_fraction, 0.780373, 0.000002,
                   "Moon fraction matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_magnitude, -11.1495, 0.0002,
                   "Moon magnitude matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_distance_km, 404656.0, 1.0,
                   "Moon distance matches frozen Astronomy reference");
  expectNearDouble(celestial.moon.angular_diameter_degrees, 0.492003,
                   0.000002,
                   "Moon diameter matches frozen Astronomy reference");
  expectNearDouble(celestial.moon_libration_latitude_degrees, -4.65849,
                   0.00002,
                   "Moon libration latitude matches frozen reference");
  expectNearDouble(celestial.moon_libration_longitude_degrees, 0.039338,
                   0.00002,
                   "Moon libration longitude matches frozen reference");
  const auto length = [](const std::array<double, 3> &direction) {
    return std::sqrt(direction[0] * direction[0] +
                     direction[1] * direction[1] +
                     direction[2] * direction[2]);
  };
  expectNearDouble(length(celestial.sun.direction), 1.0, 1e-12,
                   "Sun world direction is normalized");
  expectNearDouble(length(celestial.moon.direction), 1.0, 1e-12,
                   "Moon world direction is normalized");
  expect(celestial.sun.angular_diameter_degrees > 0.45 &&
             celestial.sun.angular_diameter_degrees < 0.56,
         "Sun apparent diameter is physically bounded");
  expect(celestial.twilight == TwilightPhase::Day,
         "positive geometric Sun altitude classifies as day");

  CelestialState rotated;
  ObserverLocation rotated_north = shanghai;
  rotated_north.north_offset_degrees = 90.0;
  expect(computeCelestialState(utc, rotated_north, rotated, &error),
         "north-offset celestial state computes");
  expectNearDouble(rotated.sun.direction[0], celestial.sun.direction[2],
                   1e-12, "north offset rotates Sun world X");
  expectNearDouble(rotated.sun.direction[2], -celestial.sun.direction[0],
                   1e-12, "north offset rotates Sun world Z");

  CelestialState one_minute;
  const UtcDateTime minute_later{2024, 1, 1, 0, 1, 0.0};
  expect(computeCelestialState(minute_later, shanghai, one_minute, &error),
         "one-minute-later celestial state computes");
  const double continuity_dot =
      celestial.sun.direction[0] * one_minute.sun.direction[0] +
      celestial.sun.direction[1] * one_minute.sun.direction[1] +
      celestial.sun.direction[2] * one_minute.sun.direction[2];
  expect(continuity_dot > 0.9999 && continuity_dot < 1.0,
         "celestial direction changes continuously over one minute");

  CelestialState night;
  const UtcDateTime night_utc{2024, 1, 1, 12, 0, 0.0};
  expect(computeCelestialState(night_utc, shanghai, night, &error) &&
             night.twilight == TwilightPhase::Night,
         "deep negative Sun altitude classifies as night");
  expect(std::string(twilightPhaseName(TwilightPhase::Civil)) == "civil",
         "twilight phase names are stable");

  CelestialState preserved;
  preserved.valid = true;
  preserved.moon_distance_km = 42.0;
  ObserverLocation invalid_observer = shanghai;
  invalid_observer.latitude_degrees = 91.0;
  expect(!computeCelestialState(utc, invalid_observer, preserved, &error),
         "invalid observer is rejected");
  expect(preserved.valid && preserved.moon_distance_km == 42.0,
         "failed celestial update is transactional");

  const BrunetonAtmosphereConfig earth_atmosphere =
      defaultEarthAtmosphereConfig();
  expect(earth_atmosphere.valid(),
         "frozen Earth Bruneton configuration is valid");
  expect(earth_atmosphere.dimensions.transmittance_width == 256u &&
             earth_atmosphere.dimensions.transmittance_height == 64u &&
             earth_atmosphere.dimensions.scatteringWidth() == 256u &&
             earth_atmosphere.dimensions.scattering_view_cosine == 128u &&
             earth_atmosphere.dimensions.scattering_radial == 32u &&
             earth_atmosphere.dimensions.irradiance_width == 64u &&
             earth_atmosphere.dimensions.irradiance_height == 16u,
         "Bruneton LUT dimensions match the frozen upstream contract");
  expectNearDouble(earth_atmosphere.physical.bottom_radius_km, 6360.0, 0.0,
                   "Earth atmosphere bottom radius is frozen");
  expectNearDouble(earth_atmosphere.physical.top_radius_km, 6420.0, 0.0,
                   "Earth atmosphere top radius is frozen");
  expectNearDouble(
      earth_atmosphere.physical.rayleigh_scattering_per_km[1],
      0.013557762447920219, 1e-16,
      "green Rayleigh coefficient matches the upstream Earth model");
  expectNearDouble(
      earth_atmosphere.physical.absorption_extinction_per_km[1],
      0.0018809, 1e-12,
      "green ozone coefficient matches the upstream Earth model");
  const std::string atmosphere_key =
      brunetonAtmosphereCacheKey(earth_atmosphere);
  expect(!atmosphere_key.empty() &&
             atmosphere_key ==
                 brunetonAtmosphereCacheKey(defaultEarthAtmosphereConfig()),
         "Bruneton cache identity is deterministic");
  BrunetonAtmosphereConfig modified_atmosphere = earth_atmosphere;
  modified_atmosphere.physical.ground_albedo[0] = 0.11;
  expect(modified_atmosphere.valid() &&
             brunetonAtmosphereCacheKey(modified_atmosphere) != atmosphere_key,
         "physical parameter changes invalidate the Bruneton cache identity");
  BrunetonAtmosphereConfig invalid_atmosphere = earth_atmosphere;
  invalid_atmosphere.physical.top_radius_km =
      invalid_atmosphere.physical.bottom_radius_km;
  expect(!invalid_atmosphere.valid() &&
             brunetonAtmosphereCacheKey(invalid_atmosphere).empty(),
         "invalid Bruneton configurations cannot acquire cache identities");

  std::vector<std::array<std::uint8_t, 4>> pixels(
      8u, std::array<std::uint8_t, 4>{64u, 64u, 64u, 129u});
  pixels[2] = {128u, 128u, 128u, 132u};
  const std::vector<std::uint8_t> hdr = makeRadianceHdr(4, 2, pixels);
  FloatEnvironmentImage image;
  expect(decodeRadianceHdr(hdr, image, &error),
         "strict 2:1 Radiance HDR decodes to float");
  expect(image.valid() && image.width == 4u && image.height == 2u,
         "decoded HDR retains dimensions");
  expect(image.rgba[2u * 4u] > image.rgba[0],
         "decoded HDR retains a bright importance texel");
  expect(image.rgba[3] == 1.0f,
         "decoded HDR synthesizes unit alpha");

  FloatEnvironmentImage preserved_image;
  preserved_image.width = 1u;
  preserved_image.height = 1u;
  preserved_image.rgba = {1.0f, 2.0f, 3.0f, 1.0f};
  const std::vector<std::uint8_t> wrong_ratio =
      makeRadianceHdr(4, 3,
                      std::vector<std::array<std::uint8_t, 4>>(
                          12u, {64u, 64u, 64u, 129u}));
  expect(!decodeRadianceHdr(wrong_ratio, preserved_image, &error),
         "non-2:1 HDR is rejected");
  expect(preserved_image.width == 1u && preserved_image.rgba[1] == 2.0f,
         "failed HDR decode is transactional");
  const std::vector<std::uint8_t> black_hdr =
      makeRadianceHdr(4, 2,
                      std::vector<std::array<std::uint8_t, 4>>(
                          8u, {0u, 0u, 0u, 0u}));
  expect(!decodeRadianceHdr(black_hdr, preserved_image, &error),
         "empty-radiance HDR is rejected");
  HdrDecodeLimits tiny_budget;
  tiny_budget.maximum_decoded_bytes = 64u;
  expect(!decodeRadianceHdr(hdr, preserved_image, &error, tiny_budget),
         "HDR decoded-memory budget is enforced");

  AliasTable alias;
  const std::array<double, 3> alias_weights{1.0, 3.0, 0.0};
  expect(alias.build(alias_weights), "alias table accepts finite weights");
  expectNearDouble(alias.probability(0), 0.25, 1e-12,
                   "alias probability preserves first weight");
  expectNearDouble(alias.probability(1), 0.75, 1e-12,
                   "alias probability preserves second weight");
  expect(alias.probability(2) == 0.0,
         "zero-weight alias entry remains unsampled");
  std::array<double, 3> reconstructed_alias_pmf{};
  for (std::size_t column = 0; column < alias.size(); ++column) {
    const double acceptance = alias.acceptance(column);
    const std::uint32_t alternate = alias.aliasIndex(column);
    expect(acceptance >= 0.0 && acceptance <= 1.0 &&
               alternate < alias.size(),
           "exported alias entry is GPU-safe");
    reconstructed_alias_pmf[column] +=
        acceptance / static_cast<double>(alias.size());
    reconstructed_alias_pmf[alternate] +=
        (1.0 - acceptance) / static_cast<double>(alias.size());
  }
  for (std::size_t index = 0; index < alias.size(); ++index) {
    expectNearDouble(reconstructed_alias_pmf[index],
                     alias.probability(index), 1e-12,
                     "exported alias entry reconstructs PMF");
  }
  expect(!AliasTable{}.valid(), "empty alias table is invalid");

  EnvironmentDistribution environment;
  expect(environment.build(image),
         "HDR radiance builds an environment distribution");
  double probability_sum = 0.0;
  double pdf_integral = 0.0;
  for (std::uint32_t y = 0; y < environment.height(); ++y) {
    const double theta =
        pi * (static_cast<double>(y) + 0.5) /
        static_cast<double>(environment.height());
    for (std::uint32_t x = 0; x < environment.width(); ++x) {
      const double phi =
          2.0 * pi * (static_cast<double>(x) + 0.5) /
          static_cast<double>(environment.width());
      const std::array<double, 3> direction{
          std::sin(theta) * std::sin(phi), std::cos(theta),
          std::sin(theta) * std::cos(phi)};
      probability_sum += environment.texelProbability(x, y);
      pdf_integral += environment.solidAnglePdf(direction) *
                      environment.texelSolidAngle(y);
    }
  }
  expectNearDouble(probability_sum, 1.0, 1e-12,
                   "environment texel probabilities normalize");
  expectNearDouble(pdf_integral, 1.0, 1e-12,
                   "environment solid-angle PDF integrates to one");
  expect(environment.texelProbability(2u, 0u) >
             environment.texelProbability(0u, 0u),
         "environment importance favors the bright texel");
  expect(environment.aliasAcceptance(2u, 0u) >= 0.0 &&
             environment.aliasAcceptance(2u, 0u) <= 1.0 &&
             environment.aliasIndex(2u, 0u) <
                 environment.width() * environment.height(),
         "environment exposes a GPU-safe alias entry");
  const EnvironmentDirectionSample environment_sample =
      environment.sample(0.37, 0.61, 0.25, 0.75, 0.7);
  expect(environment_sample.valid,
         "environment alias sampling produces a valid direction");
  expectNearDouble(environment.solidAnglePdf(environment_sample.direction, 0.7),
                   environment_sample.solid_angle_pdf, 1e-12,
                   "environment sample and evaluated PDF agree after rotation");

  HdrEnvironmentAsset hdr_asset;
  expect(buildHdrEnvironmentAsset(hdr, "fixture.hdr", "fixture-sha", 7u,
                                  hdr_asset, &error),
         "complete HDR asset commits radiance and distribution");
  expect(hdr_asset.valid() && hdr_asset.generation == 7u,
         "committed HDR asset retains identity and generation");
  HdrEnvironmentAsset preserved_asset = std::move(hdr_asset);
  const std::uint64_t preserved_generation = preserved_asset.generation;
  expect(!buildHdrEnvironmentAsset(wrong_ratio, "broken.hdr", "broken-sha",
                                   8u, preserved_asset, &error),
         "invalid replacement HDR asset is rejected");
  expect(preserved_asset.valid() &&
             preserved_asset.generation == preserved_generation &&
             preserved_asset.source_identity == "fixture.hdr",
         "failed HDR asset replacement preserves committed state");

  WorldEnvironmentState world;
  ResolvedWorldEnvironment resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.background_visible &&
             !resolved.environment_lighting,
         "default World resolves to Sky Off without IBL");
  const ResolvedEnvironmentView constant_environment_a =
      resolveEnvironmentView(resolved, {0.1f, 0.2f, 0.3f});
  const ResolvedEnvironmentView constant_environment_b =
      resolveEnvironmentView(resolved, {0.2f, 0.2f, 0.3f});
  expect(constant_environment_a.generation !=
             constant_environment_b.generation,
         "resolved Environment generation includes constant-fallback radiance");
  const ResolvedEnvironmentView analytic_environment =
      resolveEnvironmentView(resolved, {}, 1.5f);
  const EnvironmentSample analytic_sample =
      sampleEnvironment(analytic_environment, 0.4f, 0.7f, 0.0f, 0.0f);
  const EnvironmentSample analytic_non_finite_sample = sampleEnvironment(
      analytic_environment, std::numeric_limits<float>::quiet_NaN(),
      std::numeric_limits<float>::infinity(), 0.0f, 0.0f);
  expect(analytic_environment.kind ==
             ResolvedEnvironmentKind::AnalyticFallback &&
             analytic_environment.power_estimate > 0.0f &&
             analytic_sample.valid && analytic_non_finite_sample.valid &&
             std::isfinite(analytic_non_finite_sample.direction[0]) &&
             std::isfinite(analytic_non_finite_sample.direction[1]) &&
             std::isfinite(analytic_non_finite_sample.direction[2]),
         "legacy analytic sky resolves through the shared Environment seam");
  expectNear(environmentPdf(analytic_environment, analytic_sample.direction),
             analytic_sample.pdf, 1.0e-7f,
             "analytic Environment sample and PDF agree");
  const std::array<float, 3> analytic_evaluation =
      evaluateEnvironment(analytic_environment, analytic_sample.direction);
  for (std::size_t channel = 0; channel < analytic_sample.radiance.size();
       ++channel) {
    expectNear(analytic_evaluation[channel], analytic_sample.radiance[channel],
               1.0e-6f,
               "analytic Environment sample and evaluation agree");
  }
  world.sky_rendering = SkyRendering::UserHdri;
  world.selected_hdr_identity = "missing.hdr";
  world.hdr = std::move(preserved_asset);
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.warning.empty() &&
             world.sky_rendering == SkyRendering::UserHdri &&
             world.selected_hdr_identity == "missing.hdr",
         "missing selected HDR resolves Off without changing requested state");
  world.selected_hdr_identity = "fixture.hdr";
  world.global_lighting_strength_ev = std::log2(2.5f);
  world.background_exposure = 3.0f;
  world.rotation_radians = static_cast<float>(-0.5 * pi);
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::UserHdri &&
             resolved.hdr != nullptr && resolved.environment_lighting,
         "available selected HDR resolves as User HDRI");
  expectNear(resolved.environment_strength, 2.5f, 1e-6f,
             "HDR physical strength is independent");
  expectNear(resolved.global_lighting_strength, 2.5f, 1e-6f,
             "global sky EV resolves to one authoritative multiplier");
  expectNear(resolved.background_multiplier, 20.0f, 1e-5f,
             "background EV composes with physical global sky energy");
  expectNear(resolved.background_exposure, 3.0f, 1e-6f,
             "HDR background exposure is retained separately");
  expectNear(resolved.rotation_radians, static_cast<float>(1.5 * pi), 1e-6f,
             "HDR rotation is normalized");
  const ResolvedEnvironmentView hdr_environment =
      resolveEnvironmentView(resolved);
  expect(hdr_environment.kind == ResolvedEnvironmentKind::UserHdri &&
             hdr_environment.lighting_enabled &&
             hdr_environment.sampling_ready &&
             hdr_environment.power_estimate > 0.0f,
         "resolved HDR environment exposes a ready finite-power sampling view");
  const EnvironmentSample hdr_environment_sample =
      sampleEnvironment(hdr_environment, 0.37f, 0.61f, 0.25f, 0.75f);
  const std::array<float, 3> hdr_environment_evaluation =
      evaluateEnvironment(hdr_environment, hdr_environment_sample.direction);
  expect(hdr_environment_sample.valid &&
             hdr_environment_sample.pdf > 0.0f &&
             std::isfinite(hdr_environment_sample.pdf),
         "resolved HDR environment produces a finite valid sample");
  expectNear(environmentPdf(hdr_environment,
                            hdr_environment_sample.direction),
             hdr_environment_sample.pdf, 1.0e-6f,
             "resolved HDR environment sample and PDF agree");
  for (std::size_t channel = 0;
       channel < hdr_environment_sample.radiance.size(); ++channel) {
    expectNear(hdr_environment_evaluation[channel],
               hdr_environment_sample.radiance[channel], 1.0e-6f,
               "resolved HDR environment sample and evaluation agree");
  }
  UtcDateTime shifted_local;
  expect(shiftUtcDateTime(UtcDateTime{2024, 1, 1, 0, 30, 0.0},
                          -3600.0, shifted_local, &error) &&
             shifted_local.year == 2023 && shifted_local.month == 12 &&
             shifted_local.day == 31 && shifted_local.hour == 23 &&
             shifted_local.minute == 30,
         "calendar-safe local/UTC conversion crosses year boundaries");
  world.environment_lighting = false;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::UserHdri &&
             resolved.environment_strength == 0.0f &&
             resolved.background_visible,
         "background visibility does not force environment lighting");
  world.environment_lighting = true;
  world.sky_rendering = SkyRendering::ProceduralDayNight;
  world.procedural_resources_ready = false;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off,
         "unavailable procedural resources resolve to Off");
  world.procedural_resources_ready = true;
  world.celestial = celestial;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::ProceduralDayNight &&
             resolved.celestial != nullptr && resolved.atmosphere != nullptr &&
             resolved.clouds == nullptr,
         "ready procedural resources resolve with celestial and atmosphere");
  const ResolvedSunLight resolved_sun = resolveSunLight(resolved);
  expect(resolved_sun.enabled && resolved_sun.lighting_enabled &&
             resolved_sun.angular_radius > 0.0f &&
             resolved_sun.solid_angle > 0.0f &&
             resolved_sun.power_estimate > 0.0f &&
             resolved_sun.generation != 0u,
         "procedural World resolves one finite powered SunDisk");
  const RtSunDiskSample sun_sample =
      sampleRtSunDisk(resolved_sun, 0.25f, 0.75f);
  const std::array<float, 3> sun_evaluation =
      evaluateRtSunDisk(resolved_sun, sun_sample.direction);
  expect(sun_sample.valid && sun_sample.pdf > 0.0f &&
             std::isfinite(sun_sample.pdf) &&
             sun_evaluation[0] > 0.0f,
         "resolved SunDisk produces a finite in-disk sample");
  expectNear(rtSunDiskPdf(resolved_sun, sun_sample.direction),
             sun_sample.pdf, std::max(sun_sample.pdf * 1.0e-6f, 1.0e-4f),
             "SunDisk sample and solid-angle PDF agree");
  world.sun.strength *= 2.0f;
  const ResolvedSunLight stronger_sun =
      resolveSunLight(resolveWorldEnvironment(world));
  expect(stronger_sun.generation != resolved_sun.generation &&
             stronger_sun.power_estimate > resolved_sun.power_estimate,
         "resolved Sun generation and power include authored strength");
  world.sun.strength *= 0.5f;
  world.environment_lighting = false;
  const ResolvedWorldEnvironment sun_only_world =
      resolveWorldEnvironment(world);
  expect(!sun_only_world.environment_lighting &&
             resolveSunLight(sun_only_world).lighting_enabled,
         "Sun/Moon lighting remains independently controlled from Environment lighting");
  world.environment_lighting = true;

  const RtLightRegistry registry = buildRtLightRegistry(
      hdr_environment, true, resolved_sun, 77u, 3.0f, true);
  const RtLightRecord *environment_light =
      findRtLight(registry, RtLightType::Environment);
  const RtLightRecord *sun_light =
      findRtLight(registry, RtLightType::SunDisk);
  const RtLightRecord *emissive_light =
      findRtLight(registry, RtLightType::EmissiveTriangle);
  expect(registry.enabled_family_count == 3u &&
             registry.total_sampling_weight > 0.0f &&
             environment_light != nullptr && environment_light->enabled &&
             sun_light != nullptr && sun_light->enabled &&
             emissive_light != nullptr && emissive_light->enabled,
         "minimal Light Registry enables Environment, SunDisk, and Emissive families");
  expectNear(lightPdf(registry, RtLightType::Environment) +
                 lightPdf(registry, RtLightType::SunDisk) +
                 lightPdf(registry, RtLightType::EmissiveTriangle),
             1.0f, 1.0e-6f,
             "minimal Light Registry family probabilities normalize");
  const RtLightRegistry reweighted_registry = buildRtLightRegistry(
      hdr_environment, true, resolved_sun, 77u, 4.0f, true);
  expect(reweighted_registry.generation != registry.generation,
         "Light Registry generation changes when a family weight changes");
  const RtLightSelection first_selection = sampleRtLight(registry, 0.0f);
  const RtLightSelection repeated_selection = sampleRtLight(registry, 0.0f);
  const RtLightSelection last_selection = sampleRtLight(registry, 0.999999f);
  expect(first_selection.valid && repeated_selection.valid &&
             first_selection.stable_id == repeated_selection.stable_id &&
             first_selection.type == RtLightType::Environment &&
             last_selection.valid &&
             last_selection.type == RtLightType::EmissiveTriangle,
         "minimal Light Registry selection is ordered and deterministic");
  const std::array<std::uint32_t, 4> emitter_identity{3u, 5u, 7u, 11u};
  const RtStableLightId emitter_id =
      makeRtEmissiveTriangleStableId(emitter_identity, 13u);
  expect(emitter_id &&
             emitter_id ==
                 makeRtEmissiveTriangleStableId(emitter_identity, 13u) &&
             emitter_id !=
                 makeRtEmissiveTriangleStableId(emitter_identity, 14u),
         "emissive stable IDs repeat for one source and change by instance");
  const VolumetricCloudState default_clouds;
  const std::string default_cloud_key =
      volumetricCloudCacheKey(default_clouds);
  expect(default_clouds.valid() && !default_clouds.enabled &&
             !default_cloud_key.empty() &&
             default_cloud_key ==
                 volumetricCloudCacheKey(VolumetricCloudState{}),
         "disabled volumetric-cloud defaults have deterministic identity");
  VolumetricCloudState active_clouds = default_clouds;
  active_clouds.enabled = true;
  const std::string active_cloud_key =
      volumetricCloudCacheKey(active_clouds);
  expect(active_clouds.valid() && !active_cloud_key.empty() &&
             active_cloud_key != default_cloud_key,
         "enabling volumetric clouds invalidates their cache identity");
  VolumetricCloudState advected_clouds = active_clouds;
  advected_clouds.weather_offset_km[0] += 2.0f;
  advected_clouds.time_seconds += 1.0f;
  ++advected_clouds.generation;
  expect(volumetricCloudCacheKey(advected_clouds) != active_cloud_key,
         "cloud weather, time, and generation affect cache identity");
  world.clouds = active_clouds;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::ProceduralDayNight &&
             resolved.clouds == &world.clouds,
         "enabled valid clouds are exposed to the procedural renderer");
  world.clouds.ray_steps = 0u;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.warning.empty() &&
             volumetricCloudCacheKey(world.clouds).empty(),
         "invalid cloud quality resolves procedural sky to Off");
  world.clouds = default_clouds;
  world.atmosphere.physical.top_radius_km =
      world.atmosphere.physical.bottom_radius_km;
  resolved = resolveWorldEnvironment(world);
  expect(resolved.sky_rendering == SkyRendering::Off &&
             !resolved.warning.empty(),
         "invalid atmosphere resources resolve procedural sky to Off");

  const std::array<EmissivePatch, 3> patches{{
      {0u, 0u, 0u, 1.0, 1.0},
      {1u, 0u, 0u, 2.0, 2.0},
      {2u, 0u, 0u, 3.0, 0.0},
  }};
  EmissivePatchDistribution emitters;
  expect(emitters.build(patches),
         "emissive patches build area-times-luminance weights");
  expectNearDouble(emitters.probability(0), 0.2, 1e-12,
                   "first emissive patch probability is exact");
  expectNearDouble(emitters.probability(1), 0.8, 1e-12,
                   "second emissive patch probability is exact");
  expect(emitters.probability(2) == 0.0,
         "zero-radiance patch has zero probability");

  expectNearDouble(powerHeuristic(1.0, 1.0), 0.5, 1e-12,
                   "power heuristic splits equal PDFs");
  expectNearDouble(powerHeuristic(1.0, 3.0), 0.1, 1e-12,
                   "power heuristic exponent two is exact");
  expectNearDouble(powerHeuristic(1e300, 3e300), 0.1, 1e-12,
                   "power heuristic is overflow safe");
  expect(powerHeuristic(0.0, 1.0) == 0.0 &&
             powerHeuristic(1.0, 0.0) == 1.0,
         "power heuristic handles zero PDF boundaries");
  expectNearDouble(areaPdfToSolidAngle(0.25, 4.0, 0.5), 2.0, 1e-12,
                   "area PDF converts to solid-angle PDF");
  expect(areaPdfToSolidAngle(1.0, 1.0, 0.0) == 0.0,
         "solid-angle conversion rejects a grazing light");
}

void testVulkanPathTraceImplementationSelection() {
  using xpbd::gfx::queryVulkanPathTraceAvailability;
  using xpbd::gfx::RayTracingCapability;
  using xpbd::gfx::VulkanPathTraceAvailability;
  using xpbd::gfx::VulkanPathTraceImplementation;
  using xpbd::gfx::selectVulkanPathTraceImplementation;
  using xpbd::gfx::vulkanPathTraceImplementationName;

  RayTracingCapability hw{};
  hw.supported = true;
  hw.device_extensions_enabled = true;

  expect(vulkanPathTraceImplementationName(
             VulkanPathTraceImplementation::RayTracingPipeline) != nullptr,
         "Vulkan RT Pipeline name non-null");
  expect(selectVulkanPathTraceImplementation(
             false, hw, VulkanPathTraceAvailability{}) ==
             VulkanPathTraceImplementation::None,
         "user off -> None");

  // The built-in path tracer remains available through Ray Query while the
  // optional full RT Pipeline is unavailable or still being created.
  expect(selectVulkanPathTraceImplementation(
             true, hw, VulkanPathTraceAvailability{}) ==
             VulkanPathTraceImplementation::RayQuery,
         "user on + HW, no pipeline -> Ray Query fallback");
  expect(selectVulkanPathTraceImplementation(
             true, hw, VulkanPathTraceAvailability{true, false}) ==
             VulkanPathTraceImplementation::RayQuery,
         "path tracer ready without pipeline -> Ray Query fallback");
  expect(selectVulkanPathTraceImplementation(
             true, hw, VulkanPathTraceAvailability{true, true}) ==
             VulkanPathTraceImplementation::RayTracingPipeline,
         "path tracer + pipeline ready -> RT Pipeline");

  RayTracingCapability no_hw{};
  expect(selectVulkanPathTraceImplementation(
             true, no_hw, VulkanPathTraceAvailability{true, true}) ==
             VulkanPathTraceImplementation::None,
         "path tracer without HW extensions -> None");

  xpbd::gfx::setVulkanPathTraceAvailability(false, false);
  const auto cleared = queryVulkanPathTraceAvailability();
  expect(!cleared.path_tracer_ready && !cleared.ray_tracing_pipeline_ready,
         "availability reset clears both flags");
  xpbd::gfx::setVulkanPathTraceAvailability(true, true);
  const auto ready = queryVulkanPathTraceAvailability();
  expect(ready.path_tracer_ready && ready.ray_tracing_pipeline_ready,
         "availability setter exposes both built-in flags");
  xpbd::gfx::setVulkanPathTraceAvailability(false, false);
}

} // namespace

int main(int argc, char **argv) {
  if (!configureLabPbrStressSide(argc, argv)) {
    return 2;
  }
  testLogicalFramebufferViewportContract();
  testVulkanQueueFamilySelection();
  testTextureFromMemory();
  testSyntheticLargeUvFixtureAndMemoryBaseline();
  testBedrockUvDomainResolution();
  testNegativeInflatePlaneCompatibility();
  testResolvedUvDomainMaterialConsumers();
  testCc0PreviewSceneAssets();
  testEmptyTextureSample();
  testLabPbrDecode();
  testLabPbrDiscoveryAndFallback();
  testStrictLabPbrSuiteImport();
  testLabPbrAuthoringEncodingAndCoverage();
  testLabPbrCompositionAndConflicts();
  testLabPbrPngChecksumAndNormalImport();
  testLabPbrBundleExport();
  testTangentFrames();
  testRtNormalTransformAndUpdatePolicy();
  testRtSceneGenerationContract();
  testPathTraceOptionalOutputMaskContract();
  testRtNearestHitReference();
  testPathTracePbrSourceContracts();
  testEnvironmentLightAdoptionSourceContracts();
  testTransparentGuidePolicySourceContracts();
  testSelectionOutlineTemporalContract();
  testPathTraceSamplingAndAccumulation();
  testPathTraceAdjustableSettingsContract();
  testPathTraceBsdfAndDepth();
  testViewportMeshEmptyGeometry();
  testFrontFacingFlatCubePicking();
  testCanonicalCubeAndRtSceneRecords();
  testRayTracingCapability();
  testFrameGenerationStateLegality();
  testFrameGenerationDisableSourceContracts();
  testRtAlphaSemantics();
  testRtMotionProjection();
  testStaticMaterialClassification();
  testWorldEnvironmentFoundation();
  testVulkanPathTraceImplementationSelection();
  if (g_failures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("All viewport regression smoke tests passed.\n");
  return 0;
}
