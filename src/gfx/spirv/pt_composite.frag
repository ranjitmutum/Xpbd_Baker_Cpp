#version 450

layout(set = 0, binding = 0) uniform sampler2D uPathTrace;
layout(set = 0, binding = 1) uniform sampler2D uPathDepth;
layout(set = 0, binding = 2) uniform sampler2D uReconstructed;
layout(set = 0, binding = 3) uniform sampler2D uDiagnosticAov;
layout(set = 0, binding = 4) uniform sampler2D uRrMotion;
layout(set = 0, binding = 5) uniform sampler2D uRrDiffuseAlbedo;
layout(set = 0, binding = 6) uniform sampler2D uRrSpecularAlbedo;
layout(set = 0, binding = 7) uniform sampler2D uRrNormalRoughness;
layout(set = 0, binding = 8) uniform sampler2D uRrSpecularHitDistance;
layout(set = 0, binding = 9) uniform sampler2D uReactiveMask;
layout(set = 0, binding = 10) uniform sampler2D uTransparencyComposition;
layout(set = 0, binding = 11) uniform sampler2D uGuideValidity;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform CompositePush {
  vec4 display;
  uvec4 flags;
} composite_push;

vec3 whiteBalance(float kelvin) {
  float t = clamp(kelvin, 1000.0, 40000.0) / 6500.0;
  return vec3(pow(t, 0.45), 1.0, pow(1.0 / t, 0.45));
}

vec3 acesApprox(vec3 value) {
  return clamp((value * (2.51 * value + 0.03)) /
                   (value * (2.43 * value + 0.59) + 0.14),
               0.0, 1.0);
}

vec3 linearToSrgb(vec3 value) {
  value = clamp(value, vec3(0.0), vec3(1.0));
  bvec3 cutoff = lessThanEqual(value, vec3(0.0031308));
  vec3 low = value * 12.92;
  vec3 high = 1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055;
  return mix(high, low, cutoff);
}

bool reconstructedEnabled() {
  return (composite_push.flags.x & 2u) != 0u;
}

const uint kRrAovDebugOff = 0u;
const uint kRrAovDebugRawColor = 1u;
const uint kRrAovDebugReconstructedColor = 2u;
const uint kRrAovDebugDeviceDepth = 3u;
const uint kRrAovDebugLinearDepth = 4u;
const uint kRrAovDebugMotion = 5u;
const uint kRrAovDebugMotionMagnitude = 6u;
const uint kRrAovDebugPreviousUvOutside = 7u;
const uint kRrAovDebugDiffuseAlbedo = 8u;
const uint kRrAovDebugSpecularAlbedo = 9u;
const uint kRrAovDebugNormal = 10u;
const uint kRrAovDebugRoughness = 11u;
const uint kRrAovDebugSpecularHitDistance = 12u;
const uint kRrAovDebugReactiveMask = 13u;
const uint kRrAovDebugTransparencyComposition = 14u;
const uint kRrAovDebugGuideValidity = 15u;
const uint kRrAovDebugTemporalBoundaryOverlay = 16u;

uint rrAovDebugMode() {
  return composite_push.flags.w;
}

vec2 currentRawUv(vec2 outputUv) {
  if (!reconstructedEnabled()) {
    return outputUv;
  }
  // Temporal primary rays use pixelCenter-jitter. DLSS output is in
  // unjittered output space, so address the current raw frame at +jitter.
  vec2 rawSize = vec2(textureSize(uPathTrace, 0));
  vec2 jitterPixels = uintBitsToFloat(composite_push.flags.yz);
  vec2 rawUv = outputUv + jitterPixels / rawSize;
  vec2 halfTexel = vec2(0.5) / rawSize;
  return clamp(rawUv, halfTexel, vec2(1.0) - halfTexel);
}

