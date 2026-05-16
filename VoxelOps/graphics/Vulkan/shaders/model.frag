#version 460
layout(early_fragment_tests) in;

layout(set = 0, binding = 0) uniform sampler2D texSampler;

layout(set = 2, binding = 0, std140) uniform GiLightingParams {
    uvec4 header;
    uvec4 pathConfig;
    uvec4 tracingConfig;
    uvec4 nrdEncoding;
    vec4 tuning;
    vec4 sunDirection;
    ivec4 shadowOccupancyMinWordCount;
    uvec4 shadowOccupancyDims;
    ivec4 shadowWorldBoundsXy;
    ivec4 shadowWorldBoundsZ;
    vec4 shadowParams;
    vec4 screenParams;
    vec4 denoiseParams;
    vec4 nrdHitDistanceParams;
    mat4 currViewProjection;
    mat4 prevViewProjection;
    mat4 nrdPrevViewProjection;
} giParams;

layout(location = 0) in vec2 outUv;
layout(location = 1) in vec3 inWorldPos;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outComposeBase;
layout(location = 2) out vec4 outComposeIndirect;


vec3 safeNormalize(vec3 v) {
    float len2 = dot(v, v);
    if (len2 > 1.0e-8) {
        return v * inversesqrt(len2);
    }
    return vec3(0.0, 1.0, 0.0);
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


void main() {
    vec4 texel = texture(texSampler, outUv);
    if (texel.a < 0.05) {
        discard;
    }


    outComposeBase = texel;
    outComposeIndirect = vec4(0.0, 0.0, 0.0, 1.0);
    outColor = vec4(toneMapAces(texel.rgb), texel.a);
}
