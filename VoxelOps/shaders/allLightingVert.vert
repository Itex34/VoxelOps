#version 330 core
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTex;
layout(location = 3) in vec3 inColor; // AO in .r, shadow in .g

uniform mat3 normalMat;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform vec3 cameraPos;

out vec2 TexCoord;
out vec3 Normal;
out vec3 VertexColor;
out vec3 ViewDir; // fragment <- camera direction

void main() {
    vec4 worldPos = model * vec4(inPos, 1.0);
    TexCoord = inTex;
    // normals in world space
    Normal = normalMat * inNormal;
    VertexColor = inColor;
    ViewDir = normalize(cameraPos - vec3(worldPos)); // fragment -> camera
    gl_Position = projection * view * worldPos;
}
