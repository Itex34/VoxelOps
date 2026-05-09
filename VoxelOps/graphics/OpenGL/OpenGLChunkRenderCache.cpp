#include "OpenGLChunkRenderCache.hpp"

#include "RegionMeshBuffer.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {
    constexpr double kRegionRebuildLogThresholdMs = 10.0;
    constexpr bool kEnableRegionLifecycleLogs = false;
} // namespace

OpenGLChunkRenderCache::Region::Region(glm::ivec3 pos, size_t vertexBytes_, size_t indexBytes_)
    : regionPos(pos)
    , gpu(std::make_unique<RegionMeshBuffer>(vertexBytes_, indexBytes_))
    , vertexBytes(vertexBytes_)
    , indexBytes(indexBytes_) {}

int OpenGLChunkRenderCache::floorDiv(int a, int b) {
    int q = a / b;
    int r = a % b;
    if ((r != 0) && ((r > 0) != (b > 0))) {
        q--;
    }
    return q;
}

glm::ivec3 OpenGLChunkRenderCache::chunkToRegionPos(const glm::ivec3 &chunkPos) {
    return glm::ivec3(
        floorDiv(chunkPos.x, REGION_SIZE),
        floorDiv(chunkPos.y, REGION_SIZE),
        floorDiv(chunkPos.z, REGION_SIZE)
    );
}

OpenGLChunkRenderCache::Region &OpenGLChunkRenderCache::getOrCreateRegion(const glm::ivec3 &chunkPos) {
    const glm::ivec3 regionPos = chunkToRegionPos(chunkPos);
    auto it = m_regions.find(regionPos);
    if (it != m_regions.end()) {
        return it->second;
    }

    auto [newIt, inserted] =
        m_regions.emplace(regionPos, Region(regionPos, REGION_VERTEX_BYTES, REGION_INDEX_BYTES));
    (void)inserted;
    if (kEnableRegionLifecycleLogs) {
        std::cout << "[OpenGLChunkRenderCache] Created region at (" << regionPos.x << ", "
                  << regionPos.y << ", " << regionPos.z << ")\n";
    }
    return newIt->second;
}

void OpenGLChunkRenderCache::pruneMissingMeshes(
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes
) {
    for (auto regionIt = m_regions.begin(); regionIt != m_regions.end();) {
        Region &region = regionIt->second;
        for (auto chunkIt = region.chunks.begin(); chunkIt != region.chunks.end();) {
            if (cpuMeshes.find(chunkIt->first) == cpuMeshes.end()) {
                region.gpu->destroyChunkMesh(chunkIt->second.mesh);
                chunkIt = region.chunks.erase(chunkIt);
            } else {
                ++chunkIt;
            }
        }

        if (region.chunks.empty()) {
            regionIt = m_regions.erase(regionIt);
        } else {
            ++regionIt;
        }
    }
}

