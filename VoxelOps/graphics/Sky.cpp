#include "Sky.hpp"

#include "atmosphere/model.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSunAngularRadius = 0.004675;
constexpr double kBottomRadius = 6360000.0;
constexpr double kTopRadius = 6460000.0;
constexpr double kRayleighScaleHeight = 8000.0;  // 8km
constexpr double kMieScaleHeight = 1200.0;       // 1.2km
constexpr double kMiePhaseFunctionG = 0.8;
constexpr double kGroundAlbedo = 0.0;
constexpr double kInvKmToInvM = 1.0 / 1000.0;
constexpr int kBrunetonScatteringOrders = 5;

constexpr int kSkyViewLutWidth = 192;
constexpr int kSkyViewLutHeight = 108;
constexpr int kCameraVolumeSize = 32;
constexpr int kMultiScattLutSize = 32;
constexpr int kTransmittanceLutWidth = 256;
constexpr int kTransmittanceLutHeight = 64;

constexpr int kTransmittanceLutUnit = 13;
constexpr int kMultiScattLutUnit = 12;

constexpr const char* kFullscreenVs = R"(
#version 330 core
void main() {
    vec2 uv = vec2(-1.0, -1.0);
    if (gl_VertexID == 1) uv = vec2(-1.0, 3.0);
    if (gl_VertexID == 2) uv = vec2(3.0, -1.0);
    gl_Position = vec4(uv, 0.0, 1.0);
}
)";

constexpr const char* kLayeredGs = R"(
#version 330 core
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
uniform int uLayer;
void main() {
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[i].gl_Position;
        gl_Layer = uLayer;
        EmitVertex();
    }
    EndPrimitive();
}
)";

constexpr const char* kTransmittanceFs = R"(
#version 330 core
layout(location = 0) out vec4 OutColor;

uniform vec2 uOutputSize;

struct AtmosphereParameters {
    float BottomRadius;
    float TopRadius;
    float RayleighDensityExpScale;
    vec3 RayleighScattering;
    float MieDensityExpScale;
    vec3 MieScattering;
    vec3 MieExtinction;
    vec3 MieAbsorption;
    float MiePhaseG;
    float AbsorptionDensity0LayerWidth;
    float AbsorptionDensity0ConstantTerm;
    float AbsorptionDensity0LinearTerm;
    float AbsorptionDensity1ConstantTerm;
    float AbsorptionDensity1LinearTerm;
    vec3 AbsorptionExtinction;
};

struct MediumSampleRGB {
    vec3 extinction;
};

float raySphereIntersectNearest(vec3 r0, vec3 rd, vec3 s0, float sR) {
    float a = dot(rd, rd);
    vec3 s0_r0 = r0 - s0;
    float b = 2.0 * dot(rd, s0_r0);
    float c = dot(s0_r0, s0_r0) - sR * sR;
    float delta = b * b - 4.0 * a * c;
    if (delta < 0.0 || a == 0.0) return -1.0;
    float sqrtDelta = sqrt(delta);
    float sol0 = (-b - sqrtDelta) / (2.0 * a);
    float sol1 = (-b + sqrtDelta) / (2.0 * a);
    if (sol0 < 0.0 && sol1 < 0.0) return -1.0;
    if (sol0 < 0.0) return max(0.0, sol1);
    if (sol1 < 0.0) return max(0.0, sol0);
    return max(0.0, min(sol0, sol1));
}

float fromSubUvsToUnit(float u, float resolution) {
    return (u - 0.5 / resolution) * (resolution / (resolution - 1.0));
}

AtmosphereParameters GetAtmosphereParameters() {
    AtmosphereParameters a;
    a.BottomRadius = 6360.0;
    a.TopRadius = 6460.0;
    a.RayleighDensityExpScale = -1.0 / 8.0;
    a.RayleighScattering = vec3(0.005802, 0.013558, 0.033100);
    a.MieDensityExpScale = -1.0 / 1.2;
    a.MieScattering = vec3(0.003996);
    a.MieExtinction = vec3(0.004440);
    a.MieAbsorption = max(a.MieExtinction - a.MieScattering, vec3(0.0));
    a.MiePhaseG = 0.8;
    a.AbsorptionDensity0LayerWidth = 25.0;
    a.AbsorptionDensity0ConstantTerm = 0.0;
    a.AbsorptionDensity0LinearTerm = 1.0 / 15.0;
    a.AbsorptionDensity1ConstantTerm = 8.0 / 3.0;
    a.AbsorptionDensity1LinearTerm = -1.0 / 15.0;
    a.AbsorptionExtinction = vec3(0.000650, 0.001881, 0.000085);
    return a;
}

void UvToLutTransmittanceParams(
    in AtmosphereParameters atm,
    in vec2 uv,
    out float viewHeight,
    out float viewZenithCosAngle) {
    vec2 uvSub = vec2(
        fromSubUvsToUnit(uv.x, uOutputSize.x),
        fromSubUvsToUnit(uv.y, uOutputSize.y));
    float xMu = clamp(uvSub.x, 0.0, 1.0);
    float xR = clamp(uvSub.y, 0.0, 1.0);

    float H = sqrt(max(0.0, atm.TopRadius * atm.TopRadius - atm.BottomRadius * atm.BottomRadius));
    float rho = H * xR;
    viewHeight = sqrt(max(rho * rho + atm.BottomRadius * atm.BottomRadius, 0.0));

    float dMin = atm.TopRadius - viewHeight;
    float dMax = rho + H;
    float d = dMin + xMu * (dMax - dMin);
    if (d <= 0.0) {
        viewZenithCosAngle = 1.0;
    } else {
        viewZenithCosAngle = (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
        viewZenithCosAngle = clamp(viewZenithCosAngle, -1.0, 1.0);
    }
}

MediumSampleRGB sampleMediumRGB(in vec3 worldPos, in AtmosphereParameters atm) {
    float viewHeight = length(worldPos) - atm.BottomRadius;
    float densityMie = exp(atm.MieDensityExpScale * viewHeight);
    float densityRay = exp(atm.RayleighDensityExpScale * viewHeight);
    float densityOzo = clamp(viewHeight < atm.AbsorptionDensity0LayerWidth ?
        atm.AbsorptionDensity0LinearTerm * viewHeight + atm.AbsorptionDensity0ConstantTerm :
        atm.AbsorptionDensity1LinearTerm * viewHeight + atm.AbsorptionDensity1ConstantTerm,
        0.0, 1.0);

    vec3 extinctionMie = densityMie * atm.MieExtinction;
    vec3 extinctionRay = densityRay * atm.RayleighScattering;
    vec3 extinctionOzo = densityOzo * atm.AbsorptionExtinction;

    MediumSampleRGB s;
    s.extinction = extinctionMie + extinctionRay + extinctionOzo;
    return s;
}

vec3 IntegrateOpticalDepthToTopBoundary(
    in AtmosphereParameters atm,
    in vec3 worldPos,
    in vec3 worldDir,
    in float sampleCountIni) {
    float tTop = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.TopRadius);
    if (tTop <= 0.0) {
        return vec3(0.0);
    }
    float tBottom = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.BottomRadius);
    float tMax = tTop;
    if (tBottom > 0.0) {
        tMax = min(tTop, tBottom);
    }
    if (tMax <= 0.0) {
        return vec3(0.0);
    }

    float sampleCount = max(sampleCountIni, 1.0);
    float t = 0.0;
    vec3 opticalDepth = vec3(0.0);
    for (int i = 0; i < 64; ++i) {
        float s = float(i);
        if (s >= sampleCount) break;
        float newT = tMax * (s + 0.3) / sampleCount;
        float dt = newT - t;
        t = newT;
        vec3 P = worldPos + t * worldDir;
        MediumSampleRGB medium = sampleMediumRGB(P, atm);
        opticalDepth += medium.extinction * dt;
    }
    return opticalDepth;
}

