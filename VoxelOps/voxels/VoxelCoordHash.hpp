#pragma once

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>

struct IVec3Hash {
    std::size_t operator()(glm::ivec3 const &v) const noexcept {
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t z = static_cast<uint32_t>(v.z);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u) ^ (z * 83492791u);
        return static_cast<std::size_t>(h);
    }
};

struct IVec2Hash {
    std::size_t operator()(glm::ivec2 const &v) const noexcept {
        uint64_t x = static_cast<uint32_t>(v.x);
        uint64_t y = static_cast<uint32_t>(v.y);
        uint64_t h = (x * 73856093u) ^ (y * 19349663u);
        return static_cast<std::size_t>(h);
    }
};

struct IVec3Eq {
    bool operator()(glm::ivec3 const &a, glm::ivec3 const &b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};
