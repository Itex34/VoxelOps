#version 330 core
out vec4 FragColor;
in vec2 vNDC;

uniform mat4 uInvProj;
uniform mat4 uInvView;
uniform vec3 uCameraPos;
uniform vec3 uEarthCenter;
uniform vec3 uSunDir;
uniform vec2 uSunSize;
uniform float uExposure;
uniform float uLengthUnitInMeters;

vec3 GetSolarLuminance();
vec3 GetSkyLuminance(
    vec3 camera,
    vec3 view_ray,
    float shadow_length,
    vec3 sun_direction,
    out vec3 transmittance);

void main() {
    vec4 clip = vec4(vNDC, -1.0, 1.0);
    vec4 view = uInvProj * clip;
    view /= view.w;
    vec3 viewDir = normalize((uInvView * vec4(view.xyz, 0.0)).xyz);
    vec3 sunDir = normalize(uSunDir);

    vec3 camera = uCameraPos / max(uLengthUnitInMeters, 1e-4) - uEarthCenter;
    vec3 transmittance;
    vec3 radiance = GetSkyLuminance(camera, viewDir, 0.0, sunDir, transmittance);

    if (dot(viewDir, sunDir) > uSunSize.y) {
        radiance += transmittance * GetSolarLuminance();
    }

    vec3 mapped = pow(vec3(1.0) - exp(-radiance * uExposure), vec3(1.0 / 2.2));
    FragColor = vec4(clamp(mapped, 0.0, 1.0), 1.0);
}
