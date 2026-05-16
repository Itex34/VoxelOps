#version 460
layout(early_fragment_tests) in;
#extension GL_EXT_ray_query : require

layout(set = 0, binding = 0) uniform sampler2DArray texSampler;

layout(set = 2, binding = 0, std140) uniform GiLightingParams {
    uvec4 header;     // x = reserved, y = sun shadow enabled, z = path trace enabled, w = NRD debug view
    uvec4 pathConfig; // x = path rays/pixel, y = reserved, z = frame index low, w = history reset
    uvec4 tracingConfig; // x = reserved, y = hw rt supported, z = tlas valid, w = NRD history valid
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
layout(set = 2, binding = 12, r16f) uniform image2D nrdShadowInImage;
layout(set = 2, binding = 13) uniform sampler2D nrdShadowOutSampler;
layout(set = 2, binding = 14) uniform sampler2D blueNoiseTex;
layout(set = 2, binding = 15) uniform accelerationStructureEXT sceneTlas;

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


#include "gi_common.glsl"
#include "terrain_sampling.glsl"
#include "gi_materials.glsl"
#include "gi_voxel_sampling.glsl"
#include "gi_tracing.glsl"
#include "gi_pathtrace.glsl"
#include "gi_nrd.glsl"
#include "gi_debug.glsl"

void main() {
    outComposeBase = vec4(0.0);
    outComposeIndirect = vec4(0.0);
    outNrdDiffIn = vec4(0.0);
    outNrdNormalRoughnessIn = packNrdNormalRoughness(vec3(0.0, 0.0, 1.0), 1.0, 0.0);
    outNrdMotionIn = vec4(0.0);
    outNrdViewZIn = vec4(-1.0, 0.0, 0.0, 0.0);
    vec4 texel = sampleTerrainAlbedoGrad();

    vec3 normalWs = inWorldNormal;
    vec3 sunDir = giParams.sunDirection.xyz;
    float ndl = max(dot(normalWs, sunDir), 0.0);
    const bool pathTracingEnabled = (giParams.header.z != 0u);
    const uint nrdDebugView = giParams.header.w;
    float viewZ = getNrdViewZ();
    float nrdMaxDistance = max(1.0, giParams.shadowParams.x);
    ivec3 primaryVoxel = ivec3(floor(inWorldPos - (normalWs * 0.01)));
    uint rawVoxelMaterialId = sampleVoxelMaterialId(primaryVoxel);
    float nrdMaterialClass = nrdMaterialClassFromVoxelId(rawVoxelMaterialId);

    if (pathTracingEnabled) {
        PathTraceResult giTrace = tracePathTracedIndirect(inWorldPos, normalWs);
        vec3 indirectCurrent = giTrace.indirect;
        vec3 surfaceEmission = sampleSurfaceEmission(inWorldPos, normalWs);
        float direct =
            computePathTracedDirectSunNormalized(inWorldPos, normalWs, sunDir, ndl, nrdMaxDistance);
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
            viewZ,
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
            vec3 nrdMotion = computeNrdScreenMotion(inWorldPos, viewZ);

            vec3 debugColor = visualizeNrdInput(
                nrdDebugView,
                nrdInputRadiance,
                nrdInputHitDistance,
                normalWs,
                nrdMotion,
                nrdMaterialClass,
                float(rawVoxelMaterialId),
                viewZ,
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
    float sunShadow =
        computeSunShadowNormalized(inWorldPos, normalWs, sunDir, ndl, nrdMaxDistance);
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
        viewZ,
        nrdMaxDistance
    );
    if (nrdDebugView != 0u) {
        vec3 nrdMotion = computeNrdScreenMotion(inWorldPos, viewZ);
        vec3 debugColor = visualizeNrdInput(
            nrdDebugView,
            gi,
            giNrdHitDistance,
            normalWs,
            nrdMotion,
            nrdMaterialClass,
            float(rawVoxelMaterialId),
            viewZ,
            nrdMaxDistance
        );
        outColor = vec4(debugColor, texel.r);
        return;
    }
    outColor = vec4(lit, texel.a);
}


