#include "VulkanGiSceneBuffers.hpp"

#include "../../voxels/Voxel.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace {
    bool isTraceSolid(BlockID id) {
        if (id == BlockID::Air) {
            return false;
        }
        const auto it = blockTypes.find(id);
        if (it == blockTypes.end()) {
            return true;
        }
        return it->second.isSolid;
    }

    uint64_t mixGiTraceSignature(uint64_t v) {
        v ^= v >> 33;
        v *= 0xff51afd7ed558ccdULL;
        v ^= v >> 33;
        v *= 0xc4ceb9fe1a85ec53ULL;
        v ^= v >> 33;
        return v;
    }
} // namespace

bool VulkanGiSceneBuffers::rebuild(
    const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &chunks,
    const VulkanChunkRenderCache &chunkRenderCache,
    VulkanContext &context
) {
    const auto &chunkMeshes = chunkRenderCache.getChunkMeshes();
    if (chunkMeshes.empty()) {
        cleanup();
        return false;
    }

    glm::ivec3 minChunk(std::numeric_limits<int>::max());
    glm::ivec3 maxChunk(std::numeric_limits<int>::lowest());
    uint64_t signatureXor = 0;
    uint64_t signatureSum = 0;
    size_t signatureCount = 0;
    for (const auto &[chunkPos, cached] : chunkMeshes) {
        minChunk = glm::min(minChunk, chunkPos);
        maxChunk = glm::max(maxChunk, chunkPos);

        uint64_t packedPos = (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.x)) << 0u) ^
                             (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.y)) << 21u) ^
                             (static_cast<uint64_t>(static_cast<uint32_t>(chunkPos.z)) << 42u);
        const uint64_t entry = mixGiTraceSignature(packedPos ^ cached.revision);
        signatureXor ^= entry;
        signatureSum += entry;
        ++signatureCount;
    }

    const glm::ivec3 minBlocks = minChunk * CHUNK_SIZE;
    const glm::ivec3 maxBlocksExclusive = (maxChunk + glm::ivec3(1)) * CHUNK_SIZE;
    const glm::ivec3 dimsI = maxBlocksExclusive - minBlocks;
    if (dimsI.x <= 0 || dimsI.y <= 0 || dimsI.z <= 0) {
        cleanup();
        return false;
    }

    const glm::uvec3 dims(
        static_cast<uint32_t>(dimsI.x),
        static_cast<uint32_t>(dimsI.y),
        static_cast<uint32_t>(dimsI.z)
    );
    const uint64_t voxelCount64 = static_cast<uint64_t>(dims.x) * static_cast<uint64_t>(dims.y) *
                                  static_cast<uint64_t>(dims.z);
    if (voxelCount64 == 0 ||
        voxelCount64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        cleanup();
        return false;
    }
    const uint32_t voxelCount = static_cast<uint32_t>(voxelCount64);
    const uint32_t wordCount = (voxelCount + 31u) >> 5u;
    const vk::DeviceSize occupancyBytes = static_cast<vk::DeviceSize>(wordCount) * sizeof(uint32_t);
    const vk::DeviceSize materialBytes = static_cast<vk::DeviceSize>(voxelCount) * sizeof(uint32_t);

    const bool geometryUnchanged =
        m_valid && m_signatureXor == signatureXor && m_signatureSum == signatureSum &&
        m_chunkCount == signatureCount && m_minBlocks == minBlocks && m_dims == dims &&
        m_wordCount == wordCount && m_occupancyBuffer != nullptr && m_materialBuffer != nullptr;
    if (geometryUnchanged) {
        return true;
    }

    cleanup();

    const vk::raii::Device &device = context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = context.getPhysicalDevice();
    VulkanUtils::createBuffer(
        device,
        physicalDevice,
        occupancyBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        m_occupancyBuffer,
        m_occupancyBufferMemory
    );
    VulkanUtils::createBuffer(
        device,
        physicalDevice,
        materialBytes,
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        m_materialBuffer,
        m_materialBufferMemory
    );

    std::vector<uint32_t> occupancyWords(wordCount, 0u);
    std::vector<uint32_t> materialIds(voxelCount, 0u);

    for (const auto &[chunkPos, _cached] : chunkMeshes) {
        const auto chunkIt = chunks.find(chunkPos);
        if (chunkIt == chunks.end()) {
            continue;
        }
        const Chunk &chunk = chunkIt->second;
        const glm::ivec3 chunkWorldBase = chunkPos * CHUNK_SIZE;
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    const BlockID id = chunk.getBlockUnchecked(x, y, z);
                    if (id == BlockID::Air) {
                        continue;
                    }

                    const glm::ivec3 worldPos = chunkWorldBase + glm::ivec3(x, y, z);
                    const glm::ivec3 local = worldPos - minBlocks;
                    const uint32_t linear = static_cast<uint32_t>(local.x) +
                                            dims.x * (static_cast<uint32_t>(local.y) +
                                                      dims.y * static_cast<uint32_t>(local.z));
                    materialIds[linear] = static_cast<uint32_t>(id);
                    if (isTraceSolid(id)) {
                        occupancyWords[linear >> 5u] |= (1u << (linear & 31u));
                    }
                }
            }
        }
    }

    if (occupancyBytes > 0) {
        void *mapped = m_occupancyBufferMemory.mapMemory(0, occupancyBytes);
        std::memcpy(mapped, occupancyWords.data(), static_cast<size_t>(occupancyBytes));
        m_occupancyBufferMemory.unmapMemory();
    }
    if (materialBytes > 0) {
        void *mapped = m_materialBufferMemory.mapMemory(0, materialBytes);
        std::memcpy(mapped, materialIds.data(), static_cast<size_t>(materialBytes));
        m_materialBufferMemory.unmapMemory();
    }

    m_minBlocks = minBlocks;
    m_dims = dims;
    m_wordCount = wordCount;
    m_worldBoundsXy =
        glm::ivec4(minBlocks.x, maxBlocksExclusive.x - 1, minBlocks.y, maxBlocksExclusive.y - 1);
    m_worldBoundsZ = glm::ivec4(minBlocks.z, maxBlocksExclusive.z - 1, 0, 0);
    m_signatureXor = signatureXor;
    m_signatureSum = signatureSum;
    m_chunkCount = signatureCount;
    m_valid = true;
    return true;
}

void VulkanGiSceneBuffers::applyToLighting(GiLightingData &lighting) const {
    if (!m_valid || m_occupancyBuffer == nullptr || m_materialBuffer == nullptr) {
        return;
    }

    lighting.shadowOccupancyBuffer = *m_occupancyBuffer;
    lighting.traceMaterialBuffer = *m_materialBuffer;
    lighting.shadowOccupancyMinBlocks = m_minBlocks;
    lighting.shadowOccupancyDims = m_dims;
    lighting.shadowOccupancyWordCount = m_wordCount;
    lighting.shadowWorldBoundsXy = m_worldBoundsXy;
    lighting.shadowWorldBoundsZ = m_worldBoundsZ;
}

void VulkanGiSceneBuffers::cleanup() {
    m_occupancyBuffer.clear();
    m_occupancyBufferMemory.clear();
    m_materialBuffer.clear();
    m_materialBufferMemory.clear();
    m_minBlocks = glm::ivec3(0);
    m_dims = glm::uvec3(0u);
    m_worldBoundsXy = glm::ivec4(0);
    m_worldBoundsZ = glm::ivec4(0);
    m_wordCount = 0;
    m_signatureXor = 0;
    m_signatureSum = 0;
    m_chunkCount = 0;
    m_valid = false;
}