float sampleCurrentCoverage(vec2 rawUv) {
  // Spatially upscale this frame's coverage without temporal history. The
  // shared sampler remains nearest for pixel-art color/depth, so perform the
  // four-tap coverage filter explicitly.
  ivec2 size = textureSize(uPathTrace, 0);
  vec2 pixel = rawUv * vec2(size) - vec2(0.5);
  ivec2 base = ivec2(floor(pixel));
  vec2 weight = fract(pixel);
  ivec2 maximum = size - ivec2(1);
  ivec2 p00 = clamp(base, ivec2(0), maximum);
  ivec2 p10 = clamp(base + ivec2(1, 0), ivec2(0), maximum);
  ivec2 p01 = clamp(base + ivec2(0, 1), ivec2(0), maximum);
  ivec2 p11 = clamp(base + ivec2(1, 1), ivec2(0), maximum);
  float a00 = texelFetch(uPathTrace, p00, 0).a;
  float a10 = texelFetch(uPathTrace, p10, 0).a;
  float a01 = texelFetch(uPathTrace, p01, 0).a;
  float a11 = texelFetch(uPathTrace, p11, 0).a;
  return clamp(mix(mix(a00, a10, weight.x),
                   mix(a01, a11, weight.x), weight.y),
               0.0, 1.0);
}

vec3 sampleDisplayRgb(vec2 outputUv, vec2 rawUv) {
  if (!reconstructedEnabled()) {
    return texture(uPathTrace, rawUv).rgb;
  }
  return texture(uReconstructed, outputUv).rgb;
}

float sampleForegroundCoverage(vec2 outputUv, vec2 rawUv) {
  if (!reconstructedEnabled()) {
    return clamp(texture(uPathTrace, outputUv).a, 0.0, 1.0);
  }
  return sampleCurrentCoverage(rawUv);
}

vec4 sampleDisplayColor(vec2 outputUv) {
  vec2 rawUv = currentRawUv(outputUv);
  return vec4(sampleDisplayRgb(outputUv, rawUv),
              sampleForegroundCoverage(outputUv, rawUv));
}

vec3 debugMotion(vec2 rawUv) {
  vec2 motionPixels = texture(uRrMotion, rawUv).xy;
  vec2 encoded = clamp(motionPixels / 32.0, vec2(-1.0), vec2(1.0));
  return vec3(encoded * 0.5 + 0.5, 0.0);
}

bool previousUvOutsideAt(vec2 rawUv) {
  vec2 motionPixels = texture(uRrMotion, rawUv).xy;
  vec2 motionSize = vec2(textureSize(uRrMotion, 0));
  vec2 previousUv = rawUv + motionPixels / max(motionSize, vec2(1.0));
  return previousUv.x < 0.0 || previousUv.y < 0.0 ||
         previousUv.x >= 1.0 || previousUv.y >= 1.0;
}

bool guideValidAt(vec2 rawUv) {
  return texture(uGuideValidity, rawUv).r >= 0.5;
}

bool rawUvClampedAt(vec2 outputUv) {
  if (!reconstructedEnabled()) {
    return false;
  }
  vec2 rawSize = vec2(textureSize(uPathTrace, 0));
  vec2 jitterPixels = uintBitsToFloat(composite_push.flags.yz);
  vec2 requestedUv = outputUv + jitterPixels / rawSize;
  vec2 halfTexel = vec2(0.5) / rawSize;
  vec2 clampedUv = clamp(requestedUv, halfTexel, vec2(1.0) - halfTexel);
  return any(greaterThan(abs(requestedUv - clampedUv), vec2(1.0e-7)));
}

bool booleanBoundaryPreviousUv(vec2 rawUv) {
  vec2 size = vec2(textureSize(uRrMotion, 0));
  vec2 texel = 1.0 / max(size, vec2(1.0));
  bool center = previousUvOutsideAt(rawUv);
  return previousUvOutsideAt(clamp(rawUv + vec2(texel.x, 0.0),
                                   vec2(0.0), vec2(1.0))) != center ||
         previousUvOutsideAt(clamp(rawUv - vec2(texel.x, 0.0),
                                   vec2(0.0), vec2(1.0))) != center ||
         previousUvOutsideAt(clamp(rawUv + vec2(0.0, texel.y),
                                   vec2(0.0), vec2(1.0))) != center ||
         previousUvOutsideAt(clamp(rawUv - vec2(0.0, texel.y),
                                   vec2(0.0), vec2(1.0))) != center;
}

