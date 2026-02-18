#version 330 core
out vec4 FragColor;
in vec2 vNDC;

uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec3 uSunDir;
uniform float uExposure;

void main() {
    vec4 clip = vec4(vNDC, -1.0, 1.0);
    vec4 view = uInvProj * clip;
    view /= view.w;
    vec3 viewDir = normalize((uInvView * vec4(view.xyz, 0.0)).xyz);
    vec3 sunDir = normalize(uSunDir);

    float up = clamp(viewDir.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.82, 0.90, 1.00);
    vec3 zenith = vec3(0.18, 0.43, 0.86);
    vec3 sky = mix(horizon, zenith, pow(up, 0.55));

    float horizonFog = exp(-max(viewDir.y, 0.0) * 5.5);
    sky += vec3(0.05, 0.06, 0.08) * horizonFog;

    float sunCos = dot(viewDir, sunDir);
    float sunDisk = smoothstep(0.99985, 0.99998, sunCos);
    float sunGlow = exp((sunCos - 1.0) * 130.0);
    vec3 sunColor = vec3(9.0, 8.7, 8.2) * sunDisk + vec3(2.0, 1.8, 1.5) * sunGlow;

    vec3 radiance = sky + sunColor;
    vec3 mapped = pow(vec3(1.0) - exp(-radiance * max(uExposure, 1e-6)), vec3(1.0 / 2.2));
    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
