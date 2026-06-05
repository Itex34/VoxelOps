#include "WorldGen.hpp"

#include "ChunkManager.hpp"

#include <glm/common.hpp>

#include <cmath>
#include <random>
#include <vector>
#include <algorithm>

void WorldGen::applyClientDecorationPass(
    ChunkManager &cm, ServerChunk &chunk, const glm::ivec3 &chunkPos
) {
    const uint32_t seed = static_cast<uint32_t>(
        (chunkPos.x * 73856093u) ^ (chunkPos.y * 19349663u) ^ (chunkPos.z * 83492791u)
    );
    std::mt19937 gen(seed);
    std::uniform_real_distribution<> chance(0.0, 1.0);

    bool anyDecoration = false;
    for (int z = 0; z < CHUNK_SIZE; ++z) {
        for (int x = 0; x < CHUNK_SIZE; ++x) {
            int topY = -1;
            for (int y = CHUNK_SIZE - 1; y >= 0; --y) {
                if (chunk.getBlockNoTouch(x, y, z) == BlockID::Grass) {
                    topY = y;
                    break;
                }
            }

            if (topY != -1 && chance(gen) < 0.0025) {
                placeTree(cm, chunk, glm::ivec3(x, topY + 1, z), gen);
                anyDecoration = true;
            }
        }
    }

    if (anyDecoration) {
        chunk.markDirty();
    }
}

void WorldGen::generateInitialChunks(ChunkManager &cm, int radiusChunks) {
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

void WorldGen::generateInitialChunksTwoPass(ChunkManager &cm, int radiusChunks) {
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
    for (auto &[pos, chunkPtr] : snap) {
        if (!chunkPtr)
            continue;
        applyClientDecorationPass(cm, *chunkPtr, pos);
        std::lock_guard<std::shared_mutex> lk(cm.mapMutex);
        cm.decoratedChunks.insert(pos);
    }

    cm.updateDirtyChunks();
}

void WorldGen::generateChunkAt(ChunkManager &cm, const glm::ivec3 &pos) {
    if (!cm.inBounds(pos))
        return;

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
                if (worldY == WORLD_MIN_Y)
                    chunk->applyEdit(x, y, z, BlockID::Bedrock);
                else if (worldY < height - 2)
                    chunk->applyEdit(x, y, z, BlockID::Stone);
                else if (worldY < height - 1)
                    chunk->applyEdit(x, y, z, BlockID::Dirt);
                else if (worldY < height)
                    chunk->applyEdit(x, y, z, BlockID::Grass);
                else
                    chunk->applyEdit(x, y, z, BlockID::Air);
            }
        }
    }

    // Reuse the client-style two-pass decoration rules for consistency with the client worldgen.
    applyClientDecorationPass(cm, *chunk, pos);

    bool shouldDecorateExisting = false;
    ServerChunk *existingChunk = nullptr;
    {
        std::lock_guard<std::shared_mutex> lk(cm.mapMutex);
        auto [insertedChunk, inserted] = cm.m_chunks.emplaceIfEmpty(pos, std::move(chunk));
        if (inserted) {
            insertedChunk->markDirty();
            cm.decoratedChunks.insert(pos);
            return;
        }

        existingChunk = insertedChunk;
        shouldDecorateExisting = (cm.decoratedChunks.find(pos) == cm.decoratedChunks.end());
    }

    if (shouldDecorateExisting && existingChunk != nullptr) {
        // Another thread inserted terrain while we were generating; finish decoration in place.
        applyClientDecorationPass(cm, *existingChunk, pos);
        std::lock_guard<std::shared_mutex> lk(cm.mapMutex);
        cm.decoratedChunks.insert(pos);
    }
}

