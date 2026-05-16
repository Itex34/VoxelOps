#include "gi_sampling.glsl"
#include "gi_sky.glsl"

uint sampleHitMaterialIdFallback(ivec3 hitVoxel, vec3 hitPos, vec3 hitNormal, vec3 rayDir) {
    vec3 n = safeNormalize(hitNormal);
    ivec3 nStep = ivec3(
        (n.x > 0.25) ? 1 : ((n.x < -0.25) ? -1 : 0),
        (n.y > 0.25) ? 1 : ((n.y < -0.25) ? -1 : 0),
        (n.z > 0.25) ? 1 : ((n.z < -0.25) ? -1 : 0)
    );
    if (nStep != ivec3(0)) {
        uint behindFace = sampleVoxelMaterialId(hitVoxel - nStep);
        if (behindFace != 0u) {
            return behindFace;
        }
    }

    vec3 d = safeNormalize(rayDir);
    ivec3 dStep = ivec3(
        (d.x > 0.25) ? 1 : ((d.x < -0.25) ? -1 : 0),
        (d.y > 0.25) ? 1 : ((d.y < -0.25) ? -1 : 0),
        (d.z > 0.25) ? 1 : ((d.z < -0.25) ? -1 : 0)
    );
    if (dStep != ivec3(0)) {
        uint oppositeRay = sampleVoxelMaterialId(hitVoxel - dStep);
        if (oppositeRay != 0u) {
            return oppositeRay;
        }
    }

    // Final precision fallback: derive candidate from hit position around boundary.
    ivec3 centerVoxel = ivec3(floor(hitPos));
    if (centerVoxel != hitVoxel) {
        uint centerId = sampleVoxelMaterialId(centerVoxel);
        if (centerId != 0u) {
            return centerId;
        }
    }

    return 0u;
}

float traceSunVisibility(vec3 worldPos, vec3 normal, vec3 sunDir, float maxDistance) {
    vec3 n = safeNormalize(normal);
    float normalBias = max(0.02, giParams.shadowParams.y);
    vec3 origin = worldPos + (n * normalBias) + (sunDir * 0.05);
    bool blocked = traceSceneVisibility(origin, sunDir, maxDistance);
    return blocked ? 0.0 : 1.0;
}

float traceSunVisibilityNormalized(vec3 worldPos, vec3 normalWs, vec3 sunDir, float maxDistance) {
    float normalBias = max(0.02, giParams.shadowParams.y);
    vec3 origin = worldPos + (normalWs * normalBias) + (sunDir * 0.05);
    bool blocked = traceSceneVisibilityNormalized(origin, sunDir, maxDistance);
    return blocked ? 0.0 : 1.0;
}

float computeSunShadow(vec3 worldPos, vec3 normal, vec3 sunDir) {
    if (giParams.header.y == 0u) {
        return 1.0;
    }

    vec3 n = safeNormalize(normal);
    if (dot(n, sunDir) <= 0.0) {
        return 1.0;
    }

    float maxDistance = max(1.0, giParams.shadowParams.x);
    float minVisibility = clamp(giParams.tuning.w, 0.0, 1.0);
    float visibility = traceSunVisibility(worldPos, n, sunDir, maxDistance);
    return mix(minVisibility, 1.0, visibility);
}

float computeSunShadowNormalized(
    vec3 worldPos,
    vec3 normalWs,
    vec3 sunDir,
    float ndl,
    float maxDistance
) {
    if (giParams.header.y == 0u || ndl <= 0.0) {
        return 1.0;
    }

    float minVisibility = clamp(giParams.tuning.w, 0.0, 1.0);
    float visibility = traceSunVisibilityNormalized(worldPos, normalWs, sunDir, maxDistance);
    return mix(minVisibility, 1.0, visibility);
}

float computePathTracedDirectSun(vec3 worldPos, vec3 normal, vec3 sunDir) {
    vec3 n = safeNormalize(normal);
    float ndl = max(dot(n, sunDir), 0.0);
    if (ndl <= 0.0) {
        return 0.0;
    }

    float maxDistance = max(1.0, giParams.shadowParams.x);
    float visibility = traceSunVisibility(worldPos, n, sunDir, maxDistance);
    return ndl * giParams.tuning.z * visibility;
}

