#version 330 core

in vec3 Position;
in vec2 TexCoordBlocks;
in vec3 Normal;
in vec3 VertexColor; // AO in .r, sunlight in .g
flat in int TileIndex;

layout(location = 0) out vec4 FragColor;
layout(location = 1) out float FragLinearDepthKm;

uniform sampler2DArray texture1;

// directional light (direction FROM fragment TOWARD light; normalized)
uniform vec3 lightDir;
uniform vec3 lightColor;       // e.g. vec3(1.0, 0.95, 0.9)

// hemisphere ambient colors (sky top, ground bottom)
uniform vec3 skyColorTop;      // e.g. vec3(0.60,0.75,0.92)
uniform vec3 skyColorBottom;   // e.g. vec3(0.95,0.90,0.80)

uniform float ambientStrength; // base ambient multiplier
uniform float diffuseStrength; // e.g. 1.0

// AO controls
uniform float minAmbient;      // minimum ambient fraction (0..1) e.g. 0.22

// new tuning uniforms
uniform float hemiTint;   // 0 = no hemi color, 1 = full hemi color (try 0.2..0.4)
uniform float contrast;   // try 1.02..1.05
uniform float satBoost;   // try 1.04..1.10
uniform vec3 warmth;      // small color multiply to warm scene, e.g. vec3(1.03,1.00,0.96)

uniform float aoPow;      // shape remap: >1 darkens mid AO, <1 brightens it. try 1.2..1.6
uniform float aoMin;      // how dark fully occluded areas become (0..1). try 0.35..0.5
uniform float aoApplyAfterTone; // 0 = apply before tone mapping (physically nicer), 1 = apply after (more visible)
uniform int uOutputHdrLinear;
uniform float uHdrTerrainExposureScale;
uniform float uAerialDepthScaleKm;
uniform vec3 cameraPos;
uniform sampler2DShadow uSunShadowTexNear;
uniform sampler2DShadow uSunShadowTexFar;
uniform sampler2D uSunShadowMomentsNear;
uniform mat4 uSunShadowViewProjNear;
uniform mat4 uSunShadowViewProjFar;
uniform vec2 uSunShadowTexelSizeNear;
uniform vec2 uSunShadowTexelSizeFar;
uniform float uSunShadowSplitDepthKm;
uniform float uSunShadowBlendKm;
uniform int uUseSunShadowMap;
uniform int uUseSunShadowMomentsNear;
uniform int uUseBakedSunChannel;
uniform vec3 uSunShadowDirectionalBias; // x=+Y, y=side, z=-Y
uniform float uSunShadowLowSunBiasBoost;

// --- Shadow control uniforms (added) ---
uniform float shadowDarkness;   // 0..1. 1 = no extra darkening, 0.6..0.85 recommended
uniform float shadowContrast;   // >= 1.0. 1.0 = linear, >1 emphasizes shadows (1.2..1.5)

float sampleSunShadowCompareNear(vec3 uvz)
{
    return texture(uSunShadowTexNear, uvz);
}

float sampleSunShadowCompareFar(vec3 uvz)
{
    return texture(uSunShadowTexFar, uvz);
}

float chebyshevUpperBound(float mean, float meanSq, float t)
{
    float variance = max(meanSq - mean * mean, 1e-6);
    float d = t - mean;
    return clamp(variance / (variance + d * d), 0.0, 1.0);
}

float reduceLightBleeding(float pMax, float amount)
{
    return clamp((pMax - amount) / max(1.0 - amount, 1e-4), 0.0, 1.0);
}

float sampleSunShadowNearEvsmVisibility(vec2 shadowUv, float compareDepth)
{
    const float kEvsmPosExponent = 4.2;
    const float kEvsmNegExponent = 4.2;
    const float kLightBleedReduction = 0.22;

    vec4 moments = texture(uSunShadowMomentsNear, shadowUv);
    float depthNdc = clamp(compareDepth * 2.0 - 1.0, -1.0, 1.0);
    float warpedPos = exp(kEvsmPosExponent * depthNdc);
    float warpedNeg = -exp(-kEvsmNegExponent * depthNdc);

    float pPos = (warpedPos <= moments.x) ? 1.0 : chebyshevUpperBound(moments.x, moments.y, warpedPos);
    float pNeg = (warpedNeg <= moments.z) ? 1.0 : chebyshevUpperBound(moments.z, moments.w, warpedNeg);

    float vis = min(pPos, pNeg);
    return reduceLightBleeding(vis, kLightBleedReduction);
}

