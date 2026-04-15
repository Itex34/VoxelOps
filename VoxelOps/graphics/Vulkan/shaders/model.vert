#version 450

layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
} pc;

layout(set = 1, binding = 0, std430) readonly buffer ModelMatrices {
    mat4 models[];
} modelBuffer;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec2 outUv;
layout(location = 1) out vec3 outWorldPos;

void main() {
    mat4 model = modelBuffer.models[gl_InstanceIndex];
    vec4 worldPos = model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProjection * worldPos;
    outUv = inUv;
    outWorldPos = worldPos.xyz;
}