void main() {
    AtmosphereParameters atm = GetAtmosphereParameters();
    vec2 uv = gl_FragCoord.xy / max(uOutputSize, vec2(1.0));
    float viewHeight = 0.0;
    float viewZenithCosAngle = 1.0;
    UvToLutTransmittanceParams(atm, uv, viewHeight, viewZenithCosAngle);
    vec3 worldPos = vec3(0.0, 0.0, viewHeight);
    float viewZenithSinAngle = sqrt(max(0.0, 1.0 - viewZenithCosAngle * viewZenithCosAngle));
    vec3 worldDir = vec3(0.0, viewZenithSinAngle, viewZenithCosAngle);
    vec3 opticalDepth = IntegrateOpticalDepthToTopBoundary(atm, worldPos, worldDir, 40.0);
    vec3 transmittance = exp(-opticalDepth);
    OutColor = vec4(clamp(transmittance, 0.0, 1.0), 1.0);
}
)";

constexpr const char* kSkyViewFs = R"(
#version 330 core
layout(location = 0) out vec4 OutColor;

uniform sampler2D transmittance_texture;
uniform sampler2D uMultiScattTexture;
uniform vec3 uCameraPos;
uniform vec3 uEarthCenter;
uniform vec3 uSunDir;
uniform float uLengthUnitInMeters;
uniform vec2 uOutputSize;

const float PI = 3.14159265358979323846;
const float PLANET_RADIUS_OFFSET = 0.01;

struct AtmosphereParameters {
    float BottomRadius;
    float TopRadius;
    float RayleighDensityExpScale;
    vec3 RayleighScattering;
    float MieDensityExpScale;
    vec3 MieScattering;
    vec3 MieExtinction;
    vec3 MieAbsorption;
    float MiePhaseG;
    float AbsorptionDensity0LayerWidth;
    float AbsorptionDensity0ConstantTerm;
    float AbsorptionDensity0LinearTerm;
    float AbsorptionDensity1ConstantTerm;
    float AbsorptionDensity1LinearTerm;
    vec3 AbsorptionExtinction;
};

struct MediumSampleRGB {
    vec3 scattering;
    vec3 extinction;
    vec3 scatteringMie;
    vec3 scatteringRay;
};

struct IntegrationResult {
    vec3 L;
    vec3 T;
};

float raySphereIntersectNearest(vec3 r0, vec3 rd, vec3 s0, float sR) {
    float a = dot(rd, rd);
    vec3 s0_r0 = r0 - s0;
    float b = 2.0 * dot(rd, s0_r0);
    float c = dot(s0_r0, s0_r0) - sR * sR;
    float delta = b * b - 4.0 * a * c;
    if (delta < 0.0 || a == 0.0) return -1.0;
    float sqrtDelta = sqrt(delta);
    float sol0 = (-b - sqrtDelta) / (2.0 * a);
    float sol1 = (-b + sqrtDelta) / (2.0 * a);
    if (sol0 < 0.0 && sol1 < 0.0) return -1.0;
    if (sol0 < 0.0) return max(0.0, sol1);
    if (sol1 < 0.0) return max(0.0, sol0);
    return max(0.0, min(sol0, sol1));
}

float fromSubUvsToUnit(float u, float resolution) {
    return (u - 0.5 / resolution) * (resolution / (resolution - 1.0));
}

void UvToSkyViewLutParams(in AtmosphereParameters atm, out float viewZenithCosAngle, out float lightViewCosAngle, in float viewHeight, in vec2 uv) {
    uv = vec2(fromSubUvsToUnit(uv.x, 192.0), fromSubUvsToUnit(uv.y, 108.0));

    float Vhorizon = sqrt(max(0.0, viewHeight * viewHeight - atm.BottomRadius * atm.BottomRadius));
    float CosBeta = Vhorizon / max(viewHeight, 1e-4);
    float Beta = acos(clamp(CosBeta, -1.0, 1.0));
    float ZenithHorizonAngle = PI - Beta;

    if (uv.y < 0.5) {
        float coord = 2.0 * uv.y;
        coord = 1.0 - coord;
        coord *= coord;
        coord = 1.0 - coord;
        viewZenithCosAngle = cos(ZenithHorizonAngle * coord);
    } else {
        float coord = uv.y * 2.0 - 1.0;
        coord *= coord;
        viewZenithCosAngle = cos(ZenithHorizonAngle + Beta * coord);
    }

    float coord = uv.x;
    coord *= coord;
    lightViewCosAngle = -(coord * 2.0 - 1.0);
}

void LutTransmittanceParamsToUv(in AtmosphereParameters atm, in float viewHeight, in float viewZenithCosAngle, out vec2 uv) {
    float H = sqrt(max(0.0, atm.TopRadius * atm.TopRadius - atm.BottomRadius * atm.BottomRadius));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - atm.BottomRadius * atm.BottomRadius));
    float discriminant = viewHeight * viewHeight * (viewZenithCosAngle * viewZenithCosAngle - 1.0) + atm.TopRadius * atm.TopRadius;
    float d = max(0.0, (-viewHeight * viewZenithCosAngle + sqrt(max(discriminant, 0.0))));
    float dMin = atm.TopRadius - viewHeight;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-4);
    float xR = rho / max(H, 1e-4);
    uv = vec2(xMu, xR);
}

AtmosphereParameters GetAtmosphereParameters() {
    AtmosphereParameters a;
    a.BottomRadius = 6360.0;
    a.TopRadius = 6460.0;
    a.RayleighDensityExpScale = -1.0 / 8.0;
    a.RayleighScattering = vec3(0.005802, 0.013558, 0.033100);
    a.MieDensityExpScale = -1.0 / 1.2;
    a.MieScattering = vec3(0.003996);
    a.MieExtinction = vec3(0.004440);
    a.MieAbsorption = max(a.MieExtinction - a.MieScattering, vec3(0.0));
    a.MiePhaseG = 0.8;
    a.AbsorptionDensity0LayerWidth = 25.0;
    a.AbsorptionDensity0ConstantTerm = 0.0;
    a.AbsorptionDensity0LinearTerm = 1.0 / 15.0;
    a.AbsorptionDensity1ConstantTerm = 8.0 / 3.0;
    a.AbsorptionDensity1LinearTerm = -1.0 / 15.0;
    a.AbsorptionExtinction = vec3(0.000650, 0.001881, 0.000085);
    return a;
}

float fromUnitToSubUvs(float u, float resolution) {
    return (u + 0.5 / resolution) * (resolution / (resolution + 1.0));
}

vec3 GetMultipleScattering(in AtmosphereParameters atm, in vec3 worldPos, in float sunZenithCosAngle) {
    vec2 uv = clamp(vec2(
        sunZenithCosAngle * 0.5 + 0.5,
        (length(worldPos) - atm.BottomRadius) / max(atm.TopRadius - atm.BottomRadius, 1e-4)),
        vec2(0.0), vec2(1.0));
    uv = vec2(fromUnitToSubUvs(uv.x, 32.0), fromUnitToSubUvs(uv.y, 32.0));
    return texture(uMultiScattTexture, uv).rgb;
}

MediumSampleRGB sampleMediumRGB(in vec3 worldPos, in AtmosphereParameters atm) {
    float viewHeight = length(worldPos) - atm.BottomRadius;
    float densityMie = exp(atm.MieDensityExpScale * viewHeight);
    float densityRay = exp(atm.RayleighDensityExpScale * viewHeight);
    float densityOzo = clamp(viewHeight < atm.AbsorptionDensity0LayerWidth ?
        atm.AbsorptionDensity0LinearTerm * viewHeight + atm.AbsorptionDensity0ConstantTerm :
        atm.AbsorptionDensity1LinearTerm * viewHeight + atm.AbsorptionDensity1ConstantTerm,
        0.0, 1.0);

    MediumSampleRGB s;
    s.scatteringMie = densityMie * atm.MieScattering;
    s.scatteringRay = densityRay * atm.RayleighScattering;
    vec3 absorptionMie = densityMie * atm.MieAbsorption;
    vec3 extinctionMie = densityMie * atm.MieExtinction;
    vec3 extinctionRay = s.scatteringRay;
    vec3 absorptionOzo = densityOzo * atm.AbsorptionExtinction;
    vec3 extinctionOzo = absorptionOzo;
    s.scattering = s.scatteringMie + s.scatteringRay;
    s.extinction = extinctionMie + extinctionRay + extinctionOzo;
    return s;
}

