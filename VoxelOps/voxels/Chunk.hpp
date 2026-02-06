#pragma once
#include <array>
#include <cstdint>
#include <cassert>
#include <mutex>
#include "Voxel.hpp"
#include <glm/vec3.hpp>


constexpr int CHUNK_SIZE = 16;
constexpr int CHUNK_VOLUME = CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE;

struct AABB {
    glm::vec3 min;
    glm::vec3 max;

    AABB(glm::ivec3 chunkPos, float size) {
        glm::vec3 worldPos = glm::vec3(chunkPos) * size;
        min = worldPos;
        max = worldPos + glm::vec3(size);
    }
};

class Chunk {
public:
    explicit Chunk(glm::ivec3 pos = glm::ivec3(0));

    // Safe accessor: returns Air for out-of-bounds
    BlockID getBlock(int x, int y, int z) const noexcept;

    // Unchecked accessor for internal use when coords are guaranteed valid
    BlockID getBlockUnchecked(int x, int y, int z) const noexcept;
     
    // Safe setter; updates nonAirCount and dirty only when value actually changes
    void setBlock(int x, int y, int z, BlockID id);

    BlockID removeBlock(int x, int y, int z);

    bool isCompletelyAir() const noexcept { return nonAirCount == 0; }

    glm::ivec3 position;
    std::atomic<bool> dirty = true;
    std::atomic<bool> building = false;
    std::mutex mtx;

    glm::ivec3 getWorldPosition() const noexcept {
        return position * CHUNK_SIZE;
    }

    static inline constexpr bool inBounds(int x, int y, int z) noexcept {
        return (unsigned)x < CHUNK_SIZE && (unsigned)y < CHUNK_SIZE && (unsigned)z < CHUNK_SIZE;
    }
private:
    std::array<BlockID, CHUNK_VOLUME> blocks;
    uint16_t nonAirCount = 0; // max 4096 (16^3) -> fits uint16_t

    static inline constexpr int idx(int x, int y, int z) noexcept {
        return x + CHUNK_SIZE * (y + CHUNK_SIZE * z);
    }

};
