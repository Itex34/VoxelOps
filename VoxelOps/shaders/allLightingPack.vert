#version 430
// Packed attributes
layout(location = 0) in uint inLow;
layout(location = 1) in uint inHigh;

// SSBO with chunk model matrices (4 vec4 per chunk)
layout(std430, binding = 0) buffer Models {
    vec4 models[];
};

// Uniforms
uniform float u_chunkSize;
uniform mat4 viewProj;
uniform vec3 cameraPos;
uniform vec4 u_tileInfo[256];
uniform mat4 model;

out vec3 Position;
out vec2 TexCoord;
out vec3 Normal;
out vec3 VertexColor;
out vec3 ViewDir;

const uint MASK5 = 31u;  // 5 bits
const uint MASK3 = 7u;   // 3 bits
const uint MASK2 = 3u;   // 2 bits
const uint MASK4 = 15u;  // 4 bits

// Face normals
const vec3 normals[6] = vec3[6](
    vec3( 1, 0, 0), vec3(-1, 0, 0),
    vec3( 0, 1, 0), vec3( 0,-1, 0),
    vec3( 0, 0, 1), vec3( 0, 0,-1)
);

// UV corners
const vec2 corners[4] = vec2[4](
    vec2(0,0), vec2(1,0), vec2(1,1), vec2(0,1)
);

const float k15 = 1.0 / 15.0;

void main() {
    uint low  = inLow;
    uint high = inHigh;

    // ------------------------------
    //  Decode packed vertex data
    // ------------------------------
    uint qx     = (low >> 0u)  & MASK5;
    uint qy     = (low >> 5u)  & MASK5;
    uint qz     = (low >> 10u) & MASK5;
    uint face   = (low >> 15u) & MASK3;
    uint corner = (low >> 18u) & MASK2;
    uint ao_i   = (low >> 26u) & MASK4;

    uint matId  = (high >> 0u) & 0xFFu;
    uint sun_i  = (high >> 8u) & MASK4;

    vec3 local = vec3(float(qx), float(qy), float(qz));

    // ------------------------------
    //  World space position
    // ------------------------------
    vec4 worldPos = model * vec4(local, 1.0);
    Position = worldPos.xyz;

    // ------------------------------
    //  Normal
    // ------------------------------
    vec3 n = normals[face];
    Normal = normalize(mat3(model) * n);

    // ------------------------------
    //  AO / Sunlight
    // ------------------------------
    VertexColor = vec3(float(ao_i) * k15, float(sun_i) * k15, 0.0);

    // ------------------------------
    //  Camera direction
    // ------------------------------
    ViewDir = normalize(cameraPos - Position);

    // ------------------------------
    //  Texture coordinates
    // ------------------------------
    vec4 tile = u_tileInfo[int(matId)];
    vec2 uvCorner = corners[corner];
    TexCoord = tile.xy + uvCorner * tile.zw;

    gl_Position = viewProj * worldPos;
}
