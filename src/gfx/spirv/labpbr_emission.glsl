// Shared formal-RT LabPBR emission semantics. Texture fetch LOD and coverage
// traversal remain caller-owned, but both hit evaluation and explicit-light
// sampling decode and tint emitted radiance through these functions.

float decodeLabPbrEmission(float packed) {
  return packed > (254.5 / 255.0) ? 0.0
                                  : packed * (255.0 / 254.0);
}

vec3 evaluateLabPbrEmission(vec3 linearBaseColor, float packedEmission) {
  return max(linearBaseColor, vec3(0.0)) *
         decodeLabPbrEmission(packedEmission);
}

float labPbrEmissionCoverageWeight(float opacity, bool cutout,
                                    bool blend) {
  float coverage = isnan(opacity) || isinf(opacity)
                       ? 1.0
                       : clamp(opacity, 0.0, 1.0);
  if ((cutout || blend) && !(coverage >= 0.02)) {
    return 0.0;
  }
  // Blend is stochastic alpha coverage in the beauty path, so direct-light
  // evaluation uses its expectation. Surviving cutout texels are binary.
  return blend ? coverage : 1.0;
}
