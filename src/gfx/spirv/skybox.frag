#version 450
layout(set = 0, binding = 0) uniform samplerCube uSky;
layout(push_constant) uniform PC {
  mat4 uVP;
  uvec4 flags;
} pc;
layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 FragColor;

vec3 srgbToLinear(vec3 value) {
  value = clamp(value, vec3(0.0), vec3(1.0));
  bvec3 cutoff = lessThanEqual(value, vec3(0.04045));
  vec3 low = value / 12.92;
  vec3 high = pow((value + 0.055) / 1.055, vec3(2.4));
  return mix(high, low, cutoff);
}

void main() {
  vec4 sky = texture(uSky, normalize(vDir));
  if ((pc.flags.x & 1u) != 0u) {
    sky.rgb = srgbToLinear(sky.rgb);
  }
  FragColor = sky;
}
