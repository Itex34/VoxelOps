#version 460
#ifdef VOXELOPS_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif
layout(early_fragment_tests) in;

layout(set = 0, binding = 0) uniform sampler2D texSampler;

struct ProbeSample {
    vec4 irradianceDepthMean;
    vec4 depthMomentFrames;
};

struct GiCascadeParams {
    ivec4 originSpacingBlocks;
    uvec4 probeCounts;
};

layout(set = 2, binding = 0, std430) readonly buffer GiProbeCascade0 {
    ProbeSample probes0[];
};
layout(set = 2, binding = 1, std430) readonly buffer GiProbeCascade1 {
    ProbeSample probes1[];
};
layout(set = 2, binding = 2, std430) readonly buffer GiProbeCascade2 {
    ProbeSample probes2[];
};
layout(set = 2, binding = 3, std140) uniform GiLightingParams {
    uvec4 header;     // x = cascade count, y = sun shadow enabled, z = path trace enabled, w = NRD debug view
    uvec4 pathConfig; // x = path rays/pixel, y = restir history valid, z = frame index low, w = history reset
    uvec4 tracingConfig; // x = backend (0 = dda, 1 = rt), y = hw rt supported, z = tlas valid, w = NRD history valid
    vec4 tuning;  // x = base diffuse, y = gi intensity, z = sun intensity, w = sun shadow min visibility
    vec4 sunDirection;
    ivec4 shadowOccupancyMinWordCount; // xyz = min occupancy blocks, w = word count
    uvec4 shadowOccupancyDims;         // xyz = occupancy dims
    ivec4 shadowWorldBoundsXy;         // x = minX, y = maxX, z = minY, w = maxY
    ivec4 shadowWorldBoundsZ;          // x = minZ, y = maxZ
    vec4 shadowParams;                 // x = max trace distance, y = normal bias, z = max bounces, w = sky intensity
    vec4 restirParams;                 // x = temporal blend, y = spatial reuse weight, zw = inv viewport size
    vec4 denoiseParams;                // x = temporal blend, y = spatial weight, z = luma phi, w = moments blend
    mat4 currViewProjection;
    mat4 prevViewProjection;
    mat4 nrdPrevViewProjection;
    GiCascadeParams cascades[3];
} giParams;
layout(set = 2, binding = 4, std430) readonly buffer ShadowOccupancyWords {
    uint words[];
} shadowOccupancy;
layout(set = 2, binding = 5, std430) readonly buffer TraceMaterialIds {
    uint ids[];
} traceMaterials;
layout(set = 2, binding = 6) uniform sampler2D prevRestirDiSampler;
layout(set = 2, binding = 7, rgba16f) uniform writeonly image2D currRestirDiImage;
layout(set = 2, binding = 8) uniform sampler2D prevRestirValidationSampler;
layout(set = 2, binding = 9, rgba16f) uniform writeonly image2D currRestirValidationImage;
layout(set = 2, binding = 10) uniform sampler2D prevRestirMetaSampler;
layout(set = 2, binding = 11, rgba16f) uniform writeonly image2D currRestirMetaImage;
layout(set = 2, binding = 12) uniform sampler2D prevRestirGiSampler;
layout(set = 2, binding = 13, rgba16f) uniform writeonly image2D currRestirGiImage;
layout(set = 2, binding = 14) uniform sampler2D prevRestirGiMetaSampler;
layout(set = 2, binding = 15, rgba16f) uniform writeonly image2D currRestirGiMetaImage;
layout(set = 2, binding = 16, rgba16f) uniform writeonly image2D nrdInDiffRadianceHitDistImage;
layout(set = 2, binding = 17, rgba16f) uniform writeonly image2D nrdInNormalRoughnessImage;
layout(set = 2, binding = 18, rgba16f) uniform writeonly image2D nrdInMotionImage;
layout(set = 2, binding = 19, r32f) uniform writeonly image2D nrdInViewZImage;
layout(set = 2, binding = 20) uniform sampler2D nrdOutDiffRadianceHitDistSampler;
#ifdef VOXELOPS_RAY_QUERY
layout(set = 2, binding = 21) uniform accelerationStructureEXT sceneTlas;
#endif

layout(location = 0) in vec2 outUv;
layout(location = 1) in vec3 inWorldPos;
layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265358979323846;

struct GiProbeRead {
    vec3 irradiance;
    float depthMean;
    float directionality;
    vec3 dominantDir;
};

struct GiSample {
    vec3 irradiance;
    float depthMean;
    float directionality;
    vec3 dominantDir;
};

struct GiLightingSample {
    vec3 irradiance;
    float depthMean;
};

struct TraceResult {
    bool hit;
    float distance;
    ivec3 hitVoxel;
    vec3 hitNormal;
};

struct PathTraceResult {
    vec3 indirect;
    vec3 candidateRadiance;
    vec3 candidateDirection;
    float candidatePdf;
    float candidateHitDistance;
};

vec3 safeNormalize(vec3 v) {
    float len2 = dot(v, v);
    if (len2 > 1.0e-8) {
        return v * inversesqrt(len2);
    }
    return vec3(0.0, 1.0, 0.0);
}

vec3 octDecode(vec2 e) {
    vec2 f = (e * 2.0) - 1.0;
    vec3 v = vec3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (v.z < 0.0) {
        vec2 folded = (1.0 - abs(v.yx)) * sign(v.xy);
        v.x = folded.x;
        v.y = folded.y;
    }
    return safeNormalize(v);
}

vec2 octEncode(vec3 n) {
    vec3 v = safeNormalize(n);
    float invL1 = 1.0 / max(abs(v.x) + abs(v.y) + abs(v.z), 1.0e-6);
    v *= invL1;
    vec2 p = v.xy;
    if (v.z < 0.0) {
        p = (1.0 - abs(p.yx)) * sign(p.xy);
    }
    return (p * 0.5) + 0.5;
}

ProbeSample readProbe(uint cascadeIndex, uint linearIndex) {
    if (cascadeIndex == 0u) {
        return probes0[linearIndex];
    }
    if (cascadeIndex == 1u) {
        return probes1[linearIndex];
    }
    return probes2[linearIndex];
}

GiProbeRead unpackProbe(ProbeSample probe) {
    GiProbeRead outProbe;
    outProbe.irradiance = probe.irradianceDepthMean.rgb;
    outProbe.depthMean = clamp(probe.irradianceDepthMean.a, 0.0, 1.0);
    outProbe.directionality = clamp(probe.depthMomentFrames.x, 0.0, 1.0);
    outProbe.dominantDir = octDecode(probe.depthMomentFrames.zw);
    return outProbe;
}

