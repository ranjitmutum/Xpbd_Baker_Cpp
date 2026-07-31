#version 450
layout(location = 0) in vec3 aPos;
layout(push_constant) uniform PC {
  mat4 uVP;
  vec4 params; // x=time, y=scene_id, z=dynamic, w=pad
} pc;
layout(location = 0) out vec3 vDir;
void main() {
  vDir = aPos;
  vec4 clip = pc.uVP * vec4(aPos, 1.0);
  gl_Position = clip.xyww;
}
