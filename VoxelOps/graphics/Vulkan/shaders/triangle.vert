#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
} pc;

layout(set = 1, binding = 0, std430) readonly buffer ModelMatrices {
    mat4 models[];
} modelBuffer;

layout(location = 0) in uint inLow;
layout(location = 1) in uint inHigh;
layout(location = 0) out vec2 outTexCoordBlocks;
layout(location = 1) flat out uint outTileIndex;
layout(location = 2) out vec3 outWorldPos;
layout(location = 3) out vec3 outWorldNormal;

void main() {
    uint x = (inLow >> 0u) & 31u;
    uint y = (inLow >> 5u) & 31u;
    uint z = (inLow >> 10u) & 31u;
    uint face = (inLow >> 15u) & 0x7u;
    uint matId = (inHigh >> 0u) & 0xFFu;

    vec3 local = vec3(float(x), float(y), float(z));

    // Match OpenGL chunk shader UV generation so greedy meshes tile identically.
    if (face == 0u) {          // +X
        outTexCoordBlocks = vec2(-local.z, local.y);
    } else if (face == 1u) {   // -X
        outTexCoordBlocks = vec2(local.z, local.y);
    } else if (face == 2u) {   // +Y
        outTexCoordBlocks = vec2(local.x, -local.z);
    } else if (face == 3u) {   // -Y
        outTexCoordBlocks = vec2(local.x, local.z);
    } else if (face == 4u) {   // +Z
        outTexCoordBlocks = vec2(local.x, local.y);
    } else {                   // -Z
        outTexCoordBlocks = vec2(-local.x, local.y);
    }
    outTileIndex = matId;

    mat4 model = modelBuffer.models[gl_InstanceIndex];
    vec3 localNormal = vec3(0.0, 1.0, 0.0);
    if (face == 0u) {
        localNormal = vec3(1.0, 0.0, 0.0);
    } else if (face == 1u) {
        localNormal = vec3(-1.0, 0.0, 0.0);
    } else if (face == 2u) {
        localNormal = vec3(0.0, 1.0, 0.0);
    } else if (face == 3u) {
        localNormal = vec3(0.0, -1.0, 0.0);
    } else if (face == 4u) {
        localNormal = vec3(0.0, 0.0, 1.0);
    } else {
        localNormal = vec3(0.0, 0.0, -1.0);
    }

    vec4 worldPos = model * vec4(local, 1.0);
    outWorldPos = worldPos.xyz;
    outWorldNormal = normalize(mat3(model) * localNormal);
    gl_Position = pc.viewProjection * worldPos;
}