void WorldGen::generateTerrainChunkAt(ChunkManager &cm, const glm::ivec3 &pos) {
    if (!cm.inBounds(pos))
        return;

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
                if (worldY == WORLD_MIN_Y)
                    chunk->applyEdit(x, y, z, BlockID::Bedrock);
                else if (worldY < height - 2)
                    chunk->applyEdit(x, y, z, BlockID::Stone);
                else if (worldY < height - 1)
                    chunk->applyEdit(x, y, z, BlockID::Dirt);
                else if (worldY < height)
                    chunk->applyEdit(x, y, z, BlockID::Grass);
                else
                    chunk->applyEdit(x, y, z, BlockID::Air);
            }
        }
    }

    {
        std::lock_guard<std::shared_mutex> lk(cm.mapMutex);
        if (cm.m_chunks.containsLoaded(pos)) {
            return;
        }

        auto [insertedChunk, inserted] = cm.m_chunks.emplaceIfEmpty(pos, std::move(chunk));
        if (insertedChunk != nullptr) {
            insertedChunk->markDirty();
        }
        cm.decoratedChunks.erase(pos);
    }
}

void WorldGen::decorateChunkAt(ChunkManager &cm, const glm::ivec3 &pos) {
    ServerChunk *chunkPtr = nullptr;
    {
        std::lock_guard<std::shared_mutex> lk(cm.mapMutex);
        chunkPtr = cm.m_chunks.get(pos);
    }
    if (!chunkPtr)
        return;

    applyClientDecorationPass(cm, *chunkPtr, pos);

    std::lock_guard<std::shared_mutex> lk(cm.mapMutex);
    cm.decoratedChunks.insert(pos);
}

