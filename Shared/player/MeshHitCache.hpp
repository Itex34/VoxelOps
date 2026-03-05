#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Shared::MeshHitCache {

struct TriangleRecord {
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
    uint8_t region = 1; // 0=Legs, 1=Body, 2=Head
};

bool Save(const std::string& path, float referenceHeight, const std::vector<TriangleRecord>& triangles);
bool Load(const std::string& path, float& outReferenceHeight, std::vector<TriangleRecord>& outTriangles);

} // namespace Shared::MeshHitCache
