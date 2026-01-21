#version 330 core

in vec2 TexCoord;
in vec3 Normal;
in vec3 VertexColor; // AO in .r, sunlight in .g

out vec4 FragColor;

uniform sampler2D texture1;

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

// --- Shadow control uniforms (added) ---
uniform float shadowDarkness;   // 0..1. 1 = no extra darkening, 0.6..0.85 recommended
uniform float shadowContrast;   // >= 1.0. 1.0 = linear, >1 emphasizes shadows (1.2..1.5)

void main() {

    vec4 tex = texture(texture1, TexCoord);
    vec3 albedo = tex.rgb;

    // AO and sunlight from vertex color
    float aoRaw = clamp(VertexColor.r, 0.0, 1.0);
    float sun = clamp(VertexColor.g, 0.0, 1.0); // baked sunlight/shadows

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

    // Diffuse: modulate with AO and sunlight
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuseTerm = albedo * lightColor * diffuseStrength * NdotL * sun;

    // --- Shadow factor computation and application ---
    // shadowMask: 0 = fully lit, 1 = fully shadowed (based on baked sun)
    float shadowMask = clamp(1.0 - sun, 0.0, 1.0);
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

    // Tone mapping + post-processing
    vec3 color = lit;
    color = color / (color + vec3(1.0));
    color = (color - 0.5) * contrast + 0.5;
    vec3 grey = vec3(dot(color, vec3(0.2126,0.7152,0.0722)));
    color = mix(grey, color, satBoost);
    color *= warmth;
    color = clamp(color, 0.0, 1.0);

    FragColor = vec4(color, tex.a);
}
