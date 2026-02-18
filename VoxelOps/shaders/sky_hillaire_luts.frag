#version 330 core
out vec4 FragColor;

uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform vec3 uEarthCenter;
uniform vec3 uSunDir;
uniform float uLengthUnitInMeters;

uniform bool uUseDepth;
uniform sampler2D uDepthTexture;
uniform sampler2D uSkyViewLut;
uniform sampler3D uCameraScatteringVolume;
uniform sampler3D uCameraTransmittanceVolume;

const float PI = 3.14159265358979323846;
const float AP_SLICE_COUNT = 32.0;
const float AP_KM_PER_SLICE = 4.0;

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

float fromUnitToSubUvs(float u, float resolution) {
    return (u + 0.5 / resolution) * (resolution / (resolution + 1.0));
}

void SkyViewLutParamsToUv(
    in bool intersectGround,
    in float viewZenithCosAngle,
    in float lightViewCosAngle,
    in float viewHeight,
    in float bottomRadius,
    out vec2 uv) {

    float Vhorizon = sqrt(max(0.0, viewHeight * viewHeight - bottomRadius * bottomRadius));
    float CosBeta = Vhorizon / max(viewHeight, 1e-4);
    float Beta = acos(clamp(CosBeta, -1.0, 1.0));
    float ZenithHorizonAngle = PI - Beta;

    if (!intersectGround) {
        float coord = acos(clamp(viewZenithCosAngle, -1.0, 1.0)) / max(ZenithHorizonAngle, 1e-4);
        coord = 1.0 - coord;
        coord = sqrt(clamp(coord, 0.0, 1.0));
        coord = 1.0 - coord;
        uv.y = coord * 0.5;
    } else {
        float coord = (acos(clamp(viewZenithCosAngle, -1.0, 1.0)) - ZenithHorizonAngle) / max(Beta, 1e-4);
        coord = sqrt(clamp(coord, 0.0, 1.0));
        uv.y = coord * 0.5 + 0.5;
    }

    float coord = -lightViewCosAngle * 0.5 + 0.5;
    coord = sqrt(clamp(coord, 0.0, 1.0));
    uv.x = coord;

    uv = vec2(fromUnitToSubUvs(uv.x, 192.0), fromUnitToSubUvs(uv.y, 108.0));
}

vec3 GetSunLuminance(vec3 worldPos, vec3 worldDir, float planetRadius, vec3 sunDir) {
    if (dot(worldDir, sunDir) > cos(0.5 * 0.505 * PI / 180.0)) {
        float t = raySphereIntersectNearest(worldPos, worldDir, vec3(0.0), planetRadius);
        if (t < 0.0) {
            return vec3(1000000.0);
        }
    }
    return vec3(0.0);
}

void main() {
    vec2 screenSize = vec2(textureSize(uDepthTexture, 0));
    vec2 uv = gl_FragCoord.xy / max(screenSize, vec2(1.0));

    vec2 ndc = uv * 2.0 - 1.0;
    vec4 clip = vec4(ndc, -1.0, 1.0);
    vec4 view = uInvProj * clip;
    view /= view.w;

    vec3 worldDir = normalize((uInvView * vec4(view.xyz, 0.0)).xyz);
    vec3 sunDir = normalize(uSunDir);

    float unitScale = max(uLengthUnitInMeters, 1e-4);
    vec3 camera = uCameraPos / unitScale - uEarthCenter;
    float bottomRadius = max(length(uEarthCenter), 1e-3);

    float depth = uUseDepth ? texture(uDepthTexture, uv).r : 1.0;

    vec3 luminance = vec3(0.0);
    float alpha = 1.0;

    if (!uUseDepth || depth >= 1.0) {
        float viewHeight = length(camera);
        vec3 up = camera / max(viewHeight, 1e-4);
        float viewZenithCosAngle = dot(worldDir, up);

        vec3 side = cross(up, worldDir);
        float sideLen = length(side);
        if (sideLen < 1e-5) {
            side = cross(up, vec3(0.0, 0.0, 1.0));
            sideLen = length(side);
            if (sideLen < 1e-5) side = vec3(1.0, 0.0, 0.0);
        }
        side = normalize(side);

        vec3 forward = normalize(cross(side, up));
        vec2 lightOnPlane = vec2(dot(sunDir, forward), dot(sunDir, side));
        float lightLen = length(lightOnPlane);
        if (lightLen > 1e-5) {
            lightOnPlane /= lightLen;
        } else {
            lightOnPlane = vec2(1.0, 0.0);
        }

        float lightViewCosAngle = lightOnPlane.x;
        bool intersectGround = raySphereIntersectNearest(camera, worldDir, vec3(0.0), bottomRadius) >= 0.0;

        vec2 skyUv;
        SkyViewLutParamsToUv(intersectGround, viewZenithCosAngle, lightViewCosAngle, viewHeight, bottomRadius, skyUv);
        luminance = texture(uSkyViewLut, skyUv).rgb + GetSunLuminance(camera, worldDir, bottomRadius, sunDir);
        alpha = 1.0;
    } else {
        vec3 depthNdc = vec3(ndc, depth * 2.0 - 1.0);
        vec4 depthWorld = uInvViewProj * vec4(depthNdc, 1.0);
        depthWorld /= depthWorld.w;

        float tDepth = length(depthWorld.xyz - uCameraPos) / unitScale;
        float slice = tDepth * (1.0 / AP_KM_PER_SLICE);
        float weight = 1.0;
        if (slice < 0.5) {
            weight = clamp(slice * 2.0, 0.0, 1.0);
            slice = 0.5;
        }

        float w = sqrt(clamp(slice / AP_SLICE_COUNT, 0.0, 1.0));
        vec4 ap = weight * texture(uCameraScatteringVolume, vec3(uv, w));
        luminance = ap.rgb;
        alpha = clamp(ap.a, 0.0, 1.0);
    }

    vec3 whitePoint = vec3(1.08241, 0.96756, 0.95003);
    float exposure = 10.0;
    vec3 mapped = pow(vec3(1.0) - exp(-luminance / whitePoint * exposure), vec3(1.0 / 2.2));
    FragColor = vec4(clamp(mapped, 0.0, 1.0), alpha);
}