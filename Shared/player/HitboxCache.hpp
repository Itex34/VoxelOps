#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Shared::HitboxCache {

struct Record {
    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float maxZ = 0.0f;
    uint8_t region = 0; // 0=Legs, 1=Body, 2=Head
};

bool Save(const std::string& path, float referenceHeight, float referenceRadius, const std::vector<Record>& records);
bool Load(const std::string& path, float& outReferenceHeight, float& outReferenceRadius, std::vector<Record>& outRecords);

} // namespace Shared::HitboxCache
