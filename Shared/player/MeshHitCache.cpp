#include "MeshHitCache.hpp"

#include <array>
#include <filesystem>
#include <fstream>

namespace Shared::MeshHitCache {

namespace {
constexpr std::array<char, 4> kMagic{ 'M', 'H', 'C', '1' };
constexpr uint32_t kVersion = 1u;

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

bool Save(const std::string& path, float referenceHeight, const std::vector<TriangleRecord>& triangles) {
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
    const uint32_t count = static_cast<uint32_t>(triangles.size());
    WriteBinary(out, count);

    for (const TriangleRecord& tri : triangles) {
        WriteBinary(out, tri.ax);
        WriteBinary(out, tri.ay);
        WriteBinary(out, tri.az);
        WriteBinary(out, tri.bx);
        WriteBinary(out, tri.by);
        WriteBinary(out, tri.bz);
        WriteBinary(out, tri.cx);
        WriteBinary(out, tri.cy);
        WriteBinary(out, tri.cz);
        WriteBinary(out, tri.region);
    }

    return static_cast<bool>(out);
}

bool Load(const std::string& path, float& outReferenceHeight, std::vector<TriangleRecord>& outTriangles) {
    outReferenceHeight = 0.0f;
    outTriangles.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::array<char, 4> magic{};
    uint32_t version = 0;
    float referenceHeight = 0.0f;
    uint32_t count = 0;

    if (!ReadBinary(in, magic)) return false;
    if (magic != kMagic) return false;
    if (!ReadBinary(in, version)) return false;
    if (version != kVersion) return false;
    if (!ReadBinary(in, referenceHeight)) return false;
    if (!ReadBinary(in, count)) return false;
    if (count > 65535u) return false;

    outTriangles.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        TriangleRecord tri;
        if (!ReadBinary(in, tri.ax)) return false;
        if (!ReadBinary(in, tri.ay)) return false;
        if (!ReadBinary(in, tri.az)) return false;
        if (!ReadBinary(in, tri.bx)) return false;
        if (!ReadBinary(in, tri.by)) return false;
        if (!ReadBinary(in, tri.bz)) return false;
        if (!ReadBinary(in, tri.cx)) return false;
        if (!ReadBinary(in, tri.cy)) return false;
        if (!ReadBinary(in, tri.cz)) return false;
        if (!ReadBinary(in, tri.region)) return false;
        outTriangles.push_back(tri);
    }

    outReferenceHeight = referenceHeight;
    return true;
}

} // namespace Shared::MeshHitCache