float RayleighPhase(float cosTheta) {
    float factor = 3.0 / (16.0 * PI);
    return factor * (1.0 + cosTheta * cosTheta);
}

float CornetteShanksMiePhaseFunction(float g, float cosTheta) {
    float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(1.0 + g * g - 2.0 * g * -cosTheta, 1.5);
}

bool MoveToTopAtmosphere(inout vec3 worldPos, in vec3 worldDir, in float topRadius) {
    float viewHeight = length(worldPos);
    if (viewHeight > topRadius) {
        float tTop = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), topRadius);
        if (tTop < 0.0) return false;
        vec3 up = worldPos / max(viewHeight, 1e-4);
        worldPos = worldPos + worldDir * tTop + up * -PLANET_RADIUS_OFFSET;
    }
    return true;
}

IntegrationResult IntegrateScatteredLuminance(
    in vec2 pixPos,
    in vec3 worldPos,
    in vec3 worldDir,
    in vec3 sunDir,
    in AtmosphereParameters atm,
    in float sampleCountIni,
    in bool variableSampleCount,
    in bool mieRayPhase,
    in float tMaxMax) {
    IntegrationResult result;
    result.L = vec3(0.0);
    result.T = vec3(1.0);

    float tBottom = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.BottomRadius);
    float tTop = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.TopRadius);

    float tMax = 0.0;
    if (tBottom < 0.0) {
        if (tTop < 0.0) {
            return result;
        }
        tMax = tTop;
    } else {
        if (tTop > 0.0) {
            tMax = min(tTop, tBottom);
        }
    }
    tMax = min(tMax, tMaxMax);
    if (tMax <= 0.0) {
        return result;
    }

    float sampleCount = sampleCountIni;
    float sampleCountFloor = sampleCountIni;
    float tMaxFloor = tMax;
    if (variableSampleCount) {
        sampleCount = mix(12.0, 30.0, clamp(tMax * 0.01, 0.0, 1.0));
        sampleCountFloor = floor(sampleCount);
        tMaxFloor = tMax * sampleCountFloor / max(sampleCount, 1e-4);
    }

    float dt = tMax / max(sampleCount, 1.0);
    float t = 0.0;
    vec3 throughput = vec3(1.0);

    float cosTheta = dot(sunDir, worldDir);
    float miePhase = CornetteShanksMiePhaseFunction(atm.MiePhaseG, -cosTheta);
    float rayleighPhase = RayleighPhase(cosTheta);

    for (int i = 0; i < 128; ++i) {
        float s = float(i);
        if (s >= sampleCount) break;

        if (variableSampleCount) {
            float t0 = s / max(sampleCountFloor, 1.0);
            float t1 = (s + 1.0) / max(sampleCountFloor, 1.0);
            t0 = t0 * t0;
            t1 = t1 * t1;
            t0 = tMaxFloor * t0;
            t1 = (t1 > 1.0) ? tMax : (tMaxFloor * t1);
            t = t0 + (t1 - t0) * 0.3;
            dt = t1 - t0;
        } else {
            float newT = tMax * (s + 0.3) / max(sampleCount, 1.0);
            dt = newT - t;
            t = newT;
        }

        vec3 P = worldPos + t * worldDir;
        MediumSampleRGB medium = sampleMediumRGB(P, atm);
        vec3 sampleOpticalDepth = medium.extinction * dt;
        vec3 sampleTransmittance = exp(-sampleOpticalDepth);

        float pHeight = length(P);
        vec3 up = P / max(pHeight, 1e-4);
        float sunZenithCosAngle = dot(sunDir, up);
        vec2 uv;
        LutTransmittanceParamsToUv(atm, pHeight, sunZenithCosAngle, uv);
        vec3 transmittanceToSun = texture(transmittance_texture, uv).rgb;

        vec3 phaseTimesScattering = mieRayPhase ?
            (medium.scatteringMie * miePhase + medium.scatteringRay * rayleighPhase) :
            (medium.scattering * (1.0 / (4.0 * PI)));

        float tEarth = raySphereIntersectNearest(P, sunDir, vec3(0.0) + PLANET_RADIUS_OFFSET * up, atm.BottomRadius);
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;

        vec3 multiScatteredLuminance = GetMultipleScattering(atm, P, sunZenithCosAngle);
        vec3 S = earthShadow * transmittanceToSun * phaseTimesScattering + multiScatteredLuminance * medium.scattering;
        vec3 denom = max(medium.extinction, vec3(1e-6));
        vec3 Sint = (S - S * sampleTransmittance) / denom;
        result.L += throughput * Sint;
        throughput *= sampleTransmittance;
    }

    result.T = throughput;
    return result;
}

void main() {
    AtmosphereParameters atm = GetAtmosphereParameters();

    vec2 pixPos = gl_FragCoord.xy;
    vec2 uv = pixPos / max(uOutputSize, vec2(1.0));

    float unitScale = max(uLengthUnitInMeters, 1e-4);
    vec3 camera = uCameraPos / unitScale - uEarthCenter;
    vec3 sunDirWorld = normalize(uSunDir);

    float viewHeight = length(camera);

    float viewZenithCosAngle;
    float lightViewCosAngle;
    UvToSkyViewLutParams(atm, viewZenithCosAngle, lightViewCosAngle, viewHeight, uv);

    vec3 up = camera / max(viewHeight, 1e-4);
    float sunZenithCosAngle = dot(up, sunDirWorld);
    vec3 sunDirLocal = normalize(vec3(sqrt(max(0.0, 1.0 - sunZenithCosAngle * sunZenithCosAngle)), 0.0, sunZenithCosAngle));

    vec3 worldPos = vec3(0.0, 0.0, viewHeight);
    float viewZenithSinAngle = sqrt(max(0.0, 1.0 - viewZenithCosAngle * viewZenithCosAngle));
    vec3 worldDir = vec3(
        viewZenithSinAngle * lightViewCosAngle,
        viewZenithSinAngle * sqrt(max(0.0, 1.0 - lightViewCosAngle * lightViewCosAngle)),
        viewZenithCosAngle);

    if (!MoveToTopAtmosphere(worldPos, worldDir, atm.TopRadius)) {
        OutColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    IntegrationResult ss = IntegrateScatteredLuminance(
        pixPos, worldPos, worldDir, sunDirLocal, atm,
        30.0, true, true, 9000000.0);

    OutColor = vec4(max(ss.L, 0.0), 1.0);
}
)";

constexpr const char* kCameraVolumeFs = R"(
#version 330 core
layout(location = 0) out vec4 OutScattering;
layout(location = 1) out vec4 OutTransmittance;

uniform sampler2D transmittance_texture;
uniform sampler2D uMultiScattTexture;
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec3 uCameraPos;
uniform vec3 uEarthCenter;
uniform vec3 uSunDir;
uniform float uLengthUnitInMeters;
uniform vec2 uOutputSize;
uniform int uSlice;

const float PI = 3.14159265358979323846;
const float PLANET_RADIUS_OFFSET = 0.01;
const float AP_SLICE_COUNT = 32.0;
const float AP_KM_PER_SLICE = 4.0;

struct AtmosphereParameters {
    float BottomRadius;
    float TopRadius;
    float RayleighDensityExpScale;
    vec3 RayleighScattering;
    float MieDensityExpScale;
    vec3 MieScattering;
    vec3 MieExtinction;
    vec3 MieAbsorption;
    float MiePhaseG;
    float AbsorptionDensity0LayerWidth;
    float AbsorptionDensity0ConstantTerm;
    float AbsorptionDensity0LinearTerm;
    float AbsorptionDensity1ConstantTerm;
    float AbsorptionDensity1LinearTerm;
    vec3 AbsorptionExtinction;
};

struct MediumSampleRGB {
    vec3 scattering;
    vec3 extinction;
    vec3 scatteringMie;
    vec3 scatteringRay;
};

struct IntegrationResult {
    vec3 L;
    vec3 T;
};

