#version 460
layout(early_fragment_tests) in;
#ifdef VOXELOPS_RAY_QUERY
#extension GL_EXT_ray_query : require
#endif

layout(set = 0, binding = 0) uniform sampler2DArray texSampler;

layout(set = 2, binding = 0, std140) uniform GiLightingParams {
    uvec4 header;     // x = reserved, y = sun shadow enabled, z = path trace enabled, w = NRD debug view
    uvec4 pathConfig; // x = path rays/pixel, y = reserved, z = frame index low, w = history reset
    uvec4 tracingConfig; // x = backend (0 = dda, 1 = rt), y = hw rt supported, z = tlas valid, w = NRD history valid
    uvec4 nrdEncoding; // x = normal encoding, y = roughness encoding (NRD enums)
    vec4 tuning;  // x = base diffuse, y = gi intensity, z = sun intensity, w = sun shadow min visibility
    vec4 sunDirection;
    ivec4 shadowOccupancyMinWordCount; // xyz = min occupancy blocks, w = word count
    uvec4 shadowOccupancyDims;         // xyz = occupancy dims
    ivec4 shadowWorldBoundsXy;         // x = minX, y = maxX, z = minY, w = maxY
    ivec4 shadowWorldBoundsZ;          // x = minZ, y = maxZ
    vec4 shadowParams;                 // x = max trace distance, y = normal bias, z = max bounces, w = sky intensity
    vec4 screenParams;                 // zw = inv viewport size
    vec4 denoiseParams;                // x = temporal blend, y = spatial weight, z = luma phi, w = moments blend
    vec4 nrdHitDistanceParams;         // xyz = ReblurHitDistanceParameters {A, B, C}
    mat4 currViewProjection;
    mat4 prevViewProjection;
    mat4 nrdPrevViewProjection;
} giParams;
layout(set = 2, binding = 1, std430) readonly buffer ShadowOccupancyWords {
    uint words[];
} shadowOccupancy;
layout(set = 2, binding = 2, std430) readonly buffer TraceMaterialIds {
    uint ids[];
} traceMaterials;
#ifdef VOXELOPS_RAY_QUERY
layout(set = 2, binding = 12) uniform accelerationStructureEXT sceneTlas;
#endif

layout(location = 0) in vec2 inTexCoordBlocks;
layout(location = 1) flat in uint inTileIndex;
layout(location = 2) in vec3 inWorldPos;
layout(location = 3) in vec3 inWorldNormal;
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outComposeBase;
layout(location = 2) out vec4 outComposeIndirect;
layout(location = 3) out vec4 outNrdDiffIn;
layout(location = 4) out vec4 outNrdNormalRoughnessIn;
layout(location = 5) out vec4 outNrdMotionIn;
layout(location = 6) out vec4 outNrdViewZIn;

const float kPi = 3.14159265358979323846;

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

vec4 sampleTerrainAlbedoSmooth() {
    vec2 uvLayer = fract(inTexCoordBlocks);
    return texture(texSampler, vec3(uvLayer, float(inTileIndex)));
}

vec4 sampleTerrainAlbedoGrad() {
    vec2 uvLayer = fract(inTexCoordBlocks);
    vec2 dx = dFdx(inTexCoordBlocks);
    vec2 dy = dFdy(inTexCoordBlocks);
    return textureGrad(texSampler, vec3(uvLayer, float(inTileIndex)), dx, dy);
}

vec4 sampleTerrainAlbedoNearest() {
    ivec3 dims = textureSize(texSampler, 0);
    vec2 uvLayer = fract(inTexCoordBlocks);
    ivec2 texel = ivec2(floor(uvLayer * vec2(dims.xy)));
    texel = ivec2(
        int(mod(float(texel.x), float(max(dims.x, 1)))),
        int(mod(float(texel.y), float(max(dims.y, 1))))
    );
    texel = clamp(texel, ivec2(0), max(ivec2(dims.xy) - ivec2(1), ivec2(0)));
    int layer = clamp(int(inTileIndex), 0, max(dims.z - 1, 0));
    return texelFetch(texSampler, ivec3(texel, layer), 0);
}

