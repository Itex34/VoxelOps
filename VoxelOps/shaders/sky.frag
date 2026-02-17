#version 330 core
out vec4 FragColor;
in  vec2 vNDC;

// camera
uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec3 uCameraPos; // world-space camera pos (y = height)

// sky parameters (flat-ground model)
uniform vec3 uSunDir;        // normalized, pointing to sun
uniform float uSunRadiance;  // 0.6..2.0
uniform float uTurbidity;    // 1..6
uniform float uMieG;         // 0.70..0.92
uniform float uRayleighScale; // 0.6..3.0
uniform float uMieScale;      // 0.02..0.6

// tonemap / misc
uniform float uExposure;   // 0.25..1.0
uniform float uGamma;      // 2.2
uniform float uGroundY;    // y coordinate of ground plane (default 0.0)
uniform int uDebugMode;    // 0 = final, 1 = rayleigh-only, 2 = mie-only, 3 = ambient-only

// compact filmic (Uncharted2-ish)
vec3 filmic(vec3 x) {
    const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30, W = 11.2;
    vec3 curr = ((x*(A*x + C*B) + D*E) / (x*(A*x + B) + D*F)) - E/F;
    float whiteScale = (((W*(A*W + C*B) + D*E) / (W*(A*W + B) + D*F)) - E/F);
    return curr / whiteScale;
}

void main() {
    // reconstruct view direction
    vec4 clip = vec4(vNDC, -1.0, 1.0);
    vec4 view = uInvProj * clip; view /= view.w;
    vec3 viewDir = normalize((uInvView * vec4(view.xyz, 0.0)).xyz);

    // Sun and upness
    vec3 sun = normalize(uSunDir);
    float cosVS = clamp(dot(viewDir, sun), 0.0, 1.0);
    float up = clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0);

    // Bright summer palette
    vec3 zenith = vec3(0.18, 0.52, 0.98);
    vec3 midSky = vec3(0.40, 0.72, 1.00);
    vec3 horizon = vec3(0.82, 0.93, 1.00);
    vec3 groundBounce = vec3(0.98, 0.90, 0.72);

    float t1 = smoothstep(0.0, 0.45, up);
    float t2 = smoothstep(0.45, 1.0, up);
    vec3 gradLow = mix(horizon, midSky, t1);
    vec3 gradHigh = mix(midSky, zenith, t2);
    vec3 skyBase = mix(gradLow, gradHigh, t2);

    // Turbidity drives hazy horizon blend
    float haze = clamp((uTurbidity - 1.0) / 5.0, 0.0, 1.0);
    float horizonBand = pow(1.0 - up, 1.9);
    skyBase = mix(skyBase, mix(horizon, vec3(0.90, 0.95, 1.0), 0.65), horizonBand * (0.35 + 0.45 * haze));

    // Warm bounce near the lower hemisphere
    float belowHorizon = smoothstep(0.0, 0.35, 1.0 - up);
    skyBase = mix(skyBase, groundBounce, 0.09 * belowHorizon);

    // Sun disk + halo
    float disk = smoothstep(0.99980, 0.99993, cosVS);
    float haloNear = pow(cosVS, 96.0);
    float haloWide = pow(cosVS, 10.0);
    vec3 sunCore = vec3(1.00, 0.98, 0.90) * (0.90 * disk + 0.28 * haloNear);
    vec3 sunGlow = vec3(1.00, 0.92, 0.72) * (0.18 * haloWide);
    vec3 sunCol = (sunCore + sunGlow) * uSunRadiance;

    // Slight sun-direction brightening for clear midday look
    float sunForward = pow(cosVS, 4.0);
    vec3 directionalLift = vec3(0.10, 0.09, 0.07) * sunForward * uSunRadiance;

    vec3 sky = skyBase + sunCol + directionalLift;

    // debug modes
    if (uDebugMode == 1) {
        vec3 mapped = filmic(skyBase * max(1e-4, uExposure));
        mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
        FragColor = vec4(mapped, 1.0); return;
    } else if (uDebugMode == 2) {
        vec3 mapped = filmic(sunCol * max(1e-4, uExposure));
        mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
        FragColor = vec4(mapped, 1.0); return;
    } else if (uDebugMode == 3) {
        vec3 mapped = filmic(directionalLift * max(1e-4, uExposure));
        mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
        FragColor = vec4(mapped, 1.0); return;
    }

    // mild saturation boost
    float L = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    sky = mix(vec3(L), sky, 1.10);
    sky = clamp(sky, vec3(0.0), vec3(20.0));

    vec3 mapped = filmic(sky * max(1e-4, uExposure));
    mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
    mapped = clamp(mapped * vec3(1.03, 1.02, 1.00), 0.0, 1.0);

    FragColor = vec4(mapped, 1.0);
}
