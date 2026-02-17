#include "WorldGen.hpp"

#include "ChunkManager.hpp"

#include <glm/common.hpp>

#include <cmath>
#include <random>

void WorldGen::generateInitialChunks(ChunkManager& cm, int radiusChunks) {
    int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

    for (int x = -radiusChunks; x <= radiusChunks; ++x) {
        for (int z = -radiusChunks; z <= radiusChunks; ++z) {
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                generateChunkAt(cm, glm::ivec3(x, y, z));
            }
        }
    }
    cm.updateDirtyChunks();
}

void WorldGen::generateChunkAt(ChunkManager& cm, const glm::ivec3& pos) {
    auto [it, inserted] = cm.chunkMap.try_emplace(pos, pos);
    Chunk& chunk = it->second;
    const bool prevSuppress = cm.suppressSunlightAffectedRebuilds;
    cm.suppressSunlightAffectedRebuilds = true;

    std::random_device rd;
    std::mt19937 gen(rd());

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            float n = 0.0f;
            float frequency = 1.01f;
            float amplitude = 0.8f;
            float persistence = 0.5f;
            int octaves = 6;
            float maxAmplitude = 0.0f;

            for (int i = 0; i < octaves; i++) {
                n += cm.noise.GetNoise(worldX * frequency, worldZ * frequency) * amplitude;
                maxAmplitude += amplitude;
                frequency *= 2.0f;
                amplitude *= persistence;
            }
            n /= maxAmplitude;

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.0f) * 0.5f * (WORLD_MAX_Y - WORLD_MIN_Y));

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;

                if (worldY == WORLD_MIN_Y) {
                    chunk.setBlock(x, y, z, BlockID::Bedrock);
                }
                else if (worldY < height - 2) {
                    chunk.setBlock(x, y, z, BlockID::Stone);
                }
                else if (worldY < height - 1) {
                    chunk.setBlock(x, y, z, BlockID::Dirt);
                }
                else if (worldY < height) {
                    chunk.setBlock(x, y, z, BlockID::Grass);
                }
                else {
                    chunk.setBlock(x, y, z, BlockID::Air);
                }
            }
        }
    }

    std::uniform_real_distribution<> chance(0.0, 1.0);
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int topY = -1;
            for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                if (chunk.getBlock(x, y, z) == BlockID::Grass) {
                    topY = y;
                    break;
                }
            }

            if (topY != -1 && chance(gen) < 0.003) {
                placeTree(cm, chunk, glm::ivec3(x, topY - 4, z), gen);
            }
        }
    }

    chunk.dirty = true;
    cm.rebuildColumnSunCache(pos.x, pos.z);
    cm.suppressSunlightAffectedRebuilds = prevSuppress;
}

void WorldGen::placeTree(ChunkManager& cm, Chunk& chunk, const glm::ivec3& basePos, std::mt19937& gen) {
    std::uniform_int_distribution<> trunkHeightDist(10, 14);
    int trunkHeight = trunkHeightDist(gen);

    glm::ivec3 trunkOffsets[4] = {
        glm::ivec3(0, 0, 0),
        glm::ivec3(1, 0, 0),
        glm::ivec3(0, 0, 1),
        glm::ivec3(1, 0, 1)
    };

    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 pos(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            cm.setBlockSafe(chunk, pos, BlockID::Log);
        }
    }

    int topY = basePos.y + trunkHeight - 1;

    int crownBaseYOffset = 0;
    int crownThickness = 2;
    int crownRadius = 4;
    int topCapYOffset = crownBaseYOffset + crownThickness;

    std::uniform_real_distribution<float> holeChance(0.0f, 1.0f);

    for (int dy = crownBaseYOffset; dy < crownBaseYOffset + crownThickness; ++dy) {
        int layerY = topY + dy;
        for (int dx = -crownRadius; dx <= crownRadius; ++dx) {
            for (int dz = -crownRadius; dz <= crownRadius; ++dz) {
                float dist = std::sqrt(float(dx * dx + dz * dz));
                if (dist <= crownRadius + 0.25f) {
                    float edgeFactor = (dist / float(crownRadius));
                    float skipProb = glm::smoothstep(0.7f, 1.0f, edgeFactor) * 0.65f;
                    if (dy == crownBaseYOffset) skipProb *= 0.55f;

                    if (holeChance(gen) < skipProb) continue;

                    glm::ivec3 leafPos(basePos.x + dx, layerY, basePos.z + dz);
                    if (cm.getBlockSafe(chunk, leafPos) == BlockID::Air) {
                        cm.setBlockSafe(chunk, leafPos, BlockID::Leaves);
                    }
                }
            }
        }
    }

    int taperRadius = glm::max(1, crownRadius - 2);
    int taperY = topY + topCapYOffset;
    for (int dx = -taperRadius; dx <= taperRadius; ++dx) {
        for (int dz = -taperRadius; dz <= taperRadius; ++dz) {
            float dist = std::sqrt(float(dx * dx + dz * dz));
            if (dist <= taperRadius + 0.25f) {
                glm::ivec3 leafPos(basePos.x + dx, taperY, basePos.z + dz);
                if (cm.getBlockSafe(chunk, leafPos) == BlockID::Air) {
                    if (dist > (taperRadius - 0.5f) && holeChance(gen) < 0.25f) continue;
                    cm.setBlockSafe(chunk, leafPos, BlockID::Leaves);
                }
            }
        }
    }

    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 pos(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            if (cm.getBlockSafe(chunk, pos) != BlockID::Log) {
                cm.setBlockSafe(chunk, pos, BlockID::Log);
            }
        }
    }
}

