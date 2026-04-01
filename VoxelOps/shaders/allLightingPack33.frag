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
uniform sampler2DShadow uSunShadowTex;
uniform mat4 uSunShadowViewProj;
uniform vec2 uSunShadowTexelSize;
uniform int uUseSunShadowMap;
uniform int uUseBakedSunChannel;

// --- Shadow control uniforms (added) ---
uniform float shadowDarkness;   // 0..1. 1 = no extra darkening, 0.6..0.85 recommended
uniform float shadowContrast;   // >= 1.0. 1.0 = linear, >1 emphasizes shadows (1.2..1.5)

float sampleSunShadowVisibility(vec3 worldPos, vec3 normalWs, vec3 lightDirWs)
{
    if (uUseSunShadowMap == 0) {
        return 1.0;
    }

    vec4 shadowClip = uSunShadowViewProj * vec4(worldPos, 1.0);
    vec3 shadowNdc = shadowClip.xyz / max(shadowClip.w, 1e-6);
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    float shadowDepth = shadowNdc.z * 0.5 + 0.5;
    if (shadowUv.x <= 0.0 || shadowUv.x >= 1.0 || shadowUv.y <= 0.0 || shadowUv.y >= 1.0 || shadowDepth <= 0.0 || shadowDepth >= 1.0) {
        return 1.0;
    }

    float nl = clamp(dot(normalize(normalWs), normalize(lightDirWs)), 0.0, 1.0);
    // Receiver-plane + normal bias: reduce acne without introducing excessive peter-panning.
    float receiverSlope = max(abs(dFdx(shadowDepth)), abs(dFdy(shadowDepth)));
    float texelBias = 0.5 * max(uSunShadowTexelSize.x, uSunShadowTexelSize.y);
    float slopeBias = mix(0.00045, 0.00010, nl);
    float normalBias = (1.0 - nl) * 0.00035;
    float bias = min(slopeBias + normalBias + receiverSlope * 1.0 + texelBias, 0.004);
    float compareDepth = shadowDepth - bias;

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

    float vis = 0.0;
    const float radiusTexels = mix(1.9, 1.2, nl);
    for (int i = 0; i < 12; ++i) {
        vec2 tapUv = shadowUv + poisson[i] * (uSunShadowTexelSize * radiusTexels);
        vis += texture(uSunShadowTex, vec3(tapUv, compareDepth));
    }
    return vis / 12.0;
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

    float sunShadowVis = sampleSunShadowVisibility(Position, N, L);

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

    // Apply shadow to ambient (primary) and a milder effect to diffuse for stronger silhouette
    ambientTerm *= shadowFactor;
    diffuseTerm *= mix(1.0, sd, shadowMask * 0.5);

    // Combine lighting (AO applied here as originally)
    vec3 lit = (ambientTerm + diffuseTerm) * aoMul;

    vec3 color = lit;
    if (uOutputHdrLinear != 0) {
        color = max(color * uHdrTerrainExposureScale, vec3(0.0));
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
    

    FragLinearDepthKm = (uOutputHdrLinear != 0) ? max(length(cameraPos - Position) * 0.001 * max(uAerialDepthScaleKm, 0.001), 0.0) : 0.0;
    FragColor = vec4(color, tex.a);
}



