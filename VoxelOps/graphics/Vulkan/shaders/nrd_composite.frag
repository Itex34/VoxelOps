#version 460

layout(set = 0, binding = 0, std140) uniform GiLightingParams {
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

layout(set = 0, binding = 7) uniform sampler2D nrdOutDiffRadianceHitDistSampler;
layout(set = 0, binding = 10) uniform sampler2D nrdComposeBaseSampler;
layout(set = 0, binding = 11) uniform sampler2D nrdComposeIndirectSampler;
layout(set = 0, binding = 3, rgba16f) uniform readonly image2D nrdInDiffRadianceHitDistImage;
layout(set = 0, binding = 4, rgb10_a2) uniform readonly image2D nrdInNormalRoughnessImage;
layout(set = 0, binding = 5, rgba16f) uniform readonly image2D nrdInMotionImage;
layout(set = 0, binding = 6, r32f) uniform readonly image2D nrdInViewZImage;

layout(location = 0) in vec2 outUv;
layout(location = 0) out vec4 outColor;

vec3 toneMapAces(vec3 linearColor) {
    vec3 x = max(linearColor, vec3(0.0));
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 nrdYCoCgToLinear(vec3 color) {
    float t = color.x - color.z;
    vec3 outLinear;
    outLinear.y = color.x + color.z;
    outLinear.x = t + color.y;
    outLinear.z = t - color.y;
    return max(outLinear, vec3(0.0));
}

vec3 falseColorLuma(float l) {
    float x = clamp(l, 0.0, 1.0);
    vec3 c0 = vec3(0.0, 0.0, 0.2);
    vec3 c1 = vec3(0.0, 0.6, 1.0);
    vec3 c2 = vec3(0.1, 1.0, 0.1);
    vec3 c3 = vec3(1.0, 0.9, 0.0);
    vec3 c4 = vec3(1.0, 0.0, 0.0);
    if (x < 0.25) {
        return mix(c0, c1, x * 4.0);
    }
    if (x < 0.5) {
        return mix(c1, c2, (x - 0.25) * 4.0);
    }
    if (x < 0.75) {
        return mix(c2, c3, (x - 0.5) * 4.0);
    }
    return mix(c3, c4, (x - 0.75) * 4.0);
}

vec3 hashColorFromFloat(float x) {
    float h = fract(sin(x * 43758.5453) * 12543.854);
    float h2 = fract(h * 19.19 + 0.31);
    float h3 = fract(h * 73.73 + 0.67);
    return vec3(h, h2, h3);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 composeBase = texelFetch(nrdComposeBaseSampler, pixel, 0);
    vec3 composeIndirectTint = texelFetch(nrdComposeIndirectSampler, pixel, 0).rgb;
    vec4 packedNrdOut = texelFetch(nrdOutDiffRadianceHitDistSampler, pixel, 0);
    vec3 nrdIndirect = nrdYCoCgToLinear(packedNrdOut.rgb);
    vec3 indirectTermLinear = composeIndirectTint * nrdIndirect;
    vec3 litLinear = composeBase.rgb + indirectTermLinear;

    uint debugView = giParams.header.w;
    if (debugView == 10u) {
        outColor = vec4(toneMapAces(nrdIndirect), 1.0);
        return;
    }
    if (debugView == 11u) {
        outColor = vec4(toneMapAces(composeBase.rgb), 1.0);
        return;
    }
    if (debugView == 12u) {
        outColor = vec4(clamp(composeIndirectTint, 0.0, 1.0), 1.0);
        return;
    }
    if (debugView == 13u) {
        outColor = vec4(toneMapAces(indirectTermLinear), 1.0);
        return;
    }
    if (debugView == 14u) {
        outColor = vec4(toneMapAces(litLinear), composeBase.a);
        return;
    }
    if (debugView == 15u) {
        outColor = vec4(vec3(clamp(packedNrdOut.a, 0.0, 1.0)), 1.0);
        return;
    }
    if (debugView == 16u) {
        float l = dot(nrdIndirect, vec3(0.2126, 0.7152, 0.0722));
        outColor = vec4(falseColorLuma(l / (l + 1.0)), 1.0);
        return;
    }
    if (debugView == 17u) {
        outColor = vec4(vec3(clamp(composeBase.a, 0.0, 1.0)), 1.0);
        return;
    }
    if (debugView == 18u) {
        vec3 diff = abs(indirectTermLinear - nrdIndirect);
        outColor = vec4(toneMapAces(diff * 4.0), 1.0);
        return;
    }
    if (debugView == 24u) {
        outColor = vec4(toneMapAces(composeBase.rgb), composeBase.a);
        return;
    }
    if (debugView == 25u) {
        outColor = vec4(toneMapAces(indirectTermLinear), 1.0);
        return;
    }
    if (debugView == 26u) {
        outColor = vec4(vec3(clamp(texelFetch(nrdComposeIndirectSampler, pixel, 0).a, 0.0, 1.0)), 1.0);
        return;
    }
    if (debugView == 27u) {
        float t = texelFetch(nrdComposeIndirectSampler, pixel, 0).a;
        outColor = vec4(hashColorFromFloat(t), 1.0);
        return;
    }
    if (debugView == 30u) {
        vec4 packedIn = imageLoad(nrdInDiffRadianceHitDistImage, pixel);
        outColor = vec4(toneMapAces(nrdYCoCgToLinear(packedIn.rgb)), 1.0);
        return;
    }
    if (debugView == 31u) {
        vec4 packedIn = imageLoad(nrdInDiffRadianceHitDistImage, pixel);
        outColor = vec4(vec3(clamp(packedIn.a, 0.0, 1.0)), 1.0);
        return;
    }
    if (debugView == 32u) {
        vec2 motion = imageLoad(nrdInMotionImage, pixel).xy;
        float m = clamp(length(motion) * 0.1, 0.0, 1.0);
        outColor = vec4(vec3(m), 1.0);
        return;
    }
    if (debugView == 33u) {
        float viewZ = imageLoad(nrdInViewZImage, pixel).x;
        float z = clamp(abs(viewZ) / max(giParams.shadowParams.x, 1.0), 0.0, 1.0);
        outColor = vec4(vec3(z), 1.0);
        return;
    }
    if (debugView == 34u) {
        vec4 packedN = imageLoad(nrdInNormalRoughnessImage, pixel);
        outColor = vec4(clamp(packedN.xyz, 0.0, 1.0), 1.0);
        return;
    }

    outColor = vec4(toneMapAces(litLinear), composeBase.a);
}
