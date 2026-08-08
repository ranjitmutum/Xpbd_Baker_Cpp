#version 460
#extension GL_EXT_ray_tracing : require

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
  // xy barycentrics, z uintBitsToFloat(instance id), w LabPBR SSS [0,1].
  vec4 hitData;
  vec2 rayCone;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;

void main() {
  payload.baseColor = vec3(0.0);
  payload.t = -1.0;
  payload.shadingNormal = vec3(0.0, 1.0, 0.0);
  payload.ggxAlpha = 1.0;
  payload.geometricNormal = vec3(0.0, 1.0, 0.0);
  payload.ior = 1.5;
  payload.f0 = vec3(0.04);
  payload.transmission = 0.0;
  payload.emission = vec3(0.0);
  payload.opacity = 0.0;
  payload.attenuation = vec4(1.0, 1.0, 1.0, 0.0);
  payload.status = uvec4(0u);
  payload.hitData = vec4(0.0);
}
