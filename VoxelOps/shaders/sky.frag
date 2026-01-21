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

const float PI = 3.14159265359;
const vec3 lambda_um = vec3(0.68, 0.55, 0.44);

// helpers
vec3 spectralRayleigh() { return 1.0 / pow(lambda_um, vec3(4.0)); }

// Slightly stronger base Rayleigh (helps blue zenith)
vec3 betaRayleigh(float scale) {
    float base = 1.0e-5; // was 6.5e-6 -> bumped
    return spectralRayleigh() * (base * scale);
}

// Leave Mie base small but scaleable
vec3 betaMie(float turb, float scale) {
    float base = 8.0e-7 * clamp(turb, 1.0, 10.0) * scale; // slightly reduced base
    return vec3(base);
}

float phaseRayleigh(float cosTheta) {
    return (3.0 / (16.0 * PI)) * (1.0 + cosTheta*cosTheta);
}
float phaseMie(float cosTheta, float g) {
    float g2 = g*g;
    float denom = pow(1.0 + g2 - 2.0*g*cosTheta, 1.5);
    return (1.0 - g2) / (4.0 * PI * max(1e-6, denom));
}

// secant optical depth approx (flat ground uses viewDir.y as cosZen)
float opticDepth(float camH, float scaleH, float cosZen) {
    cosZen = max(cosZen, 0.01);
    return exp(-camH / scaleH) / cosZen;
}

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

    // sun
    vec3 sun = normalize(uSunDir);
    float cosVS = clamp(dot(viewDir, sun), -1.0, 1.0);
    float gammaAngle = acos(cosVS);

    // camera height above ground
    float camH = max(uCameraPos.y - uGroundY, 0.0);

    // in flat model horizon is at viewDir.y == 0
    float normalizedUp = clamp(viewDir.y, 0.0, 1.0); // 0 at horizon, 1 at zenith

    // scattering coefficients (attenuate a bit with height)
    vec3 Br = betaRayleigh(max(0.001, uRayleighScale)) * exp(-camH / 8000.0);
    vec3 Bm = betaMie(uTurbidity, max(0.0001, uMieScale)) * exp(-camH / 1200.0);

    // optical depth scalars using viewDir.y as cosZen (flat ground)
    float cosZen = max(viewDir.y, 0.01);
    float odR = 8000.0 * opticDepth(camH, 8000.0, cosZen);
    float odM = 1200.0 * opticDepth(camH, 1200.0, cosZen);

    vec3 Tr = exp(-Br * odR);
    vec3 Tm = exp(-Bm * odM);

    // single scattering (approx)
    float pr = phaseRayleigh(cosVS);
    float pm = phaseMie(cosVS, clamp(uMieG, 0.0, 0.99));
    vec3 tau_view = Br * odR + Bm * odM;
    vec3 invTau = 1.0 / max(tau_view, vec3(1e-6));

    // transmittance from sun approximated using sun.y as cosZen_sun
    float cosZenSun = max(sun.y, 0.01);
    vec3 Tr_sun = exp(-Br * (8000.0 * opticDepth(camH, 8000.0, cosZenSun)));
    vec3 Tm_sun = exp(-Bm * (1200.0 * opticDepth(camH, 1200.0, cosZenSun)));

    vec3 singleR = (Br * pr) * ((vec3(1.0) - exp(-tau_view)) * invTau) * (Tr_sun * Tm_sun) * uSunRadiance;
    vec3 singleM = (Bm * pm) * ((vec3(1.0) - exp(-tau_view)) * invTau) * (Tr_sun * Tm_sun) * uSunRadiance;
    vec3 singleScattered = singleR + singleM;

    // cheap multi-scatter/ambient fill (crucial for blue zenith)
    float zenFactor = pow(normalizedUp, 0.55); // slightly stronger zenith weighting (was 0.6)
    // Boost Rayleigh ambient for blue zenith; reduce Mie ambient which causes gray wash
    vec3 ambientRay = Br * (vec3(1.0) - Tr) * uSunRadiance * (1.15 + 1.05 * zenFactor); // was 0.9 + 0.6*
    vec3 ambientMie = Bm * (vec3(1.0) - Tm) * uSunRadiance * (0.12 + 0.45 * (1.0 - normalizedUp)); // was stronger
    vec3 ambient = ambientRay + ambientMie;

    // combine
    vec3 sky = singleScattered + ambient;

    // sun disk + halo (conservative)
    const float sunAngularRadius = 0.004675;
    float sunDisk = 1.0 - smoothstep(sunAngularRadius * 0.96, sunAngularRadius * 1.04, gammaAngle);
    float circumSigma = sunAngularRadius * (1.0 + 0.55 * clamp(uTurbidity - 1.0, 0.0, 5.0));
    float circum = exp(- (gammaAngle * gammaAngle) / (2.0 * circumSigma * circumSigma));
    float sunMult = 8.5; // stronger sun disk/halo (was 4.0)
    float sunAbove = step(0.0, sun.y); // sun visible if above flat horizon
    vec3 sunCol = vec3(1.0, 0.98, 0.92) * uSunRadiance * sunMult * (0.92*sunDisk + 0.35*circum) * sunAbove;
    sky += sunCol;

    // horizon warm tint (use normalizedUp so it blends at horizon)
    float horizonFactor = pow(clamp(1.0 - normalizedUp, 0.0, 1.0), 2.0); // slightly softer exponent
    vec3 horizonTint = vec3(1.0, 0.78, 0.48) * (0.12 + 0.88 * clamp((uTurbidity - 1.0) / 5.0, 0.0, 1.0));
    // stronger mix near horizon
    sky = mix(sky, horizonTint * uSunRadiance * 0.35, horizonFactor); // was 0.10 -> 0.35

    // debug modes (unchanged)
    if (uDebugMode == 1) {
        vec3 outc = singleR;
        vec3 mapped = filmic(outc * max(1e-4, uExposure));
        mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
        FragColor = vec4(mapped, 1.0); return;
    } else if (uDebugMode == 2) {
        vec3 outc = singleM;
        vec3 mapped = filmic(outc * max(1e-4, uExposure));
        mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
        FragColor = vec4(mapped, 1.0); return;
    } else if (uDebugMode == 3) {
        vec3 mapped = filmic(ambient * max(1e-4, uExposure));
        mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));
        FragColor = vec4(mapped, 1.0); return;
    }

    // minor saturation preserve (avoid extrapolation weirdness by clamping factor)
    float L = dot(sky, vec3(0.2126, 0.7152, 0.0722));
    // clamp lerp factor to [0,1] by using an explicit small boost instead of >1 mix
    sky = mix(vec3(L), sky, clamp(1.02, 0.0, 1.0)); // small saturation boost

    // clamp to safe range
    sky = clamp(sky, vec3(0.0), vec3(12.0)); // allow slightly larger range before filmic

    // tonemap + gamma
    vec3 mapped = filmic(sky * max(1e-4, uExposure));
    mapped = pow(clamp(mapped, 0.0, 1.0), vec3(1.0 / max(uGamma, 0.1)));

    // tiny final lift
    mapped = mix(mapped, mapped * vec3(1.01, 1.00, 0.995), 0.004);

    FragColor = vec4(mapped, 1.0);
}