float raySphereIntersectNearest(vec3 r0, vec3 rd, vec3 s0, float sR) {
    float a = dot(rd, rd);
    vec3 s0_r0 = r0 - s0;
    float b = 2.0 * dot(rd, s0_r0);
    float c = dot(s0_r0, s0_r0) - sR * sR;
    float delta = b * b - 4.0 * a * c;
    if (delta < 0.0 || a == 0.0) return -1.0;
    float sqrtDelta = sqrt(delta);
    float sol0 = (-b - sqrtDelta) / (2.0 * a);
    float sol1 = (-b + sqrtDelta) / (2.0 * a);
    if (sol0 < 0.0 && sol1 < 0.0) return -1.0;
    if (sol0 < 0.0) return max(0.0, sol1);
    if (sol1 < 0.0) return max(0.0, sol0);
    return max(0.0, min(sol0, sol1));
}

float AerialPerspectiveSliceToDepth(float slice) {
    return slice * AP_KM_PER_SLICE;
}

void LutTransmittanceParamsToUv(in AtmosphereParameters atm, in float viewHeight, in float viewZenithCosAngle, out vec2 uv) {
    float H = sqrt(max(0.0, atm.TopRadius * atm.TopRadius - atm.BottomRadius * atm.BottomRadius));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - atm.BottomRadius * atm.BottomRadius));
    float discriminant = viewHeight * viewHeight * (viewZenithCosAngle * viewZenithCosAngle - 1.0) + atm.TopRadius * atm.TopRadius;
    float d = max(0.0, (-viewHeight * viewZenithCosAngle + sqrt(max(discriminant, 0.0))));
    float dMin = atm.TopRadius - viewHeight;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-4);
    float xR = rho / max(H, 1e-4);
    uv = vec2(xMu, xR);
}

AtmosphereParameters GetAtmosphereParameters() {
    AtmosphereParameters a;
    a.BottomRadius = 6360.0;
    a.TopRadius = 6460.0;
    a.RayleighDensityExpScale = -1.0 / 8.0;
    a.RayleighScattering = vec3(0.005802, 0.013558, 0.033100);
    a.MieDensityExpScale = -1.0 / 1.2;
    a.MieScattering = vec3(0.003996);
    a.MieExtinction = vec3(0.004440);
    a.MieAbsorption = max(a.MieExtinction - a.MieScattering, vec3(0.0));
    a.MiePhaseG = 0.8;
    a.AbsorptionDensity0LayerWidth = 25.0;
    a.AbsorptionDensity0ConstantTerm = 0.0;
    a.AbsorptionDensity0LinearTerm = 1.0 / 15.0;
    a.AbsorptionDensity1ConstantTerm = 8.0 / 3.0;
    a.AbsorptionDensity1LinearTerm = -1.0 / 15.0;
    a.AbsorptionExtinction = vec3(0.000650, 0.001881, 0.000085);
    return a;
}

float fromUnitToSubUvs(float u, float resolution) {
    return (u + 0.5 / resolution) * (resolution / (resolution + 1.0));
}

vec3 GetMultipleScattering(in AtmosphereParameters atm, in vec3 worldPos, in float sunZenithCosAngle) {
    vec2 uv = clamp(vec2(
        sunZenithCosAngle * 0.5 + 0.5,
        (length(worldPos) - atm.BottomRadius) / max(atm.TopRadius - atm.BottomRadius, 1e-4)),
        vec2(0.0), vec2(1.0));
    uv = vec2(fromUnitToSubUvs(uv.x, 32.0), fromUnitToSubUvs(uv.y, 32.0));
    return texture(uMultiScattTexture, uv).rgb;
}

MediumSampleRGB sampleMediumRGB(in vec3 worldPos, in AtmosphereParameters atm) {
    float viewHeight = length(worldPos) - atm.BottomRadius;
    float densityMie = exp(atm.MieDensityExpScale * viewHeight);
    float densityRay = exp(atm.RayleighDensityExpScale * viewHeight);
    float densityOzo = clamp(viewHeight < atm.AbsorptionDensity0LayerWidth ?
        atm.AbsorptionDensity0LinearTerm * viewHeight + atm.AbsorptionDensity0ConstantTerm :
        atm.AbsorptionDensity1LinearTerm * viewHeight + atm.AbsorptionDensity1ConstantTerm,
        0.0, 1.0);

    MediumSampleRGB s;
    s.scatteringMie = densityMie * atm.MieScattering;
    s.scatteringRay = densityRay * atm.RayleighScattering;
    vec3 absorptionMie = densityMie * atm.MieAbsorption;
    vec3 extinctionMie = densityMie * atm.MieExtinction;
    vec3 extinctionRay = s.scatteringRay;
    vec3 absorptionOzo = densityOzo * atm.AbsorptionExtinction;
    vec3 extinctionOzo = absorptionOzo;
    s.scattering = s.scatteringMie + s.scatteringRay;
    s.extinction = extinctionMie + extinctionRay + extinctionOzo;
    return s;
}

float RayleighPhase(float cosTheta) {
    float factor = 3.0 / (16.0 * PI);
    return factor * (1.0 + cosTheta * cosTheta);
}

float CornetteShanksMiePhaseFunction(float g, float cosTheta) {
    float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(1.0 + g * g - 2.0 * g * -cosTheta, 1.5);
}

bool MoveToTopAtmosphere(inout vec3 worldPos, in vec3 worldDir, in float topRadius) {
    float viewHeight = length(worldPos);
    if (viewHeight > topRadius) {
        float tTop = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), topRadius);
        if (tTop < 0.0) return false;
        vec3 up = worldPos / max(viewHeight, 1e-4);
        worldPos = worldPos + worldDir * tTop + up * -PLANET_RADIUS_OFFSET;
    }
    return true;
}

IntegrationResult IntegrateScatteredLuminance(
    in vec2 pixPos,
    in vec3 worldPos,
    in vec3 worldDir,
    in vec3 sunDir,
    in AtmosphereParameters atm,
    in float sampleCountIni,
    in bool mieRayPhase,
    in float tMaxMax) {
    IntegrationResult result;
    result.L = vec3(0.0);
    result.T = vec3(1.0);

    float tBottom = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.BottomRadius);
    float tTop = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.TopRadius);

    float tMax = 0.0;
    if (tBottom < 0.0) {
        if (tTop < 0.0) return result;
        tMax = tTop;
    } else if (tTop > 0.0) {
        tMax = min(tTop, tBottom);
    }
    tMax = min(tMax, tMaxMax);
    if (tMax <= 0.0) return result;

    float sampleCount = max(sampleCountIni, 1.0);
    float dt = tMax / sampleCount;
    float t = 0.0;
    vec3 throughput = vec3(1.0);

    float cosTheta = dot(sunDir, worldDir);
    float miePhase = CornetteShanksMiePhaseFunction(atm.MiePhaseG, -cosTheta);
    float rayleighPhase = RayleighPhase(cosTheta);

    for (int i = 0; i < 128; ++i) {
        float s = float(i);
        if (s >= sampleCount) break;
        float newT = tMax * (s + 0.3) / sampleCount;
        dt = newT - t;
        t = newT;

        vec3 P = worldPos + t * worldDir;
        MediumSampleRGB medium = sampleMediumRGB(P, atm);
        vec3 sampleOpticalDepth = medium.extinction * dt;
        vec3 sampleTransmittance = exp(-sampleOpticalDepth);

        float pHeight = length(P);
        vec3 up = P / max(pHeight, 1e-4);
        float sunZenithCosAngle = dot(sunDir, up);
        vec2 uv;
        LutTransmittanceParamsToUv(atm, pHeight, sunZenithCosAngle, uv);
        vec3 transmittanceToSun = texture(transmittance_texture, uv).rgb;

        vec3 phaseTimesScattering = mieRayPhase ?
            (medium.scatteringMie * miePhase + medium.scatteringRay * rayleighPhase) :
            (medium.scattering * (1.0 / (4.0 * PI)));

        float tEarth = raySphereIntersectNearest(P, sunDir, vec3(0.0) + PLANET_RADIUS_OFFSET * up, atm.BottomRadius);
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;

        vec3 multiScatteredLuminance = GetMultipleScattering(atm, P, sunZenithCosAngle);
        vec3 S = earthShadow * transmittanceToSun * phaseTimesScattering + multiScatteredLuminance * medium.scattering;
        vec3 denom = max(medium.extinction, vec3(1e-6));
        vec3 Sint = (S - S * sampleTransmittance) / denom;
        result.L += throughput * Sint;
        throughput *= sampleTransmittance;
    }



    result.T = throughput;
    return result;
}