GiSample sampleCascadeGi(uint cascadeIndex, vec3 worldPos) {
    GiCascadeParams c = giParams.cascades[cascadeIndex];
    uvec3 counts = c.probeCounts.xyz;
    if (counts.x == 0u || counts.y == 0u || counts.z == 0u) {
        GiSample empty;
        empty.irradiance = vec3(0.0);
        empty.depthMean = 1.0;
        empty.directionality = 0.0;
        empty.dominantDir = vec3(0.0, 1.0, 0.0);
        return empty;
    }

    float spacing = max(1.0, float(c.originSpacingBlocks.w));
    vec3 origin = vec3(c.originSpacingBlocks.xyz);
    vec3 probeCoord = ((worldPos - origin) / spacing) - vec3(0.5);
    ivec3 base = ivec3(floor(probeCoord));
    vec3 fracv = clamp(fract(probeCoord), 0.0, 1.0);
    ivec3 maxCoord = ivec3(counts) - ivec3(1);

    GiSample result;
    result.irradiance = vec3(0.0);
    result.depthMean = 0.0;
    result.directionality = 0.0;
    result.dominantDir = vec3(0.0);
    float dominantWeightAccum = 0.0;
    for (int oz = 0; oz <= 1; ++oz) {
        for (int oy = 0; oy <= 1; ++oy) {
            for (int ox = 0; ox <= 1; ++ox) {
                ivec3 coord = clamp(base + ivec3(ox, oy, oz), ivec3(0), maxCoord);
                uint linear = uint(coord.x) +
                    (counts.x * (uint(coord.y) + (counts.y * uint(coord.z))));
                float wx = (ox == 0) ? (1.0 - fracv.x) : fracv.x;
                float wy = (oy == 0) ? (1.0 - fracv.y) : fracv.y;
                float wz = (oz == 0) ? (1.0 - fracv.z) : fracv.z;
                float w = wx * wy * wz;
                GiProbeRead probe = unpackProbe(readProbe(cascadeIndex, linear));
                result.irradiance += probe.irradiance * w;
                result.depthMean += probe.depthMean * w;
                result.directionality += probe.directionality * w;
                float dirW = w * mix(0.20, 1.0, probe.directionality);
                result.dominantDir += probe.dominantDir * dirW;
                dominantWeightAccum += dirW;
            }
        }
    }

    if (dominantWeightAccum > 1.0e-5) {
        result.dominantDir = safeNormalize(result.dominantDir / dominantWeightAccum);
    } else {
        result.dominantDir = vec3(0.0, 1.0, 0.0);
    }
    result.directionality = clamp(result.directionality, 0.0, 1.0);
    return result;
}

GiLightingSample sampleGiLighting(vec3 worldPos, vec3 normal) {
    GiLightingSample outSample;
    uint cascadeCount = min(giParams.header.x, 3u);
    if (cascadeCount == 0u) {
        outSample.irradiance = vec3(0.0);
        outSample.depthMean = 1.0;
        return outSample;
    }

    vec3 giAccum = vec3(0.0);
    vec3 dominantAccum = vec3(0.0);
    float directionalityAccum = 0.0;
    float depthMeanAccum = 0.0;
    float weightAccum = 0.0;
    for (uint i = 0u; i < cascadeCount; ++i) {
        GiCascadeParams c = giParams.cascades[i];
        vec3 origin = vec3(c.originSpacingBlocks.xyz);
        float spacing = max(1.0, float(c.originSpacingBlocks.w));
        vec3 minPos = origin + vec3(0.5 * spacing);
        vec3 maxPos = origin + ((vec3(c.probeCounts.xyz) - vec3(0.5)) * spacing);
        vec3 border = min(worldPos - minPos, maxPos - worldPos);
        float edgeMin = min(border.x, min(border.y, border.z));
        bool inside = edgeMin >= 0.0;
        if (!inside) {
            continue;
        }
        float cascadeWeight = clamp(edgeMin / max(spacing, 1.0), 0.0, 1.0);
        cascadeWeight = mix(0.20, 1.0, cascadeWeight);
        float sampleOffset = min(0.35 * spacing, 1.5);
        GiSample giSample = sampleCascadeGi(i, worldPos + (safeNormalize(normal) * sampleOffset));
        giAccum += giSample.irradiance * cascadeWeight;
        dominantAccum += giSample.dominantDir * (giSample.directionality * cascadeWeight);
        directionalityAccum += giSample.directionality * cascadeWeight;
        depthMeanAccum += giSample.depthMean * cascadeWeight;
        weightAccum += cascadeWeight;
    }

    GiSample fallback = sampleCascadeGi(0u, worldPos + (safeNormalize(normal) * 0.6));
    vec3 gi = (weightAccum > 0.0) ? (giAccum / weightAccum) : fallback.irradiance;
    float directionality = (weightAccum > 0.0) ? clamp(directionalityAccum / weightAccum, 0.0, 1.0) : fallback.directionality;
    float depthMean = (weightAccum > 0.0) ? clamp(depthMeanAccum / weightAccum, 0.0, 1.0) : fallback.depthMean;
    vec3 dominantDir = fallback.dominantDir;
    if (length(dominantAccum) > 1.0e-5) {
        dominantDir = safeNormalize(dominantAccum);
    }

    vec3 n = safeNormalize(normal);
    float ndlGi = max(dot(n, dominantDir), 0.0);
    float directionalTerm = mix(0.65, 1.45, ndlGi);
    float directionalFactor = mix(1.0, directionalTerm, directionality);
    float openness = clamp((depthMean * 1.25) + 0.10, 0.45, 1.0);
    outSample.irradiance = gi * directionalFactor * mix(1.0, openness, 0.55);
    outSample.depthMean = depthMean;
    return outSample;
}

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
    return traceMaterials.ids[linear];
}

vec3 materialAlbedo(uint materialId) {
    switch (materialId) {
    case 1u: return vec3(0.34, 0.56, 0.26); // Grass
    case 2u: return vec3(0.45, 0.33, 0.22); // Dirt
    case 3u: return vec3(0.54, 0.54, 0.57); // Stone
    case 4u: return vec3(0.20, 0.20, 0.22); // Bedrock
    case 5u: return vec3(0.78, 0.72, 0.55); // Sand
    case 6u: return vec3(0.46, 0.34, 0.21); // Log
    case 7u: return vec3(0.62, 0.62, 0.64); // StoneBrick
    case 8u: return vec3(0.72, 0.67, 0.58); // TempleBrick
    case 9u: return vec3(0.55, 0.41, 0.27); // Wood
    case 10u: return vec3(0.26, 0.46, 0.24); // Leaves
    case 11u: return vec3(0.56, 0.50, 0.43); // IronOre
    case 12u: return vec3(0.72, 0.72, 0.74); // IronBlock
    case 13u: return vec3(0.30, 0.58, 0.38); // EmeraldOre
    case 14u: return vec3(0.70, 0.22, 0.22); // RedBerry
    case 15u: return vec3(0.78, 0.44, 0.20); // OrangeBerry
    case 16u: return vec3(0.26, 0.56, 0.78); // SapphireGem
    case 17u: return vec3(0.66, 0.24, 0.26); // RubyGem
    case 18u: return vec3(0.58, 0.44, 0.30); // CraftingTable
    case 19u: return vec3(0.35, 0.35, 0.35); // Bomb
    case 20u: return vec3(0.32, 0.62, 0.29); // Cactus
    case 21u: return vec3(0.52, 0.16, 0.18); // RubyBlock
    case 22u: return vec3(0.22, 0.42, 0.62); // SapphireBlock
    default: return vec3(0.50);
    }
}

vec3 materialEmission(uint materialId) {
    switch (materialId) {
    case 14u: return vec3(2.80, 0.25, 0.25); // RedBerry
    case 15u: return vec3(2.30, 1.15, 0.22); // OrangeBerry
    case 16u: return vec3(0.22, 0.95, 2.40); // SapphireGem
    case 17u: return vec3(2.20, 0.30, 0.45); // RubyGem
    case 21u: return vec3(1.20, 0.10, 0.16); // RubyBlock
    case 22u: return vec3(0.10, 0.38, 1.25); // SapphireBlock
    default: return vec3(0.0);
    }
}

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

