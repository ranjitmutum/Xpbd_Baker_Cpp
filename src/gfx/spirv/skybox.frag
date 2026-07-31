#version 450
layout(set = 0, binding = 0) uniform samplerCube uSky;
layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 FragColor;

void main() {
  FragColor = texture(uSky, normalize(vDir));
}
