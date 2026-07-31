#version 460
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec4 aCol;
layout(push_constant) uniform PC {
  mat4 uMVP;
  vec4 lightDirAmb;
  vec4 lightColorInt;
} pc;
layout(location = 0) out vec4 vCol;
layout(location = 1) out vec3 vNrm;
layout(location = 2) out vec3 vWorldPos;
void main() {
  vCol = aCol;
  vNrm = aNrm;
  vWorldPos = aPos;
  gl_Position = pc.uMVP * vec4(aPos, 1.0);
}
