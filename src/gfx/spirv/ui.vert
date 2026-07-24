#version 450
layout(location=0) in vec2 Position;
layout(location=1) in vec2 TexCoord;
layout(location=2) in vec4 Color;
layout(push_constant) uniform PC { vec4 uST; } pc; // xy scale, zw translate
layout(location=0) out vec2 Frag_UV;
layout(location=1) out vec4 Frag_Color;
void main() {
  Frag_UV = TexCoord;
  Frag_Color = Color;
  vec2 p = Position * pc.uST.xy + pc.uST.zw;
  gl_Position = vec4(p, 0.0, 1.0);
}