void WorldGen::placeTree(
    ChunkManager &cm, ServerChunk &chunk, const glm::ivec3 &basePos, std::mt19937 &gen
) {
    std::uniform_real_distribution<float> rand01(0.0f, 1.0f);
    std::uniform_int_distribution<> driftDist(-1, 1);
    std::uniform_int_distribution<> dirDist(0, 7);

    int baseRadius = std::uniform_int_distribution<>(1, 3)(gen);

    int trunkHeight =
        std::uniform_int_distribution<>(12 + baseRadius * 3, 16 + baseRadius * 5)(gen);

    float taperStrength = std::uniform_real_distribution<float>(0.75f, 1.35f)(gen);

    int branchCount = std::uniform_int_distribution<>(3 + baseRadius, 5 + baseRadius)(gen);

    auto placeLog = [&](const glm::ivec3 &p) { cm.setBlockSafe(chunk, p, BlockID::Log); };

    auto placeLeaf = [&](const glm::ivec3 &p) {
        if (cm.getBlockSafe(chunk, p) == BlockID::Air) {
            cm.setBlockSafe(chunk, p, BlockID::Leaves);
        }
    };

    auto trunkRadiusAt = [&](int y) {
        float t = float(y) / float(glm::max(1, trunkHeight - 1));

        float r = float(baseRadius) * std::pow(1.0f - t, taperStrength);

        if (t < 0.78f)
            r = glm::max(r, 1.0f);

        if (t > 0.88f)
            r *= 0.65f;

        return glm::max(0, int(std::round(r)));
    };

    auto placeTrunkSection = [&](const glm::ivec3 &center, int radius, bool extraIrregularity) {
        if (radius <= 0) {
            placeLog(center);
            return;
        }

        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dz = -radius; dz <= radius; ++dz) {
                float dist = std::sqrt(float(dx * dx + dz * dz));

                float edgeNoise = rand01(gen) * 0.45f;

                if (extraIrregularity && rand01(gen) < 0.18f)
                    edgeNoise += 0.45f;

                if (dist <= float(radius) + edgeNoise) {
                    placeLog(center + glm::ivec3(dx, 0, dz));
                }
            }
        }
    };

    auto placeLeafBlob = [&](const glm::ivec3 &center, int radius) {
        for (int dx = -radius; dx <= radius; ++dx) {
            for (int dy = -radius; dy <= radius; ++dy) {
                for (int dz = -radius; dz <= radius; ++dz) {
                    float dist = std::sqrt(float(dx * dx + dy * dy + dz * dz));

                    if (dist <= radius + 0.35f) {
                        float edge = dist / float(radius);
                        float skipChance = glm::smoothstep(0.65f, 1.0f, edge) * 0.55f;

                        if (rand01(gen) < skipChance)
                            continue;

                        placeLeaf(center + glm::ivec3(dx, dy, dz));
                    }
                }
            }
        }
    };

    glm::ivec3 dirs[8] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {1, 0, 1}, {-1, 0, 1}, {1, 0, -1}, {-1, 0, -1}
    };

    std::vector<glm::ivec3> trunkCenters;
    trunkCenters.reserve(trunkHeight + 4);

    int trunkOffsetX = 0;
    int trunkOffsetZ = 0;

    for (int y = 0; y < trunkHeight; ++y) {
        if (y > trunkHeight / 3 && rand01(gen) < 0.10f) {
            trunkOffsetX += driftDist(gen);
            trunkOffsetX = glm::clamp(trunkOffsetX, -2, 2);
        }

        if (y > trunkHeight / 3 && rand01(gen) < 0.10f) {
            trunkOffsetZ += driftDist(gen);
            trunkOffsetZ = glm::clamp(trunkOffsetZ, -2, 2);
        }

        glm::ivec3 trunkPos = basePos + glm::ivec3(trunkOffsetX, y, trunkOffsetZ);
        trunkCenters.push_back(trunkPos);

        int radius = trunkRadiusAt(y);
        bool irregular = y < trunkHeight * 0.25f || rand01(gen) < 0.25f;

        placeTrunkSection(trunkPos, radius, irregular);
    }

    for (int b = 0; b < branchCount; ++b) {
        int branchY = std::uniform_int_distribution<>(trunkHeight / 2, trunkHeight - 5)(gen);
        int branchLength = std::uniform_int_distribution<>(4 + baseRadius, 7 + baseRadius * 2)(gen);

        glm::ivec3 dir = dirs[dirDist(gen)];
        glm::ivec3 start = trunkCenters[branchY];

        glm::ivec3 branchEnd = start;

        for (int i = 1; i <= branchLength; ++i) {
            float progress = float(i) / float(branchLength);

            int bx = int(std::round(dir.x * i));
            int bz = int(std::round(dir.z * i));
            int by = int(progress * progress * float(3 + baseRadius));

            glm::ivec3 p = start + glm::ivec3(bx, by, bz);

            placeLog(p);

            if (i <= baseRadius + 1) {
                placeLog(p + glm::ivec3(0, -1, 0));
            }

            if (baseRadius >= 2 && i < branchLength / 2 && rand01(gen) < 0.45f) {
                if (dir.x != 0)
                    placeLog(p + glm::ivec3(0, 0, 1));
                if (dir.z != 0)
                    placeLog(p + glm::ivec3(1, 0, 0));
            }

            branchEnd = p;
        }

        int leafRadius = std::uniform_int_distribution<>(2 + baseRadius, 3 + baseRadius)(gen);

        placeLeafBlob(branchEnd, leafRadius);
        placeLeafBlob(branchEnd + glm::ivec3(0, 1, 0), glm::max(2, leafRadius - 1));
    }

    glm::ivec3 top = trunkCenters.back();

    int crownRadius = 3 + baseRadius;

    placeLeafBlob(top + glm::ivec3(0, -3, 0), crownRadius);
    placeLeafBlob(top + glm::ivec3(0, 0, 0), crownRadius + 1);
    placeLeafBlob(top + glm::ivec3(0, 3, 0), glm::max(2, crownRadius - 1));

    for (int y = trunkHeight - 5; y < trunkHeight; ++y) {
        if (y >= 0 && y < int(trunkCenters.size())) {
            placeTrunkSection(trunkCenters[y], glm::max(0, trunkRadiusAt(y)), false);
        }
    }
}