bool OpenGLChunkRenderCache::rebuildRegion(
    const glm::ivec3 &regionPos,
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes,
    size_t reserveVertices,
    size_t reserveIndices
) {
    auto it = m_regions.find(regionPos);
    if (it == m_regions.end()) {
        return false;
    }

    Region &oldRegion = it->second;
    const auto rebuildStart = std::chrono::steady_clock::now();
    const size_t chunkCount = oldRegion.chunks.size();

    struct BuiltChunkData {
        glm::ivec3 chunkPos;
        std::vector<VoxelVertex> vertices;
        std::vector<uint16_t> indices;
        uint64_t revision = 0;
    };

    std::vector<BuiltChunkData> rebuiltData;
    rebuiltData.reserve(oldRegion.chunks.size());

    size_t requiredVertices = reserveVertices;
    size_t requiredIndices = reserveIndices;
    for (const auto &[chunkPos, regionMesh] : oldRegion.chunks) {
        auto cpuIt = cpuMeshes.find(chunkPos);
        if (cpuIt == cpuMeshes.end()) {
            continue;
        }

        const CpuChunkMesh &cpuMesh = cpuIt->second;
        requiredVertices += cpuMesh.vertices.size();
        requiredIndices += cpuMesh.indices.size();
        rebuiltData.push_back({chunkPos, cpuMesh.vertices, cpuMesh.indices, regionMesh.revision});
    }

    size_t newVertexBytes = oldRegion.vertexBytes;
    size_t newIndexBytes = oldRegion.indexBytes;

    const auto vertexCapacityFromBytes = [](size_t bytes) -> size_t {
        return bytes / sizeof(VoxelVertex);
    };
    const auto indexCapacityFromBytes = [](size_t bytes) -> size_t {
        return bytes / sizeof(uint16_t);
    };

    while (vertexCapacityFromBytes(newVertexBytes) < requiredVertices) {
        newVertexBytes *= 2;
    }
    while (indexCapacityFromBytes(newIndexBytes) < requiredIndices) {
        newIndexBytes *= 2;
    }

    auto newGpu = std::make_unique<RegionMeshBuffer>(newVertexBytes, newIndexBytes);
    std::unordered_map<glm::ivec3, RegionChunkMesh, IVec3Hash> newMeshes;
    newMeshes.reserve(rebuiltData.size());

    for (auto &entry : rebuiltData) {
        ChunkMesh mesh = newGpu->createChunkMesh(entry.vertices, entry.indices);
        if (!mesh.valid) {
            std::cerr << "[OpenGLChunkRenderCache] Region rebuild failed\n";
            return false;
        }
        newMeshes.emplace(entry.chunkPos, RegionChunkMesh{std::move(mesh), entry.revision});
    }

    oldRegion.vertexBytes = newVertexBytes;
    oldRegion.indexBytes = newIndexBytes;
    oldRegion.gpu = std::move(newGpu);
    oldRegion.chunks = std::move(newMeshes);

    const auto rebuildEnd = std::chrono::steady_clock::now();
    const double rebuildMs =
        std::chrono::duration<double, std::milli>(rebuildEnd - rebuildStart).count();
    if (rebuildMs >= kRegionRebuildLogThresholdMs) {
        std::cerr << "[chunk/region] rebuildMs=" << rebuildMs << " region=(" << regionPos.x << ","
                  << regionPos.y << "," << regionPos.z << ")"
                  << " chunks=" << chunkCount << " verts=" << requiredVertices
                  << " idx=" << requiredIndices << " vboBytes=" << oldRegion.vertexBytes
                  << " eboBytes=" << oldRegion.indexBytes << "\n";
    }

    return true;
}

void OpenGLChunkRenderCache::syncFromCpuChunkMeshes(
    const std::unordered_map<glm::ivec3, CpuChunkMesh, IVec3Hash> &cpuMeshes
) {
    pruneMissingMeshes(cpuMeshes);

    for (const auto &[chunkPos, cpuMesh] : cpuMeshes) {
        if (cpuMesh.vertices.empty() || cpuMesh.indices.empty()) {
            continue;
        }

        Region &region = getOrCreateRegion(chunkPos);
        auto existingIt = region.chunks.find(chunkPos);
        if (existingIt != region.chunks.end()) {
            if (existingIt->second.revision == cpuMesh.revision) {
                continue;
            }
            region.gpu->destroyChunkMesh(existingIt->second.mesh);
            region.chunks.erase(existingIt);
        }

        ChunkMesh mesh = region.gpu->createChunkMesh(cpuMesh.vertices, cpuMesh.indices);
        if (mesh.status == ChunkMeshStatus::OutOfMemory) {
            const glm::ivec3 regionPos = chunkToRegionPos(chunkPos);
            const bool rebuilt = rebuildRegion(
                regionPos, cpuMeshes, cpuMesh.vertices.size(), cpuMesh.indices.size()
            );
            if (!rebuilt) {
                std::cerr << "[OpenGLChunkRenderCache] Region rebuild failed permanently\n";
                continue;
            }

            Region &rebuiltRegion = getOrCreateRegion(chunkPos);
            mesh = rebuiltRegion.gpu->createChunkMesh(cpuMesh.vertices, cpuMesh.indices);
            if (!mesh.valid) {
                std::cerr << "[OpenGLChunkRenderCache] Chunk mesh upload failed after rebuild\n";
                continue;
            }
            rebuiltRegion.chunks.emplace(
                chunkPos, RegionChunkMesh{std::move(mesh), cpuMesh.revision}
            );
            continue;
        }

        if (!mesh.valid) {
            std::cerr << "[OpenGLChunkRenderCache] Chunk mesh upload failed\n";
            continue;
        }

        region.chunks.emplace(chunkPos, RegionChunkMesh{std::move(mesh), cpuMesh.revision});
    }
}