TraceResult traceVoxelDda(vec3 origin, vec3 direction, float maxDistance) {
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

    ivec3 voxel = ivec3(floor(origin));
    if (sampleVoxelSolid(voxel)) {
        outResult.hit = true;
        outResult.distance = 0.0;
        outResult.hitVoxel = voxel;
        outResult.hitNormal = -dir;
        return outResult;
    }

    ivec3 step = ivec3(0);
    vec3 tMax = vec3(1.0e30);
    vec3 tDelta = vec3(1.0e30);

    for (int axis = 0; axis < 3; ++axis) {
        float comp = dir[axis];
        if (abs(comp) < 1.0e-8) {
            continue;
        }

        if (comp > 0.0) {
            step[axis] = 1;
            float nextBoundary = float(voxel[axis] + 1);
            tMax[axis] = (nextBoundary - origin[axis]) / comp;
        } else {
            step[axis] = -1;
            float nextBoundary = float(voxel[axis]);
            tMax[axis] = (nextBoundary - origin[axis]) / comp;
        }
        tDelta[axis] = abs(1.0 / comp);
    }

    float traveled = 0.0;
    for (int i = 0; i < 2048 && traveled <= maxDistance; ++i) {
        int axis = 0;
        if (tMax.y < tMax.x) {
            axis = 1;
        }
        if (tMax.z < tMax[axis]) {
            axis = 2;
        }

        traveled = tMax[axis];
        tMax[axis] += tDelta[axis];
        voxel[axis] += step[axis];

        if (sampleVoxelSolid(voxel)) {
            outResult.hit = true;
            outResult.distance = min(traveled, maxDistance);
            outResult.hitVoxel = voxel;
            outResult.hitNormal = vec3(0.0);
            outResult.hitNormal[axis] = float(-step[axis]);
            return outResult;
        }
    }

    outResult.hit = false;
    outResult.distance = maxDistance;
    return outResult;
}

vec3 estimateVoxelNormal(ivec3 voxel, vec3 fallbackDirection) {
    float gx =
        (sampleVoxelSolid(voxel + ivec3(1, 0, 0)) ? 1.0 : 0.0) -
        (sampleVoxelSolid(voxel - ivec3(1, 0, 0)) ? 1.0 : 0.0);
    float gy =
        (sampleVoxelSolid(voxel + ivec3(0, 1, 0)) ? 1.0 : 0.0) -
        (sampleVoxelSolid(voxel - ivec3(0, 1, 0)) ? 1.0 : 0.0);
    float gz =
        (sampleVoxelSolid(voxel + ivec3(0, 0, 1)) ? 1.0 : 0.0) -
        (sampleVoxelSolid(voxel - ivec3(0, 0, 1)) ? 1.0 : 0.0);
    vec3 grad = vec3(gx, gy, gz);
    if (dot(grad, grad) > 1.0e-6) {
        return safeNormalize(grad);
    }
    return -safeNormalize(fallbackDirection);
}

#ifdef VOXELOPS_RAY_QUERY
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
        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
        0xFFu,
        origin,
        0.001,
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
    ivec3 hitVoxel = ivec3(floor(hitPos - (dir * 0.001)));

    outResult.hit = true;
    outResult.distance = t;
    outResult.hitVoxel = hitVoxel;
    outResult.hitNormal = estimateVoxelNormal(hitVoxel, dir);
    return outResult;
}
#endif

TraceResult traceScene(vec3 origin, vec3 direction, float maxDistance) {
    if (giParams.tracingConfig.x == 1u &&
        giParams.tracingConfig.y == 1u &&
        giParams.tracingConfig.z == 1u) {
#ifdef VOXELOPS_RAY_QUERY
        return traceSceneRt(origin, direction, maxDistance);
#else
        return traceVoxelDda(origin, direction, maxDistance);
#endif
    }
    return traceVoxelDda(origin, direction, maxDistance);
}

uint hashU32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uint initSeed(vec3 worldPos, uint sampleIndex) {
    uvec3 bits = floatBitsToUint(worldPos * 0.125 + vec3(11.0, 23.0, 37.0));
    uint seed = bits.x ^ (bits.y * 1664525u) ^ (bits.z * 1013904223u);
    seed ^= uint(gl_FragCoord.x) * 374761393u;
    seed ^= uint(gl_FragCoord.y) * 668265263u;
    seed ^= sampleIndex * 2246822519u;
    seed ^= giParams.pathConfig.z * 3266489917u;
    return hashU32(seed + 0x9e3779b9u);
}

uint initStableSeed(vec3 worldPos) {
    uvec3 bits = floatBitsToUint(worldPos * 0.125 + vec3(11.0, 23.0, 37.0));
    uint seed = bits.x ^ (bits.y * 1664525u) ^ (bits.z * 1013904223u);
    seed ^= uint(gl_FragCoord.x) * 374761393u;
    seed ^= uint(gl_FragCoord.y) * 668265263u;
    return hashU32(seed + 0x9e3779b9u);
}

float randNext(inout uint state) {
    state = hashU32(state + 0x9e3779b9u);
    return float(state & 0x00ffffffu) * (1.0 / 16777216.0);
}

