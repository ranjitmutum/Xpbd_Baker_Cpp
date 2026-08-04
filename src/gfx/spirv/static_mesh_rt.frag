#version 460
#extension GL_EXT_ray_query : require

layout(set = 0, binding = 1) uniform sampler2D uTexture;
layout(set = 0, binding = 2) uniform accelerationStructureEXT topLevelAS;
layout(set = 0, binding = 3) uniform sampler2D uNormalTexture;
layout(set = 0, binding = 4) uniform sampler2D uSpecularTexture;

layout(location = 0) in vec4 vTint;
layout(location = 1) in vec3 vNrm;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vFlags;
layout(location = 4) in vec3 vWorldPos;
layout(location = 5) in vec4 vTangent;
layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform PC {
  mat4 uMVP;
  vec4 lightDirAmb;
  vec4 lightColorInt;
  uvec4 materialDebug;
} pc;

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

vec3 labPbrDebugColor(uint view, vec3 baseColor, vec3 tangentNormal,
                      float ao, float ggx_alpha, vec3 f0, vec3 emission,
                      float opacity) {
  if (view == 2u) {
    return tangentNormal * 0.5 + 0.5;
  }
  if (view == 3u) {
    return vec3(ao);
  }
  if (view == 4u) {
    return vec3(ggx_alpha);
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

float rayTracedShadow(vec3 world_pos, vec3 normal, vec3 light_dir) {
  // light_dir points toward the light (same as lighting).
  vec3 L = normalize(light_dir);
  float ndl = max(dot(normalize(normal), L), 0.0);
  if (ndl <= 0.0) {
    return 0.35; // backface: soft ambient only
  }
  // Bias along normal + light to reduce self-shadow acne on cube edges.
  vec3 origin = world_pos + normalize(normal) * 0.02 + L * 0.01;
  const float t_min = 0.01;
  const float t_max = 500.0;

  rayQueryEXT rq;
  rayQueryInitializeEXT(
      rq, topLevelAS,
      gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT |
          gl_RayFlagsSkipClosestHitShaderEXT,
      0xFFu, origin, t_min, L, t_max);
  while (rayQueryProceedEXT(rq)) {
  }
  if (rayQueryGetIntersectionTypeEXT(rq, true) !=
      gl_RayQueryCommittedIntersectionNoneEXT) {
    return 0.28; // in shadow
  }
  return 1.0;
}

void main() {
  bool textured = (vFlags & 1u) != 0u;
  vec4 base_sample = textured ? texture(uTexture, vUV) : vec4(1.0);
  vec4 color =
      textured
          ? vec4(base_sample.rgb * vTint.rgb, base_sample.a * vTint.a)
          : vTint;
  if (color.a < 0.02) {
    discard;
  }

  vec3 geometric_normal = normalize(vNrm);
  vec3 tangent =
      normalize(vTangent.xyz -
                geometric_normal * dot(geometric_normal, vTangent.xyz));
  vec3 bitangent = cross(geometric_normal, tangent) * vTangent.w;
  bool normal_map_active = (pc.materialDebug.y & 1u) != 0u;
  vec4 normal_sample =
      textured && normal_map_active
          ? texture(uNormalTexture, vUV)
          : vec4(0.5, 0.5, 1.0, 1.0);
  vec3 tangent_normal = decodeLabPbrNormal(normal_sample);
  vec3 normal = normalize(mat3(tangent, bitangent, geometric_normal) *
                          tangent_normal);
  bool specular_map_active = (pc.materialDebug.y & 2u) != 0u;
  vec4 specular_sample =
      textured && specular_map_active
          ? texture(uSpecularTexture, vUV)
          : vec4(0.0, 0.04, 0.0, 1.0);
  float perceptual_roughness =
      specular_map_active ? 1.0 - specular_sample.r : 1.0;
  float ggx_alpha = perceptual_roughness * perceptual_roughness;
  bool metal = false;
  bool predefined_metal = false;
  vec3 f0 = specular_map_active
                ? decodeLabPbrF0(specular_sample.g, color.rgb, metal,
                                 predefined_metal)
                : vec3(0.04);
  float emission = specular_map_active
                       ? decodeLabPbrEmission(specular_sample.a)
                       : 0.0;
  vec3 emissive = color.rgb * emission;
  if (textured && pc.materialDebug.x != 0u) {
    FragColor =
        vec4(labPbrDebugColor(pc.materialDebug.x, color.rgb, tangent_normal,
                              normal_sample.b, ggx_alpha, f0, emissive,
                              color.a),
             color.a);
    return;
  }
  vec3 L = normalize(pc.lightDirAmb.xyz);
  float nd = max(dot(normal, L), 0.0);
  float wrap = max(nd, max(dot(-normal, L), 0.0) * 0.30);
  float ambient = pc.lightDirAmb.w;
  float intensity = pc.lightColorInt.w;
  vec3 light = pc.lightColorInt.xyz;
  float shadow = rayTracedShadow(vWorldPos, normal, L);

  if (textured) {
    float shade = ambient * normal_sample.b +
                  intensity * wrap * 0.55 * shadow;
    float specular_power = mix(96.0, 4.0, ggx_alpha);
    float specular = pow(max(nd, 0.0), specular_power) * shadow;
    vec3 diffuse = metal ? vec3(0.0) : color.rgb * shade * light;
    vec3 reflection_tint = predefined_metal ? color.rgb : vec3(1.0);
    vec3 reflected =
        reflection_tint * f0 * specular * intensity * light;
    FragColor =
        vec4(max(diffuse + reflected + emissive, vec3(0.0)), color.a);
    return;
  }

  float top = clamp(normal.y, 0.0, 1.0);
  float side = clamp(abs(normal.x) * 0.55 + abs(normal.z) * 0.45, 0.0, 1.0);
  float bot = clamp(-normal.y, 0.0, 1.0);
  float face_mul = 0.85 + 0.15 * top + 0.05 * side - 0.08 * bot;
  vec3 hue = vec3(0.0);
  if (abs(normal.y) >= abs(normal.x) && abs(normal.y) >= abs(normal.z)) {
    hue.r = 0.04 * top;
    hue.g = 0.02 * top;
  } else if (abs(normal.x) >= abs(normal.z)) {
    hue.b = 0.05 * side;
    hue.r = -0.02 * side;
  } else {
    hue.g = 0.03 * side;
    hue.b = 0.02 * side;
  }
  float shade = ambient + intensity * wrap * shadow;
  vec3 lit = color.rgb * face_mul * shade * light + hue;
  FragColor = vec4(clamp(lit, 0.0, 1.0), color.a);
}
