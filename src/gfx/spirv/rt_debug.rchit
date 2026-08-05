#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 2, std430) readonly buffer NormalBuffer {
  vec4 normals[];
};
layout(set = 0, binding = 3, std430) readonly buffer IndexBuffer {
  uint indices[];
};
layout(set = 0, binding = 4, std430) readonly buffer UvBuffer {
  vec2 uvs[];
};
layout(set = 0, binding = 5) uniform sampler2D albedoTexture;
layout(set = 0, binding = 6, std430) readonly buffer ColorBuffer {
  vec4 colors[];
};
layout(set = 0, binding = 7, std430) readonly buffer PrimitiveFlagBuffer {
  uint primitiveFlags[];
};
layout(set = 0, binding = 8, std430) readonly buffer PrimitiveMetadataBuffer {
  uvec4 primitiveMetadata[];
};
layout(set = 0, binding = 9, std430) readonly buffer InstanceMetadataBuffer {
  uvec4 instanceMetadata[];
};
layout(set = 0, binding = 11, std430) readonly buffer TangentBuffer {
  vec4 tangents[];
};
layout(set = 0, binding = 12) uniform sampler2D normalTexture;
layout(set = 0, binding = 13) uniform sampler2D specularTexture;
layout(set = 0, binding = 17, std430) readonly buffer PositionBuffer {
  float positions[];
};
layout(set = 0, binding = 20, std430) readonly buffer MotionFrameBuffer {
  mat4 previousViewProj;
  uvec4 motionInfo;
  vec4 reconstructionInfo;
};
struct SurfaceOpticsGpu {
  vec4 parameters;
  vec4 attenuationColor;
};
layout(set = 0, binding = 28, std430) readonly buffer PrimitiveOpticsBuffer {
  SurfaceOpticsGpu primitiveOptics[];
};

layout(push_constant) uniform PC {
  mat4 invViewProj;
  mat4 viewProj;
  vec4 cameraEnvironment;
  uvec4 sizeMode;
  uvec4 sampling;
  vec4 lightDirectionAmbient;
  vec4 lightColorIntensity;
  uvec4 depthLimits;
  vec4 integrator;
} pc;

