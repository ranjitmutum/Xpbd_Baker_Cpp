#ifndef XPBD_ATMOSPHERE_COMMON_GLSL
#define XPBD_ATMOSPHERE_COMMON_GLSL

#define IN(x) const in x
#define OUT(x) out x
#define TEMPLATE(x)
#define TEMPLATE_ARGUMENT(x)
#define assert(x)
#define COMBINED_SCATTERING_TEXTURES

const int TRANSMITTANCE_TEXTURE_WIDTH = 256;
const int TRANSMITTANCE_TEXTURE_HEIGHT = 64;
const int SCATTERING_TEXTURE_R_SIZE = 32;
const int SCATTERING_TEXTURE_MU_SIZE = 128;
const int SCATTERING_TEXTURE_MU_S_SIZE = 32;
const int SCATTERING_TEXTURE_NU_SIZE = 8;
const int IRRADIANCE_TEXTURE_WIDTH = 64;
const int IRRADIANCE_TEXTURE_HEIGHT = 16;

#include "third_party/bruneton/atmosphere/definitions.glsl"

// Frozen Earth radiance-mode parameters from the upstream reference demo,
// expressed in a one-kilometre shader length unit at 680/550/440 nm.
const AtmosphereParameters ATMOSPHERE = AtmosphereParameters(
    vec3(1.474, 1.8504, 1.91198),
    0.004675,
    6360.0,
    6420.0,
    DensityProfile(DensityProfileLayer[2](
        DensityProfileLayer(0.0, 0.0, 0.0, 0.0, 0.0),
        DensityProfileLayer(0.0, 1.0, -0.125, 0.0, 0.0))),
    vec3(0.0058023393817123806, 0.013557762447920219,
         0.033100005976367732),
    DensityProfile(DensityProfileLayer[2](
        DensityProfileLayer(0.0, 0.0, 0.0, 0.0, 0.0),
        DensityProfileLayer(0.0, 1.0, -0.8333333333333333, 0.0, 0.0))),
    vec3(0.003996),
    vec3(0.00444),
    0.8,
    DensityProfile(DensityProfileLayer[2](
        DensityProfileLayer(25.0, 0.0, 0.0, 0.06666666666666667,
                            -0.6666666666666666),
        DensityProfileLayer(0.0, 0.0, 0.0, -0.06666666666666667,
                            2.6666666666666665))),
    vec3(0.0006497166, 0.0018809, 0.00008501668),
    vec3(0.1),
    -0.20791169081775934);

#include "third_party/bruneton/atmosphere/functions.glsl"

#endif