void WorldGen::generateTerrainChunkAt(ChunkManager& cm, const glm::ivec3& pos) {
    auto [it, inserted] = cm.chunkMap.try_emplace(pos, pos);
    Chunk& chunk = it->second;

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            float n = 0.0f;
            float frequency = 1.0f;
            float amplitude = 1.9f;
            float persistence = 0.5f;
            int octaves = 6;
            float maxAmplitude = 0.0f;

            for (int i = 0; i < octaves; i++) {
                n += cm.noise.GetNoise(worldX * frequency, worldZ * frequency) * amplitude;
                maxAmplitude += amplitude;
                frequency *= 2.0f;
                amplitude *= persistence;
            }
            if (maxAmplitude > 0.0f) n /= maxAmplitude;

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.0f) * 0.5f * (WORLD_MAX_Y - WORLD_MIN_Y));

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;

                if (worldY == WORLD_MIN_Y) {
                    chunk.setBlock(x, y, z, BlockID::Bedrock);
                }
                else if (worldY < height - 2) {
                    chunk.setBlock(x, y, z, BlockID::Stone);
                }
                else if (worldY < height - 1) {
                    chunk.setBlock(x, y, z, BlockID::Dirt);
                }
                else if (worldY < height) {
                    chunk.setBlock(x, y, z, BlockID::Grass);
                }
                else {
                    chunk.setBlock(x, y, z, BlockID::Air);
                }
            }
        }
    }

    chunk.dirty = true;
    cm.rebuildColumnSunCache(pos.x, pos.z);
}

void WorldGen::generateInitialChunksTwoPass(ChunkManager& cm, int radiusChunks) {
    int minChunkY = static_cast<int>(std::floor(float(WORLD_MIN_Y) / CHUNK_SIZE));
    int maxChunkY = static_cast<int>(std::floor(float(WORLD_MAX_Y) / CHUNK_SIZE));

    for (int x = -radiusChunks; x <= radiusChunks; ++x) {
        for (int z = -radiusChunks; z <= radiusChunks; ++z) {
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                generateTerrainChunkAt(cm, glm::ivec3(x, y, z));
            }
        }
    }

    const bool prevSuppress = cm.suppressSunlightAffectedRebuilds;
    cm.suppressSunlightAffectedRebuilds = true;
    for (auto& [pos, chunkRef] : cm.chunkMap) {
        uint32_t seed = static_cast<uint32_t>((pos.x * 73856093u) ^ (pos.y * 19349663u) ^ (pos.z * 83492791u));
        std::mt19937 gen(seed);
        std::uniform_real_distribution<> chance(0.0, 1.0);

        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int x = 0; x < CHUNK_SIZE; ++x) {
                int topY = -1;
                for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                    if (chunkRef.getBlock(x, y, z) == BlockID::Grass) {
                        topY = y;
                        break;
                    }
                }

                if (topY != -1 && chance(gen) < 0.02) {
                    placeTree(cm, chunkRef, glm::ivec3(x, topY + 1, z), gen);
                    chunkRef.dirty = true;
                }
            }
        }
    }
    cm.suppressSunlightAffectedRebuilds = prevSuppress;

    cm.updateDirtyChunks();
}