bool booleanBoundaryGuideValidity(vec2 rawUv) {
  vec2 size = vec2(textureSize(uGuideValidity, 0));
  vec2 texel = 1.0 / max(size, vec2(1.0));
  bool center = guideValidAt(rawUv);
  return guideValidAt(clamp(rawUv + vec2(texel.x, 0.0),
                            vec2(0.0), vec2(1.0))) != center ||
         guideValidAt(clamp(rawUv - vec2(texel.x, 0.0),
                            vec2(0.0), vec2(1.0))) != center ||
         guideValidAt(clamp(rawUv + vec2(0.0, texel.y),
                            vec2(0.0), vec2(1.0))) != center ||
         guideValidAt(clamp(rawUv - vec2(0.0, texel.y),
                            vec2(0.0), vec2(1.0))) != center;
}

vec3 temporalBoundaryOverlayColor(vec2 outputUv, vec2 rawUv,
                                  vec3 displayColor) {
  bool previousBoundary = booleanBoundaryPreviousUv(rawUv);
  bool guideBoundary = booleanBoundaryGuideValidity(rawUv);
  bool clampBoundary = rawUvClampedAt(outputUv);

  if (previousBoundary && guideBoundary) {
    return vec3(1.0, 1.0, 0.0);
  }
  if (previousBoundary) {
    return vec3(1.0, 0.0, 0.0);
  }
  if (guideBoundary) {
    return vec3(0.0, 1.0, 1.0);
  }
  if (clampBoundary) {
    return vec3(1.0, 0.0, 1.0);
  }
  return displayColor;
}

vec3 debugPreviousUvOutside(vec2 rawUv) {
  bool reconstructedOutside = previousUvOutsideAt(rawUv);
  // The MotionDisocclusion AOV preserves probe.motion.z/w that the actual
  // RG32F Streamline motion image cannot store. White means both paths agree;
  // red means only the shader-side flag rejected history; green means only
  // the RG motion reconstruction falls outside.
  vec4 diagnosticMotion = texture(uDiagnosticAov, rawUv);
  bool shaderOutside = diagnosticMotion.z > 0.5 ||
                       diagnosticMotion.w < 0.5;
  if (shaderOutside && reconstructedOutside) {
    return vec3(1.0);
  }
  if (shaderOutside) {
    return vec3(1.0, 0.0, 0.0);
  }
  if (reconstructedOutside) {
    return vec3(0.0, 1.0, 0.0);
  }
  return vec3(0.0);
}

vec3 sampleRrAovDebug(uint mode, vec2 outputUv, vec2 rawUv) {
  if (mode == kRrAovDebugRawColor) {
    return clamp(texture(uPathTrace, rawUv).rgb, vec3(0.0), vec3(1.0));
  }
  if (mode == kRrAovDebugReconstructedColor) {
    return clamp(texture(uReconstructed, outputUv).rgb,
                 vec3(0.0), vec3(1.0));
  }
  if (mode == kRrAovDebugDeviceDepth) {
    return vec3(clamp(texture(uPathDepth, rawUv).r, 0.0, 1.0));
  }
  if (mode == kRrAovDebugLinearDepth) {
    float linearDepth = max(texture(uDiagnosticAov, rawUv).a, 0.0);
    float mapped = log2(1.0 + linearDepth) / log2(1001.0);
    return vec3(clamp(mapped, 0.0, 1.0));
  }
  if (mode == kRrAovDebugMotion) {
    return debugMotion(rawUv);
  }
  if (mode == kRrAovDebugMotionMagnitude) {
    float magnitude = length(texture(uRrMotion, rawUv).xy);
    return vec3(clamp(magnitude / 32.0, 0.0, 1.0));
  }
  if (mode == kRrAovDebugPreviousUvOutside) {
    return debugPreviousUvOutside(rawUv);
  }
  if (mode == kRrAovDebugDiffuseAlbedo) {
    return clamp(texture(uRrDiffuseAlbedo, rawUv).rgb,
                 vec3(0.0), vec3(1.0));
  }
  if (mode == kRrAovDebugSpecularAlbedo) {
    return clamp(texture(uRrSpecularAlbedo, rawUv).rgb,
                 vec3(0.0), vec3(1.0));
  }
  if (mode == kRrAovDebugNormal) {
    vec3 normal = texture(uRrNormalRoughness, rawUv).xyz;
    float lengthSquared = dot(normal, normal);
    normal = lengthSquared > 1.0e-8
                 ? normal * inversesqrt(lengthSquared)
                 : vec3(0.0, 0.0, 1.0);
    return normal * 0.5 + 0.5;
  }
  if (mode == kRrAovDebugRoughness) {
    return vec3(clamp(texture(uRrNormalRoughness, rawUv).a, 0.0, 1.0));
  }
  if (mode == kRrAovDebugSpecularHitDistance) {
    float distance = max(texture(uRrSpecularHitDistance, rawUv).r, 0.0);
    float mapped = log2(1.0 + distance) / log2(1001.0);
    return vec3(clamp(mapped, 0.0, 1.0));
  }
  if (mode == kRrAovDebugReactiveMask) {
    return vec3(clamp(texture(uReactiveMask, rawUv).r, 0.0, 1.0));
  }
  if (mode == kRrAovDebugTransparencyComposition) {
    return vec3(clamp(texture(uTransparencyComposition, rawUv).r,
                      0.0, 1.0));
  }
  if (mode == kRrAovDebugGuideValidity) {
    return vec3(clamp(texture(uGuideValidity, rawUv).r, 0.0, 1.0));
  }
  return vec3(0.0);
}

