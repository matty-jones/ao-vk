// The air as a planet's atmosphere (rvk atmosphere.cpp; docs/dusk-plan.md §8): after Hillaire 2020, "A Scalable and
// Production Ready Sky and Atmosphere Rendering Technique" - Rayleigh scattering (the sky's blue), Mie (haze, the halo
// round the sun), ozone (absorbing orange: the blue hour's blue), over an Earth-sized planet (Rubi-Ka's size unknown).
// Lengths in km, from the planet's centre. The tables:
//   transmittance (256 x 64): how much of the light gets through to the top of the air, by height and angle
//   multiple scattering (32 x 32): the light scattered more than once, by height and the sun's angle
//   sky view (192 x 108): the sky's light in every direction from the eye (atmos_view.frag), read by the sky pass
#ifndef ATMOS_COMMON_GLSL
#define ATMOS_COMMON_GLSL

const float kAtmosPi = 3.14159265;
const float kGroundR = 6360.0, kTopR = 6460.0;
const vec3 kRayleighScat = vec3(5.802, 13.558, 33.1) * 1e-3;
const float kRayleighH = 8.0;
const float kMieScat = 3.996e-3, kMieExt = 4.44e-3, kMieH = 1.2;
const vec3 kOzoneAbs = vec3(0.65, 1.881, 0.085) * 1e-3;

// The air at a height (km): its scattering (Rayleigh rgb, Mie) and its extinction. tune: x the Mie's amount
// (RVK_SkyGlow: the halo, the haze), y the ozone's (RVK_BlueHour).
struct AtmosMedium { vec3 rayleigh; float mie; vec3 extinction; };
AtmosMedium AtmosAt(float h, vec2 tune)
{
    float r = exp(-max(h, 0.0) / kRayleighH), m = exp(-max(h, 0.0) / kMieH) * tune.x;
    float o = max(0.0, 1.0 - abs(h - 25.0) / 15.0) * tune.y;
    AtmosMedium a;
    a.rayleigh = kRayleighScat * r;
    a.mie = kMieScat * m;
    a.extinction = a.rayleigh + vec3(kMieExt * m) + kOzoneAbs * o;
    return a;
}

// Along a ray from o (unit d) to the sphere of radius R: the nearest hit ahead, -1 none.
float AtmosSphere(vec3 o, vec3 d, float R)
{
    float b = dot(o, d), c = dot(o, o) - R * R, disc = b * b - c;
    if (disc < 0.0)
        return -1.0;
    float s = sqrt(disc), t0 = -b - s, t1 = -b + s;
    return t0 >= 0.0 ? t0 : t1 >= 0.0 ? t1 : -1.0;
}

// The transmittance table's coordinates (Bruneton's): r the distance from the centre, mu the cosine up.
vec2 AtmosTransUv(float r, float mu)
{
    float H = sqrt(kTopR * kTopR - kGroundR * kGroundR);
    float rho = sqrt(max(r * r - kGroundR * kGroundR, 0.0));
    float disc = r * r * (mu * mu - 1.0) + kTopR * kTopR;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dMin = kTopR - r, dMax = rho + H;
    return vec2((d - dMin) / max(dMax - dMin, 1e-4), rho / H);
}

void AtmosTransFromUv(vec2 uv, out float r, out float mu)
{
    float H = sqrt(kTopR * kTopR - kGroundR * kGroundR);
    float rho = H * uv.y;
    r = sqrt(rho * rho + kGroundR * kGroundR);
    float dMin = kTopR - r, dMax = rho + H, d = dMin + uv.x * (dMax - dMin);
    mu = d <= 0.0 ? 1.0 : clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// The multiple scattering table's coordinates: x the sun's cosine up (-1..1), y the height (0..top).
vec2 AtmosMultiUv(float r, float muSun)
{
    return vec2(muSun * 0.5 + 0.5, clamp((r - kGroundR) / (kTopR - kGroundR), 0.0, 1.0));
}

float AtmosRayleighPhase(float c) { return 3.0 / (16.0 * kAtmosPi) * (1.0 + c * c); }

// Cornette-Shanks (Mie, g ~0.8: a strong forward lobe - the bright air round the sun).
float AtmosMiePhase(float c, float g)
{
    float gg = g * g;
    float k = 3.0 / (8.0 * kAtmosPi) * (1.0 - gg) / (2.0 + gg);
    return k * (1.0 + c * c) / pow(max(1.0 + gg - 2.0 * g * c, 1e-4), 1.5);
}

#endif