vec3 hashColorFromUint(uint v) {
    uint x = v * 1664525u + 1013904223u;
    float r = float((x >> 0u) & 255u) / 255.0;
    float g = float((x >> 8u) & 255u) / 255.0;
    float b = float((x >> 16u) & 255u) / 255.0;
    return vec3(r, g, b);
}

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

GiLightingSample sampleGiLighting(vec3 worldPos, vec3 normal) {
    worldPos = vec3(0.0);
    normal = vec3(0.0);
    GiLightingSample outSample;
    outSample.irradiance = vec3(0.0);
    outSample.depthMean = 1.0;
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
    case 12u: return vec3(0.2, 100.0, 0.2); // IronBlock
    case 14u: return vec3(2.80, 0.25, 0.25); // RedBerry
    case 15u: return vec3(2.30, 1.15, 0.22); // OrangeBerry
    case 16u: return vec3(0.22, 0.95, 2.40); // SapphireGem
    case 17u: return vec3(2.20, 0.30, 0.45); // RubyGem
    case 21u: return vec3(100.20, 0.10, 0.16); // RubyBlock
    case 22u: return vec3(0.10, 0.38, 100.0); // SapphireBlock
    default: return vec3(0.0);
    }
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

ivec3 resolveRtHitVoxel(vec3 hitPos, vec3 rayDir) {
    const float kHitVoxelEpsilon = 1.0e-5;

    ivec3 centerVoxel = ivec3(floor(hitPos));
    if (sampleVoxelSolid(centerVoxel)) {
        return centerVoxel;
    }

    ivec3 backVoxel = ivec3(floor(hitPos - (rayDir * kHitVoxelEpsilon)));
    if (sampleVoxelSolid(backVoxel)) {
        return backVoxel;
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
    ivec3 hitVoxel = resolveRtHitVoxel(hitPos, dir);

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
    return gl_FragCoord.xy * giParams.screenParams.zw;
}

vec3 computeNrdScreenMotion(vec3 worldPos, float viewZ) {
    vec2 currUv = gl_FragCoord.xy * giParams.screenParams.zw;
    vec2 prevUv = vec2(0.0);
    float prevClipW = 0.0;
    if (!projectWorldToUvRaw(giParams.nrdPrevViewProjection, worldPos, prevUv, prevClipW)) {
        return vec3(0.0);
    }

    vec2 uvDelta = prevUv - currUv;
    vec2 invExtent = max(giParams.screenParams.zw, vec2(1.0e-6));
    vec2 extent = 1.0 / invExtent;
    float prevViewZ = -prevClipW;
    return vec3(uvDelta * extent, prevViewZ - viewZ);
}

float packNrdRoughness(float roughness) {
    float r = clamp(roughness, 0.0, 1.0);
    uint roughnessEncoding = giParams.nrdEncoding.y;
    if (roughnessEncoding == 0u) {
        return r * r;
    }
    if (roughnessEncoding == 2u) {
        return sqrt(r);
    }
    return r;
}

bool nrdIsInvalid(float v) {
    return isnan(v) || isinf(v);
}

bool nrdIsInvalidVec3(vec3 v) {
    return any(isnan(v)) || any(isinf(v));
}

vec4 packNrdNormalRoughness(vec3 normal, float roughness, float materialClass) {
    float packedRoughness = packNrdRoughness(roughness);
    vec3 n = safeNormalize(normal);
    uint normalEncoding = giParams.nrdEncoding.x;
    if (normalEncoding == 2u) {
        float nAbs = max(1.0e-6, abs(n.x) + abs(n.y) + abs(n.z));
        n /= nAbs;

        vec3 encoded;
        encoded.y = n.y * 0.5 + 0.5;
        encoded.x = n.x * 0.5 + encoded.y;
        encoded.y -= n.x * 0.5;

        float signedRoughness = (n.z < 0.0) ? -max(packedRoughness, 1.5 / 512.0)
                                            : max(packedRoughness, 1.5 / 512.0);
        encoded.z = signedRoughness * 0.5 + 0.5;
        float packedMaterial = clamp(materialClass / 3.0, 0.0, 1.0);
        return vec4(clamp(encoded, 0.0, 1.0), packedMaterial);
    }

    float scale = max(abs(n.x), max(abs(n.y), abs(n.z)));
    if (scale > 1.0e-6) {
        n /= scale;
    }
    if (normalEncoding == 0u || normalEncoding == 3u) {
        n = n * 0.5 + 0.5;
    }
    return vec4(n, packedRoughness);
}

float getNrdNormHitDistance(float hitDistance, float viewZ, float roughness) {
    float r = clamp(roughness, 0.0, 1.0);
    float smc = 1.0 - exp2(-200.0 * r * r);
    smc *= pow(max(r, 1.0e-6), 0.5);
    vec3 p = giParams.nrdHitDistanceParams.xyz;
    float f = (p.x + abs(viewZ) * p.y) * mix(p.z, 1.0, smc);
    return clamp(hitDistance / max(f, 1.0e-4), 0.0, 1.0);
}

void writeNrdInputs(
    vec3 worldPos,
    vec3 normal,
    vec3 diffuseRadianceIn,
    float hitDistance,
    float roughness,
    float materialClass,
    float viewZ,
    float maxDistance
) {
    vec3 safeRadiance = diffuseRadianceIn;
    if (nrdIsInvalidVec3(safeRadiance)) {
        safeRadiance = vec3(0.0);
    }
    safeRadiance = clamp(safeRadiance, vec3(0.0), vec3(65504.0));
    vec3 encodedRadiance = nrdLinearToYCoCg(safeRadiance);

    float safeViewZ = nrdIsInvalid(viewZ) ? -1.0 : viewZ;
    if (abs(safeViewZ) < 1.0e-4) {
        safeViewZ = (safeViewZ < 0.0) ? -1.0e-4 : 1.0e-4;
    }

    vec3 motion = computeNrdScreenMotion(worldPos, safeViewZ);
    float normHitDist = getNrdNormHitDistance(hitDistance, viewZ, roughness);
    if (nrdIsInvalid(normHitDist)) {
        normHitDist = 0.0;
    }
    normHitDist = clamp(normHitDist, 0.0, 1.0);

    vec3 writeNormal = nrdIsInvalidVec3(normal) ? vec3(0.0, 0.0, 1.0) : normal;
    float writeRoughness = nrdIsInvalid(roughness) ? 1.0 : roughness;
    float writeMaterialClass = nrdIsInvalid(materialClass) ? 0.0 : materialClass;
    vec3 writeMotion = nrdIsInvalidVec3(motion) ? vec3(0.0) : motion;
    writeRoughness = clamp(writeRoughness, 0.0, 1.0);
    writeMaterialClass = clamp(writeMaterialClass, 0.0, 3.0);

    uint guideOverrideMode = giParams.nrdEncoding.z;
    if (guideOverrideMode >= 1u) {
        writeNormal = vec3(0.0, 1.0, 0.0);
        writeRoughness = 1.0;
        writeMaterialClass = 0.0;
    }
    if (guideOverrideMode >= 2u) {
        writeMotion = vec3(0.0);
    }

    outNrdDiffIn = vec4(encodedRadiance, normHitDist);
    outNrdNormalRoughnessIn =
        packNrdNormalRoughness(writeNormal, writeRoughness, writeMaterialClass);
    outNrdMotionIn = vec4(writeMotion, 0.0);
    outNrdViewZIn = vec4(safeViewZ, 0.0, 0.0, 0.0);
}

vec4 buildComposeBase(
    vec3 directBaseLinear,
    float alpha
) {
    return vec4(max(directBaseLinear, vec3(0.0)), alpha);
}

vec4 buildComposeIndirect(vec3 indirectTintLinear, float writerTag) {
    return vec4(max(indirectTintLinear, vec3(0.0)), clamp(writerTag, 0.0, 1.0));
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

void main() {
    outComposeBase = vec4(0.0);
    outComposeIndirect = vec4(0.0);
    outNrdDiffIn = vec4(0.0);
    outNrdNormalRoughnessIn = packNrdNormalRoughness(vec3(0.0, 0.0, 1.0), 1.0, 0.0);
    outNrdMotionIn = vec4(0.0);
    outNrdViewZIn = vec4(-1.0, 0.0, 0.0, 0.0);
    vec4 texel = sampleTerrainAlbedoGrad();

    vec3 normalWs = safeNormalize(inWorldNormal);
    vec3 sunDir = safeNormalize(giParams.sunDirection.xyz);
    bool pathTracingEnabled = (giParams.header.z != 0u);
    uint nrdDebugView = giParams.header.w;
    float nrdMaxDistance = max(1.0, giParams.shadowParams.x);
    ivec3 primaryVoxel = ivec3(floor(inWorldPos - (normalWs * 0.01)));
    uint rawVoxelMaterialId = sampleVoxelMaterialId(primaryVoxel);
    float nrdMaterialClass = nrdMaterialClassFromVoxelId(rawVoxelMaterialId);

    if (pathTracingEnabled) {
        PathTraceResult giTrace = tracePathTracedIndirect(inWorldPos, normalWs);
        vec3 indirectCurrent = giTrace.indirect;
        vec3 surfaceEmission = sampleSurfaceEmission(inWorldPos, normalWs);
        float direct = computePathTracedDirectSun(inWorldPos, normalWs, sunDir);
        vec3 directBaseLinear = texel.rgb * vec3(giParams.tuning.x + direct);
        directBaseLinear += surfaceEmission;
        vec3 indirectTintLinear = texel.rgb * giParams.tuning.y;
        vec3 noisyLitLinear = directBaseLinear + (indirectCurrent * indirectTintLinear);
        vec3 nrdInputRadiance = max(indirectCurrent, vec3(0.0));
        float nrdInputHitDistance = giTrace.candidateHitDistance;
        writeNrdInputs(
            inWorldPos,
            normalWs,
            nrdInputRadiance,
            nrdInputHitDistance,
            1.0,
            nrdMaterialClass,
            getNrdViewZ(),
            nrdMaxDistance
        );
        float writerTag = clamp(float(rawVoxelMaterialId) / 255.0, 0.0, 1.0);
        outComposeBase = buildComposeBase(directBaseLinear, texel.a);
        outComposeIndirect = buildComposeIndirect(indirectTintLinear, writerTag);
        if (nrdDebugView != 0u) {
            if (nrdDebugView == 6u) {
                outColor = vec4(toneMapAces(noisyLitLinear), texel.a);
                return;
            }
            vec3 nrdMotion = computeNrdScreenMotion(inWorldPos, getNrdViewZ());
            vec3 debugColor = visualizeNrdInput(
                nrdDebugView,
                nrdInputRadiance,
                nrdInputHitDistance,
                normalWs,
                nrdMotion,
                nrdMaterialClass,
                float(rawVoxelMaterialId),
                getNrdViewZ(),
                nrdMaxDistance
            );
            outColor = vec4(debugColor, texel.a);
            return;
        }
        outColor = vec4(toneMapAces(noisyLitLinear), texel.a);
        return;
    }

    GiLightingSample giSample = sampleGiLighting(inWorldPos, normalWs);
    vec3 gi = giSample.irradiance;
    float giNrdHitDistance = estimateNrdHitDistanceFromDepthMean(giSample.depthMean, nrdMaxDistance);
    vec3 surfaceEmission = sampleSurfaceEmission(inWorldPos, normalWs);
    float ndl = max(dot(normalWs, sunDir), 0.0);
    float sunShadow = computeSunShadow(inWorldPos, normalWs, sunDir);
    float direct = ndl * giParams.tuning.z * sunShadow;
    vec3 litLinear = texel.rgb * (vec3(giParams.tuning.x + direct) + (gi * giParams.tuning.y));
    litLinear += surfaceEmission;
    vec3 lit = toneMapAces(litLinear);
    writeNrdInputs(
        inWorldPos,
        normalWs,
        gi,
        giNrdHitDistance,
        1.0,
        nrdMaterialClass,
        getNrdViewZ(),
        nrdMaxDistance
    );
    if (nrdDebugView != 0u) {
        vec3 nrdMotion = computeNrdScreenMotion(inWorldPos, getNrdViewZ());
        vec3 debugColor = visualizeNrdInput(
            nrdDebugView,
            gi,
            giNrdHitDistance,
            normalWs,
            nrdMotion,
            nrdMaterialClass,
            float(rawVoxelMaterialId),
            getNrdViewZ(),
            nrdMaxDistance
        );
        outColor = vec4(debugColor, texel.a);
        return;
    }
    outColor = vec4(lit, texel.a);
}


