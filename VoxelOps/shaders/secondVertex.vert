#version 330 core

layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoords;
layout (location = 3) in vec2 aTileMin;
layout (location = 4) in vec2 aTileSize;

uniform mat4 uMVP;
uniform mat4 uModel;

out VS_OUT {
    vec2 texCoords;
    vec2 tileMin;
    vec2 tileSize;
    vec3 normal;
} vs_out;

void main() {
    gl_Position     = uMVP * vec4(aPos, 1.0);
    vs_out.texCoords = aTexCoords;
    vs_out.tileMin  = aTileMin;
    vs_out.tileSize = aTileSize;
    vs_out.normal   = mat3(uModel) * aNormal;
}