float computePathTracedDirectSunNormalized(
    vec3 worldPos,
    vec3 normalWs,
    vec3 sunDir,
    float ndl,
    float maxDistance
) {
    if (ndl <= 0.0) {
        return 0.0;
    }

    float visibility = traceSunVisibilityNormalized(worldPos, normalWs, sunDir, maxDistance);
    return ndl * giParams.tuning.z * visibility;
}




PathTraceResult tracePathTracedIndirect(vec3 worldPos, vec3 normal) {
    uint raysPerPixel = clamp(giParams.pathConfig.x, 1u, 8u);
    uint maxBounces = uint(clamp(giParams.shadowParams.z, 1.0, 4.0));
    float maxDistance = max(1.0, giParams.shadowParams.x);
    float skyIntensity = max(0.0, giParams.shadowParams.w);
    vec3 sunDir = safeNormalize(giParams.sunDirection.xyz);
    vec3 surfaceNormal = safeNormalize(normal);

    PathTraceResult outResult;
    outResult.indirect = vec3(0.0);
    outResult.candidateHitDistance = maxDistance;

    float hitDistanceAccum = 0.0;

    vec3 radianceAccum = vec3(0.0);
    for (uint sampleIndex = 0u; sampleIndex < raysPerPixel; ++sampleIndex) {
        uint rng = initSeed(worldPos, sampleIndex);
        vec3 rayOrigin = worldPos + (surfaceNormal * 0.04);
        vec3 rayDir = sampleCosineHemisphere(surfaceNormal, rng);
        float firstHitDistance = maxDistance;
        vec3 throughput = vec3(1.0);
        vec3 sampleRadiance = vec3(0.0);

        for (uint bounce = 0u; bounce < maxBounces; ++bounce) {
            TraceResult hit = traceScene(rayOrigin, rayDir, maxDistance);
            if (!hit.hit) {
                sampleRadiance += throughput * skyRadiance(rayDir) * skyIntensity;
                break;
            }
            if (bounce == 0u) {
                firstHitDistance = hit.distance;
            }

            vec3 hitPos = rayOrigin + (rayDir * hit.distance);
            uint materialId = sampleVoxelMaterialId(hit.hitVoxel);
            if (materialId == 0u) {
                materialId =
                    sampleHitMaterialIdFallback(hit.hitVoxel, hitPos, hit.hitNormal, rayDir);
            }
            if (materialId == 0u) {
                sampleRadiance += throughput * skyRadiance(rayDir) * skyIntensity;
                break;
            }
            vec3 hitNormal = safeNormalize(hit.hitNormal);
            vec3 albedo = materialAlbedo(materialId);
            vec3 emission = materialEmission(materialId);
            sampleRadiance += throughput * emission;

            float ndlSun = max(dot(hitNormal, sunDir), 0.0);
            if (ndlSun > 0.0) {
                float sunVisibility = traceSunVisibility(hitPos, hitNormal, sunDir, maxDistance);
                vec3 sunRadiance = vec3(1.0, 0.92, 0.76) * (giParams.tuning.z * ndlSun * sunVisibility);
                sampleRadiance += throughput * albedo * sunRadiance;
            }

            throughput *= albedo;
            throughput = min(throughput, vec3(4.0));
            if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01) {
                break;
            }

            if (bounce >= 1u) {
                float rr = max(throughput.r, max(throughput.g, throughput.b));
                float terminateProb = clamp(1.0 - rr, 0.0, 0.92);
                if (randNext(rng) < terminateProb) {
                    break;
                }
                throughput /= max(1.0 - terminateProb, 1.0e-3);
            }

            rayOrigin = hitPos + (hitNormal * 0.05);
            rayDir = sampleCosineHemisphere(hitNormal, rng);
        }

        hitDistanceAccum += firstHitDistance;
        radianceAccum += sampleRadiance;
    }

    outResult.indirect = radianceAccum / float(raysPerPixel);
    outResult.candidateHitDistance = clamp(hitDistanceAccum / float(raysPerPixel), 0.0, maxDistance);
    return outResult;
}