struct PrimaryPayload {
  vec3 baseColor;
  float t;
  vec3 shadingNormal;
  float ggxAlpha;
  vec3 geometricNormal;
  float ior;
  vec3 f0;
  float transmission;
  vec3 emission;
  float opacity;
  vec4 attenuation;
  uvec4 status;
  vec4 hitData;
  vec2 rayCone;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;
hitAttributeEXT vec2 hitBarycentrics;

const uint kMaterialTextured = 1u << 0u;
const uint kMaterialCutout = 1u << 1u;
const uint kMaterialBlend = 1u << 2u;

bool finiteFloat(float value) {
  return !isnan(value) && !isinf(value);
}

bool finite3(vec3 value) {
  return !any(isnan(value)) && !any(isinf(value));
}

vec3 safeNormalizeNormal(vec3 value, vec3 fallbackValue) {
  float lengthSquared = dot(value, value);
  if (!finiteFloat(lengthSquared) || lengthSquared < 1.0e-8) {
    return fallbackValue;
  }
  return value * inversesqrt(lengthSquared);
}

float rayConeTextureLod(sampler2D sampledTexture, float uvFootprint) {
  ivec2 extent = textureSize(sampledTexture, 0);
  float texels = max(uvFootprint, 0.0) *
                 float(max(extent.x, extent.y));
  return texels > 1.0 ? max(log2(texels), 0.0) : 0.0;
}

float continuousTextureLod(sampler2D sampledTexture, float uvFootprint) {
  float baseLod = rayConeTextureLod(sampledTexture, uvFootprint);
  float mipBias = clamp(reconstructionInfo.x, -2.0, 0.0);
  // The role-specific Vulkan sampler supplies the semantic safeMaxLod. Keep
  // the explicit lower bound here so negative reconstruction bias never asks
  // for a non-existent sharper-than-base level.
  return max(baseLod + mipBias, 0.0);
}

vec4 sampleAlbedoRayCone(vec2 uv, float uvFootprint,
                         bool preserveCoverageLod) {
  float baseLod = rayConeTextureLod(albedoTexture, uvFootprint);
  float biasedLod = continuousTextureLod(albedoTexture, uvFootprint);
  vec4 packed = textureLod(albedoTexture, uv, biasedLod);
  // DLSS Mip Bias is a color-detail correction, not an alpha-test policy.
  // Keep cutout/blend coverage on the unbiased ray-cone LOD used by any-hit.
  if (preserveCoverageLod && abs(biasedLod - baseLod) > 1.0e-6) {
    packed.a = textureLod(albedoTexture, uv, baseLod).a;
  }
  if (any(isnan(packed)) || any(isinf(packed))) {
    return vec4(1.0);
  }
  return clamp(packed, vec4(0.0), vec4(1.0));
}

vec3 sampleNormalRayCone(vec2 uv, float uvFootprint) {
  vec3 packed = textureLod(
      normalTexture, uv,
      continuousTextureLod(normalTexture, uvFootprint)).rgb;
  return finite3(packed) ? clamp(packed, vec3(0.0), vec3(1.0))
                         : vec3(0.5, 0.5, 1.0);
}

vec4 sampleSpecularRayCone(vec2 uv, float uvFootprint) {
  vec4 packed = textureLod(
      specularTexture, uv,
      rayConeTextureLod(specularTexture, uvFootprint));
  return any(isnan(packed)) || any(isinf(packed))
             ? vec4(0.0, 10.0 / 255.0, 0.0, 1.0)
             : clamp(packed, vec4(0.0), vec4(1.0));
}

vec3 objectPosition(uint vertex) {
  uint base = vertex * 3u;
  return vec3(
      positions[base + 0u], positions[base + 1u], positions[base + 2u]);
}

float rayConeUvFootprint(vec2 cone, float hitDistance,
                         vec3 worldEdge1, vec3 worldEdge2,
                         vec2 uv0, vec2 uv1, vec2 uv2) {
  float worldDoubleArea = length(cross(worldEdge1, worldEdge2));
  vec2 uvEdge1 = uv1 - uv0;
  vec2 uvEdge2 = uv2 - uv0;
  float uvDoubleArea =
      abs(uvEdge1.x * uvEdge2.y - uvEdge1.y * uvEdge2.x);
  if (!(worldDoubleArea > 1.0e-20) || !(uvDoubleArea > 0.0)) {
    return 0.0;
  }
  float coneWidth =
      max(cone.x, 0.0) + abs(hitDistance) * max(cone.y, 0.0);
  return coneWidth * sqrt(uvDoubleArea / worldDoubleArea);
}

vec3 decodeLabPbrNormal(vec3 packed) {
  vec2 xy = vec2(packed.r * 2.0 - 1.0, 1.0 - packed.g * 2.0);
  float xy2 = dot(xy, xy);
  if (!finiteFloat(xy2)) {
    return vec3(0.0, 0.0, 1.0);
  }
  if (xy2 > 1.0) {
    xy *= inversesqrt(xy2);
  }
  return safeNormalizeNormal(
      vec3(xy, sqrt(max(0.0, 1.0 - dot(xy, xy)))),
      vec3(0.0, 0.0, 1.0));
}

float decodeLabPbrMicrofacetAlpha(float smoothness) {
  float perceptualRoughness =
      finiteFloat(smoothness) ? clamp(1.0 - smoothness, 0.0, 1.0) : 1.0;
  return perceptualRoughness * perceptualRoughness;
}

float decodeLabPbrEmission(float packed) {
  return packed > (254.5 / 255.0) ? 0.0
                                  : packed * (255.0 / 254.0);
}

vec3 decodeLabPbrF0(float packed, vec3 baseColor, out bool metal,
                    out bool predefinedMetal) {
  uint code = uint(round(packed * 255.0));
  metal = code >= 230u;
  predefinedMetal = code >= 230u && code <= 237u;
  if (!metal) {
    return vec3(packed);
  }
  vec3 n;
  vec3 k;
  if (code == 230u) {
    n = vec3(2.9114, 2.9497, 2.5845);
    k = vec3(3.0893, 2.9318, 2.7670);
  } else if (code == 231u) {
    n = vec3(0.18299, 0.42108, 1.3734);
    k = vec3(3.4242, 2.3459, 1.7704);
  } else if (code == 232u) {
    n = vec3(1.3456, 0.96521, 0.61722);
    k = vec3(7.4746, 6.3995, 5.3031);
  } else if (code == 233u) {
    n = vec3(3.1071, 3.1812, 2.3230);
    k = vec3(3.3314, 3.3291, 3.1350);
  } else if (code == 234u) {
    n = vec3(0.27105, 0.67693, 1.3164);
    k = vec3(3.6092, 2.6248, 2.2921);
  } else if (code == 235u) {
    n = vec3(1.91, 1.83, 1.44);
    k = vec3(3.51, 3.40, 3.18);
  } else if (code == 236u) {
    n = vec3(2.3757, 2.0847, 1.8453);
    k = vec3(4.2655, 3.7153, 3.1365);
  } else if (code == 237u) {
    n = vec3(0.15943, 0.14512, 0.13547);
    k = vec3(3.9291, 3.19, 2.3808);
  } else {
    return baseColor;
  }
  vec3 numerator = (n - 1.0) * (n - 1.0) + k * k;
  vec3 denominator = (n + 1.0) * (n + 1.0) + k * k;
  return numerator / denominator;
}

vec3 identityColor(uint value) {
  uint x = value + 1u;
  x ^= x >> 16u;
  x *= 0x7feb352du;
  x ^= x >> 15u;
  x *= 0x846ca68bu;
  x ^= x >> 16u;
  return vec3(float((x >> 0u) & 255u),
              float((x >> 8u) & 255u),
              float((x >> 16u) & 255u)) /
         255.0;
}

vec3 labPbrDebugColor(uint view, vec3 baseColor, vec3 tangentNormal,
                      float ao, float ggxAlpha, vec3 f0, vec3 emission,
                      float opacity) {
  if (view == 1u) {
    return baseColor;
  }
  if (view == 2u) {
    return tangentNormal * 0.5 + 0.5;
  }
  if (view == 3u) {
    return vec3(ao);
  }
  if (view == 4u) {
    return vec3(ggxAlpha);
  }
  if (view == 5u) {
    return f0;
  }
  if (view == 6u) {
    return emission;
  }
  if (view == 7u) {
    return vec3(opacity);
  }
  return baseColor;
}

void main() {
  uint instanceId = gl_InstanceCustomIndexEXT;
  uint globalPrimitive =
      instanceMetadata[instanceId].x + gl_PrimitiveID;
  uvec4 metadata = primitiveMetadata[globalPrimitive];
  uint i0 = indices[globalPrimitive * 3u + 0u];
  uint i1 = indices[globalPrimitive * 3u + 1u];
  uint i2 = indices[globalPrimitive * 3u + 2u];
  float w0 = 1.0 - hitBarycentrics.x - hitBarycentrics.y;
  vec3 interpolatedNormal =
      normals[i0].xyz * w0 +
      normals[i1].xyz * hitBarycentrics.x +
      normals[i2].xyz * hitBarycentrics.y;
  interpolatedNormal = safeNormalizeNormal(
      interpolatedNormal, vec3(0.0, 1.0, 0.0));
  vec3 objectPosition0 = objectPosition(i0);
  vec3 objectPosition1 = objectPosition(i1);
  vec3 objectPosition2 = objectPosition(i2);
  // Transform edge vectors with w=0 so large instance translations cannot
  // erase a small triangle before the cross product. This also preserves the
  // winding flip from negative/non-uniform instance scales.
  vec3 worldEdge1 = gl_ObjectToWorldEXT *
                    vec4(objectPosition1 - objectPosition0, 0.0);
  vec3 worldEdge2 = gl_ObjectToWorldEXT *
                    vec4(objectPosition2 - objectPosition0, 0.0);
  vec3 geometricNormal = safeNormalizeNormal(
      cross(worldEdge1, worldEdge2),
      interpolatedNormal);
  if (dot(interpolatedNormal, geometricNormal) < 0.0) {
    interpolatedNormal = -interpolatedNormal;
  }

  payload.baseColor = vec3(0.0);
  payload.t = gl_HitTEXT;
  payload.shadingNormal = interpolatedNormal;
  payload.ggxAlpha = 1.0;
  payload.geometricNormal = geometricNormal;
  payload.ior = 1.5;
  payload.f0 = vec3(0.04);
  payload.transmission = 0.0;
  payload.emission = vec3(0.0);
  payload.opacity = 1.0;
  payload.attenuation = vec4(1.0, 1.0, 1.0, 0.0);
  payload.status =
      uvec4(1u, 0u, primitiveFlags[globalPrimitive],
            globalPrimitive + 1u);
  payload.hitData =
      vec4(hitBarycentrics,
           uintBitsToFloat(instanceId), 0.0);

  uint mode = pc.sizeMode.z;
  if (mode >= 1u && mode <= 6u) {
    if (mode == 1u) {
      payload.baseColor = identityColor(instanceId);
    } else if (mode == 2u) {
      payload.baseColor = identityColor(globalPrimitive);
    } else if (mode == 3u) {
      payload.baseColor = metadata.x == 0xffffffffu
                              ? vec3(0.18)
                              : identityColor(metadata.x);
    } else if (mode == 4u) {
      const vec3 faceColors[6] = vec3[6](
          vec3(0.92, 0.20, 0.18), vec3(0.18, 0.78, 0.28),
          vec3(0.20, 0.38, 0.92), vec3(0.92, 0.72, 0.16),
          vec3(0.70, 0.24, 0.88), vec3(0.16, 0.78, 0.82));
      payload.baseColor =
          metadata.y < 6u ? faceColors[metadata.y] : vec3(0.18);
    } else if (mode == 5u) {
      payload.baseColor = identityColor(metadata.z);
    } else {
      payload.baseColor = geometricNormal * 0.5 + 0.5;
    }
    return;
  }

  uint flags = primitiveFlags[globalPrimitive];
  vec4 vertexColor =
      colors[i0] * w0 +
      colors[i1] * hitBarycentrics.x +
      colors[i2] * hitBarycentrics.y;
  vec2 uv =
      uvs[i0] * w0 +
      uvs[i1] * hitBarycentrics.x +
      uvs[i2] * hitBarycentrics.y;
  float uvFootprint = rayConeUvFootprint(
      payload.rayCone, gl_HitTEXT, worldEdge1, worldEdge2,
      uvs[i0], uvs[i1], uvs[i2]);
  vec4 baseColor = vertexColor;
  bool textured = (flags & kMaterialTextured) != 0u;
  if (textured) {
    bool preserveCoverageLod =
        (flags & (kMaterialCutout | kMaterialBlend)) != 0u;
    vec4 sampledAlbedo =
        sampleAlbedoRayCone(uv, uvFootprint, preserveCoverageLod);
    baseColor = vec4(sampledAlbedo.rgb * vertexColor.rgb,
                     sampledAlbedo.a * vertexColor.a);
  }
  baseColor = clamp(baseColor, vec4(0.0), vec4(1.0));

  uint materialFeatures = (pc.sampling.w >> 8u) & 0xffu;
  bool normalMapActive =
      textured && (materialFeatures & 1u) != 0u;
  bool specularMapActive =
      textured && (materialFeatures & 2u) != 0u;
  vec4 tangentData =
      tangents[i0] * w0 +
      tangents[i1] * hitBarycentrics.x +
      tangents[i2] * hitBarycentrics.y;
  vec3 tangent =
      tangentData.xyz -
      interpolatedNormal * dot(interpolatedNormal, tangentData.xyz);
  float tangentLengthSquared = dot(tangent, tangent);
  if (!finiteFloat(tangentLengthSquared) ||
      tangentLengthSquared <= 1.0e-10) {
    vec3 helper = abs(interpolatedNormal.z) < 0.999
                      ? vec3(0.0, 0.0, 1.0)
                      : vec3(0.0, 1.0, 0.0);
    tangent = normalize(cross(helper, interpolatedNormal));
  } else {
    tangent *= inversesqrt(tangentLengthSquared);
  }
  vec3 bitangent =
      cross(interpolatedNormal, tangent) *
      (tangentData.w < 0.0 ? -1.0 : 1.0);
  vec3 normalSample =
      normalMapActive ? sampleNormalRayCone(uv, uvFootprint)
                      : vec3(0.5, 0.5, 1.0);
  vec3 tangentNormal =
      normalMapActive ? decodeLabPbrNormal(normalSample)
                      : vec3(0.0, 0.0, 1.0);
  vec3 materialNormal =
      safeNormalizeNormal(mat3(tangent, bitangent, interpolatedNormal) *
                              tangentNormal,
                          interpolatedNormal);
  if (dot(materialNormal, geometricNormal) < 0.0) {
    materialNormal = -materialNormal;
  }
  float ambientOcclusion = clamp(normalSample.b, 0.0, 1.0);
  float ggxAlpha = 1.0;
  vec3 f0 = vec3(0.04);
  bool metal = false;
  bool predefinedMetal = false;
  vec3 emission = vec3(0.0);
  if (specularMapActive) {
    vec4 specularSample = sampleSpecularRayCone(uv, uvFootprint);
    ggxAlpha = decodeLabPbrMicrofacetAlpha(specularSample.r);
    f0 = decodeLabPbrF0(specularSample.g, baseColor.rgb, metal,
                        predefinedMetal);
    emission =
        baseColor.rgb * decodeLabPbrEmission(specularSample.a);
  }
  float scalarF0 = clamp(f0.r, 0.0, 0.9604);
  float rootF0 = sqrt(scalarF0);
  float ior =
      clamp((1.0 + rootF0) / max(1.0 - rootF0, 0.02),
            1.0001, 99.0);
  // Base-texture alpha is coverage, not dielectric refraction. Physical
  // transmission only comes from the source-independent optics seam.
  vec4 surfaceOptics = primitiveOptics[globalPrimitive].parameters;
  vec4 attenuationOptics = primitiveOptics[globalPrimitive].attenuationColor;

  payload.baseColor = baseColor.rgb;
  payload.shadingNormal = materialNormal;
  payload.ggxAlpha = clamp(ggxAlpha, 0.0, 1.0);
  payload.geometricNormal = geometricNormal;
  payload.transmission = clamp(surfaceOptics.x, 0.0, 1.0);
  payload.ior = payload.transmission > 0.0
                    ? clamp(surfaceOptics.y, 1.0001, 99.0)
                    : ior;
  payload.f0 = clamp(f0, vec3(0.0), vec3(0.99));
  payload.emission = max(emission, vec3(0.0));
  payload.opacity = baseColor.a;
  payload.attenuation = vec4(
      clamp(attenuationOptics.rgb, vec3(1.0e-6), vec3(1.0)),
      max(surfaceOptics.z, 0.0));
  // status.y bit 0: metal, bit 1: predefined metal whose complete reflected
  // lobe must be tinted by linear albedo (custom metals already use it as F0).
  payload.status.y = (metal ? 1u : 0u) | (predefinedMetal ? 2u : 0u) |
                     ((floatBitsToUint(surfaceOptics.w) != 0u) ? 4u : 0u);

  if (mode == 7u) {
    payload.baseColor = baseColor.rgb;
    return;
  } else if (mode == 8u) {
    payload.baseColor = vec3(ggxAlpha);
    return;
  } else if (mode == 9u) {
    payload.baseColor = emission;
    return;
  }

  uint materialDebug = (pc.sampling.w >> 16u) & 0xffu;
  if (materialDebug != 0u) {
    payload.baseColor = labPbrDebugColor(
        materialDebug, baseColor.rgb, tangentNormal, ambientOcclusion,
        ggxAlpha, f0, emission, baseColor.a);
  }
}
