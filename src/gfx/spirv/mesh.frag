#version 450
layout(location=0) in vec4 vCol;
layout(location=1) in vec3 vNrm;
layout(location=0) out vec4 FragColor;
layout(push_constant) uniform PC {
  mat4 uMVP;
  vec4 lightDirAmb;   // xyz = light direction (toward light), w = ambient
  vec4 lightColorInt; // xyz = light color, w = intensity
} pc;
void main() {
  if (vCol.a < 0.02) discard;
  vec3 n = vNrm;
  float nlen2 = dot(n, n);
  // Line lists / unlit helpers use zero-length normals → flat vertex color.
  if (nlen2 < 1e-8) {
    FragColor = vCol;
    return;
  }
  n = normalize(n);
  vec3 L = normalize(pc.lightDirAmb.xyz);
  float nd = max(dot(n, L), 0.0);
  // Soft wrap for double-sided mesh (no cull).
  float wrap = max(nd, max(dot(-n, L), 0.0) * 0.35);
  float ambient = pc.lightDirAmb.w;
  float intensity = pc.lightColorInt.w;
  vec3 light = pc.lightColorInt.xyz;
  // Cheap glint without camera uniform: approximate view as light + up.
  // Helps ocean wave crests and any lit mesh read more 3D.
  vec3 V = normalize(L + vec3(0.0, 0.9, 0.0));
  vec3 H = normalize(L + V);
  float spec = pow(max(dot(n, H), 0.0), 56.0) * intensity * 0.28;
  // Slightly stronger on more-opaque fragments (foam / solid).
  spec *= mix(0.55, 1.15, clamp(vCol.a, 0.0, 1.0));
  vec3 lit = vCol.rgb * (ambient + intensity * wrap * light) + light * spec;
  FragColor = vec4(clamp(lit, 0.0, 1.0), vCol.a);
}
