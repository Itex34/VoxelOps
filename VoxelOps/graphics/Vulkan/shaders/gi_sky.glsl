vec3 skyRadiance(vec3 direction) {
    vec3 dir = safeNormalize(direction);
    float up = clamp((dir.y * 0.5) + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.22, 0.28, 0.34);
    vec3 zenith = vec3(0.52, 0.62, 0.82);
    vec3 sunDir = safeNormalize(giParams.sunDirection.xyz);
    float sunLobe = pow(clamp(dot(dir, sunDir), 0.0, 1.0), 96.0);
    vec3 base = mix(horizon, zenith, up);
    return base + vec3(1.0, 0.92, 0.76) * (0.40 * giParams.tuning.z * sunLobe);
}
