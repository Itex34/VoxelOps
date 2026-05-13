#version 460

layout(set = 0, binding = 7) uniform sampler2D nrdOutDiffRadianceHitDistSampler;
layout(set = 0, binding = 10) uniform sampler2D nrdComposeBaseSampler;
layout(set = 0, binding = 11) uniform sampler2D nrdComposeIndirectSampler;

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

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 composeBase = texelFetch(nrdComposeBaseSampler, pixel, 0);
    vec3 composeIndirectTint = texelFetch(nrdComposeIndirectSampler, pixel, 0).rgb;
    vec4 packedNrdOut = texelFetch(nrdOutDiffRadianceHitDistSampler, pixel, 0);
    vec3 nrdIndirect = nrdYCoCgToLinear(packedNrdOut.rgb);
    vec3 litLinear = composeBase.rgb + (composeIndirectTint * nrdIndirect);
    outColor = vec4(toneMapAces(litLinear), composeBase.a);
}
