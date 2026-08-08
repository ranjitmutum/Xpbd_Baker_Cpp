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
  vec4 hitData;
  vec2 rayCone;
};

layout(location = 0) rayPayloadInEXT PrimaryPayload payload;

void main() {
  // Any-hit may already have accumulated deterministic Coverage/Transmission
  // visibility through ignored layers. Preserve it on miss.
  payload.transmission = clamp(payload.transmission, 0.0, 1.0);
}