float sampleSunShadowNearPcf12(vec2 uv, float compareDepth, vec2 texelSize)
{
    vec2 safeTexel = max(texelSize, vec2(1e-6));
    const vec2 poisson[12] = vec2[12](
        vec2(-0.326212, -0.405810),
        vec2(-0.840144, -0.073580),
        vec2(-0.695914,  0.457137),
        vec2(-0.203345,  0.620716),
        vec2( 0.962340, -0.194983),
        vec2( 0.473434, -0.480026),
        vec2( 0.519456,  0.767022),
        vec2( 0.185461, -0.893124),
        vec2( 0.507431,  0.064425),
        vec2( 0.896420,  0.412458),
        vec2(-0.321940, -0.932615),
        vec2(-0.791559, -0.597710)
    );

    float visibility = 0.0;
    for (int i = 0; i < 12; ++i) {
        vec2 sampleUv = uv + poisson[i] * safeTexel * 1.7;
        visibility += sampleSunShadowCompareNear(vec3(sampleUv, compareDepth));
    }
    return visibility * (1.0 / 12.0);
}

float sampleSunShadowFarPcf12(vec2 uv, float compareDepth, vec2 texelSize)
{
    vec2 safeTexel = max(texelSize, vec2(1e-6));
    const vec2 poisson[12] = vec2[12](
        vec2(-0.326212, -0.405810),
        vec2(-0.840144, -0.073580),
        vec2(-0.695914,  0.457137),
        vec2(-0.203345,  0.620716),
        vec2( 0.962340, -0.194983),
        vec2( 0.473434, -0.480026),
        vec2( 0.519456,  0.767022),
        vec2( 0.185461, -0.893124),
        vec2( 0.507431,  0.064425),
        vec2( 0.896420,  0.412458),
        vec2(-0.321940, -0.932615),
        vec2(-0.791559, -0.597710)
    );

    float visibility = 0.0;
    for (int i = 0; i < 12; ++i) {
        vec2 sampleUv = uv + poisson[i] * safeTexel * 2.1;
        visibility += sampleSunShadowCompareFar(vec3(sampleUv, compareDepth));
    }
    return visibility * (1.0 / 12.0);
}

float sampleSunShadowVisibilityFarPcf(
    vec3 worldPos,
    vec3 normalWs,
    vec3 lightDirWs,
    mat4 shadowViewProj,
    vec2 shadowTexelSize);

float sampleSunShadowVisibilityNearPcf(
    vec3 worldPos,
    vec3 normalWs,
    vec3 lightDirWs)
{
    vec3 n = normalize(normalWs);
    vec3 l = normalize(lightDirWs);
    float sunGrazing = 1.0 - clamp(abs(l.y), 0.0, 1.0);
    float lowSunBoost = 1.0 + sunGrazing * sunGrazing * clamp(uSunShadowLowSunBiasBoost, 0.0, 4.0);
    float dotNl = dot(n, l);
    float absNl = clamp(abs(dotNl), 0.0, 1.0);
    float upFacing = clamp(n.y, 0.0, 1.0);
    float downFacing = clamp(-n.y, 0.0, 1.0);
    float sideFacing = 1.0 - clamp(abs(n.y), 0.0, 1.0);
    vec3 dirBiasWeights = vec3(upFacing, sideFacing, downFacing);
    float directionalBias = max(dot(max(uSunShadowDirectionalBias, vec3(0.0)), dirBiasWeights), 0.0) *
        mix(1.0, lowSunBoost, sideFacing);
    float skyFacing = smoothstep(0.10, 0.95, clamp(n.y, 0.0, 1.0));
    float receiverNormalOffset =
        (0.00008 * mix(1.0, 0.45, absNl) + skyFacing * 0.00003) *
        mix(1.0, lowSunBoost, 0.72);
    vec3 receiverPos = worldPos + n * receiverNormalOffset;

    vec4 shadowClip = uSunShadowViewProjNear * vec4(receiverPos, 1.0);
    vec3 shadowNdc = shadowClip.xyz / max(shadowClip.w, 1e-6);
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    float shadowDepth = shadowNdc.z * 0.5 + 0.5;
    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 || shadowUv.y <= 0.0 || shadowUv.y >= 1.0 || shadowDepth <= 0.0 || shadowDepth >= 1.0) {
        // If the near cascade doesn't cover this fragment, use far cascade instead
        // so nearby shadows do not pop in abruptly at near-cascade bounds.
        return sampleSunShadowVisibilityFarPcf(
            worldPos,
            normalWs,
            lightDirWs,
            uSunShadowViewProjFar,
            uSunShadowTexelSizeFar);
    }

    float receiverSlope = max(abs(dFdx(shadowDepth)), abs(dFdy(shadowDepth)));
    float texelBias = 0.14 * max(uSunShadowTexelSizeNear.x, uSunShadowTexelSizeNear.y) * lowSunBoost;
    float slopeBias = mix(0.00013, 0.00007, absNl) * mix(1.0, lowSunBoost, 0.9);
    float normalBias = (1.0 - absNl) * 0.00007 * mix(1.0, lowSunBoost, 0.7);
    float skyBias = skyFacing * (0.45 * texelBias + 0.00002 + (1.0 - absNl) * 0.00003);
    float lowSunExtraBias = sunGrazing * sunGrazing * mix(0.00008, 0.00020, sideFacing);
    float maxBias = mix(0.00110, 0.00250, clamp((lowSunBoost - 1.0) / 4.0, 0.0, 1.0));
    float bias = min(
        slopeBias + normalBias + skyBias + directionalBias + lowSunExtraBias +
            receiverSlope * (0.022 * mix(1.0, lowSunBoost, 0.8)),
        maxBias);
    float compareDepth = shadowDepth - bias;

    if (uUseSunShadowMomentsNear != 0) {
        return sampleSunShadowNearEvsmVisibility(shadowUv, compareDepth);
    }
    return sampleSunShadowCompareNear(vec3(shadowUv, compareDepth));
}

