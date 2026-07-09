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
    if (any(lessThan(local, ivec3(0))) ||
        local.x >= int(giParams.shadowOccupancyDims.x) ||
        local.y >= int(giParams.shadowOccupancyDims.y) ||
        local.z >= int(giParams.shadowOccupancyDims.z)) {
        return false;
    }

    uint linear = uint(local.x) +
        giParams.shadowOccupancyDims.x * (uint(local.y) + (giParams.shadowOccupancyDims.y * uint(local.z)));
    uint wordIndex = linear >> 5u;
    uint wordCount = uint(max(giParams.shadowOccupancyMinWordCount.w, 0));
    if (wordIndex >= wordCount) {
        return false;
    }
    uint mask = 1u << (linear & 31u);
    return (shadowOccupancy.words[wordIndex] & mask) != 0u;
}

uint sampleVoxelMaterialId(ivec3 voxel) {
    ivec3 local = voxel - giParams.shadowOccupancyMinWordCount.xyz;
    if (any(lessThan(local, ivec3(0))) ||
        local.x >= int(giParams.shadowOccupancyDims.x) ||
        local.y >= int(giParams.shadowOccupancyDims.y) ||
        local.z >= int(giParams.shadowOccupancyDims.z)) {
        return 0u;
    }

    uint linear = uint(local.x) +
        giParams.shadowOccupancyDims.x * (uint(local.y) + (giParams.shadowOccupancyDims.y * uint(local.z)));
    uint word = traceMaterials.ids[linear >> 2u];
    return (word >> ((linear & 3u) * 8u)) & 0xFFu;
}
