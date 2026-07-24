#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNrm;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uint aBoneIndex;
layout(location = 4) in uint aFlags;

struct BoneState {
  mat4 transform;
  vec4 tint;
};

layout(std430, set = 0, binding = 0) readonly buffer BoneBuffer {
  BoneState bones[];
};

layout(push_constant) uniform PC {
  mat4 uMVP;
} pc;

layout(location = 0) out vec4 vTint;
layout(location = 1) out vec3 vNrm;
layout(location = 2) out vec2 vUV;
layout(location = 3) flat out uint vFlags;

void main() {
  BoneState bone = bones[aBoneIndex];
  vec4 world = bone.transform * vec4(aPos, 1.0);
  vTint = bone.tint;
  vNrm = normalize(mat3(bone.transform) * aNrm);
  vUV = aUV;
  vFlags = aFlags;
  gl_Position = pc.uMVP * world;
}
