#include "WorldGen.hpp"

#include "ChunkManager.hpp"

#include <glm/common.hpp>

#include <cmath>
#include <random>

void WorldGen::applyClientDecorationPass(ChunkManager& cm, ServerChunk& chunk, const glm::ivec3& chunkPos) {
    const uint32_t seed = static_cast<uint32_t>(
        (chunkPos.x * 73856093u) ^
        (chunkPos.y * 19349663u) ^
        (chunkPos.z * 83492791u)
    );
    std::mt19937 gen(seed);
    std::uniform_real_distribution<> chance(0.0, 1.0);

    bool anyDecoration = false;
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int topY = -1;
            for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                if (chunk.getBlock(x, y, z) == BlockID::Grass) {
                    topY = y;
                    break;
                }
            }

            if (topY != -1 && chance(gen) < 0.02) {
                placeTree(cm, chunk, glm::ivec3(x, topY + 1, z), gen);
                anyDecoration = true;
            }
        }
    }

    if (anyDecoration) {
        chunk.markDirty();
    }
}

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

void WorldGen::generateInitialChunksTwoPass(ChunkManager& cm, int radiusChunks) {
    int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
    int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

    // PASS 1: terrain-only
    for (int x = -radiusChunks; x <= radiusChunks; ++x) {
        for (int z = -radiusChunks; z <= radiusChunks; ++z) {
            for (int y = minChunkY; y <= maxChunkY; ++y) {
                generateTerrainChunkAt(cm, glm::ivec3(x, y, z));
            }
        }
    }

    // PASS 2: decoration (mirrors client WorldGen two-pass decoration behavior)
    auto snap = cm.snapshotChunkMap();
    for (auto& [pos, chunkPtr] : snap) {
        if (!chunkPtr) continue;
        applyClientDecorationPass(cm, *chunkPtr, pos);
        std::lock_guard<std::mutex> lk(cm.mapMutex);
        cm.decoratedChunks.insert(pos);
    }

    cm.updateDirtyChunks();
}

void WorldGen::generateChunkAt(ChunkManager& cm, const glm::ivec3& pos) {
    if (!cm.inBounds(pos)) return;

    auto chunk = std::make_unique<ServerChunk>(pos);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            float n = 0.f;
            float freq = 1.01f;
            float amp = 0.8f;
            float persistence = 0.5f;
            int octaves = 6;
            int maxYrange = WORLD_MAX_Y - WORLD_MIN_Y;
            float maxAmp = 0.f;

            for (int o = 0; o < octaves; ++o) {
                n += cm.noise.GetNoise(worldX * freq, worldZ * freq) * amp;
                maxAmp += amp;
                freq *= 2.f;
                amp *= persistence;
            }
            n /= (maxAmp > 0.f ? maxAmp : 1.f);

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.f) * 0.5f * maxYrange);

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;
                if (worldY == WORLD_MIN_Y) chunk->applyEdit(x, y, z, BlockID::Bedrock);
                else if (worldY < height - 2) chunk->applyEdit(x, y, z, BlockID::Stone);
                else if (worldY < height - 1) chunk->applyEdit(x, y, z, BlockID::Dirt);
                else if (worldY < height) chunk->applyEdit(x, y, z, BlockID::Grass);
                else chunk->applyEdit(x, y, z, BlockID::Air);
            }
        }
    }

    // Reuse the client-style two-pass decoration rules for consistency with the client worldgen.
    applyClientDecorationPass(cm, *chunk, pos);

    {
        std::lock_guard<std::mutex> lk(cm.mapMutex);
        cm.chunkMap[pos] = std::move(chunk);
        cm.chunkMap[pos]->markDirty();
        cm.decoratedChunks.insert(pos);
    }
}

void WorldGen::generateTerrainChunkAt(ChunkManager& cm, const glm::ivec3& pos) {
    if (!cm.inBounds(pos)) return;

    auto chunk = std::make_unique<ServerChunk>(pos);

    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int worldX = pos.x * CHUNK_SIZE + x;
            int worldZ = pos.z * CHUNK_SIZE + z;

            float n = 0.f;
            float freq = 1.f;
            float amp = 1.9f;
            float persistence = 0.5f;
            int octaves = 6;
            int maxYrange = WORLD_MAX_Y - WORLD_MIN_Y;
            float maxAmp = 0.f;

            for (int o = 0; o < octaves; ++o) {
                n += cm.noise.GetNoise(worldX * freq, worldZ * freq) * amp;
                maxAmp += amp;
                freq *= 2.f;
                amp *= persistence;
            }
            n /= (maxAmp > 0.f ? maxAmp : 1.f);

            int height = WORLD_MIN_Y + static_cast<int>((n + 1.f) * 0.5f * maxYrange);

            for (int y = 0; y < CHUNK_SIZE; ++y) {
                int worldY = pos.y * CHUNK_SIZE + y;
                if (worldY == WORLD_MIN_Y) chunk->applyEdit(x, y, z, BlockID::Bedrock);
                else if (worldY < height - 2) chunk->applyEdit(x, y, z, BlockID::Stone);
                else if (worldY < height - 1) chunk->applyEdit(x, y, z, BlockID::Dirt);
                else if (worldY < height) chunk->applyEdit(x, y, z, BlockID::Grass);
                else chunk->applyEdit(x, y, z, BlockID::Air);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(cm.mapMutex);
        cm.chunkMap[pos] = std::move(chunk);
        cm.chunkMap[pos]->markDirty();
        cm.decoratedChunks.erase(pos);
    }
}

void WorldGen::decorateChunkAt(ChunkManager& cm, const glm::ivec3& pos) {
    ServerChunk* chunkPtr = nullptr;
    {
        std::lock_guard<std::mutex> lk(cm.mapMutex);
        auto it = cm.chunkMap.find(pos);
        if (it != cm.chunkMap.end()) {
            chunkPtr = it->second.get();
        }
    }
    if (!chunkPtr) return;

    applyClientDecorationPass(cm, *chunkPtr, pos);

    std::lock_guard<std::mutex> lk(cm.mapMutex);
    cm.decoratedChunks.insert(pos);
}

void WorldGen::placeTree(ChunkManager& cm, ServerChunk& chunk, const glm::ivec3& basePos, std::mt19937& gen) {
    std::uniform_int_distribution<> trunkH(10, 14);
    int trunkHeight = trunkH(gen);

    glm::ivec3 trunkOffsets[4] = {
        glm::ivec3(0, 0, 0), glm::ivec3(1, 0, 0),
        glm::ivec3(0, 0, 1), glm::ivec3(1, 0, 1)
    };

    for (int i = 0; i < trunkHeight; ++i) {
        int y = basePos.y + i;
        for (int t = 0; t < 4; ++t) {
            glm::ivec3 p(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            cm.setBlockSafe(chunk, p, BlockID::Log);
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
            glm::ivec3 p(basePos.x + trunkOffsets[t].x, y, basePos.z + trunkOffsets[t].z);
            if (cm.getBlockSafe(chunk, p) != BlockID::Log) {
                cm.setBlockSafe(chunk, p, BlockID::Log);
            }
        }
    }
}
