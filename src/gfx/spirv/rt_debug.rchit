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
  float roughness;
  vec3 geometricNormal;
  float ior;
  vec3 f0;
  float transmission;
  vec3 emission;
  float opacity;
  uvec4 status;
  vec4 hitData;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;
hitAttributeEXT vec2 hitBarycentrics;

const uint kMaterialTextured = 1u << 0u;

vec3 srgbToLinear(vec3 value) {
  bvec3 cutoff = lessThanEqual(value, vec3(0.04045));
  vec3 low = value / 12.92;
  vec3 high = pow((value + 0.055) / 1.055, vec3(2.4));
  return mix(high, low, cutoff);
}

vec3 decodeLabPbrNormal(vec4 packed) {
  vec2 xy = vec2(packed.r * 2.0 - 1.0, 1.0 - packed.g * 2.0);
  float xy2 = dot(xy, xy);
  if (xy2 > 1.0) {
    xy *= inversesqrt(xy2);
  }
  return vec3(xy, sqrt(max(0.0, 1.0 - dot(xy, xy))));
}

float decodeLabPbrEmission(float packed) {
  return packed > (254.5 / 255.0) ? 0.0
                                  : packed * (255.0 / 254.0);
}

vec3 decodeLabPbrF0(float packed, vec3 baseColor, out bool metal) {
  uint code = uint(round(packed * 255.0));
  metal = code >= 230u;
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
                      float ao, float roughness, vec3 f0, vec3 emission,
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
    return vec3(roughness);
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
  vec3 geometricNormal =
      normals[i0].xyz * w0 +
      normals[i1].xyz * hitBarycentrics.x +
      normals[i2].xyz * hitBarycentrics.y;
  geometricNormal = dot(geometricNormal, geometricNormal) > 1.0e-12
                        ? normalize(geometricNormal)
                        : vec3(0.0, 1.0, 0.0);

  payload.baseColor = vec3(0.0);
  payload.t = gl_HitTEXT;
  payload.shadingNormal = geometricNormal;
  payload.roughness = 1.0;
  payload.geometricNormal = geometricNormal;
  payload.ior = 1.5;
  payload.f0 = vec3(0.04);
  payload.transmission = 0.0;
  payload.emission = vec3(0.0);
  payload.opacity = 1.0;
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
  vec4 baseColor = vertexColor;
  bool textured = (flags & kMaterialTextured) != 0u;
  if (textured) {
    vec4 packed = textureLod(albedoTexture, uv, 0.0);
    baseColor =
        vec4(srgbToLinear(packed.rgb) * vertexColor.rgb,
             packed.a * vertexColor.a);
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
      geometricNormal * dot(geometricNormal, tangentData.xyz);
  if (dot(tangent, tangent) <= 1.0e-10) {
    vec3 helper = abs(geometricNormal.z) < 0.999
                      ? vec3(0.0, 0.0, 1.0)
                      : vec3(0.0, 1.0, 0.0);
    tangent = normalize(cross(helper, geometricNormal));
  } else {
    tangent = normalize(tangent);
  }
  vec3 bitangent =
      cross(geometricNormal, tangent) *
      (tangentData.w < 0.0 ? -1.0 : 1.0);
  vec4 normalSample =
      normalMapActive ? textureLod(normalTexture, uv, 0.0)
                      : vec4(0.5, 0.5, 1.0, 1.0);
  vec3 tangentNormal =
      normalMapActive ? decodeLabPbrNormal(normalSample)
                      : vec3(0.0, 0.0, 1.0);
  vec3 materialNormal =
      normalize(mat3(tangent, bitangent, geometricNormal) *
                tangentNormal);
  float ambientOcclusion = normalSample.b;
  float linearRoughness = 1.0;
  vec3 f0 = vec3(0.04);
  bool metal = false;
  vec3 emission = vec3(0.0);
  if (specularMapActive) {
    vec4 specularSample = textureLod(specularTexture, uv, 0.0);
    float perceptualRoughness = 1.0 - specularSample.r;
    linearRoughness =
        perceptualRoughness * perceptualRoughness;
    f0 = decodeLabPbrF0(specularSample.g, baseColor.rgb, metal);
    emission =
        baseColor.rgb * decodeLabPbrEmission(specularSample.a);
  }
  float scalarF0 = clamp(f0.r, 0.0, 0.9604);
  float rootF0 = sqrt(scalarF0);
  float ior =
      clamp((1.0 + rootF0) / max(1.0 - rootF0, 0.02),
            1.0001, 99.0);
  // Base-texture alpha is coverage, not dielectric refraction.  RayGen
  // handles Blend coverage stochastically and only shades accepted layers.
  float transmission = 0.0;

  payload.baseColor = baseColor.rgb;
  payload.shadingNormal = materialNormal;
  payload.roughness = clamp(linearRoughness, 0.02, 1.0);
  payload.geometricNormal = geometricNormal;
  payload.ior = ior;
  payload.f0 = clamp(f0, vec3(0.0), vec3(0.99));
  payload.transmission = clamp(transmission, 0.0, 1.0);
  payload.emission = max(emission, vec3(0.0));
  payload.opacity = baseColor.a;
  payload.status.y = metal ? 1u : 0u;

  if (mode == 7u) {
    payload.baseColor = baseColor.rgb;
    return;
  } else if (mode == 8u) {
    payload.baseColor = vec3(linearRoughness);
    return;
  } else if (mode == 9u) {
    payload.baseColor = emission;
    return;
  }

  uint materialDebug = (pc.sampling.w >> 16u) & 0xffu;
  if (materialDebug != 0u) {
    payload.baseColor = labPbrDebugColor(
        materialDebug, baseColor.rgb, tangentNormal, ambientOcclusion,
        linearRoughness, f0, emission, baseColor.a);
  }
}
