#version 450
layout(set=0, binding=0) uniform sampler2D Texture;
layout(location=0) in vec2 Frag_UV;
layout(location=1) in vec4 Frag_Color;
layout(location=0) out vec4 Out_Color;
void main() {
  // Font atlas: white RGB + coverage in A (ALPHA8 expanded on upload).
  vec4 t = texture(Texture, Frag_UV);
  Out_Color = vec4(Frag_Color.rgb * t.rgb, Frag_Color.a * t.a);
}
