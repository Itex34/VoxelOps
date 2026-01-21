#version 330 core

in vec2 TexCoord;
in vec3 Normal;         // optional: used only for hemisphere tint
in vec3 VertexColor;    // AO stored in .r

out vec4 FragColor;

uniform sampler2D texture1;

// hemisphere ambient colors (sky top, ground bottom)
uniform vec3 skyColorTop;      // e.g. vec3(0.60,0.75,0.92)
uniform vec3 skyColorBottom;   // e.g. vec3(0.95,0.90,0.80)
uniform float hemiTint;        // 0 = no hemi color, 1 = full hemi color (try 0.2..0.4)

uniform float ambientStrength; // base ambient multiplier
uniform float minAmbient;      // minimum ambient fraction (0..1) e.g. 0.22

// AO remap
uniform float aoPow;    // shape remap: >1 darkens mid AO, <1 brightens it. e.g. 1.2..1.6
uniform float aoMin;    // how dark fully occluded areas become (0..1). e.g. 0.35..0.5
uniform float aoApplyAfterTone; // 0 = apply before tone mapping, 1 = apply after

// post-processing / tuning
uniform float contrast;   // try 1.02..1.05
uniform float satBoost;   // try 1.04..1.10
uniform vec3 warmth;      // e.g. vec3(1.03,1.00,0.96)

void main()
{
    // sample base texture
    vec4 tex = texture(texture1, TexCoord);
    vec3 albedo = tex.rgb;

    // AO from vertex color (r)
    float aoRaw = clamp(VertexColor.r, 0.0, 1.0);
    // remap AO curve (use aoPow = 1.0 to skip pow cost)
    float aoRemap = pow(aoRaw, max(0.0001, aoPow));
    // final multiplicative AO: ao==0 -> aoMin, ao==1 -> 1.0
    float aoMul = mix(aoMin, 1.2, aoRemap);

    // hemisphere ambient tint (optional)
    vec3 ambientTint = vec3(1.0);
    if (hemiTint > 0.0) {
        float upness = clamp(normalize(Normal).y * 0.5 + 0.5, 0.0, 1.0);
        vec3 hemiColor = mix(skyColorBottom, skyColorTop, upness);
        ambientTint = mix(vec3(1.0), hemiColor, clamp(hemiTint, 0.0, 1.0));
    }

    // ambient factor (this keeps a min ambient even when AO is 0)
    float ambientFactor = ambientStrength * (minAmbient + (1.0 - minAmbient) * aoRaw);

    // ambient term: base texture tinted by hemisphere * ambient factor
    vec3 ambient = albedo * ambientTint * ambientFactor;

    // apply AO either before or after tone-mapping depending on aoApplyAfterTone
    vec3 color;
    if (aoApplyAfterTone < 0.5) {
        // apply AO before tone mapping (physically nicer)
        color = ambient * aoMul;

        // tone mapping + post processing
        color = color / (color + vec3(1.0));
        color = (color - 0.5) * contrast + 0.5;
        vec3 grey = vec3(dot(color, vec3(0.2126, 0.7152, 0.0722)));
        color = mix(grey, color, satBoost);
        color *= warmth;
    } else {
        // tone map first, AO after (AO more visible)
        color = ambient;
        color = color / (color + vec3(1.0));
        color = (color - 0.5) * contrast + 0.5;
        vec3 grey = vec3(dot(color, vec3(0.2126, 0.7152, 0.0722)));
        color = mix(grey, color, satBoost);
        color *= warmth;

        // now darken by AO
        color *= aoMul;
    }

    color = clamp(color, 0.0, 1.0);
    FragColor = vec4(color, tex.a);
    
    
}