void main() {
    AtmosphereParameters atm = GetAtmosphereParameters();

    vec2 pixPos = gl_FragCoord.xy;
    vec2 ndc = (pixPos / max(uOutputSize, vec2(1.0))) * 2.0 - 1.0;
    vec4 clip = vec4(ndc, 1.0, 1.0);
    vec4 view = uInvProj * clip;
    view /= view.w;

    vec3 worldDir = normalize((uInvView * vec4(view.xyz, 0.0)).xyz);

    vec3 sunDir = normalize(uSunDir);

    float unitScale = max(uLengthUnitInMeters, 1e-4);
    vec3 camPos = uCameraPos / unitScale - uEarthCenter;

    float slice = ((float(uSlice) + 0.5) / AP_SLICE_COUNT);
    slice *= slice;
    slice *= AP_SLICE_COUNT;

    vec3 worldPos = camPos;
    float tMax = AerialPerspectiveSliceToDepth(slice);
    vec3 newWorldPos = worldPos + tMax * worldDir;

    float viewHeight = length(newWorldPos);
    if (viewHeight <= (atm.BottomRadius + PLANET_RADIUS_OFFSET)) {
        newWorldPos = normalize(newWorldPos) * (atm.BottomRadius + PLANET_RADIUS_OFFSET + 0.001);
        worldDir = normalize(newWorldPos - camPos);
        tMax = length(newWorldPos - camPos);
    }
    float tMaxMax = tMax;

    viewHeight = length(worldPos);
    if (viewHeight >= atm.TopRadius) {
        vec3 prevWorldPos = worldPos;
        if (!MoveToTopAtmosphere(worldPos, worldDir, atm.TopRadius)) {
            OutScattering = vec4(0.0, 0.0, 0.0, 1.0);
            OutTransmittance = vec4(1.0, 1.0, 1.0, 1.0);
            return;
        }
        float lengthToAtmosphere = length(prevWorldPos - worldPos);
        if (tMaxMax < lengthToAtmosphere) {
            OutScattering = vec4(0.0, 0.0, 0.0, 1.0);
            OutTransmittance = vec4(1.0, 1.0, 1.0, 1.0);
            return;
        }
        tMaxMax = max(0.0, tMaxMax - lengthToAtmosphere);
    }

    float sampleCount = max(1.0, (float(uSlice) + 1.0) * 2.0);
    IntegrationResult ss = IntegrateScatteredLuminance(
        pixPos, worldPos, worldDir, sunDir, atm, sampleCount, true, tMaxMax);
    


    float trans = dot(ss.T, vec3(1.0 / 3.0));
    OutScattering = vec4(max(ss.L, 0.0), clamp(1.0 - trans, 0.0, 1.0));
    OutTransmittance = vec4(clamp(ss.T, 0.0, 1.0), 1.0);
}
)";

constexpr const char* kMultiScattFs = R"(
#version 330 core
layout(location = 0) out vec4 OutColor;

uniform sampler2D transmittance_texture;
uniform vec2 uOutputSize;

const float PI = 3.14159265358979323846;
const float PLANET_RADIUS_OFFSET = 0.01;
const float MULTI_SCATTERING_FACTOR = 1.0;

struct AtmosphereParameters {
    float BottomRadius;
    float TopRadius;
    float RayleighDensityExpScale;
    vec3 RayleighScattering;
    float MieDensityExpScale;
    vec3 MieScattering;
    vec3 MieExtinction;
    vec3 MieAbsorption;
    float MiePhaseG;
    float AbsorptionDensity0LayerWidth;
    float AbsorptionDensity0ConstantTerm;
    float AbsorptionDensity0LinearTerm;
    float AbsorptionDensity1ConstantTerm;
    float AbsorptionDensity1LinearTerm;
    vec3 AbsorptionExtinction;
};

struct MediumSampleRGB {
    vec3 scattering;
    vec3 extinction;
};

struct IntegrationResult {
    vec3 L;
    vec3 T;
    vec3 MultiScatAs1;
};

float raySphereIntersectNearest(vec3 r0, vec3 rd, vec3 s0, float sR) {
    float a = dot(rd, rd);
    vec3 s0_r0 = r0 - s0;
    float b = 2.0 * dot(rd, s0_r0);
    float c = dot(s0_r0, s0_r0) - sR * sR;
    float delta = b * b - 4.0 * a * c;
    if (delta < 0.0 || a == 0.0) return -1.0;
    float sqrtDelta = sqrt(delta);
    float sol0 = (-b - sqrtDelta) / (2.0 * a);
    float sol1 = (-b + sqrtDelta) / (2.0 * a);
    if (sol0 < 0.0 && sol1 < 0.0) return -1.0;
    if (sol0 < 0.0) return max(0.0, sol1);
    if (sol1 < 0.0) return max(0.0, sol0);
    return max(0.0, min(sol0, sol1));
}

float fromSubUvsToUnit(float u, float resolution) {
    return (u - 0.5 / resolution) * (resolution / (resolution - 1.0));
}

void LutTransmittanceParamsToUv(in AtmosphereParameters atm, in float viewHeight, in float viewZenithCosAngle, out vec2 uv) {
    float H = sqrt(max(0.0, atm.TopRadius * atm.TopRadius - atm.BottomRadius * atm.BottomRadius));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - atm.BottomRadius * atm.BottomRadius));
    float discriminant = viewHeight * viewHeight * (viewZenithCosAngle * viewZenithCosAngle - 1.0) + atm.TopRadius * atm.TopRadius;
    float d = max(0.0, (-viewHeight * viewZenithCosAngle + sqrt(max(discriminant, 0.0))));
    float dMin = atm.TopRadius - viewHeight;
    float dMax = rho + H;
    float xMu = (d - dMin) / max(dMax - dMin, 1e-4);
    float xR = rho / max(H, 1e-4);
    uv = vec2(xMu, xR);
}

AtmosphereParameters GetAtmosphereParameters() {
    AtmosphereParameters a;
    a.BottomRadius = 6360.0;
    a.TopRadius = 6460.0;
    a.RayleighDensityExpScale = -1.0 / 8.0;
    a.RayleighScattering = vec3(0.005802, 0.013558, 0.033100);
    a.MieDensityExpScale = -1.0 / 1.2;
    a.MieScattering = vec3(0.003996);
    a.MieExtinction = vec3(0.004440);
    a.MieAbsorption = max(a.MieExtinction - a.MieScattering, vec3(0.0));
    a.MiePhaseG = 0.8;
    a.AbsorptionDensity0LayerWidth = 25.0;
    a.AbsorptionDensity0ConstantTerm = 0.0;
    a.AbsorptionDensity0LinearTerm = 1.0 / 15.0;
    a.AbsorptionDensity1ConstantTerm = 8.0 / 3.0;
    a.AbsorptionDensity1LinearTerm = -1.0 / 15.0;
    a.AbsorptionExtinction = vec3(0.000650, 0.001881, 0.000085);
    return a;
}

MediumSampleRGB sampleMediumRGB(in vec3 worldPos, in AtmosphereParameters atm) {
    float viewHeight = length(worldPos) - atm.BottomRadius;
    float densityMie = exp(atm.MieDensityExpScale * viewHeight);
    float densityRay = exp(atm.RayleighDensityExpScale * viewHeight);
    float densityOzo = clamp(viewHeight < atm.AbsorptionDensity0LayerWidth ?
        atm.AbsorptionDensity0LinearTerm * viewHeight + atm.AbsorptionDensity0ConstantTerm :
        atm.AbsorptionDensity1LinearTerm * viewHeight + atm.AbsorptionDensity1ConstantTerm,
        0.0, 1.0);

    vec3 scatteringMie = densityMie * atm.MieScattering;
    vec3 scatteringRay = densityRay * atm.RayleighScattering;
    vec3 extinctionMie = densityMie * atm.MieExtinction;
    vec3 extinctionRay = scatteringRay;
    vec3 extinctionOzo = densityOzo * atm.AbsorptionExtinction;

    MediumSampleRGB s;
    s.scattering = scatteringMie + scatteringRay;
    s.extinction = extinctionMie + extinctionRay + extinctionOzo;
    return s;
}

