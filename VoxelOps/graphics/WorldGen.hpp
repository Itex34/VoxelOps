#pragma once

#include <glm/glm.hpp>
#include <random>

class Chunk;
class ChunkManager;

class WorldGen
{
public:
    static void generateInitialChunks(ChunkManager& cm, int radiusChunks);
    static void generateInitialChunksTwoPass(ChunkManager& cm, int radiusChunks);
    static void generateChunkAt(ChunkManager& cm, const glm::ivec3& pos);
    static void generateTerrainChunkAt(ChunkManager& cm, const glm::ivec3& pos);
    static void placeTree(ChunkManager& cm, Chunk& chunk, const glm::ivec3& basePos, std::mt19937& gen);
};
