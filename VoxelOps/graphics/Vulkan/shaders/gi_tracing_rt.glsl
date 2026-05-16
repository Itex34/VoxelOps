float computeRtHitVoxelEpsilon(vec3 hitPos, float hitDistance) {
    float maxComp = max(abs(hitPos.x), max(abs(hitPos.y), abs(hitPos.z)));
    float epsFromPos = maxComp * 2.0e-7;
    float epsFromDistance = max(hitDistance, 1.0) * 2.0e-5;
    return clamp(max(1.0e-4, max(epsFromPos, epsFromDistance)), 1.0e-4, 5.0e-2);
}

ivec3 resolveRtHitVoxel(vec3 hitPos, vec3 rayDir, float hitDistance) {
    float kHitVoxelEpsilon = computeRtHitVoxelEpsilon(hitPos, hitDistance);

    ivec3 centerVoxel = ivec3(floor(hitPos));

    ivec3 backVoxel = ivec3(floor(hitPos - (rayDir * kHitVoxelEpsilon)));
    if (sampleVoxelSolid(backVoxel)) {
        return backVoxel;
    }

    if (sampleVoxelSolid(centerVoxel)) {
        return centerVoxel;
    }

    ivec3 frontVoxel = ivec3(floor(hitPos + (rayDir * kHitVoxelEpsilon)));
    if (sampleVoxelSolid(frontVoxel)) {
        return frontVoxel;
    }

    ivec3 axisFallback = centerVoxel;
    if (abs(rayDir.x) >= abs(rayDir.y) && abs(rayDir.x) >= abs(rayDir.z)) {
        axisFallback.x += (rayDir.x > 0.0) ? -1 : ((rayDir.x < 0.0) ? 1 : 0);
    } else if (abs(rayDir.y) >= abs(rayDir.x) && abs(rayDir.y) >= abs(rayDir.z)) {
        axisFallback.y += (rayDir.y > 0.0) ? -1 : ((rayDir.y < 0.0) ? 1 : 0);
    } else {
        axisFallback.z += (rayDir.z > 0.0) ? -1 : ((rayDir.z < 0.0) ? 1 : 0);
    }
    if (sampleVoxelSolid(axisFallback)) {
        return axisFallback;
    }

    return centerVoxel;
}

vec3 computeVoxelFaceNormalFromHit(ivec3 hitVoxel, vec3 hitPos, vec3 rayDir) {
    vec3 voxelCenter = vec3(hitVoxel) + vec3(0.5);
    vec3 local = hitPos - voxelCenter;

    // Snap to one of the 6 axis-aligned voxel face normals.
    vec3 absLocal = abs(local);
    vec3 n = vec3(0.0);
    if (absLocal.x >= absLocal.y && absLocal.x >= absLocal.z) {
        n.x = (local.x >= 0.0) ? 1.0 : -1.0;
    } else if (absLocal.y >= absLocal.x && absLocal.y >= absLocal.z) {
        n.y = (local.y >= 0.0) ? 1.0 : -1.0;
    } else {
        n.z = (local.z >= 0.0) ? 1.0 : -1.0;
    }

    vec3 dir = safeNormalize(rayDir);
    if (dot(n, dir) > 0.0) {
        n = -n;
    }
    return n;
}

bool traceSceneRtVisibilityNormalized(vec3 origin, vec3 dir, float maxDistance) {
    if (maxDistance <= 0.0) {
        return false;
    }

    if (dot(dir, dir) <= 1.0e-8) {
        return false;
    }

    rayQueryEXT query;
    rayQueryInitializeEXT(
        query,
        sceneTlas,
        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
        0xFFu,
        origin,
        0.0001,
        dir,
        maxDistance
    );

    while (rayQueryProceedEXT(query)) {
    }

    return rayQueryGetIntersectionTypeEXT(query, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

bool traceSceneRtVisibility(vec3 origin, vec3 direction, float maxDistance) {
    vec3 dir = safeNormalize(direction);
    return traceSceneRtVisibilityNormalized(origin, dir, maxDistance);
}

TraceResult traceSceneRt(vec3 origin, vec3 direction, float maxDistance) {
    TraceResult outResult;
    outResult.hit = false;
    outResult.distance = maxDistance;
    outResult.hitVoxel = ivec3(0);
    outResult.hitNormal = vec3(0.0, 1.0, 0.0);

    if (maxDistance <= 0.0) {
        outResult.distance = 0.0;
        return outResult;
    }

    vec3 dir = safeNormalize(direction);
    if (dot(dir, dir) <= 1.0e-8) {
        outResult.distance = 0.0;
        return outResult;
    }

    rayQueryEXT query;
    rayQueryInitializeEXT(
        query,
        sceneTlas,
        gl_RayFlagsOpaqueEXT,
        0xFFu,
        origin,
        0.0001,
        dir,
        maxDistance
    );

    while (rayQueryProceedEXT(query)) {
    }

    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT) {
        return outResult;
    }

    float t = rayQueryGetIntersectionTEXT(query, true);
    t = clamp(t, 0.0, maxDistance);
    vec3 hitPos = origin + (dir * t);
    ivec3 hitVoxel = resolveRtHitVoxel(hitPos, dir, t);

    outResult.hit = true;
    outResult.distance = t;
    outResult.hitVoxel = hitVoxel;
    outResult.hitNormal = computeVoxelFaceNormalFromHit(hitVoxel, hitPos, dir);
    return outResult;
}
