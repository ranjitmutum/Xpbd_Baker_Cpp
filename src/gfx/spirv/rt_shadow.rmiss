#version 460
#extension GL_EXT_ray_tracing : require

struct ShadowPayload {
  float visibility;
  float reserved;
  vec2 rayCone;
};

layout(location = 1) rayPayloadInEXT ShadowPayload shadowPayload;

void main() {
  // Any-hit may already have accumulated deterministic Coverage/Transmission
  // visibility through ignored layers. Preserve it on miss.
  shadowPayload.visibility = clamp(shadowPayload.visibility, 0.0, 1.0);
}