IntegrationResult IntegrateScatteredLuminance(
    in vec3 worldPos,
    in vec3 worldDir,
    in vec3 sunDir,
    in AtmosphereParameters atm,
    in float sampleCountIni) {

    IntegrationResult result;
    result.L = vec3(0.0);
    result.T = vec3(1.0);
    result.MultiScatAs1 = vec3(0.0);

    float tBottom = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.BottomRadius);
    float tTop = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), atm.TopRadius);

    float tMax = 0.0;
    if (tBottom < 0.0) {
        if (tTop < 0.0) return result;
        tMax = tTop;
    } else if (tTop > 0.0) {
        tMax = min(tTop, tBottom);
    }
    if (tMax <= 0.0) return result;

    float sampleCount = max(sampleCountIni, 1.0);
    float t = 0.0;
    vec3 throughput = vec3(1.0);
    float uniformPhase = 1.0 / (4.0 * PI);

    for (int i = 0; i < 64; ++i) {
        float s = float(i);
        if (s >= sampleCount) break;
        float newT = tMax * (s + 0.3) / sampleCount;
        float dt = newT - t;
        t = newT;

        vec3 P = worldPos + t * worldDir;
        MediumSampleRGB medium = sampleMediumRGB(P, atm);
        vec3 sampleOpticalDepth = medium.extinction * dt;
        vec3 sampleTransmittance = exp(-sampleOpticalDepth);

        float pHeight = length(P);
        vec3 up = P / max(pHeight, 1e-4);
        float sunZenithCosAngle = dot(sunDir, up);
        vec2 uv;
        LutTransmittanceParamsToUv(atm, pHeight, sunZenithCosAngle, uv);
        vec3 transmittanceToSun = texture(transmittance_texture, uv).rgb;

        float tEarth = raySphereIntersectNearest(P, sunDir, PLANET_RADIUS_OFFSET * up, atm.BottomRadius);
        float earthShadow = tEarth >= 0.0 ? 0.0 : 1.0;

        vec3 S = earthShadow * transmittanceToSun * (medium.scattering * uniformPhase);
        vec3 denom = max(medium.extinction, vec3(1e-6));
        vec3 Sint = (S - S * sampleTransmittance) / denom;
        result.L += throughput * Sint;

        vec3 MSint = (medium.scattering - medium.scattering * sampleTransmittance) / denom;
        result.MultiScatAs1 += throughput * MSint;

        throughput *= sampleTransmittance;
    }

    result.T = throughput;
    return result;
}

void main() {
    AtmosphereParameters atm = GetAtmosphereParameters();

    vec2 pixPos = gl_FragCoord.xy;
    vec2 uv = pixPos / max(uOutputSize, vec2(1.0));
    uv = vec2(fromSubUvsToUnit(uv.x, uOutputSize.x), fromSubUvsToUnit(uv.y, uOutputSize.y));

    float cosSunZenithAngle = uv.x * 2.0 - 1.0;
    vec3 sunDir = vec3(0.0, sqrt(max(0.0, 1.0 - cosSunZenithAngle * cosSunZenithAngle)), cosSunZenithAngle);
    float viewHeight = atm.BottomRadius + clamp(uv.y + PLANET_RADIUS_OFFSET, 0.0, 1.0) * (atm.TopRadius - atm.BottomRadius - PLANET_RADIUS_OFFSET);

    vec3 worldPos = vec3(0.0, 0.0, viewHeight);

    const float sphereSolidAngle = 4.0 * PI;
    const float isotropicPhase = 1.0 / sphereSolidAngle;

    vec3 accumAs1 = vec3(0.0);
    vec3 accumL = vec3(0.0);

    for (int iy = 0; iy < 8; ++iy) {
        for (int ix = 0; ix < 8; ++ix) {
            float randA = (float(ix) + 0.5) / 8.0;
            float randB = (float(iy) + 0.5) / 8.0;
            float theta = 2.0 * PI * randA;
            float phi = acos(1.0 - 2.0 * randB);
            float cosPhi = cos(phi);
            float sinPhi = sin(phi);
            float cosTheta = cos(theta);
            float sinTheta = sin(theta);
            vec3 worldDir = vec3(cosTheta * sinPhi, sinTheta * sinPhi, cosPhi);

            IntegrationResult r = IntegrateScatteredLuminance(worldPos, worldDir, sunDir, atm, 20.0);
            accumAs1 += r.MultiScatAs1 * sphereSolidAngle / 64.0;
            accumL += r.L * sphereSolidAngle / 64.0;
        }
    }

    vec3 multiScatAs1 = accumAs1 * isotropicPhase;
    vec3 inScattered = accumL * isotropicPhase;

    vec3 rr = max(multiScatAs1, vec3(0.0));
    vec3 rr2 = rr * rr;
    vec3 sumAll = vec3(1.0) + rr + rr2 + rr * rr2 + rr2 * rr2;
    vec3 L = inScattered * sumAll;

    OutColor = vec4(MULTI_SCATTERING_FACTOR * max(L, 0.0), 1.0);
}
)";
GLuint compileShader(GLenum stage, const char* source) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        std::string log;
        log.resize(static_cast<size_t>(length > 0 ? length : 1));
        glGetShaderInfoLog(shader, length, nullptr, log.data());
        std::cerr << "Shader compile failed:\n" << log << "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint linkProgram(GLuint vs, GLuint fs, GLuint gs = 0, GLuint extraFs = 0) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    if (gs != 0) {
        glAttachShader(program, gs);
    }
    if (extraFs != 0) {
        glAttachShader(program, extraFs);
    }
    glLinkProgram(program);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        std::string log;
        log.resize(static_cast<size_t>(length > 0 ? length : 1));
        glGetProgramInfoLog(program, length, nullptr, log.data());
        std::cerr << "Program link failed:\n" << log << "\n";
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

} // namespace

Sky::~Sky() {
    destroy();
}

