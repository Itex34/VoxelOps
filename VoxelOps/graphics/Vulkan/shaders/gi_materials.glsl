uint sampleVoxelMaterialId(ivec3 voxel);

const vec3 kMaterialAlbedoLut[23] = vec3[](
    vec3(0.50),             // 0: Air/default
    vec3(0.34, 0.56, 0.26), // 1: Grass
    vec3(0.45, 0.33, 0.22), // 2: Dirt
    vec3(0.54, 0.54, 0.57), // 3: Stone
    vec3(0.20, 0.20, 0.22), // 4: Bedrock
    vec3(0.78, 0.72, 0.55), // 5: Sand
    vec3(0.46, 0.34, 0.21), // 6: Log
    vec3(0.62, 0.62, 0.64), // 7: StoneBrick
    vec3(0.72, 0.67, 0.58), // 8: TempleBrick
    vec3(0.55, 0.41, 0.27), // 9: Wood
    vec3(0.26, 0.46, 0.24), // 10: Leaves
    vec3(0.56, 0.50, 0.43), // 11: IronOre
    vec3(0.72, 0.72, 0.74), // 12: IronBlock
    vec3(0.30, 0.58, 0.38), // 13: EmeraldOre
    vec3(0.70, 0.22, 0.22), // 14: RedBerry
    vec3(0.78, 0.44, 0.20), // 15: OrangeBerry
    vec3(0.26, 0.56, 0.78), // 16: SapphireGem
    vec3(0.66, 0.24, 0.26), // 17: RubyGem
    vec3(0.58, 0.44, 0.30), // 18: CraftingTable
    vec3(0.35, 0.35, 0.35), // 19: Bomb
    vec3(0.32, 0.62, 0.29), // 20: Cactus
    vec3(0.52, 0.16, 0.18), // 21: RubyBlock
    vec3(0.22, 0.42, 0.62)  // 22: SapphireBlock
);

const vec3 kMaterialEmissionLut[23] = vec3[](
    vec3(0.0),              // 0: Air/default
    vec3(0.0),              // 1: Grass
    vec3(0.0),              // 2: Dirt
    vec3(0.0),              // 3: Stone
    vec3(0.0),              // 4: Bedrock
    vec3(0.0),              // 5: Sand
    vec3(0.0),              // 6: Log
    vec3(0.0),              // 7: StoneBrick
    vec3(0.0),              // 8: TempleBrick
    vec3(0.0),              // 9: Wood
    vec3(0.0),              // 10: Leaves
    vec3(0.0),              // 11: IronOre
    vec3(0.2, 100.0, 0.2),  // 12: IronBlock
    vec3(0.0),              // 13: EmeraldOre
    vec3(2.80, 0.25, 0.25), // 14: RedBerry
    vec3(2.30, 1.15, 0.22), // 15: OrangeBerry
    vec3(0.22, 0.95, 2.40), // 16: SapphireGem
    vec3(2.20, 0.30, 0.45), // 17: RubyGem
    vec3(0.0),              // 18: CraftingTable
    vec3(0.0),              // 19: Bomb
    vec3(0.0),              // 20: Cactus
    vec3(100.20, 0.10, 0.16), // 21: RubyBlock
    vec3(0.10, 0.38, 100.0)   // 22: SapphireBlock
);

vec3 materialAlbedo(uint materialId) {
    if (materialId < 23u) {
        return kMaterialAlbedoLut[int(materialId)];
    }
    return vec3(0.50);
}

vec3 materialEmission(uint materialId) {
    if (materialId < 23u) {
        return kMaterialEmissionLut[int(materialId)];
    }
    return vec3(0.0);
}

float nrdMaterialClassFromVoxelId(uint materialId) {
    // REBLUR material IDs are 2-bit classes (0..3).
    // Keep foliage and emissive materials separated to reduce cross-material bleeding.
    switch (materialId) {
    case 1u:  // Grass
    case 10u: // Leaves
    case 20u: // Cactus
        return 1.0;
    case 12u: // IronBlock
    case 14u: // RedBerry
    case 15u: // OrangeBerry
    case 16u: // SapphireGem
    case 17u: // RubyGem
    case 21u: // RubyBlock
    case 22u: // SapphireBlock
        return 2.0;
    default:
        return 0.0;
    }
}

vec3 sampleSurfaceEmission(vec3 worldPos, vec3 normal) {
    vec3 n = safeNormalize(normal);
    ivec3 insideVoxel = ivec3(floor(worldPos - (n * 0.03)));
    uint materialId = sampleVoxelMaterialId(insideVoxel);
    if (materialId == 0u) {
        ivec3 fallbackVoxel = ivec3(floor(worldPos + (n * 0.03)));
        materialId = sampleVoxelMaterialId(fallbackVoxel);
    }
    return materialEmission(materialId);
}
