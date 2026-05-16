vec3 visualizeNrdInput(
    uint debugView,
    vec3 diffuseRadiance,
    float hitDistance,
    vec3 normal,
    vec3 motion,
    float materialClass,
    float rawVoxelMaterialId,
    float viewZ,
    float maxDistance
) {
    if (debugView == 1u) {
        return toneMapAces(max(diffuseRadiance, vec3(0.0)));
    }
    if (debugView == 2u) {
        float d = clamp(hitDistance / max(maxDistance, 1.0e-4), 0.0, 1.0);
        return vec3(d);
    }
    if (debugView == 3u) {
        return (safeNormalize(normal) * 0.5) + 0.5;
    }
    if (debugView == 4u) {
        float uvMotion = length(motion.xy * giParams.screenParams.zw);
        float m = clamp(uvMotion * 48.0, 0.0, 1.0);
        return vec3(m);
    }
    if (debugView == 5u) {
        return vec3(clamp(abs(viewZ) / max(maxDistance, 1.0e-4), 0.0, 1.0));
    }
    if (debugView == 6u) {
        return clamp(diffuseRadiance / (diffuseRadiance + vec3(1.0)), 0.0, 1.0);
    }
    if (debugView == 7u) {
        float m = clamp(materialClass, 0.0, 1.0);
        return vec3(m);
    }
    if (debugView == 8u) {
        vec4 packed = packNrdNormalRoughness(normal, 1.0, materialClass);
        return clamp(packed.xyz, 0.0, 1.0);
    }
    if (debugView == 9u) {
        float normalized = clamp(rawVoxelMaterialId / 22.0, 0.0, 1.0);
        return vec3(normalized);
    }
    if (debugView == 19u) {
        return hashColorFromUint(inTileIndex);
    }
    if (debugView == 20u) {
        return clamp(sampleTerrainAlbedoSmooth().rgb, 0.0, 1.0);
    }
    if (debugView == 21u) {
        return clamp(sampleTerrainAlbedoGrad().rgb, 0.0, 1.0);
    }
    if (debugView == 22u) {
        return clamp(sampleTerrainAlbedoNearest().rgb, 0.0, 1.0);
    }
    if (debugView == 23u) {
        return vec3(fract(inTexCoordBlocks), 0.0);
    }
    if (debugView == 28u) {
        bool invalid = nrdIsInvalidVec3(diffuseRadiance) ||
                       nrdIsInvalid(hitDistance) ||
                       nrdIsInvalidVec3(normal) ||
                       nrdIsInvalidVec3(motion) ||
                       nrdIsInvalid(materialClass) ||
                       nrdIsInvalid(viewZ);
        return invalid ? vec3(1.0, 0.0, 1.0) : vec3(0.0);
    }
    if (debugView == 29u) {
        bool invalidSignal = nrdIsInvalidVec3(diffuseRadiance) || nrdIsInvalid(hitDistance);
        bool invalidGuideGeom = nrdIsInvalidVec3(normal) || nrdIsInvalid(viewZ);
        bool invalidGuideMotion = nrdIsInvalidVec3(motion) || nrdIsInvalid(materialClass);
        return vec3(
            invalidSignal ? 1.0 : 0.0,
            invalidGuideGeom ? 1.0 : 0.0,
            invalidGuideMotion ? 1.0 : 0.0
        );
    }
    return vec3(0.0);
}