bool Sky::initialize(const glm::vec3& sunDir) {
    (void)sunDir;
    destroy();

    GLint major = 0;
    GLint minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    const bool canUseAdvancedBackend = (major > 4) || (major == 4 && minor >= 3);
    if (!canUseAdvancedBackend) {
        backend_ = Backend::Simple33;
        sunSize_ = glm::vec2(
            static_cast<float>(std::tan(kSunAngularRadius)),
            static_cast<float>(std::cos(kSunAngularRadius)));
        initialized_ = true;
        std::cout << "Simple sky backend initialized (OpenGL " << major << "." << minor << ").\n";
        return true;
    }

    const bool useHalfPrecision = false;
    const bool useCombinedTextures = true;
    const double maxSunZenithAngle = 120.0 / 180.0 * kPi;

    atmosphere::DensityProfileLayer rayleighLayer(
        0.0, 1.0, -1.0 / kRayleighScaleHeight, 0.0, 0.0);
    atmosphere::DensityProfileLayer mieLayer(
        0.0, 1.0, -1.0 / kMieScaleHeight, 0.0, 0.0);

    std::vector<atmosphere::DensityProfileLayer> ozoneDensity;
    ozoneDensity.emplace_back(25000.0, 0.0, 0.0, 1.0 / 15000.0, -2.0 / 3.0);
    ozoneDensity.emplace_back(0.0, 0.0, 0.0, -1.0 / 15000.0, 8.0 / 3.0);

    // Match D3D11 sample coefficients (1/km converted to 1/m), 3-channel mode.
    std::vector<double> wavelengths = { 440.0, 550.0, 680.0 };
    std::vector<double> solarIrradiance = { 1.0, 1.0, 1.0 };
    std::vector<double> rayleighScattering = {
        0.033100 * kInvKmToInvM,
        0.013558 * kInvKmToInvM,
        0.005802 * kInvKmToInvM
    };
    std::vector<double> mieScattering = {
        0.003996 * kInvKmToInvM,
        0.003996 * kInvKmToInvM,
        0.003996 * kInvKmToInvM
    };
    std::vector<double> mieExtinction = {
        0.004440 * kInvKmToInvM,
        0.004440 * kInvKmToInvM,
        0.004440 * kInvKmToInvM
    };
    std::vector<double> absorptionExtinction = {
        0.000085 * kInvKmToInvM,
        0.001881 * kInvKmToInvM,
        0.000650 * kInvKmToInvM
    };
    std::vector<double> groundAlbedo = {
        kGroundAlbedo,
        kGroundAlbedo,
        kGroundAlbedo
    };

    model_ = std::make_unique<atmosphere::Model>(
        wavelengths,
        solarIrradiance,
        kSunAngularRadius,
        kBottomRadius,
        kTopRadius,
        std::vector<atmosphere::DensityProfileLayer>{rayleighLayer},
        rayleighScattering,
        std::vector<atmosphere::DensityProfileLayer>{mieLayer},
        mieScattering,
        mieExtinction,
        kMiePhaseFunctionG,
        ozoneDensity,
        absorptionExtinction,
        groundAlbedo,
        maxSunZenithAngle,
        static_cast<double>(lengthUnitInMeters_),
        3,
        useCombinedTextures,
        useHalfPrecision);

    model_->Init(kBrunetonScatteringOrders);
    backend_ = Backend::Advanced43;

    if (!initAdvancedProgramsAndTextures()) {
        std::cerr << "Failed to initialize advanced LUT programs/textures.\n";
        destroy();
        return false;
    }

    earthCenter_ = glm::vec3(0.0f, static_cast<float>(-kBottomRadius / lengthUnitInMeters_), 0.0f);
    sunSize_ = glm::vec2(
        static_cast<float>(std::tan(kSunAngularRadius)),
        static_cast<float>(std::cos(kSunAngularRadius)));
    initialized_ = true;

    std::cout << "Advanced atmosphere backend initialized (Hillaire LUT pass chain, OpenGL "
              << major << "." << minor << ").\n";
    return true;
}

void Sky::destroy() {
    destroyAdvancedResources();
    model_.reset();
    initialized_ = false;
    backend_ = Backend::Simple33;
}

