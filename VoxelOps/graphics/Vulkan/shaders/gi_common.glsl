const float kPi = 3.14159265358979323846;

struct GiLightingSample {
    vec3 irradiance;
    float depthMean;
};

struct TraceResult {
    bool hit;
    float distance;
    ivec3 hitVoxel;
    vec3 hitNormal;
};

struct PathTraceResult {
    vec3 indirect;
    float candidateHitDistance;
};

vec3 hashColorFromUint(uint v) {
    uint x = v * 1664525u + 1013904223u;
    float r = float((x >> 0u) & 255u) / 255.0;
    float g = float((x >> 8u) & 255u) / 255.0;
    float b = float((x >> 16u) & 255u) / 255.0;
    return vec3(r, g, b);
}

vec3 safeNormalize(vec3 v) {
    float len2 = dot(v, v);
    if (len2 > 1.0e-8) {
        return v * inversesqrt(len2);
    }
    return vec3(0.0, 1.0, 0.0);
}

vec3 octDecode(vec2 e) {
    vec2 f = (e * 2.0) - 1.0;
    vec3 v = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (v.z < 0.0) {
        vec2 folded = (1.0 - abs(v.yx)) * sign(v.xy);
        v.x = folded.x;
        v.y = folded.y;
    }
    return safeNormalize(v);
}

vec2 octEncode(vec3 n) {
    vec3 v = safeNormalize(n);
    float invL1 = 1.0 / max(abs(v.x) + abs(v.y) + abs(v.z), 1.0e-6);
    v *= invL1;
    vec2 p = v.xy;
    if (v.z < 0.0) {
        p = (1.0 - abs(p.yx)) * sign(p.xy);
    }
    return (p * 0.5) + 0.5;
}

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 toneMapAces(vec3 linearColor) {
    vec3 x = max(linearColor, vec3(0.0));
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

GiLightingSample sampleGiLighting(vec3 worldPos, vec3 normal) {
    GiLightingSample outSample;
    outSample.irradiance = vec3(0.0);
    outSample.depthMean = 1.0;
    return outSample;
}
