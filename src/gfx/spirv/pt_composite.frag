#version 450

layout(set = 0, binding = 0) uniform sampler2D uPathTrace;
layout(set = 0, binding = 1) uniform sampler2D uPathDepth;
layout(set = 0, binding = 2) uniform sampler2D uReconstructed;
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

vec2 currentRawUv(vec2 outputUv) {
  if (!reconstructedEnabled()) {
    return outputUv;
  }
  // Temporal primary rays use pixelCenter-jitter. DLSS output is in
  // unjittered output space, so address the current raw frame at +jitter.
  vec2 jitterPixels = uintBitsToFloat(composite_push.flags.yz);
  return outputUv +
         jitterPixels / vec2(textureSize(uPathTrace, 0));
}

float sampleCurrentCoverage(vec2 rawUv) {
  if (any(lessThan(rawUv, vec2(0.0))) ||
      any(greaterThanEqual(rawUv, vec2(1.0)))) {
    return 0.0;
  }
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

vec4 sampleDisplayColor(vec2 outputUv) {
  if (!reconstructedEnabled()) {
    vec4 raw = texture(uPathTrace, outputUv);
    raw.a = clamp(raw.a, 0.0, 1.0);
    return raw;
  }
  vec4 reconstructed = texture(uReconstructed, outputUv);
  // Never consume DLSS output alpha here. It is temporal and can trail across
  // the independent raster sky while the camera rotates.
  reconstructed.a = sampleCurrentCoverage(currentRawUv(outputUv));
  return reconstructed;
}

void main() {
  vec2 depthUv = currentRawUv(vUV);
  vec4 color = sampleDisplayColor(vUV);
  float depth = clamp(texture(uPathDepth, depthUv).r, 0.0, 1.0);
  // Current-frame coverage is the single authority for foreground visibility.
  // Do not combine temporally reconstructed alpha with raw current depth.
  if (color.a < 0.001) {
    discard;
  }

  // The path-trace target stores straight RGBA. The sampled depth belongs to
  // the nearest opaque RT surface, allowing
  // later raster grid/axis/skeleton passes to remain correctly occluded.
  gl_FragDepth = depth;
  color.rgb *= max(composite_push.display.x, 0.0);
  vec3 whiteBalanceScale = whiteBalance(composite_push.display.y);
  color.rgb *= whiteBalanceScale;

  float bloom = max(composite_push.display.z, 0.0);
  if (bloom > 0.0) {
    ivec2 sourceSize =
        (composite_push.flags.x & 2u) != 0u
            ? textureSize(uReconstructed, 0)
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
  // Path tracing and DLSS exchange straight RGBA. The Vulkan blend state
  // consumes premultiplied color, so associate RGB with coverage exactly once,
  // after nonlinear display transforms.
  color.rgb *= color.a;
  FragColor = color;
}
