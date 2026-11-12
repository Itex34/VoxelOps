bool sampleVoxelSolid(ivec3 voxel) {
    if (voxel.y < giParams.shadowWorldBoundsXy.z) {
        return true;
    }
    if (voxel.y > giParams.shadowWorldBoundsXy.w) {
        return false;
    }
    if (voxel.x < giParams.shadowWorldBoundsXy.x || voxel.x > giParams.shadowWorldBoundsXy.y ||
        voxel.z < giParams.shadowWorldBoundsZ.x || voxel.z > giParams.shadowWorldBoundsZ.y) {
        return false;
    }

    ivec3 local = voxel - giParams.shadowOccupancyMinWordCount.xyz;
    // Unsigned conversion also rejects negative coordinates, so one vector
    // comparison covers both sides of the occupancy bounds.
    if (any(greaterThanEqual(uvec3(local), giParams.shadowOccupancyDims.xyz))) {
        return false;
    }

    uint linear = uint(local.x) +
        giParams.shadowOccupancyDims.x * (uint(local.y) + (giParams.shadowOccupancyDims.y * uint(local.z)));
    uint wordIndex = linear >> 5u;
    uint wordCount = uint(max(giParams.shadowOccupancyMinWordCount.w, 0));
    if (wordIndex >= wordCount) {
        return false;
    }
    // The CPU derives this mask from the same block definitions previously
    // used to build the occupancy buffer. Unknown IDs remain solid.
    uint materialWord = traceMaterials.ids[linear >> 2u];
    uint materialId = (materialWord >> ((linear & 3u) * 8u)) & 0xFFu;
    uint solidMask = uint(giParams.shadowWorldBoundsZ.z);
    return materialId >= 32u || ((solidMask >> (materialId & 31u)) & 1u) != 0u;
}

uint sampleVoxelMaterialId(ivec3 voxel) {
    ivec3 local = voxel - giParams.shadowOccupancyMinWordCount.xyz;
    if (any(greaterThanEqual(uvec3(local), giParams.shadowOccupancyDims.xyz))) {
        return 0u;
    }

    uint linear = uint(local.x) +
        giParams.shadowOccupancyDims.x * (uint(local.y) + (giParams.shadowOccupancyDims.y * uint(local.z)));
    uint word = traceMaterials.ids[linear >> 2u];
    return (word >> ((linear & 3u) * 8u)) & 0xFFu;
}
