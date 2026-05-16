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
    return 1.0 / max(gl_FragCoord.w, 1.0e-6);
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
    float prevViewZ = prevClipW;
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

    float safeViewZ = nrdIsInvalid(viewZ) ? 1.0 : viewZ;
    if (abs(safeViewZ) < 1.0e-4) {
        safeViewZ = 1.0e-4;
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