float sampleSunShadowVisibilityFarPcf(
    vec3 worldPos,
    vec3 normalWs,
    vec3 lightDirWs,
    mat4 shadowViewProj,
    vec2 shadowTexelSize)
{
    vec3 n = normalize(normalWs);
    vec3 l = normalize(lightDirWs);
    float sunGrazing = 1.0 - clamp(abs(l.y), 0.0, 1.0);
    float lowSunBoost = 1.0 + sunGrazing * sunGrazing * clamp(uSunShadowLowSunBiasBoost, 0.0, 4.0);
    float dotNl = dot(n, l);
    float absNl = clamp(abs(dotNl), 0.0, 1.0);
    float upFacing = clamp(n.y, 0.0, 1.0);
    float downFacing = clamp(-n.y, 0.0, 1.0);
    float sideFacing = 1.0 - clamp(abs(n.y), 0.0, 1.0);
    vec3 dirBiasWeights = vec3(upFacing, sideFacing, downFacing);
    float directionalBias = max(dot(max(uSunShadowDirectionalBias, vec3(0.0)), dirBiasWeights), 0.0) *
        (1.35 * mix(1.0, lowSunBoost, sideFacing));
    float skyFacing = smoothstep(0.10, 0.95, clamp(n.y, 0.0, 1.0));
    float receiverNormalOffset =
        (0.0002 * mix(1.0, 0.35, absNl) + skyFacing * 0.00006) *
        mix(1.0, lowSunBoost, 0.8);
    vec3 receiverPos = worldPos + n * receiverNormalOffset;

    vec4 shadowClip = shadowViewProj * vec4(receiverPos, 1.0);
    vec3 shadowNdc = shadowClip.xyz / max(shadowClip.w, 1e-6);
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    float shadowDepth = shadowNdc.z * 0.5 + 0.5;
    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 || shadowUv.y <= 0.0 || shadowUv.y >= 1.0 || shadowDepth <= 0.0 || shadowDepth >= 1.0) {
        return 1.0;
    }

    // Receiver-plane + normal bias: reduce acne without introducing excessive peter-panning.
    float receiverSlope = max(abs(dFdx(shadowDepth)), abs(dFdy(shadowDepth)));
    float texelBias = 0.18 * max(shadowTexelSize.x, shadowTexelSize.y) * lowSunBoost;
    float slopeBias = mix(0.00015, 0.00008, absNl) * mix(1.0, lowSunBoost, 0.9);
    float normalBias = (1.0 - absNl) * 0.00009 * mix(1.0, lowSunBoost, 0.7);
    float skyBias = skyFacing * (0.55 * texelBias + 0.00003 + (1.0 - absNl) * 0.00005);
    float lowSunExtraBias = sunGrazing * sunGrazing * mix(0.00014, 0.00030, sideFacing);
    float maxBias = mix(0.00180, 0.00410, clamp((lowSunBoost - 1.0) / 4.0, 0.0, 1.0));
    float bias = min(
        slopeBias + normalBias + skyBias + directionalBias + lowSunExtraBias +
            receiverSlope * (0.026 * mix(1.0, lowSunBoost, 0.85)),
        maxBias);
    float compareDepth = shadowDepth - bias;

    return sampleSunShadowCompareFar(vec3(shadowUv, compareDepth));
}

