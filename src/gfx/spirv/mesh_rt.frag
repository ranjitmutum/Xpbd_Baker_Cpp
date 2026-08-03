#version 460
#extension GL_EXT_ray_query : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT topLevelAS;

layout(location = 0) in vec4 vCol;
layout(location = 1) in vec3 vNrm;
layout(location = 2) in vec3 vWorldPos;
layout(location = 0) out vec4 FragColor;
layout(push_constant) uniform PC {
  mat4 uMVP;
  vec4 lightDirAmb;
  vec4 lightColorInt;
} pc;

float rayTracedShadow(vec3 world_pos, vec3 normal, vec3 light_dir) {
  vec3 L = normalize(light_dir);
  float nlen2 = dot(normal, normal);
  vec3 N = nlen2 > 1e-8 ? normalize(normal) : vec3(0.0, 1.0, 0.0);
  vec3 origin = world_pos + N * 0.03 + L * 0.01;
  rayQueryEXT rq;
  rayQueryInitializeEXT(
      rq, topLevelAS,
      gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT |
          gl_RayFlagsSkipClosestHitShaderEXT,
      0xFFu, origin, 0.02, L, 500.0);
  while (rayQueryProceedEXT(rq)) {
  }
  if (rayQueryGetIntersectionTypeEXT(rq, true) !=
      gl_RayQueryCommittedIntersectionNoneEXT) {
    return 0.32;
  }
  return 1.0;
}

void main() {
  if (vCol.a < 0.02) discard;
  vec3 n = vNrm;
  float nlen2 = dot(n, n);
  if (nlen2 < 1e-8) {
    FragColor = vCol;
    return;
  }
  n = normalize(n);
  vec3 L = normalize(pc.lightDirAmb.xyz);
  float nd = max(dot(n, L), 0.0);
  float wrap = max(nd, max(dot(-n, L), 0.0) * 0.35);
  float ambient = pc.lightDirAmb.w;
  float intensity = pc.lightColorInt.w;
  vec3 light = pc.lightColorInt.xyz;
  float shadow = rayTracedShadow(vWorldPos, n, L);
  vec3 V = normalize(L + vec3(0.0, 0.9, 0.0));
  vec3 H = normalize(L + V);
  float spec = pow(max(dot(n, H), 0.0), 56.0) * intensity * 0.28 * shadow;
  spec *= mix(0.55, 1.15, clamp(vCol.a, 0.0, 1.0));
  vec3 lit =
      vCol.rgb * (ambient + intensity * wrap * light * shadow) + light * spec;
  FragColor = vec4(clamp(lit, 0.0, 1.0), vCol.a);
}