bool Sky::initAdvancedProgramsAndTextures() {
    destroyAdvancedResources();

    glGenVertexArrays(1, &fsTriangleVao_);
    glGenFramebuffers(1, &lutFbo_);

    glGenTextures(1, &transmittanceLutTex_);
    glBindTexture(GL_TEXTURE_2D, transmittanceLutTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA16F, kTransmittanceLutWidth, kTransmittanceLutHeight, 0, GL_RGBA, GL_FLOAT, nullptr);

    glGenTextures(1, &skyViewLutTex_);
    glBindTexture(GL_TEXTURE_2D, skyViewLutTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA16F, kSkyViewLutWidth, kSkyViewLutHeight, 0, GL_RGBA, GL_FLOAT, nullptr);

    glGenTextures(1, &multiScattLutTex_);
    glBindTexture(GL_TEXTURE_2D, multiScattLutTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA16F, kMultiScattLutSize, kMultiScattLutSize, 0, GL_RGBA, GL_FLOAT, nullptr);

    glGenTextures(1, &cameraScatteringVolumeTex_);
    glBindTexture(GL_TEXTURE_3D, cameraScatteringVolumeTex_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(
        GL_TEXTURE_3D, 0, GL_RGBA16F, kCameraVolumeSize, kCameraVolumeSize, kCameraVolumeSize, 0, GL_RGBA, GL_FLOAT, nullptr);

    glGenTextures(1, &cameraTransmittanceVolumeTex_);
    glBindTexture(GL_TEXTURE_3D, cameraTransmittanceVolumeTex_);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(
        GL_TEXTURE_3D, 0, GL_RGBA16F, kCameraVolumeSize, kCameraVolumeSize, kCameraVolumeSize, 0, GL_RGBA, GL_FLOAT, nullptr);

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindTexture(GL_TEXTURE_3D, 0);

    const GLuint atmosphereFs = 0;
    GLuint vs = compileShader(GL_VERTEX_SHADER, kFullscreenVs);
    GLuint fsTransmittance = compileShader(GL_FRAGMENT_SHADER, kTransmittanceFs);
    GLuint fsSkyView = compileShader(GL_FRAGMENT_SHADER, kSkyViewFs);
    GLuint fsCameraVolume = compileShader(GL_FRAGMENT_SHADER, kCameraVolumeFs);
    GLuint fsMultiScatt = compileShader(GL_FRAGMENT_SHADER, kMultiScattFs);
    GLuint gs = compileShader(GL_GEOMETRY_SHADER, kLayeredGs);
    if (vs == 0 || fsTransmittance == 0 || fsSkyView == 0 || fsCameraVolume == 0 || fsMultiScatt == 0 || gs == 0) {
        if (vs) glDeleteShader(vs);
        if (fsTransmittance) glDeleteShader(fsTransmittance);
        if (fsSkyView) glDeleteShader(fsSkyView);
        if (fsCameraVolume) glDeleteShader(fsCameraVolume);
        if (gs) glDeleteShader(gs);
        if (fsMultiScatt) glDeleteShader(fsMultiScatt);
        return false;
    }

    transmittanceProgram_ = linkProgram(vs, fsTransmittance, 0, atmosphereFs);
    skyViewProgram_ = linkProgram(vs, fsSkyView, 0, atmosphereFs);
    cameraVolumeProgram_ = linkProgram(vs, fsCameraVolume, gs, atmosphereFs);
    multiScattProgram_ = linkProgram(vs, fsMultiScatt, 0, atmosphereFs);

    glDeleteShader(vs);
    glDeleteShader(fsTransmittance);
    glDeleteShader(fsSkyView);
    glDeleteShader(fsCameraVolume);
    glDeleteShader(fsMultiScatt);
    glDeleteShader(gs);

    if (transmittanceProgram_ == 0 || skyViewProgram_ == 0 || cameraVolumeProgram_ == 0 || multiScattProgram_ == 0) {
        return false;
    }

    return true;
}

void Sky::destroyAdvancedResources() {
    if (transmittanceProgram_ != 0) {
        glDeleteProgram(transmittanceProgram_);
        transmittanceProgram_ = 0;
    }
    if (skyViewProgram_ != 0) {
        glDeleteProgram(skyViewProgram_);
        skyViewProgram_ = 0;
    }
    if (cameraVolumeProgram_ != 0) {
        glDeleteProgram(cameraVolumeProgram_);
        cameraVolumeProgram_ = 0;
    }
    if (multiScattProgram_ != 0) {
        glDeleteProgram(multiScattProgram_);
        multiScattProgram_ = 0;
    }
    if (skyViewLutTex_ != 0) {
        glDeleteTextures(1, &skyViewLutTex_);
        skyViewLutTex_ = 0;
    }
    if (transmittanceLutTex_ != 0) {
        glDeleteTextures(1, &transmittanceLutTex_);
        transmittanceLutTex_ = 0;
    }
    if (multiScattLutTex_ != 0) {
        glDeleteTextures(1, &multiScattLutTex_);
        multiScattLutTex_ = 0;
    }
    if (cameraScatteringVolumeTex_ != 0) {
        glDeleteTextures(1, &cameraScatteringVolumeTex_);
        cameraScatteringVolumeTex_ = 0;
    }
    if (cameraTransmittanceVolumeTex_ != 0) {
        glDeleteTextures(1, &cameraTransmittanceVolumeTex_);
        cameraTransmittanceVolumeTex_ = 0;
    }
    if (lutFbo_ != 0) {
        glDeleteFramebuffers(1, &lutFbo_);
        lutFbo_ = 0;
    }
    if (fsTriangleVao_ != 0) {
        glDeleteVertexArrays(1, &fsTriangleVao_);
        fsTriangleVao_ = 0;
    }
}

void Sky::prepareAdvancedLuts(
    const glm::mat4& invProj,
    const glm::mat4& invView,
    const glm::vec3& cameraPos,
    const glm::vec3& sunDir) {
    if (!initialized_ || backend_ != Backend::Advanced43 || !model_) {
        return;
    }

    GLint previousViewport[4] = { 0, 0, 0, 0 };
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    glBindVertexArray(fsTriangleVao_);
    glBindFramebuffer(GL_FRAMEBUFFER, lutFbo_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    // Transmittance LUT pass.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, transmittanceLutTex_, 0);
    GLenum drawBuffersTrans[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBuffersTrans);
    glViewport(0, 0, kTransmittanceLutWidth, kTransmittanceLutHeight);
    glUseProgram(transmittanceProgram_);
    glUniform2f(glGetUniformLocation(transmittanceProgram_, "uOutputSize"),
        float(kTransmittanceLutWidth), float(kTransmittanceLutHeight));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Multi-scattering LUT pass.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, multiScattLutTex_, 0);
    GLenum drawBuffersMs[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBuffersMs);
    glViewport(0, 0, kMultiScattLutSize, kMultiScattLutSize);
    glUseProgram(multiScattProgram_);
    model_->SetProgramUniforms(multiScattProgram_, 3, 4, 5, 6);
    glActiveTexture(GL_TEXTURE0 + kTransmittanceLutUnit);
    glBindTexture(GL_TEXTURE_2D, transmittanceLutTex_);
    glUniform1i(glGetUniformLocation(multiScattProgram_, "transmittance_texture"), kTransmittanceLutUnit);
    glUniform2f(glGetUniformLocation(multiScattProgram_, "uOutputSize"), float(kMultiScattLutSize), float(kMultiScattLutSize));
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Sky-view LUT pass.
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, skyViewLutTex_, 0);
    GLenum drawBuffersSky[1] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, drawBuffersSky);
    glViewport(0, 0, kSkyViewLutWidth, kSkyViewLutHeight);
    glUseProgram(skyViewProgram_);
    model_->SetProgramUniforms(skyViewProgram_, 3, 4, 5, 6);
    glUniformMatrix4fv(glGetUniformLocation(skyViewProgram_, "uInvProj"), 1, GL_FALSE, &invProj[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(skyViewProgram_, "uInvView"), 1, GL_FALSE, &invView[0][0]);
    glUniform3fv(glGetUniformLocation(skyViewProgram_, "uCameraPos"), 1, &cameraPos[0]);
    glUniform3fv(glGetUniformLocation(skyViewProgram_, "uEarthCenter"), 1, &earthCenter_[0]);
    glUniform3fv(glGetUniformLocation(skyViewProgram_, "uSunDir"), 1, &sunDir[0]);
    glUniform1f(glGetUniformLocation(skyViewProgram_, "uLengthUnitInMeters"), lengthUnitInMeters_);
    glUniform2f(glGetUniformLocation(skyViewProgram_, "uOutputSize"), float(kSkyViewLutWidth), float(kSkyViewLutHeight));
    glActiveTexture(GL_TEXTURE0 + kTransmittanceLutUnit);
    glBindTexture(GL_TEXTURE_2D, transmittanceLutTex_);
    glUniform1i(glGetUniformLocation(skyViewProgram_, "transmittance_texture"), kTransmittanceLutUnit);
    glActiveTexture(GL_TEXTURE0 + kMultiScattLutUnit);
    glBindTexture(GL_TEXTURE_2D, multiScattLutTex_);
    glUniform1i(glGetUniformLocation(skyViewProgram_, "uMultiScattTexture"), kMultiScattLutUnit);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Camera-volume pass.
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, cameraScatteringVolumeTex_, 0);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, cameraTransmittanceVolumeTex_, 0);
    GLenum drawBuffersVol[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, drawBuffersVol);
    glViewport(0, 0, kCameraVolumeSize, kCameraVolumeSize);
    glUseProgram(cameraVolumeProgram_);
    model_->SetProgramUniforms(cameraVolumeProgram_, 3, 4, 5, 6);
    glUniformMatrix4fv(glGetUniformLocation(cameraVolumeProgram_, "uInvProj"), 1, GL_FALSE, &invProj[0][0]);
    glUniformMatrix4fv(glGetUniformLocation(cameraVolumeProgram_, "uInvView"), 1, GL_FALSE, &invView[0][0]);
    glUniform3fv(glGetUniformLocation(cameraVolumeProgram_, "uCameraPos"), 1, &cameraPos[0]);
    glUniform3fv(glGetUniformLocation(cameraVolumeProgram_, "uEarthCenter"), 1, &earthCenter_[0]);
    glUniform3fv(glGetUniformLocation(cameraVolumeProgram_, "uSunDir"), 1, &sunDir[0]);
    glUniform1f(glGetUniformLocation(cameraVolumeProgram_, "uLengthUnitInMeters"), lengthUnitInMeters_);
    glUniform2f(glGetUniformLocation(cameraVolumeProgram_, "uOutputSize"), float(kCameraVolumeSize), float(kCameraVolumeSize));
    glActiveTexture(GL_TEXTURE0 + kTransmittanceLutUnit);
    glBindTexture(GL_TEXTURE_2D, transmittanceLutTex_);
    glUniform1i(glGetUniformLocation(cameraVolumeProgram_, "transmittance_texture"), kTransmittanceLutUnit);
    glActiveTexture(GL_TEXTURE0 + kMultiScattLutUnit);
    glBindTexture(GL_TEXTURE_2D, multiScattLutTex_);
    glUniform1i(glGetUniformLocation(cameraVolumeProgram_, "uMultiScattTexture"), kMultiScattLutUnit);
    for (int slice = 0; slice < kCameraVolumeSize; ++slice) {
        glUniform1i(glGetUniformLocation(cameraVolumeProgram_, "uLayer"), slice);
        glUniform1i(glGetUniformLocation(cameraVolumeProgram_, "uSlice"), slice);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

void Sky::bindForSkyPass(
    GLuint program,
    GLuint transmittanceUnit,
    GLuint scatteringUnit,
    GLuint irradianceUnit,
    GLuint singleMieUnit) const {
    if (!initialized_ || backend_ != Backend::Advanced43 || !model_) {
        return;
    }
    model_->SetProgramUniforms(
        program, transmittanceUnit, scatteringUnit, irradianceUnit, singleMieUnit);
}

void Sky::bindAdvancedCompositeTextures(
    GLuint program,
    GLuint skyViewUnit,
    GLuint cameraScatteringUnit,
    GLuint cameraTransmittanceUnit) const {
    if (!initialized_ || backend_ != Backend::Advanced43) {
        return;
    }

    glActiveTexture(GL_TEXTURE0 + skyViewUnit);
    glBindTexture(GL_TEXTURE_2D, skyViewLutTex_);
    glUniform1i(glGetUniformLocation(program, "uSkyViewLut"), skyViewUnit);

    glActiveTexture(GL_TEXTURE0 + cameraScatteringUnit);
    glBindTexture(GL_TEXTURE_3D, cameraScatteringVolumeTex_);
    glUniform1i(glGetUniformLocation(program, "uCameraScatteringVolume"), cameraScatteringUnit);

    glActiveTexture(GL_TEXTURE0 + cameraTransmittanceUnit);
    glBindTexture(GL_TEXTURE_3D, cameraTransmittanceVolumeTex_);
    glUniform1i(glGetUniformLocation(program, "uCameraTransmittanceVolume"), cameraTransmittanceUnit);
}

GLuint Sky::atmosphereShader() const {
    if (!initialized_ || backend_ != Backend::Advanced43 || !model_) {
        return 0;
    }
    return model_->shader();
}

const char* Sky::fragmentShaderPath() const {
    return backend_ == Backend::Advanced43 ?
        "../../../../VoxelOps/shaders/sky_hillaire_luts.frag" :
        "../../../../VoxelOps/shaders/sky_simple.frag";
}
