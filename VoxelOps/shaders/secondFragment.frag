#version 330 core

in VS_OUT {
    vec2 texCoords;
    vec2 tileMin;
    vec2 tileSize;
    vec3 normal;
} fs_in;

uniform sampler2D uAtlas;
out vec4 FragColor;

void main() {
    vec2 uv = fs_in.tileMin + fract(fs_in.texCoords) * fs_in.tileSize;
    vec4 texColor = texture(uAtlas, uv);
    FragColor = texColor;
}