void main() {
  uint debugMode = rrAovDebugMode();
  if (debugMode != kRrAovDebugOff &&
      debugMode != kRrAovDebugTemporalBoundaryOverlay) {
    vec2 rawUv = currentRawUv(vUV);
    vec3 debugColor = sampleRrAovDebug(debugMode, vUV, rawUv);
    if ((composite_push.flags.x & 4u) != 0u) {
      debugColor = linearToSrgb(
          clamp(debugColor, vec3(0.0), vec3(1.0)));
    }
    gl_FragDepth = 1.0;
    FragColor = vec4(debugColor, 1.0);
    return;
  }
  vec2 rawUv = currentRawUv(vUV);
  vec3 rgb = sampleDisplayRgb(vUV, rawUv);
  float coverage = sampleForegroundCoverage(vUV, rawUv);
  float depth = clamp(texture(uPathDepth, rawUv).r, 0.0, 1.0);
  // Current-frame coverage is the single authority for foreground visibility.
  // Do not combine temporally reconstructed alpha with raw current depth.
  if (coverage < 0.001) {
    discard;
  }

  // The path-trace target stores straight RGBA. The sampled depth belongs to
  // the nearest opaque RT surface, allowing
  // later raster grid/axis/skeleton passes to remain correctly occluded.
  gl_FragDepth = depth;
  vec4 color = vec4(rgb, coverage);
  color.rgb *= max(composite_push.display.x, 0.0);
  vec3 whiteBalanceScale = whiteBalance(composite_push.display.y);
  color.rgb *= whiteBalanceScale;

  float bloom = max(composite_push.display.z, 0.0);
  if (bloom > 0.0) {
    ivec2 sourceSize =
        reconstructedEnabled() ? textureSize(uReconstructed, 0)
                               : textureSize(uPathTrace, 0);
    vec2 texel = 1.0 / vec2(sourceSize);
    vec3 glow = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        vec4 sampleValue =
            sampleDisplayColor(vUV + vec2(x, y) * texel);
        vec3 sampleColor =
            sampleValue.rgb * clamp(sampleValue.a, 0.0, 1.0) *
            max(composite_push.display.x, 0.0) * whiteBalanceScale;
        glow += max(sampleColor - vec3(1.0), vec3(0.0));
      }
    }
    color.rgb += glow * (bloom / 9.0);
  }

  int toneMapping = int(round(composite_push.display.w));
  if (toneMapping == 1) {
    color.rgb = color.rgb / (vec3(1.0) + color.rgb);
  } else if (toneMapping == 2) {
    color.rgb = acesApprox(color.rgb);
  }
  if ((composite_push.flags.x & 4u) != 0u) {
    color.rgb = linearToSrgb(color.rgb);
  }
  if (debugMode == kRrAovDebugTemporalBoundaryOverlay) {
    color.rgb = temporalBoundaryOverlayColor(vUV, rawUv, color.rgb);
  }
  // Path tracing and DLSS exchange straight RGBA. The Vulkan blend state
  // consumes premultiplied color, so associate RGB with coverage exactly once,
  // after nonlinear display transforms.
  color.rgb *= color.a;
  FragColor = color;
}
