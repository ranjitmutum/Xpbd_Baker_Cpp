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

vec4 samplePathColor(vec2 uv) {
  return (composite_push.flags.x & 2u) != 0u
             ? texture(uReconstructed, uv)
             : texture(uPathTrace, uv);
}

void main() {
  vec4 color = samplePathColor(vUV);
  color.a = clamp(color.a, 0.0, 1.0);
  vec2 depthUv = vUV;
  if ((composite_push.flags.x & 2u) != 0u) {
    // Temporal primary rays use pixelCenter-jitter. Reconstructed color is
    // back in unjittered output space, so query raw render-resolution depth
    // with the inverse (+jitter) offset before later raster overlays test it.
    vec2 jitterPixels = uintBitsToFloat(composite_push.flags.yz);
    depthUv += jitterPixels / vec2(textureSize(uPathDepth, 0));
  }
  float depth = clamp(texture(uPathDepth, depthUv).r, 0.0, 1.0);
  bool transparentBackground =
      (composite_push.flags.x & 1u) != 0u && depth >= 0.999999;
  if (color.a < 0.001 || transparentBackground) {
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
            samplePathColor(vUV + vec2(x, y) * texel);
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
