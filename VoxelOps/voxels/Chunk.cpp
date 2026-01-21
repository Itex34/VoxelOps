#include "Chunk.hpp"

Chunk::Chunk(glm::ivec3 pos)
    : position(pos), blocks{}, nonAirCount(0), dirty(true)
{
    blocks.fill(BlockID::Air);
}

BlockID Chunk::getBlock(int x, int y, int z) const noexcept {
    if (!inBounds(x, y, z)) return BlockID::Air;
    return blocks[idx(x, y, z)];
}

BlockID Chunk::getBlockUnchecked(int x, int y, int z) const noexcept {
    // caller must ensure coords are in bounds
    return blocks[idx(x, y, z)];
}

void Chunk::setBlock(int x, int y, int z, BlockID id) {
    if (!inBounds(x, y, z)) {
        assert(false && "setBlock out of bounds");
        return;
    }

    int i = idx(x, y, z);
    BlockID old = blocks[i];
    if (old == id) return; // no-op, don't mark dirty

    // update nonAirCount
    if (old == BlockID::Air && id != BlockID::Air) ++nonAirCount;
    else if (old != BlockID::Air && id == BlockID::Air) --nonAirCount;

    blocks[i] = id;
    dirty = true;
}



BlockID Chunk::removeBlock(int x, int y, int z) {

    if (!inBounds(x, y, z)) {
        assert(false && "removeBlock out of bounds");
        return BlockID::Air;
    }

    int i = idx(x, y, z);
    BlockID old = blocks[i];

    // update nonAirCount
    if (old != BlockID::Air) --nonAirCount; // if the old block is air no need to chenge the nonAirCount


    blocks[i] = BlockID::Air;
    dirty = true;
    return old;
}