void buildBasis(vec3 n, out vec3 tangent, out vec3 bitangent) {
    vec3 up = (abs(n.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    tangent = safeNormalize(cross(up, n));
    bitangent = cross(n, tangent);
}

vec3 sampleCosineHemisphere(vec3 normal, inout uint rng) {
    float u1 = randNext(rng);
    float u2 = randNext(rng);
    float r = sqrt(max(u1, 0.0));
    float phi = 2.0 * kPi * u2;

    float x = r * cos(phi);
    float z = r * sin(phi);
    float y = sqrt(max(0.0, 1.0 - u1));

    vec3 tangent;
    vec3 bitangent;
    buildBasis(normal, tangent, bitangent);
    return safeNormalize(tangent * x + normal * y + bitangent * z);
}

float cosineHemispherePdf(vec3 normal, vec3 direction) {
    float ndl = max(dot(safeNormalize(normal), safeNormalize(direction)), 0.0);
    return ndl * (1.0 / kPi);
}

float traceSunVisibility(vec3 worldPos, vec3 normal, vec3 sunDir, float maxDistance) {
    vec3 n = safeNormalize(normal);
    float normalBias = max(0.02, giParams.shadowParams.y);
    vec3 origin = worldPos + (n * normalBias) + (sunDir * 0.05);
    TraceResult hit = traceScene(origin, sunDir, maxDistance);
    return hit.hit ? 0.0 : 1.0;
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

PathTraceResult tracePathTracedIndirect(vec3 worldPos, vec3 normal) {
    uint raysPerPixel = clamp(giParams.pathConfig.x, 1u, 8u);
    uint maxBounces = uint(clamp(giParams.shadowParams.z, 1.0, 4.0));
    float maxDistance = max(1.0, giParams.shadowParams.x);
    float skyIntensity = max(0.0, giParams.shadowParams.w);
    vec3 sunDir = safeNormalize(giParams.sunDirection.xyz);
    vec3 surfaceNormal = safeNormalize(normal);

    PathTraceResult outResult;
    outResult.indirect = vec3(0.0);
    outResult.candidateRadiance = vec3(0.0);
    outResult.candidateDirection = surfaceNormal;
    outResult.candidatePdf = 1.0 / kPi;
    outResult.candidateHitDistance = maxDistance;

    uint reservoirRng = initStableSeed(worldPos + (surfaceNormal * 7.13));
    float reservoirWeightSum = 0.0;
    float hitDistanceAccum = 0.0;

    vec3 radianceAccum = vec3(0.0);
    for (uint sampleIndex = 0u; sampleIndex < raysPerPixel; ++sampleIndex) {
        uint rng = initSeed(worldPos, sampleIndex);
        vec3 rayOrigin = worldPos + (surfaceNormal * 0.04);
        vec3 rayDir = sampleCosineHemisphere(surfaceNormal, rng);
        vec3 firstDirection = rayDir;
        float firstPdf = max(cosineHemispherePdf(surfaceNormal, rayDir), 1.0e-4);
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

            vec3 hitNormal = safeNormalize(hit.hitNormal);
            vec3 hitPos = rayOrigin + (rayDir * hit.distance);
            uint materialId = sampleVoxelMaterialId(hit.hitVoxel);
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

        sampleRadiance = min(sampleRadiance, vec3(24.0));
        hitDistanceAccum += firstHitDistance;
        radianceAccum += sampleRadiance;
        float sampleLuma = max(dot(sampleRadiance, vec3(0.2126, 0.7152, 0.0722)), 1.0e-4);
        float sampleWeight = sampleLuma / max(firstPdf, 1.0e-4);
        reservoirWeightSum += sampleWeight;
        if (sampleIndex == 0u || randNext(reservoirRng) < (sampleWeight / max(reservoirWeightSum, 1.0e-6))) {
            outResult.candidateRadiance = sampleRadiance;
            outResult.candidateDirection = firstDirection;
            outResult.candidatePdf = firstPdf;
            outResult.candidateHitDistance = firstHitDistance;
        }
    }

    outResult.indirect = radianceAccum / float(raysPerPixel);
    outResult.candidateRadiance = max(outResult.candidateRadiance, vec3(0.0));
    outResult.candidateDirection = safeNormalize(outResult.candidateDirection);
    outResult.candidatePdf = max(outResult.candidatePdf, 1.0e-4);
    outResult.candidateHitDistance = clamp(hitDistanceAccum / float(raysPerPixel), 0.0, maxDistance);
    return outResult;
}

vec3 evaluateReconnectedGiCandidate(vec3 worldPos, vec3 normal, vec3 reusedDirection) {
    uint maxBounces = uint(clamp(giParams.shadowParams.z, 1.0, 4.0));
    float maxDistance = max(1.0, giParams.shadowParams.x);
    float skyIntensity = max(0.0, giParams.shadowParams.w);
    vec3 sunDir = safeNormalize(giParams.sunDirection.xyz);
    vec3 surfaceNormal = safeNormalize(normal);
    vec3 rayDir = safeNormalize(reusedDirection);
    if (dot(rayDir, surfaceNormal) <= 0.01) {
        return vec3(0.0);
    }

    uint rng = initSeed(worldPos + (rayDir * 17.0) + (surfaceNormal * 3.0), giParams.pathConfig.z);
    vec3 rayOrigin = worldPos + (surfaceNormal * 0.04);
    vec3 throughput = vec3(1.0);
    vec3 sampleRadiance = vec3(0.0);

    for (uint bounce = 0u; bounce < maxBounces; ++bounce) {
        TraceResult hit = traceScene(rayOrigin, rayDir, maxDistance);
        if (!hit.hit) {
            sampleRadiance += throughput * skyRadiance(rayDir) * skyIntensity;
            break;
        }

        vec3 hitNormal = safeNormalize(hit.hitNormal);
        vec3 hitPos = rayOrigin + (rayDir * hit.distance);
        uint materialId = sampleVoxelMaterialId(hit.hitVoxel);
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

    return min(max(sampleRadiance, vec3(0.0)), vec3(24.0));
}

float luminance(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

vec3 toneMapAces(vec3 linearColor) {
    vec3 x = max(linearColor, vec3(0.0));
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 nrdLinearToYCoCg(vec3 color) {
    float y = dot(color, vec3(0.25, 0.5, 0.25));
    float co = dot(color, vec3(0.5, 0.0, -0.5));
    float cg = dot(color, vec3(-0.25, 0.5, -0.25));
    return vec3(y, co, cg);
}

vec3 nrdYCoCgToLinear(vec3 color) {
    float t = color.x - color.z;
    vec3 outColor;
    outColor.y = color.x + color.z;
    outColor.x = t + color.y;
    outColor.z = t - color.y;
    return max(outColor, vec3(0.0));
}

float getNrdViewZ() {
    return -1.0 / max(gl_FragCoord.w, 1.0e-6);
}

float depthRelativeDelta(float depthA, float depthB) {
    float denom = max(max(abs(depthA), abs(depthB)), 1.0);
    return abs(depthA - depthB) / denom;
}

bool projectWorldToUvRaw(mat4 viewProjection, vec3 worldPos, out vec2 uv, out float clipW) {
    vec4 clip = viewProjection * vec4(worldPos, 1.0);
    clipW = clip.w;
    if (abs(clip.w) <= 1.0e-5) {
        uv = vec2(0.0);
        return false;
    }

    uv = (clip.xy / clip.w) * 0.5 + 0.5;
    return true;
}

bool projectWorldToUv(mat4 viewProjection, vec3 worldPos, out vec2 uv) {
    float clipW = 0.0;
    if (!projectWorldToUvRaw(viewProjection, worldPos, uv, clipW)) {
        return false;
    }

    if (any(lessThan(uv, vec2(0.001))) || any(greaterThan(uv, vec2(0.999)))) {
        return false;
    }
    return true;
}

vec2 getCurrentSurfaceUv(vec3 worldPos) {
    return gl_FragCoord.xy * giParams.restirParams.zw;
}

vec3 computeNrdScreenMotion(vec3 worldPos, float viewZ) {
    vec2 currUv = gl_FragCoord.xy * giParams.restirParams.zw;
    vec2 prevUv = vec2(0.0);
    float prevClipW = 0.0;
    if (!projectWorldToUvRaw(giParams.nrdPrevViewProjection, worldPos, prevUv, prevClipW)) {
        return vec3(0.0);
    }

    vec2 uvDelta = prevUv - currUv;
    vec2 invExtent = max(giParams.restirParams.zw, vec2(1.0e-6));
    vec2 extent = 1.0 / invExtent;
    float prevViewZ = -prevClipW;
    return vec3(uvDelta * extent, prevViewZ - viewZ);
}

vec4 packNrdNormalRoughness(vec3 normal, float roughness) {
    vec3 n = safeNormalize(normal);
    float nAbs = max(1.0e-6, abs(n.x) + abs(n.y) + abs(n.z));
    n /= nAbs;

    vec3 encoded;
    encoded.y = n.y * 0.5 + 0.5;
    encoded.x = n.x * 0.5 + encoded.y;
    encoded.y -= n.x * 0.5;

    float linearRoughness = max(clamp(roughness, 0.0, 1.0), 1.5 / 512.0);
    float signedRoughness = (n.z < 0.0) ? -linearRoughness : linearRoughness;
    encoded.z = signedRoughness * 0.5 + 0.5;

    return vec4(clamp(encoded, 0.0, 1.0), 0.0);
}

float getNrdNormHitDistance(float hitDistance, float viewZ, float roughness) {
    float r = clamp(roughness, 0.0, 1.0);
    float smc = 1.0 - exp2(-200.0 * r * r);
    smc *= pow(max(r, 1.0e-6), 0.5);
    float f = (3.0 + abs(viewZ) * 0.1) * mix(20.0, 1.0, smc);
    return clamp(hitDistance / max(f, 1.0e-4), 0.0, 1.0);
}

void writeNrdInputs(
    vec3 worldPos,
    vec3 normal,
    vec3 diffuseRadianceIn,
    float hitDistance,
    float roughness,
    float viewZ,
    float maxDistance
) {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec3 encodedRadiance = nrdLinearToYCoCg(max(diffuseRadianceIn, vec3(0.0)));
    float safeViewZ = viewZ;
    if (abs(safeViewZ) < 1.0e-4) {
        safeViewZ = (safeViewZ < 0.0) ? -1.0e-4 : 1.0e-4;
    }
    vec3 motion = computeNrdScreenMotion(worldPos, safeViewZ);
    vec4 diffRadianceHitDistIn = vec4(
        encodedRadiance,
        getNrdNormHitDistance(hitDistance, viewZ, roughness)
    );
    imageStore(nrdInDiffRadianceHitDistImage, pixel, diffRadianceHitDistIn);
    imageStore(nrdInNormalRoughnessImage, pixel, packNrdNormalRoughness(normal, roughness));
    imageStore(nrdInMotionImage, pixel, vec4(motion, 0.0));
    imageStore(nrdInViewZImage, pixel, vec4(safeViewZ, 0.0, 0.0, 0.0));
}

float estimateNrdHitDistanceFromDepthMean(float depthMean, float maxDistance) {
    float depthNorm = clamp(depthMean, 0.0, 1.0);
    return mix(0.5, maxDistance, depthNorm);
}

vec3 visualizeNrdInput(
    uint debugView,
    vec3 diffuseRadiance,
    float hitDistance,
    vec3 normal,
    vec3 motion,
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
        float uvMotion = length(motion.xy * giParams.restirParams.zw);
        float m = clamp(uvMotion * 48.0, 0.0, 1.0);
        return vec3(m);
    }
    if (debugView == 5u) {
        return vec3(clamp(abs(viewZ) / max(maxDistance, 1.0e-4), 0.0, 1.0));
    }
    return vec3(0.0);
}

vec3 sampleNrdDiffuseHistory(vec2 uv) {
    vec4 packed = texture(nrdOutDiffRadianceHitDistSampler, uv);
    return nrdYCoCgToLinear(packed.rgb);
}

vec3 sampleRestirGiEstimate(vec2 uv) {
    vec4 reservoir = texture(prevRestirGiSampler, uv);
    float w = clamp(reservoir.a, 0.0, 4.0);
    return max(reservoir.rgb * w, vec3(0.0));
}

vec3 resolveRestirGiFinal(
    vec3 temporalResolvedIndirect,
    vec3 worldPos,
    vec3 currentNormal,
    float currentDepth,
    float currentAlbedoLuma,
    out float outMoment1,
    out float outMoment2
) {
    vec3 baseIndirect = max(temporalResolvedIndirect, vec3(0.0));
    float baseLuma = max(luminance(baseIndirect), 0.0);
    outMoment1 = baseLuma;
    outMoment2 = baseLuma * baseLuma;

    if (giParams.pathConfig.y == 0u || giParams.pathConfig.w != 0u) {
        return baseIndirect;
    }

    vec2 prevUv = vec2(0.0);
    if (!projectWorldToUv(giParams.prevViewProjection, worldPos, prevUv)) {
        return baseIndirect;
    }

    vec2 currUv = getCurrentSurfaceUv(worldPos);
    vec2 texel = giParams.restirParams.zw;
    vec3 n = safeNormalize(currentNormal);

    vec4 prevValidationCenter = texture(prevRestirValidationSampler, prevUv);
    vec3 prevNormalCenter = octDecode(prevValidationCenter.xy);
    float prevDepthCenter = prevValidationCenter.z;
    float prevAlbedoCenter = prevValidationCenter.w;

    float depthReject = smoothstep(0.010, 0.080, depthRelativeDelta(prevDepthCenter, currentDepth));
    float normalReject = 1.0 - smoothstep(0.72, 0.96, dot(prevNormalCenter, n));
    float albedoReject = smoothstep(0.05, 0.16, abs(prevAlbedoCenter - currentAlbedoLuma));
    float centerValidation = 1.0 - clamp(max(depthReject, max(normalReject, albedoReject)), 0.0, 1.0);

    vec4 prevMoments = texture(prevRestirMetaSampler, prevUv);
    float prevM1 = max(prevMoments.y, 0.0);
    float prevM2 = max(prevMoments.z, prevM1 * prevM1);
    float sigma = sqrt(max(prevM2 - (prevM1 * prevM1), 1.0e-5));
    float prevReservoirM = clamp(texture(prevRestirGiMetaSampler, prevUv).x, 1.0, 8.0);

    vec3 prevEstimate = sampleRestirGiEstimate(prevUv);
    float prevLuma = max(luminance(prevEstimate), 0.0);
    float lumaPhi = max(giParams.denoiseParams.z, 0.05);
    float clampRadius = (lumaPhi * sigma) + (0.02 + currentAlbedoLuma * 0.03);
    float clampedPrevLuma = clamp(prevLuma, max(baseLuma - clampRadius, 0.0), baseLuma + clampRadius);
    if (prevLuma > 1.0e-4) {
        prevEstimate *= clampedPrevLuma / prevLuma;
    }

    float motion = length(prevUv - currUv);
    float motionWeight = 1.0 - smoothstep(0.006, 0.060, motion);
    float temporalBlend = clamp(giParams.denoiseParams.x, 0.0, 0.98);
    float historyMWeight = clamp(0.20 + ((prevReservoirM - 1.0) * (0.80 / 7.0)), 0.20, 1.0);
    float darkTemporalBoost = 1.0 - smoothstep(0.03, 0.20, baseLuma);
    historyMWeight = max(historyMWeight, mix(0.20, 0.60, darkTemporalBoost));
    float temporalWeight = temporalBlend * centerValidation * motionWeight * historyMWeight;
    temporalWeight = clamp(temporalWeight * mix(1.0, 1.25, darkTemporalBoost), 0.0, 0.995);
    vec3 temporalIndirect = mix(baseIndirect, prevEstimate, temporalWeight);

    const vec2 offsets[4] = vec2[](
        vec2(1.0, 0.0),
        vec2(-1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(0.0, -1.0)
    );

    float spatialStrength = clamp(giParams.denoiseParams.y, 0.0, 0.35);
    vec3 spatialAccum = temporalIndirect;
    float spatialWeightSum = 1.0;
    float temporalLuma = max(luminance(temporalIndirect), 1.0e-4);
    for (int i = 0; i < 4; ++i) {
        vec2 sampleUv = prevUv + (offsets[i] * texel);
        if (any(lessThan(sampleUv, vec2(0.001))) || any(greaterThan(sampleUv, vec2(0.999)))) {
            continue;
        }

        vec4 neighborValidation = texture(prevRestirValidationSampler, sampleUv);
        vec3 neighborNormal = octDecode(neighborValidation.xy);
        float neighborDepth = neighborValidation.z;
        float neighborAlbedo = neighborValidation.w;
        vec3 neighborEstimate = sampleRestirGiEstimate(sampleUv);

        float depthDeltaNorm = depthRelativeDelta(neighborDepth, currentDepth);
        float wDepth = exp(-depthDeltaNorm * 24.0);
        float wNormal = pow(max(dot(neighborNormal, n), 0.0), 10.0);
        float wAlbedo = exp(-abs(neighborAlbedo - currentAlbedoLuma) * 12.0);
        float wLuma = exp(-abs(luminance(neighborEstimate) - temporalLuma) * 2.2);
        float w = spatialStrength * wDepth * wNormal * wAlbedo * wLuma;
        if (w <= 1.0e-5) {
            continue;
        }

        spatialAccum += neighborEstimate * w;
        spatialWeightSum += w;
    }

    vec3 filteredIndirect = max(spatialAccum / max(spatialWeightSum, 1.0e-4), vec3(0.0));
    float filteredLuma = max(luminance(filteredIndirect), 0.0);
    float momentBlend = clamp(giParams.denoiseParams.w, 0.0, 1.0);
    outMoment1 = mix(prevM1, filteredLuma, momentBlend);
    outMoment2 = mix(prevM2, filteredLuma * filteredLuma, momentBlend);
    outMoment2 = max(outMoment2, outMoment1 * outMoment1);
    return filteredIndirect;
}

vec4 applyRestirDiTemporal(
    vec3 currentDirect,
    vec3 worldPos,
    vec3 currentNormal,
    float currentDepth,
    float currentAlbedoLuma,
    out float outReservoirM,
    out float outResolvedDirect
) {
    const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float currentDirectLuma = max(dot(currentDirect, kLumaWeights), 1.0e-4);
    vec3 selectedSample = currentDirect;
    float weightSum = currentDirectLuma;
    float sampleCount = 1.0;
    float centerValidationWeight = 1.0;
    float maxDisocclusion = 0.0;
    uint rng = initSeed(worldPos, giParams.pathConfig.z ^ 0x1f123bb5u);

    if (giParams.pathConfig.y == 0u) {
        outReservoirM = 1.0;
        outResolvedDirect = max(currentDirectLuma, max(0.02, currentAlbedoLuma * 0.04));
        return vec4(currentDirect, 1.0);
    }

    vec2 prevUv = vec2(0.0);
    if (!projectWorldToUv(giParams.prevViewProjection, worldPos, prevUv)) {
        outReservoirM = 1.0;
        outResolvedDirect = max(currentDirectLuma, max(0.02, currentAlbedoLuma * 0.04));
        return vec4(currentDirect, 1.0);
    }

    vec2 currUv = getCurrentSurfaceUv(worldPos);
    float motion = length(prevUv - currUv);
    float motionWeight = exp(-motion * 96.0);
    float motionAttenuation = 1.0 - smoothstep(0.008, 0.070, motion);
    vec2 texel = giParams.restirParams.zw;

    const vec2 reuseOffsets[1] = vec2[](
        vec2(0.0, 0.0)
    );
    const float spatialKernel[1] = float[](
        1.0
    );

    for (int i = 0; i < 1; ++i) {
        vec2 sampleUv = prevUv + (reuseOffsets[i] * texel);
        if (any(lessThan(sampleUv, vec2(0.001))) || any(greaterThan(sampleUv, vec2(0.999)))) {
            continue;
        }

        vec4 previousReservoir = texture(prevRestirDiSampler, sampleUv);
        vec4 previousValidation = texture(prevRestirValidationSampler, sampleUv);
        vec4 previousMeta = texture(prevRestirMetaSampler, sampleUv);
        vec3 previousNormal = octDecode(previousValidation.xy);
        float previousDepth = previousValidation.z;
        float previousAlbedoLuma = previousValidation.w;
        float previousM = clamp(previousMeta.x, 1.0, 8.0);
        float previousW = clamp(previousReservoir.a, 0.0, 4.0);
        float normalDot = dot(previousNormal, safeNormalize(currentNormal));
        float albedoDelta = abs(previousAlbedoLuma - currentAlbedoLuma);

        float depthReject = smoothstep(0.010, 0.080, depthRelativeDelta(previousDepth, currentDepth));
        float normalReject = 1.0 - smoothstep(0.75, 0.95, normalDot);
        float albedoReject = smoothstep(0.04, 0.12, albedoDelta);
        float validationWeight = 1.0 - clamp(max(depthReject, max(normalReject, albedoReject)), 0.0, 1.0);
        if (i == 0) {
            centerValidationWeight = validationWeight;
            maxDisocclusion = 1.0 - validationWeight;
        }

        float candidateDirect = max(dot(previousReservoir.rgb, kLumaWeights), 1.0e-4);
        float lumaDenom = max(max(currentDirectLuma, candidateDirect), 0.14);
        float lumaDelta = abs(candidateDirect - currentDirectLuma) / lumaDenom;
        float reject = smoothstep(0.24, 0.92, lumaDelta);

        float reuseWeight = motionWeight * motionAttenuation * (1.0 - reject);
        reuseWeight *= spatialKernel[i];
        if (i > 0) {
            reuseWeight *= clamp(giParams.restirParams.y, 0.0, 0.50);
        } else {
            reuseWeight *= clamp(giParams.restirParams.x, 0.0, 0.85);
        }

        float sourceM = previousM * reuseWeight * validationWeight;
        if (sourceM <= 1.0e-4) {
            continue;
        }

        float candidateWeight = candidateDirect * previousW * previousM * reuseWeight * validationWeight;
        if (candidateWeight <= 1.0e-6) {
            continue;
        }

        weightSum += candidateWeight;
        sampleCount += sourceM;
        if (randNext(rng) < (candidateWeight / max(weightSum, 1.0e-6))) {
            selectedSample = previousReservoir.rgb;
        }
    }

    outReservoirM = clamp(sampleCount, 1.0, 8.0);
    float selectedLuma = max(dot(selectedSample, kLumaWeights), 1.0e-4);
    float reservoirW = weightSum / max(outReservoirM * selectedLuma, 1.0e-4);
    reservoirW = clamp(reservoirW, 0.0, 4.0);

    float reservoirDirectLuma = selectedLuma * reservoirW;
    float clampSpan = max(0.30, currentDirectLuma * 0.9 + 0.08);
    reservoirDirectLuma = clamp(
        reservoirDirectLuma,
        currentDirectLuma - clampSpan,
        currentDirectLuma + clampSpan
    );

    float normalizedM = clamp((outReservoirM - 1.0) * (1.0 / 7.0), 0.0, 1.0);
    float historyConfidence = normalizedM * motionAttenuation * centerValidationWeight;
    historyConfidence *= (1.0 - smoothstep(0.45, 0.95, maxDisocclusion));
    float directDarkBoost = 1.0 + (1.0 - smoothstep(0.03, 0.20, currentDirectLuma)) * 0.22;
    float directTemporalStrength = mix(0.55, 1.0, clamp(giParams.restirParams.x, 0.0, 1.0));
    historyConfidence = clamp(historyConfidence * directDarkBoost * directTemporalStrength, 0.0, 0.98);

    float resolvedLuma = mix(currentDirectLuma, reservoirDirectLuma, historyConfidence);
    float directFloor = max(0.02, currentAlbedoLuma * 0.04);
    outResolvedDirect = max(resolvedLuma, directFloor);

    return vec4(selectedSample, reservoirW);
}

vec4 applyRestirGiTemporal(
    vec3 currentIndirect,
    vec3 currentCandidateRadiance,
    vec3 currentCandidateDirection,
    float currentCandidatePdf,
    vec3 worldPos,
    vec3 currentNormal,
    float currentDepth,
    float currentAlbedoLuma,
    out float outReservoirM,
    out vec3 outResolvedIndirect,
    out vec3 outSelectedDirection,
    out float outSelectedPdf
) {
    const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float currentIndirectLuma = max(dot(currentIndirect, kLumaWeights), 1.0e-4);
    float darkAccumulationBoost = 1.0 - smoothstep(0.03, 0.20, currentIndirectLuma);
    vec3 currentDir = safeNormalize(currentCandidateDirection);
    float currentPdf = max(currentCandidatePdf, 1.0e-4);
    vec3 selectedSample = currentCandidateRadiance;
    vec3 selectedDirection = currentDir;
    float selectedPdf = currentPdf;
    float currentTarget = max(dot(currentCandidateRadiance, kLumaWeights), 1.0e-4);
    float weightSum = currentTarget / currentPdf;
    float sampleCount = 1.0;
    float centerValidationWeight = 1.0;
    float maxDisocclusion = 0.0;
    uint rng = initSeed(worldPos + (safeNormalize(currentNormal) * 0.173), giParams.pathConfig.z ^ 0xa511e9b3u);

    if (giParams.pathConfig.y == 0u) {
        outReservoirM = 1.0;
        outResolvedIndirect = currentIndirect;
        outSelectedDirection = currentDir;
        outSelectedPdf = currentPdf;
        return vec4(currentCandidateRadiance, 1.0);
    }

    vec2 prevUv = vec2(0.0);
    if (!projectWorldToUv(giParams.prevViewProjection, worldPos, prevUv)) {
        outReservoirM = 1.0;
        outResolvedIndirect = currentIndirect;
        outSelectedDirection = currentDir;
        outSelectedPdf = currentPdf;
        return vec4(currentCandidateRadiance, 1.0);
    }

    vec2 currUv = getCurrentSurfaceUv(worldPos);
    float motion = length(prevUv - currUv);
    float motionWeight = exp(-motion * 88.0);
    float motionAttenuation = 1.0 - smoothstep(0.006, 0.060, motion);
    vec2 texel = giParams.restirParams.zw;

    const vec2 reuseOffsets[1] = vec2[](
        vec2(0.0, 0.0)
    );
    const float spatialKernel[1] = float[](
        1.0
    );

    float centerReject = 1.0;
    for (int i = 0; i < 1; ++i) {
        vec2 sampleUv = prevUv + (reuseOffsets[i] * texel);
        if (any(lessThan(sampleUv, vec2(0.001))) || any(greaterThan(sampleUv, vec2(0.999)))) {
            continue;
        }

        vec4 previousReservoir = texture(prevRestirGiSampler, sampleUv);
        vec4 previousValidation = texture(prevRestirValidationSampler, sampleUv);
        vec4 previousMeta = texture(prevRestirGiMetaSampler, sampleUv);
        vec3 previousNormal = octDecode(previousValidation.xy);
        float previousDepth = previousValidation.z;
        float previousAlbedoLuma = previousValidation.w;
        float previousM = clamp(previousMeta.x, 1.0, 8.0);
        vec3 previousDirection = octDecode(previousMeta.yz);
        float previousPdf = clamp(previousMeta.w, 1.0e-4, 1.0);
        float previousW = clamp(previousReservoir.a, 0.0, 4.0);
        float normalDot = dot(previousNormal, safeNormalize(currentNormal));
        float albedoDelta = abs(previousAlbedoLuma - currentAlbedoLuma);

        float depthReject = smoothstep(0.010, 0.085, depthRelativeDelta(previousDepth, currentDepth));
        float normalReject = 1.0 - smoothstep(0.70, 0.95, normalDot);
        float albedoReject = smoothstep(0.05, 0.16, albedoDelta);
        float validationWeight = 1.0 - clamp(max(depthReject, max(normalReject, albedoReject)), 0.0, 1.0);
        if (i == 0) {
            centerValidationWeight = validationWeight;
            maxDisocclusion = 1.0 - validationWeight;
        }

        vec3 reconnectedRadiance = evaluateReconnectedGiCandidate(worldPos, currentNormal, previousDirection);
        vec3 reconClampSpan = max(vec3(0.12), (currentIndirect * 1.05) + vec3(currentAlbedoLuma * 0.12));
        reconnectedRadiance = clamp(
            reconnectedRadiance,
            max(currentIndirect - reconClampSpan, vec3(0.0)),
            currentIndirect + reconClampSpan
        );
        float candidateIndirect = max(dot(reconnectedRadiance, kLumaWeights), 1.0e-4);
        float lumaDenomFloor = mix(0.20, 0.65, darkAccumulationBoost);
        float lumaDenom = max(max(currentIndirectLuma, candidateIndirect), lumaDenomFloor);
        float lumaDelta = abs(candidateIndirect - currentIndirectLuma) / lumaDenom;
        float rejectLow = mix(0.32, 0.52, darkAccumulationBoost);
        float rejectHigh = mix(1.20, 1.85, darkAccumulationBoost);
        float reject = smoothstep(rejectLow, rejectHigh, lumaDelta);
        if (i == 0) {
            centerReject = reject;
        }

        float reuseWeight = motionWeight * motionAttenuation * (1.0 - reject) * validationWeight;
        reuseWeight *= spatialKernel[i];
        if (i > 0) {
            reuseWeight *= clamp(giParams.restirParams.y, 0.0, 0.60);
        } else {
            reuseWeight *= clamp(giParams.restirParams.x, 0.0, 0.92);
        }

        float sourceM = previousM * reuseWeight;
        if (sourceM > 1.0e-4) {
            float candidateWeight = (candidateIndirect / previousPdf) * previousW * sourceM;
            if (candidateWeight > 1.0e-6) {
                weightSum += candidateWeight;
                sampleCount += sourceM;
                if (randNext(rng) < (candidateWeight / max(weightSum, 1.0e-6))) {
                    selectedSample = reconnectedRadiance;
                    selectedDirection = previousDirection;
                    selectedPdf = previousPdf;
                }
            }
        }
    }

    outReservoirM = clamp(sampleCount, 1.0, 8.0);
    float selectedLuma = max(dot(selectedSample, kLumaWeights), 1.0e-4);
    float selectedImportance = selectedLuma / max(selectedPdf, 1.0e-4);
    float reservoirW = weightSum / max(outReservoirM * selectedImportance, 1.0e-4);
    reservoirW = clamp(reservoirW, 0.0, 4.0);

    vec3 reservoirIndirect = selectedSample * reservoirW;
    vec3 clampSpan = max(vec3(0.10), (currentIndirect * 1.15) + vec3(currentAlbedoLuma * 0.10));
    reservoirIndirect = clamp(reservoirIndirect, currentIndirect - clampSpan, currentIndirect + clampSpan);

    float normalizedM = clamp((outReservoirM - 1.0) * (1.0 / 7.0), 0.0, 1.0);
    float historyMFloor = mix(0.18, 0.55, darkAccumulationBoost);
    float historyMWeight = max(normalizedM, historyMFloor);
    float historyConfidence = historyMWeight * motionAttenuation * centerValidationWeight * (1.0 - centerReject);
    float disocclusionReject = smoothstep(0.45, 0.95, maxDisocclusion);
    float disocclusionScale = mix(1.0, 0.45, darkAccumulationBoost);
    historyConfidence *= (1.0 - (disocclusionReject * disocclusionScale));
    float giDarkBoost = 1.0 + (darkAccumulationBoost * 0.45);
    float giTemporalStrength = mix(0.60, 1.0, clamp(giParams.restirParams.x, 0.0, 1.0));
    historyConfidence = clamp(historyConfidence * giDarkBoost * giTemporalStrength, 0.0, 0.985);
    outResolvedIndirect = mix(currentIndirect, reservoirIndirect, historyConfidence);
    outResolvedIndirect = max(outResolvedIndirect, vec3(0.0));
    outSelectedDirection = safeNormalize(selectedDirection);
    outSelectedPdf = max(selectedPdf, 1.0e-4);

    return vec4(selectedSample, reservoirW);
}

void main() {
    vec4 texel = texture(texSampler, outUv);
    if (texel.a < 0.05) {
        discard;
    }

    vec3 geomNormal = safeNormalize(cross(dFdx(inWorldPos), dFdy(inWorldPos)));
    if (!gl_FrontFacing) {
        geomNormal = -geomNormal;
    }

    vec3 sunDir = safeNormalize(giParams.sunDirection.xyz);
    bool pathTracingEnabled = (giParams.header.z != 0u);
    uint nrdDebugView = giParams.header.w;
    float nrdMaxDistance = max(1.0, giParams.shadowParams.x);

    if (pathTracingEnabled) {
        PathTraceResult giTrace = tracePathTracedIndirect(inWorldPos, geomNormal);
        vec3 indirectCurrent = giTrace.indirect;
        float currentAlbedoLuma = luminance(texel.rgb);
        float reservoirM = 1.0;
        float resolvedDirect = 0.0;
        float giReservoirM = 1.0;
        vec3 selectedGiDirection = giTrace.candidateDirection;
        float selectedGiPdf = giTrace.candidatePdf;
        vec3 temporalResolvedIndirect = indirectCurrent;
        float validationDepth = getNrdViewZ();
        vec4 directReservoir = applyRestirDiTemporal(
            vec3(computePathTracedDirectSun(inWorldPos, geomNormal, sunDir)),
            inWorldPos,
            geomNormal,
            validationDepth,
            currentAlbedoLuma,
            reservoirM,
            resolvedDirect
        );
        vec4 giReservoir = applyRestirGiTemporal(
            indirectCurrent,
            giTrace.candidateRadiance,
            giTrace.candidateDirection,
            giTrace.candidatePdf,
            inWorldPos,
            geomNormal,
            validationDepth,
            currentAlbedoLuma,
            giReservoirM,
            temporalResolvedIndirect,
            selectedGiDirection,
            selectedGiPdf
        );
        float denoiseMoment1 = 0.0;
        float denoiseMoment2 = 0.0;
        vec3 finalResolvedIndirect = resolveRestirGiFinal(
            temporalResolvedIndirect,
            inWorldPos,
            geomNormal,
            validationDepth,
            currentAlbedoLuma,
            denoiseMoment1,
            denoiseMoment2
        );
        vec3 nrdResolvedIndirect = finalResolvedIndirect;
        if (giParams.tracingConfig.w != 0u) {
            vec2 prevUv = vec2(0.0);
            if (projectWorldToUv(giParams.nrdPrevViewProjection, inWorldPos, prevUv)) {
                nrdResolvedIndirect = sampleNrdDiffuseHistory(prevUv);
            }
        }
        float direct = resolvedDirect;
        vec3 litLinear = texel.rgb * (vec3(giParams.tuning.x + direct) + (nrdResolvedIndirect * giParams.tuning.y));
        vec3 lit = toneMapAces(litLinear);
        imageStore(
            currRestirValidationImage,
            ivec2(gl_FragCoord.xy),
            vec4(octEncode(geomNormal), validationDepth, currentAlbedoLuma)
        );
        imageStore(currRestirDiImage, ivec2(gl_FragCoord.xy), directReservoir);
        imageStore(currRestirMetaImage, ivec2(gl_FragCoord.xy), vec4(reservoirM, denoiseMoment1, denoiseMoment2, 0.0));
        imageStore(currRestirGiImage, ivec2(gl_FragCoord.xy), giReservoir);
        imageStore(
            currRestirGiMetaImage,
            ivec2(gl_FragCoord.xy),
            vec4(giReservoirM, octEncode(selectedGiDirection), selectedGiPdf)
        );
        vec3 nrdInputRadiance = max(indirectCurrent, vec3(0.0));
        float nrdInputHitDistance = giTrace.candidateHitDistance;
        writeNrdInputs(
            inWorldPos,
            geomNormal,
            nrdInputRadiance,
            nrdInputHitDistance,
            1.0,
            getNrdViewZ(),
            nrdMaxDistance
        );
        if (nrdDebugView != 0u) {
            vec3 nrdMotion = computeNrdScreenMotion(inWorldPos, getNrdViewZ());
            vec3 debugColor = visualizeNrdInput(
                nrdDebugView,
                nrdInputRadiance,
                nrdInputHitDistance,
                geomNormal,
                nrdMotion,
                getNrdViewZ(),
                nrdMaxDistance
            );
            outColor = vec4(debugColor, texel.a);
            return;
        }
        outColor = vec4(lit, texel.a);
        return;
    }

    GiLightingSample giSample = sampleGiLighting(inWorldPos, geomNormal);
    vec3 gi = giSample.irradiance;
    float giNrdHitDistance = estimateNrdHitDistanceFromDepthMean(giSample.depthMean, nrdMaxDistance);
    float ndl = max(dot(safeNormalize(geomNormal), sunDir), 0.0);
    float sunShadow = computeSunShadow(inWorldPos, geomNormal, sunDir);
    float direct = ndl * giParams.tuning.z * sunShadow;
    vec3 litLinear = texel.rgb * (vec3(giParams.tuning.x + direct) + (gi * giParams.tuning.y));
    vec3 lit = toneMapAces(litLinear);
    writeNrdInputs(
        inWorldPos,
        geomNormal,
        gi,
        giNrdHitDistance,
        1.0,
        getNrdViewZ(),
        nrdMaxDistance
    );
    if (nrdDebugView != 0u) {
        vec3 nrdMotion = computeNrdScreenMotion(inWorldPos, getNrdViewZ());
        vec3 debugColor = visualizeNrdInput(
            nrdDebugView,
            gi,
            giNrdHitDistance,
            geomNormal,
            nrdMotion,
            getNrdViewZ(),
            nrdMaxDistance
        );
        outColor = vec4(debugColor, texel.a);
        return;
    }
    outColor = vec4(lit, texel.a);
}
