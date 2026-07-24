#version 450
layout(location=0) in vec4 vCol;
layout(location=1) in vec3 vNrm;
layout(location=0) out vec4 FragColor;
void main() {
  if (vCol.a < 0.02) discard;
  float nd = abs(dot(normalize(vNrm), normalize(vec3(0.35, 0.85, 0.40))));
  float shade = 0.92 + 0.08 * nd;
  FragColor = vec4(vCol.rgb * shade, vCol.a);
}
