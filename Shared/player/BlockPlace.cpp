#include "BlockPlace.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace BlockPlace {
    namespace {
        glm::ivec3 ClampFaceNormal(glm::ivec3 normal) {
            if (normal.x > 1)
                normal.x = 1;
            if (normal.x < -1)
                normal.x = -1;
            if (normal.y > 1)
                normal.y = 1;
            if (normal.y < -1)
                normal.y = -1;
            if (normal.z > 1)
                normal.z = 1;
            if (normal.z < -1)
                normal.z = -1;

            if (normal == glm::ivec3(0)) {
                return glm::ivec3(0, 1, 0);
            }
            return normal;
        }

        glm::ivec3 QuantizeHorizontalDirection(
            float x, float z, const glm::ivec3 &fallbackDirection
        ) {
            if (!std::isfinite(x) || !std::isfinite(z)) {
                return fallbackDirection;
            }
            const float absX = std::fabs(x);
            const float absZ = std::fabs(z);
            if (absX < 1e-5f && absZ < 1e-5f) {
                return fallbackDirection;
            }
            if (absX >= absZ) {
                return glm::ivec3((x >= 0.0f) ? 1 : -1, 0, 0);
            }
            return glm::ivec3(0, 0, (z >= 0.0f) ? 1 : -1);
        }

        glm::ivec3 BuildLookAlignedForward(const glm::ivec3 &normal, const glm::vec3 &lookDirection) {
            if (normal.y != 0) {
                return QuantizeHorizontalDirection(lookDirection.x, lookDirection.z, glm::ivec3(0, 0, 1));
            }

            const glm::ivec3 tangent(-normal.z, 0, normal.x);
            float signMetric = (lookDirection.x * static_cast<float>(tangent.x)) +
                               (lookDirection.z * static_cast<float>(tangent.z));

            // If player is looking straight into the wall normal, use camera-right direction.
            if (std::fabs(signMetric) < 1e-5f) {
                const float rightX = -lookDirection.z;
                const float rightZ = lookDirection.x;
                signMetric = (rightX * static_cast<float>(tangent.x)) +
                             (rightZ * static_cast<float>(tangent.z));
            }

            if (std::fabs(signMetric) < 1e-5f) {
                signMetric = 1.0f;
            }

            return (signMetric >= 0.0f) ? tangent : -tangent;
        }
    } // namespace

    BlockMode NextMode(BlockMode mode) noexcept {
        const uint8_t count = static_cast<uint8_t>(BlockMode::COUNT);
        const uint8_t current = static_cast<uint8_t>(mode);
        if (count == 0) {
            return BlockMode::Block;
        }
        const uint8_t next = static_cast<uint8_t>((current + 1u) % count);
        return static_cast<BlockMode>(next);
    }

    std::string_view ToString(BlockMode mode) noexcept {
        switch (mode) {
        case BlockMode::Block:
            return "Block";
        case BlockMode::Wall:
            return "Wall";
        case BlockMode::Stair:
            return "Stair";
        case BlockMode::Floor:
            return "Floor";
        default:
            return "Block";
        }
    }

    std::vector<glm::ivec3> BuildPlacementOffsets(
        BlockMode mode, const glm::ivec3 &faceNormal, const glm::vec3 &lookDirection
    ) {
        const glm::ivec3 normal = ClampFaceNormal(faceNormal);
        std::vector<glm::ivec3> offsets;

        switch (mode) {
        case BlockMode::Block: {
            offsets.push_back(glm::ivec3(0));
            break;
        }
        case BlockMode::Wall: {
            glm::ivec3 wallRight(1, 0, 0);
            if (normal.y != 0) {
                const glm::ivec3 lookForward = BuildLookAlignedForward(normal, lookDirection);
                wallRight = glm::ivec3(-lookForward.z, 0, lookForward.x);
                if (wallRight == glm::ivec3(0)) {
                    wallRight = glm::ivec3(1, 0, 0);
                }
            } else {
                wallRight = BuildLookAlignedForward(normal, lookDirection);
            }
            offsets.reserve(9);
            for (int x = -1; x <= 1; ++x) {
                for (int y = 0; y <= 2; ++y) {
                    offsets.push_back((wallRight * x) + glm::ivec3(0, y, 0));
                }
            }
            break;
        }
        case BlockMode::Floor: {
            offsets.reserve(9);
            for (int x = -1; x <= 1; ++x) {
                for (int z = -1; z <= 1; ++z) {
                    offsets.push_back(glm::ivec3(x, 0, z));
                }
            }
            break;
        }
        case BlockMode::Stair: {
            const glm::ivec3 stairForward = BuildLookAlignedForward(normal, lookDirection);
            const glm::ivec3 stairRight(-stairForward.z, 0, stairForward.x);
            offsets.reserve(12);

            // Bottom slab: 2 forward x 3 wide.
            for (int forward = 0; forward <= 1; ++forward) {
                for (int lateral = -1; lateral <= 1; ++lateral) {
                    offsets.push_back((stairForward * forward) + (stairRight * lateral));
                }
            }

            // Top slab: same 2x3 footprint, shifted +1 forward and +1 up.
            for (int forward = 1; forward <= 2; ++forward) {
                for (int lateral = -1; lateral <= 1; ++lateral) {
                    offsets.push_back(
                        (stairForward * forward) + (stairRight * lateral) + glm::ivec3(0, 1, 0)
                    );
                }
            }
            break;
        }
        default:
            offsets.push_back(glm::ivec3(0));
            break;
        }

        return offsets;
    }

    std::vector<glm::ivec3> BuildPlacementPositions(
        BlockMode mode,
        const glm::ivec3 &hitBlockWorld,
        const glm::ivec3 &adjacentAirBlockWorld,
        const glm::vec3 &lookDirection
        ,
        bool floorTopFaceForwardAssist
    ) {
        const glm::ivec3 normal = ClampFaceNormal(adjacentAirBlockWorld - hitBlockWorld);

        if (mode == BlockMode::Floor && normal == glm::ivec3(0, 1, 0) &&
            floorTopFaceForwardAssist) {
            // Top-face floor placement rule:
            // - Place on the same Y as the looked-at block.
            // - Place a full 3x3 one block in front, extending 3 blocks forward.
            const glm::ivec3 forward = BuildLookAlignedForward(normal, lookDirection);
            glm::ivec3 right(-forward.z, 0, forward.x);
            if (right == glm::ivec3(0)) {
                right = glm::ivec3(1, 0, 0);
            }

            const glm::ivec3 base = hitBlockWorld + forward;
            std::vector<glm::ivec3> out;
            out.reserve(9);
            for (int depth = 0; depth <= 2; ++depth) {
                for (int lateral = -1; lateral <= 1; ++lateral) {
                    out.push_back(base + (forward * depth) + (right * lateral));
                }
            }
            return out;
        }

        const std::vector<glm::ivec3> offsets = BuildPlacementOffsets(mode, normal, lookDirection);
        glm::ivec3 anchor = adjacentAirBlockWorld;
        if (mode == BlockMode::Floor && normal.y == 0) {

            anchor += normal;
        }

        std::vector<glm::ivec3> out;
        out.reserve(offsets.size());
        for (const glm::ivec3 &offset : offsets) {
            out.push_back(anchor + offset);
        }

        std::sort(out.begin(), out.end(), [](const glm::ivec3 &a, const glm::ivec3 &b) {
            if (a.x != b.x)
                return a.x < b.x;
            if (a.y != b.y)
                return a.y < b.y;
            return a.z < b.z;
        });
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

} // namespace BlockPlace