float sampleSunShadowVisibility(vec3 worldPos, vec3 normalWs, vec3 lightDirWs, float depthKm)
{
    if (uUseSunShadowMap == 0) {
        return 1.0;
    }

    const float blendKm = max(uSunShadowBlendKm, 0.001);
    const float splitKm = max(uSunShadowSplitDepthKm, blendKm);
    const float blendStartKm = splitKm - blendKm;
    const float blendEndKm = splitKm + blendKm;

    float nearVis = sampleSunShadowVisibilityNearPcf(worldPos, normalWs, lightDirWs);
    if (depthKm <= blendStartKm) {
        return nearVis;
    }

    float farVis = sampleSunShadowVisibilityFarPcf(
        worldPos,
        normalWs,
        lightDirWs,
        uSunShadowViewProjFar,
        uSunShadowTexelSizeFar);
    if (depthKm >= blendEndKm) {
        return farVis;
    }

    float t = clamp((depthKm - blendStartKm) / max(blendEndKm - blendStartKm, 1e-4), 0.0, 1.0);
    return mix(nearVis, farVis, t);
}

void main() {

    vec2 uvLayer = fract(TexCoordBlocks);
    vec4 tex = texture(texture1, vec3(uvLayer, float(TileIndex)));
    vec3 albedo = tex.rgb;

    // AO and sunlight from vertex color
    float aoRaw = clamp(VertexColor.r, 0.0, 1.0);
    float sun = clamp(VertexColor.g, 0.0, 1.0); // baked sunlight/shadows
    float bakedSun = (uUseBakedSunChannel != 0) ? sun : 1.0;

    float ao = pow(aoRaw, aoPow);            // remap AO
    float aoMul = mix(aoMin, 1.0, ao);       // shape AO curve

    // Normal and light direction
    vec3 N = normalize(Normal);
    vec3 L = normalize(lightDir); // fragment -> light

    // Hemisphere ambient
    float upness = clamp(N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 hemiColor = mix(skyColorBottom, skyColorTop, upness);
    vec3 ambientTint = mix(vec3(1.0), hemiColor, clamp(hemiTint, 0.0, 1.0));

    // Ambient: AO affects ambient (original)
    float ambientFactor = ambientStrength * (minAmbient + (1.0 - minAmbient) * aoRaw);
    vec3 ambientTerm = albedo * ambientTint * ambientFactor;

    vec3 toCamera = cameraPos - Position;
    float depthKm = max(length(toCamera) * 0.001 * max(uAerialDepthScaleKm, 0.001), 0.0);
    float shadowDepthKm = depthKm;
    float sunShadowVis = sampleSunShadowVisibility(Position, N, L, shadowDepthKm);

    // Diffuse: modulate with AO, baked sunlight and dynamic shadow map.
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuseTerm = albedo * lightColor * diffuseStrength * NdotL * bakedSun * sunShadowVis;

    // --- Shadow factor computation and application ---
    // shadowMask: 0 = fully lit, 1 = fully shadowed (baked + dynamic)
    float shadowMask = clamp(max(1.0 - bakedSun, 1.0 - sunShadowVis), 0.0, 1.0);
    // emphasize penumbra/core using shadowContrast (>=1)
    shadowMask = pow(shadowMask, max(0.0001, shadowContrast));

    // final shadow factor: mix between no-change (1.0) and shadowDarkness (darker)
    float sd = clamp(shadowDarkness, 0.0, 1.0);
    float shadowFactor = mix(1.0, sd, shadowMask);

    // Keep shadows on all faces; only soften side-face aliasing instead of removing shading.
    float ndotlAbs = abs(dot(N, L));
    float ambientShadowWeight = mix(0.45, 1.0, smoothstep(0.04, 0.22, ndotlAbs));
    ambientTerm *= mix(1.0, shadowFactor, ambientShadowWeight);
    diffuseTerm *= mix(1.0, sd, shadowMask * 0.5);

    // Combine lighting (AO applied here as originally)
    vec3 lit = (ambientTerm + diffuseTerm) * aoMul;

    vec3 color = max(lit * max(uHdrTerrainExposureScale, 0.0), vec3(0.0));
    if (uOutputHdrLinear != 0) {
        color = max(color, vec3(0.0));
    }
    else {
        // Legacy LDR look for performance/potato path.
        color = color / (color + vec3(1.0));
        color = (color - 0.5) * contrast + 0.5;
        vec3 grey = vec3(dot(color, vec3(0.2126,0.7152,0.0722)));
        color = mix(grey, color, satBoost);
        color *= warmth;
        color = clamp(color, 0.0, 1.0);
    }
    
    FragLinearDepthKm = (uOutputHdrLinear != 0) ? depthKm : 0.0;
    FragColor = vec4(color, tex.a);
}



