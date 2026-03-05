#include "HitboxCache.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Shared::HitboxCache {

namespace {
constexpr std::array<char, 4> kMagic{ 'H', 'B', 'X', '1' };
constexpr uint32_t kVersion = 3u;

template <typename T>
void WriteBinary(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
bool ReadBinary(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}
}

bool Save(const std::string& path, float referenceHeight, float referenceRadius, const std::vector<Record>& records) {
    if (path.empty()) {
        return false;
    }

    std::error_code ec;
    const std::filesystem::path outPath(path);
    const std::filesystem::path parent = outPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }

    WriteBinary(out, kMagic);
    WriteBinary(out, kVersion);
    WriteBinary(out, referenceHeight);
    WriteBinary(out, referenceRadius);
    const uint32_t count = static_cast<uint32_t>(records.size());
    WriteBinary(out, count);

    for (const Record& rec : records) {
        WriteBinary(out, rec.minX);
        WriteBinary(out, rec.minY);
        WriteBinary(out, rec.minZ);
        WriteBinary(out, rec.maxX);
        WriteBinary(out, rec.maxY);
        WriteBinary(out, rec.maxZ);
        WriteBinary(out, rec.region);
    }

    return static_cast<bool>(out);
}

bool Load(const std::string& path, float& outReferenceHeight, float& outReferenceRadius, std::vector<Record>& outRecords) {
    outReferenceHeight = 0.0f;
    outReferenceRadius = 0.0f;
    outRecords.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::array<char, 4> magic{};
    uint32_t version = 0;
    float refHeight = 0.0f;
    float refRadius = 0.0f;
    uint32_t count = 0;

    if (!ReadBinary(in, magic)) return false;
    if (magic != kMagic) return false;
    if (!ReadBinary(in, version)) return false;
    if (version != kVersion) return false;
    if (!ReadBinary(in, refHeight)) return false;
    if (!ReadBinary(in, refRadius)) return false;
    if (!ReadBinary(in, count)) return false;
    if (count > 256u) return false;

    outRecords.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Record rec;
        if (!ReadBinary(in, rec.minX)) return false;
        if (!ReadBinary(in, rec.minY)) return false;
        if (!ReadBinary(in, rec.minZ)) return false;
        if (!ReadBinary(in, rec.maxX)) return false;
        if (!ReadBinary(in, rec.maxY)) return false;
        if (!ReadBinary(in, rec.maxZ)) return false;
        if (!ReadBinary(in, rec.region)) return false;
        outRecords.push_back(rec);
    }

    outReferenceHeight = refHeight;
    outReferenceRadius = refRadius;
    return true;
}

} // namespace Shared::HitboxCache
