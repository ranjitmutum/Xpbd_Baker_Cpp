#version 450

layout(set = 0, binding = 1) uniform sampler2D uTexture;

layout(location = 0) in vec4 vTint;
layout(location = 1) in vec3 vNrm;
layout(location = 2) in vec2 vUV;
layout(location = 3) flat in uint vFlags;
layout(location = 0) out vec4 FragColor;

void main() {
  bool textured = (vFlags & 1u) != 0u;
  vec4 color = textured ? texture(uTexture, vUV) * vTint : vTint;
  if (color.a < 0.02) {
    discard;
  }
  if (textured) {
    FragColor = color;
    return;
  }

  vec3 normal = normalize(vNrm);
  float top = clamp(normal.y, 0.0, 1.0);
  float side = clamp(abs(normal.x) * 0.55 + abs(normal.z) * 0.45, 0.0, 1.0);
  float bot = clamp(-normal.y, 0.0, 1.0);
  float multiplier = 0.72 + 0.28 * top + 0.08 * side - 0.12 * bot;
  vec3 hue = vec3(0.0);
  if (abs(normal.y) >= abs(normal.x) &&
      abs(normal.y) >= abs(normal.z)) {
    hue.r = 0.04 * top;
    hue.g = 0.02 * top;
  } else if (abs(normal.x) >= abs(normal.z)) {
    hue.b = 0.05 * side;
    hue.r = -0.02 * side;
  } else {
    hue.g = 0.03 * side;
    hue.b = 0.02 * side;
  }
  FragColor = vec4(clamp(color.rgb * multiplier + hue, 0.0, 1.0), color.a);
}
