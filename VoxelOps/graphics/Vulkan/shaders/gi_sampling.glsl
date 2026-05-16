uint hashU32(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

uvec3 worldPosSeedCoords(vec3 worldPos) {
    // Use integerized world-space cells to avoid float-bit precision collapse far from origin.
    const float kSeedCellScale = 64.0; // 1/64 block resolution
    ivec3 cell = ivec3(floor(worldPos * kSeedCellScale));
    return uvec3(cell);
}

uint initSeed(vec3 worldPos, uint sampleIndex) {
    uvec3 cell = worldPosSeedCoords(worldPos);
    uint seed = (cell.x * 73856093u) ^ (cell.y * 19349663u) ^ (cell.z * 83492791u);
    seed ^= uint(gl_FragCoord.x) * 374761393u;
    seed ^= uint(gl_FragCoord.y) * 668265263u;
    seed ^= sampleIndex * 2246822519u;
    seed ^= giParams.pathConfig.z * 3266489917u;
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
