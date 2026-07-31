#version 450
layout(set = 0, binding = 0) uniform samplerCube uSky;
layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 FragColor;

layout(push_constant) uniform PC {
  mat4 uVP;
  vec4 params; // x=time y=scene_id z=dynamic w=seed
} pc;

const float SID_SKY     = 2.0;
const float SID_NIGHT   = 3.0;
const float SID_SUNSET  = 4.0;
const float SID_DAWN    = 5.0;
const float SID_SPACE   = 6.0;
const float SID_END     = 7.0;
const float SID_DESERT  = 8.0;
const float SID_OCEAN   = 9.0;
const float SID_STORM   = 10.0;

// --- Cheap noise (performance-first for dynamic sky) -----------------------

vec3 hash33(vec3 p) {
  p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
           dot(p, vec3(269.5, 183.3, 246.1)),
           dot(p, vec3(113.5, 271.9, 124.6)));
  return fract(sin(p) * 43758.5453123);
}

float hash13(vec3 p) {
  return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

vec3 ghash(vec3 p) { return -1.0 + 2.0 * hash33(p); }

vec3 fade(vec3 t) {
  return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float gnoise(vec3 p) {
  vec3 i = floor(p);
  vec3 f = fract(p);
  vec3 u = fade(f);
  return mix(
    mix(mix(dot(ghash(i), f),
            dot(ghash(i + vec3(1,0,0)), f - vec3(1,0,0)), u.x),
        mix(dot(ghash(i + vec3(0,1,0)), f - vec3(0,1,0)),
            dot(ghash(i + vec3(1,1,0)), f - vec3(1,1,0)), u.x), u.y),
    mix(mix(dot(ghash(i + vec3(0,0,1)), f - vec3(0,0,1)),
            dot(ghash(i + vec3(1,0,1)), f - vec3(1,0,1)), u.x),
        mix(dot(ghash(i + vec3(0,1,1)), f - vec3(0,1,1)),
            dot(ghash(i + vec3(1,1,1)), f - vec3(1,1,1)), u.x), u.y),
    u.z);
}

// 2-octave FBM — primary path for dynamic (fast).
float fbm2(vec3 p) {
  return gnoise(p) * 0.6 + gnoise(p * 2.05) * 0.4;
}

// 3-octave when static cubemap quality or space nebula needs it.
float fbm3(vec3 p) {
  return gnoise(p) * 0.5 + gnoise(p * 2.02) * 0.33 + gnoise(p * 4.1) * 0.17;
}

float softCloud(vec3 p) {
  float w = fbm2(p * 0.7 + vec3(1.7, 2.1, 0.4));
  return fbm2(p + 0.4 * w);
}

float hash21(vec2 p) {
  return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

// Fast stars: single-cell + optional 1-ring only for bright layer.
vec3 starsFast(vec3 d, float dens, float scale, float sizeMul, float bright) {
  vec3 p = d * scale;
  vec3 i = floor(p);
  vec3 f = fract(p);
  vec3 acc = vec3(0.0);
  // Center cell always; 6-neighbor only for denser look without full 27.
  for (int k = 0; k < 7; ++k) {
    vec3 o = vec3(0.0);
    if (k == 1) o = vec3(1,0,0); else if (k == 2) o = vec3(-1,0,0);
    else if (k == 3) o = vec3(0,1,0); else if (k == 4) o = vec3(0,-1,0);
    else if (k == 5) o = vec3(0,0,1); else if (k == 6) o = vec3(0,0,-1);
    vec3 cell = i + o;
    float roll = hash21(cell.xy + cell.z * 19.0);
    if (roll > dens) continue;
    vec3 j = hash33(cell);
    vec3 c = o + j;
    float dist2 = dot(f - c, f - c);
    float size = (0.02 + 0.05 * hash21(cell.yz + 3.0)) * sizeMul;
    float core = exp(-dist2 / max(size * size, 1e-6));
    float halo = exp(-dist2 / max(size * size * 8.0, 1e-6)) * 0.25;
    float mag = (0.5 + 0.9 * hash21(cell.zx + 9.0)) * bright;
    float cool = hash21(cell.xy + 17.0);
    vec3 tint = mix(vec3(1.0, 0.92, 0.8), vec3(0.8, 0.9, 1.0), cool);
    acc += tint * (core + halo) * mag;
  }
  return acc;
}

vec3 atmosphere(vec3 zenith, vec3 horizon, vec3 nadir, float elev, bool fullSphere) {
  // Soft wide horizon — avoid hard "lid" / ring at elev≈0 (night/sunset/dawn).
  float e = clamp(elev, -1.0, 1.0);
  if (fullSphere) {
    if (e >= 0.0) {
      float t = e * e * (3.0 - 2.0 * e);
      return mix(horizon, zenith, pow(t, 0.9));
    }
    float u = -e;
    float t = u * u * (3.0 - 2.0 * u);
    // Nadir stays close to horizon so lower hemisphere never plates.
    return mix(horizon, nadir, pow(clamp(t, 0.0, 1.0), 1.1) * 0.75);
  }
  // Upper sky: gentle power so zenith doesn't snap.
  if (e >= 0.0) {
    float t = pow(e, 0.9);
    // Wide horizon glow band (sunset/dawn read as soft, not a hard line).
    float band = exp(-e * e * 18.0);
    vec3 upper = mix(horizon, zenith, t);
    return mix(upper, horizon, band * 0.22);
  }
  // Lower: very soft fade into ground tint (no hard step at -0.15).
  float down = -e;
  float t = down * down * (3.0 - 2.0 * down); // smoothstep-ish on [0,1]
  t = pow(clamp(t, 0.0, 1.0), 1.15);
  return mix(horizon, mix(horizon, nadir, 0.55), t * 0.7);
}

// Safe sun disc + bloom — never fills half the sky (seed bugs used to blow this up).
void applySunSafe(inout vec3 col, vec3 d, vec3 sunDir, vec3 sunTint,
                  float sunSize, float sunBloom) {
  d = normalize(d);
  sunDir = normalize(sunDir);
  float cosA = clamp(dot(d, sunDir), -1.0, 1.0);
  float ang = 1.0 - cosA; // 0 at disc center
  // Cap disc angular radius tightly (~sun-sized, not sky-filling).
  float size = clamp(sunSize, 0.0, 0.07);
  float discR = max(size * size * 2.2, 1e-5);
  float disc = clamp(1.0 - ang / discR, 0.0, 1.0);
  col = mix(col, sunTint, disc * disc * 0.92);
  // Bloom must decay fast enough that ang>0.15 is nearly zero.
  float bloomAmt = clamp(sunBloom, 0.0, 0.45);
  float bloomK = clamp(14.0 / max(bloomAmt, 0.08), 18.0, 90.0);
  float bloom = exp(-ang * bloomK) * bloomAmt;
  col += sunTint * bloom;
}

// kind: 0 fair, 1 sunset, 2 dawn, 3 ocean, 4 desert, 5 night
void applyCloudLayers(inout vec3 col, vec3 d, float elev, float time, float seed,
                      float amount, int kind, vec3 horizon, vec3 zenith) {
  if (amount < 0.001 || elev < -0.12) return;
  float elevGate = clamp(elev * 1.3 + 0.3, 0.0, 1.0);
  float drift = time * 0.007;
  // Seed only shifts noise domain (keep small-ish so hashes stay stable).
  vec3 so = vec3(fract(seed * 0.173) * 40.0, fract(seed * 0.291) * 40.0,
                 fract(seed * 0.417) * 40.0);

  float cover = (kind == 0 || kind == 1 || kind == 2) ? 1.35 :
                (kind == 5) ? 1.15 : 0.9;

  // 1 Cirrus
  {
    vec3 p = d * 9.5 + so + vec3(drift * 2.0, 0.8, -drift * 0.5);
    p.x *= 0.4;
    float dens = smoothstep(-0.3, 0.3, softCloud(p)) *
                 smoothstep(-0.35, 0.35, softCloud(p * 2.1 + 2.0));
    dens *= amount * cover * elevGate * 0.9 * smoothstep(-0.05, 0.4, elev);
    vec3 c = mix(vec3(0.97, 0.98, 1.0), horizon, 0.1);
    if (kind == 1) c = mix(c, vec3(1.0, 0.7, 0.45), 0.45);
    if (kind == 2) c = mix(c, vec3(1.0, 0.85, 0.7), 0.35);
    if (kind == 5) c = mix(c, vec3(0.55, 0.58, 0.7), 0.55);
    col = mix(col, c, clamp(dens * 0.58, 0.0, 0.8));
  }

  // 2 Altostratus bands
  {
    vec3 p = d * 4.0 + so * 0.7 + vec3(drift * 0.8, 0.2, -drift * 0.4);
    p.y *= 0.5;
    float n = softCloud(p);
    float bands = 0.5 + 0.5 * sin(d.x * 5.5 + d.z * 3.8 + n * 3.0 + seed);
    float dens = smoothstep(-0.25, 0.35, n) * smoothstep(0.12, 0.7, bands);
    dens *= amount * cover * elevGate * 0.72;
    vec3 c = mix(mix(horizon, zenith, 0.2), vec3(0.88, 0.9, 0.94), 0.5);
    if (kind == 1) c = mix(c, vec3(0.9, 0.45, 0.3), 0.5);
    if (kind == 5) c = mix(c, vec3(0.25, 0.27, 0.35), 0.65);
    col = mix(col, c, clamp(dens * 0.5, 0.0, 0.75));
  }

  // 3 Stratus sheet
  {
    vec3 p = d * 2.6 + so * 0.5 + vec3(drift * 0.5, 0.1, -drift * 0.3);
    float dens = smoothstep(-0.4, 0.35, softCloud(p));
    dens *= amount * cover * elevGate * 0.65 * clamp(1.0 - elev * 0.85, 0.3, 1.0);
    vec3 c = mix(mix(horizon, zenith, 0.12), vec3(0.9, 0.91, 0.94), 0.55);
    if (kind == 1) c = mix(c, vec3(0.85, 0.4, 0.28), 0.45);
    if (kind == 5) c = mix(c, vec3(0.18, 0.2, 0.28), 0.7);
    col = mix(col, c, clamp(dens * 0.48, 0.0, 0.7));
  }

  // 4 Cumulus (main bulk)
  {
    vec3 p = d * 5.0 + so + vec3(drift * 1.1, 0.25, -drift * 0.85);
    p += 0.45 * vec3(fbm2(p + 1.0), fbm2(p + 4.0), fbm2(p + 7.0));
    float dens = smoothstep(-0.35, 0.45, softCloud(p)) *
                 smoothstep(-0.4, 0.4, softCloud(p * 1.8 + 2.0));
    dens *= amount * cover * elevGate * 1.05;
    vec3 top = mix(vec3(0.99, 0.99, 1.0), horizon, 0.08);
    vec3 bot = mix(horizon, zenith, 0.1) * 0.7;
    if (kind == 1) { top = mix(top, vec3(1.0, 0.8, 0.45), 0.55); bot = mix(bot, vec3(0.45, 0.2, 0.18), 0.55); }
    if (kind == 2) { top = mix(top, vec3(1.0, 0.9, 0.75), 0.4); bot = mix(bot, vec3(0.55, 0.35, 0.35), 0.4); }
    if (kind == 5) { top = mix(top, vec3(0.45, 0.48, 0.58), 0.5); bot = mix(bot, vec3(0.12, 0.14, 0.2), 0.6); }
    float shade = clamp(0.3 + 0.7 * elev, 0.0, 1.0);
    col = mix(col, mix(bot, top, shade), clamp(dens * 0.8, 0.0, 0.9));
  }

  // 5 Stratocumulus / broken
  {
    vec3 p = d * 6.8 + so * 1.2 + vec3(drift * 1.3, 0.4, -drift * 0.6);
    float cells = 0.5 + 0.5 * gnoise(d * 7.5 + so);
    float dens = smoothstep(-0.2, 0.3, softCloud(p)) * smoothstep(0.2, 0.75, cells);
    dens *= amount * cover * elevGate * 0.6;
    vec3 c = mix(vec3(0.94, 0.95, 0.98), horizon, 0.2);
    if (kind == 5) c = mix(c, vec3(0.3, 0.32, 0.4), 0.6);
    col = mix(col, c, clamp(dens * 0.52, 0.0, 0.7));
  }

  // 6 Lenticular (lens stacks, mid-high)
  if (kind == 0 || kind == 1 || kind == 2) {
    vec3 p = d * 7.5 + so * 0.8;
    p.y *= 2.2;
    float n = softCloud(p + vec3(0.0, seed, 0.0));
    float lens = smoothstep(0.25, 0.65, n) * smoothstep(0.15, 0.45, elev) *
                 smoothstep(0.75, 0.35, elev);
    lens *= amount * 0.4;
    vec3 c = mix(vec3(0.96, 0.97, 1.0), horizon, 0.15);
    if (kind == 1) c = mix(c, vec3(1.0, 0.75, 0.5), 0.4);
    col = mix(col, c, clamp(lens, 0.0, 0.55));
  }

  // 7 Cumulonimbus towers
  if (kind == 0 || kind == 1 || kind == 2) {
    vec3 p = d * 3.4 + so * 0.6 + vec3(drift * 0.35, 0.0, -drift * 0.2);
    p.y *= 1.8;
    float ridge = 1.0 - abs(2.0 * softCloud(p + 5.0) - 1.0);
    float dens = smoothstep(0.25, 0.75, ridge) * smoothstep(0.05, 0.5, elev);
    dens *= amount * elevGate * (kind == 1 ? 0.7 : 0.4);
    vec3 tower = mix(vec3(0.5, 0.48, 0.52), vec3(0.96, 0.9, 0.84), clamp(elev * 1.3, 0.0, 1.0));
    if (kind == 1)
      tower = mix(vec3(0.28, 0.16, 0.2), vec3(1.0, 0.6, 0.28), clamp(elev * 1.5, 0.0, 1.0));
    col = mix(col, tower, clamp(dens * 0.72, 0.0, 0.85));
  }

  // 8 Mammatus (sunset)
  if (kind == 1) {
    vec3 p = d * 6.2 + so + vec3(drift * 0.25, -0.35, 0.0);
    float bumps = smoothstep(0.15, 0.5, softCloud(p)) *
                  smoothstep(0.0, 0.22, elev) * smoothstep(0.5, 0.12, elev);
    col = mix(col, vec3(0.32, 0.16, 0.2), clamp(bumps * amount * 0.5, 0.0, 0.5));
  }

  // 9 Contrail / linear streaks
  if (kind == 0 || kind == 2 || kind == 5) {
    float line = abs(d.x * 0.7 + d.z * 0.3 + softCloud(d * 3.0 + so) * 0.15);
    float dens = exp(-line * line * 120.0) * smoothstep(0.2, 0.55, elev) * amount * 0.25;
    vec3 c = (kind == 5) ? vec3(0.5, 0.55, 0.7) : vec3(0.98, 0.99, 1.0);
    col = mix(col, c, clamp(dens, 0.0, 0.4));
  }

  // 10 Fractus / ragged low scraps
  {
    vec3 p = d * 8.0 + so * 1.5 + vec3(drift * 1.6, -0.2, -drift);
    float dens = smoothstep(-0.1, 0.35, softCloud(p)) *
                 smoothstep(0.2, -0.02, elev) * smoothstep(-0.25, 0.08, elev);
    dens *= amount * 0.55;
    vec3 c = (kind == 5) ? vec3(0.2, 0.22, 0.3) : mix(horizon, vec3(0.85, 0.86, 0.9), 0.4);
    col = mix(col, c, clamp(dens, 0.0, 0.55));
  }

  // Sun-edge gold (sunset/dawn)
  if (kind == 1 || kind == 2) {
    vec3 sunDir = (kind == 1) ? normalize(vec3(0.75, 0.15, 0.35))
                              : normalize(vec3(-0.7, 0.2, 0.4));
    float rim = pow(max(0.0, dot(d, sunDir)), 8.0) * amount * 0.32 * elevGate;
    col += ((kind == 1) ? vec3(1.0, 0.55, 0.22) : vec3(1.0, 0.78, 0.45)) * rim;
  }
}

// ---------------------------------------------------------------------------
// END — Bliss-style: void + swirling end-storm fog + cyan haze + lightning.
// NOT space (no dense starfield / colorful galaxies).
// ---------------------------------------------------------------------------
vec3 proceduralEnd(vec3 d, float time, float seed) {
  // Near-black void (vanilla End + Bliss dark base).
  vec3 col = vec3(0.006, 0.003, 0.014);
  float elev = d.y;
  vec3 so = vec3(fract(seed * 0.211) * 30.0, fract(seed * 0.073) * 30.0,
                 fract(seed * 0.149) * 30.0);

  // Soft purple horizon band (fog color, not nebula islands).
  float horiz = exp(-abs(elev) * 2.2);
  col = mix(col, vec3(0.06, 0.02, 0.12), horiz * 0.55);

  // --- Bliss end storm: clumpy swirling density on the sphere --------------
  // Pseudo world sample along view ray (Bliss samples world fog; we project).
  float swirlT = time * 0.02 + seed;
  // Angular coordinates with vertical stretch like fogShape samplePos.y/48
  vec3 sp = normalize(d + 0.02 * so);
  float ang = atan(sp.x, sp.z);
  float rot = swirlT * 0.4 + sp.y * 2.2;
  float ca = cos(rot), sa = sin(rot);
  vec2 xz = mat2(ca, -sa, sa, ca) * sp.xz;

  // Multi-scale clump density (Bliss densityAtPosFog analogue).
  float n1 = softCloud(vec3(xz * 3.5, sp.y * 1.2) + so);
  float n2 = softCloud(vec3(xz * 7.0, sp.y * 2.0) - so * 0.5 + swirlT * 0.15);
  float erosion = 1.0 - softCloud(vec3(xz * 12.0, sp.y * 3.0) - swirlT * 0.2);
  float clump = max(exp(-mix(4.0, 2.2, 0.5) * (0.55 + 0.45 * n1)) - erosion * 0.25, 0.0);
  clump *= smoothstep(-0.55, 0.35, n2);

  // Vortex bound: stronger near "center high" direction (0,1,0)-ish.
  vec3 vortexAxis = normalize(vec3(0.05 + 0.1 * sin(seed), 1.0, 0.08));
  float vortexCore = pow(max(0.0, dot(sp, vortexAxis)), 3.5);
  float vortexRing = exp(-pow(abs(dot(sp, vortexAxis)) - 0.35, 2.0) * 18.0);
  float storm = clump * (0.35 + 0.9 * vortexRing + 0.55 * vortexCore);
  // Clear band near player "horizon" lower hemisphere a bit less dense.
  storm *= 0.55 + 0.45 * clamp(elev + 0.4, 0.0, 1.0);

  // Storm body: magenta/purple (lightning lit) + cyan vortex light.
  vec3 stormCol = mix(vec3(0.45, 0.12, 0.7), vec3(0.25, 0.55, 0.95), vortexCore);
  col += stormCol * storm * 0.85;
  // Bright vortex shaft (Bliss VORTEX light).
  col += vec3(0.35, 0.65, 1.0) * vortexCore * (0.25 + 0.75 * clump) * 0.7;

  // Torus-like end island glow (Bliss VolumeBounds torus around radius).
  float elevRing = abs(length(sp.xz) - 0.55);
  float torus = exp(-elevRing * elevRing * 28.0) * smoothstep(-0.2, 0.4, elev);
  col += vec3(0.55, 0.2, 0.85) * torus * (0.4 + 0.6 * n1) * 0.4;

  // Cyan haze (Bliss END_HAZE)
  float skyPhase = 0.5 + pow(clamp(elev * 0.5 + 0.5, 0.0, 1.0), 4.0) * 2.0;
  col += vec3(0.22, 0.45, 0.85) * 0.08 * skyPhase * (0.6 + 0.4 * n2);

  // Localized lightning (Bliss flash) — cone + storm light, not full-sky grey.
  float flashSeed = floor(time * 0.4 + seed * 3.0);
  float phase = fract(time * 0.4 + seed * 0.1);
  float env = 0.0;
  if (phase < 0.06) env = 1.0 - phase / 0.06;
  else if (phase < 0.1) env = 0.35 * (1.0 - (phase - 0.06) / 0.04);
  vec3 flashDir = normalize(ghash(vec3(flashSeed, seed, 2.3)) * vec3(1.0, 0.4, 1.0) +
                            vec3(0.0, 0.5, 0.0));
  float cone = pow(max(0.0, dot(sp, flashDir)), 12.0);
  col += vec3(0.7, 0.45, 1.0) * env * (0.25 + cone * 1.6 + storm * 0.8);

  // Very sparse dim stars only (End is not deep space).
  col += starsFast(d, 0.06, 160.0, 0.9, 0.55) * 0.35;

  return clamp(col, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// SPACE — dense stars + colorful nebulae (distinct from End).
// ---------------------------------------------------------------------------
vec3 proceduralSpace(vec3 d, float time, float seed) {
  vec3 col = vec3(0.0012, 0.0015, 0.0035);
  float drift = time * 0.002 + fract(seed) * 0.01;
  vec3 so = vec3(fract(seed * 0.13) * 30.0, fract(seed * 0.21) * 30.0,
                 fract(seed * 0.07) * 30.0);

  // 5 discrete nebulae
  vec3 centers[5] = vec3[](
    normalize(vec3(0.7, 0.15, -0.4) + so * 0.05),
    normalize(vec3(-0.55, 0.35, 0.6) - so * 0.04),
    normalize(vec3(0.2, -0.65, 0.45)),
    normalize(vec3(-0.25, 0.55, -0.7)),
    normalize(vec3(0.45, 0.4, 0.55) + so * 0.03));
  vec3 hues[5] = vec3[](
    vec3(0.55, 0.12, 0.7), vec3(0.15, 0.35, 0.85), vec3(0.75, 0.2, 0.35),
    vec3(0.1, 0.55, 0.55), vec3(0.4, 0.15, 0.9));
  for (int k = 0; k < 5; ++k) {
    float ang = 1.0 - max(0.0, dot(d, centers[k]));
    float island = exp(-ang * (11.0 + float(k)));
    float n = 0.5 + 0.5 * fbm2(d * (2.5 + 0.3 * float(k)) + so + drift);
    float dens = island * pow(n, 1.15);
    col += hues[k] * dens * 0.9;
    col += hues[k] * pow(island, 2.5) * 0.5;
  }

  float plane = d.x * 0.28 + d.y * 0.08 - d.z * 0.78 + seed * 0.02;
  float band = exp(-plane * plane * 16.0);
  float structure = 0.5 + 0.5 * fbm2(d * 5.0 + so + drift);
  col += vec3(0.55, 0.5, 0.72) * band * pow(structure, 1.2) * 1.1;

  col += starsFast(d, 0.45, 320.0, 1.1, 1.3);
  col += starsFast(d, 0.2, 180.0, 1.6, 2.0);
  col += starsFast(d, 0.07, 100.0, 2.3, 2.8);
  col += starsFast(d, 0.02, 65.0, 3.0, 3.5);

  return clamp(col, 0.0, 1.0);
}

// Storm: layered + rain + bolts (kept cheaper).
vec3 proceduralStorm(vec3 d, float time, float seed) {
  float elev = d.y;
  vec3 col = atmosphere(vec3(0.1, 0.12, 0.16), vec3(0.28, 0.3, 0.34),
                        vec3(0.14, 0.15, 0.17), elev, true);
  float drift = time * 0.02;
  vec3 so = vec3(fract(seed * 0.11) * 25.0, fract(seed * 0.19) * 25.0,
                 fract(seed * 0.05) * 25.0);

  float n = softCloud(d * 2.8 + so + vec3(drift * 0.8, 0.1, -drift * 0.4));
  float dens = smoothstep(-0.15, 0.45, n);
  dens *= clamp(elev * 0.5 + 0.7, 0.4, 1.0);
  col = mix(col, vec3(0.2, 0.22, 0.26), dens * 0.65);
  col = mix(col, vec3(0.08, 0.09, 0.11), smoothstep(0.4, 0.9, dens) * 0.55);

  float n2 = softCloud(d * 4.5 + so * 1.2 + vec3(drift * 1.3, 0.2, -drift));
  float cells = 0.5 + 0.5 * gnoise(d * 5.5 + so);
  float mid = smoothstep(-0.05, 0.5, n2) * smoothstep(0.2, 0.85, cells);
  mid *= clamp(1.0 - elev * 0.4, 0.35, 1.0);
  col = mix(col, vec3(0.13, 0.14, 0.17), mid * 0.7);

  float rain = smoothstep(0.15, 0.55, n) *
               smoothstep(0.55, 0.95, 0.5 + 0.5 * sin(d.x * 70.0 + d.z * 50.0 - time * 8.0));
  rain *= clamp(0.55 - elev * 0.8, 0.0, 0.7);
  col = mix(col, col * 0.75, rain * 0.5);

  float fseed = floor(time * 0.55 + seed);
  float phase = fract(time * 0.55);
  float env = (phase < 0.04) ? (1.0 - phase / 0.04) :
              (phase < 0.07) ? 0.4 * (1.0 - (phase - 0.04) / 0.03) : 0.0;
  vec3 bolt = normalize(ghash(vec3(fseed, 4.2, seed)) * vec3(1, 0.35, 1) + vec3(0, 0.55, 0));
  float shaft = pow(max(0.0, dot(d, bolt)), 40.0);
  col += vec3(0.75, 0.82, 1.0) * shaft * env * 1.8;
  col += vec3(0.4, 0.48, 0.6) * env * pow(max(0.0, dot(d, bolt)), 4.0) * 0.45;

  return clamp(col, 0.0, 1.0);
}

vec3 proceduralSky(vec3 dir, float time, float sid, float seed) {
  vec3 d = normalize(dir);
  float elev = d.y;

  if (abs(sid - SID_SPACE) < 0.5) return proceduralSpace(d, time, seed);
  if (abs(sid - SID_END) < 0.5)   return proceduralEnd(d, time, seed);
  if (abs(sid - SID_STORM) < 0.5) return proceduralStorm(d, time, seed);

  vec3 zenith, horizon, nadir, sunTint;
  vec3 sunDir = normalize(vec3(0.35, 0.70, 0.40));
  float sunSize = 0.045;
  float sunBloom = 0.2;
  float cloud = 0.7;
  int cloudKind = 0;
  bool noSun = false;

  if (abs(sid - SID_NIGHT) < 0.5) {
    zenith = vec3(0.015, 0.025, 0.08);
    horizon = vec3(0.05, 0.07, 0.16);
    // Keep nadir close to horizon — soft ground, no dark plate.
    nadir = vec3(0.04, 0.055, 0.12);
    sunTint = vec3(0.85, 0.88, 1.0);
    sunDir = normalize(vec3(-0.35, 0.62, 0.35));
    sunSize = 0.026;
    sunBloom = 0.10;
    cloud = 0.55;
    cloudKind = 5;
  } else if (abs(sid - SID_SUNSET) < 0.5) {
    zenith = vec3(0.10, 0.16, 0.42);
    horizon = vec3(0.98, 0.42, 0.16);
    nadir = vec3(0.72, 0.38, 0.22); // closer to horizon
    sunTint = vec3(1.0, 0.72, 0.32);
    sunDir = normalize(vec3(0.75, 0.15, 0.35));
    sunSize = 0.055; sunBloom = 0.28;
    cloud = 0.8; cloudKind = 1;
  } else if (abs(sid - SID_DAWN) < 0.5) {
    zenith = vec3(0.22, 0.38, 0.72);
    horizon = vec3(1.0, 0.62, 0.45);
    nadir = vec3(0.62, 0.48, 0.42);
    sunTint = vec3(1.0, 0.85, 0.55);
    sunDir = normalize(vec3(-0.7, 0.2, 0.4));
    sunSize = 0.048; sunBloom = 0.24;
    cloud = 0.72; cloudKind = 2;
  } else if (abs(sid - SID_DESERT) < 0.5) {
    zenith = vec3(0.30, 0.58, 0.92);
    horizon = vec3(0.94, 0.78, 0.50);
    nadir = vec3(0.70, 0.56, 0.36);
    sunTint = vec3(1.0, 0.94, 0.72);
    sunDir = normalize(vec3(0.25, 0.78, 0.35));
    sunSize = 0.052; sunBloom = 0.24;
    cloud = 0.16; cloudKind = 4;
  } else if (abs(sid - SID_OCEAN) < 0.5) {
    // Maritime: deep zenith blue, cyan horizon (matches water haze).
    zenith = vec3(0.12, 0.38, 0.88);
    horizon = vec3(0.55, 0.78, 0.92);
    nadir = vec3(0.14, 0.36, 0.52);
    sunTint = vec3(1.0, 0.98, 0.90);
    sunDir = normalize(vec3(0.42, 0.68, 0.22));
    sunSize = 0.042; sunBloom = 0.28;
    cloud = 0.42; cloudKind = 3;
  } else {
    zenith = vec3(0.26, 0.50, 0.92);
    horizon = vec3(0.70, 0.84, 0.95);
    nadir = vec3(0.48, 0.58, 0.62);
    sunTint = vec3(1.0, 0.97, 0.88);
    sunDir = normalize(vec3(0.35, 0.70, 0.40));
    cloud = 0.75; cloudKind = 0;
  }

  vec3 col = atmosphere(zenith, horizon, nadir, elev, false);
  applyCloudLayers(col, d, elev, time, seed, cloud, cloudKind, horizon, zenith);

  if (abs(sid - SID_NIGHT) < 0.5) {
    col += starsFast(d, 0.2, 240.0, 1.2, 1.1) * 0.7;
    col += starsFast(d, 0.05, 120.0, 1.8, 1.8) * 0.9;
  }

  if (!noSun) {
    applySunSafe(col, d, sunDir, sunTint, sunSize, sunBloom);
  }
  return clamp(col, 0.0, 1.0);
}

void main() {
  vec3 dir = normalize(vDir);
  if (pc.params.z > 0.5) {
    FragColor = vec4(proceduralSky(dir, pc.params.x, pc.params.y, pc.params.w), 1.0);
  } else {
    FragColor = texture(uSky, dir);
  }
}
