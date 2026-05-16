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
