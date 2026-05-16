#include "VulkanGiSceneBuffers.hpp"

#include "../../voxels/Voxel.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {
    constexpr uint64_t kMinGiRebuildIntervalFrames = 3u;
    constexpr size_t kGiBufferSlotCount = 6u;
    constexpr uint64_t kGiRetireDelayFrames = 24u;
    constexpr uint64_t kGiSlotReuseLagFrames = 6u;
    constexpr int kGiChunkBoundsPaddingXZ = 2;
    constexpr int kGiChunkBoundsPaddingY = 0;

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

    vk::DeviceSize roundGiBufferCapacity(vk::DeviceSize requestedBytes) {
        constexpr vk::DeviceSize kMinCapacity = 4 * 1024;
        vk::DeviceSize capacity = kMinCapacity;
        const vk::DeviceSize target = std::max(requestedBytes, kMinCapacity);
        while (capacity < target && capacity < (std::numeric_limits<vk::DeviceSize>::max() >> 1u)) {
            capacity <<= 1u;
        }
        return capacity;
    }

} // namespace

bool VulkanGiSceneBuffers::rebuild(
    const std::unordered_map<glm::ivec3, Chunk, IVec3Hash> &chunks,
    const VulkanChunkRenderCache &chunkRenderCache,
    VulkanContext &context,
    uint64_t frameCounter
) {
    const bool hasActiveSlot = (m_activeBufferSlot < m_bufferSlots.size());
    const bool hadActiveState = m_valid || hasActiveSlot || m_chunkCount != 0 || m_wordCount != 0;
    const auto &chunkMeshes = chunkRenderCache.getChunkMeshes();
    if (chunkMeshes.empty()) {
        resetActiveState();
        m_chunkSnapshotCache.clear();
        m_hostMaterialIds.clear();
        m_hostOccupancyWords.clear();
        if (hadActiveState) {
            ++m_contentVersion;
        }
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

    const glm::ivec3 chunkPadding(
        kGiChunkBoundsPaddingXZ, kGiChunkBoundsPaddingY, kGiChunkBoundsPaddingXZ
    );

    glm::ivec3 boundsMinChunk = minChunk - chunkPadding;
    glm::ivec3 boundsMaxChunk = maxChunk + chunkPadding;
    if (m_valid && m_dims.x > 0u && m_dims.y > 0u && m_dims.z > 0u) {
        const glm::ivec3 currentMinChunk = m_minBlocks / CHUNK_SIZE;
        const glm::ivec3 currentExtentChunks(
            static_cast<int>(m_dims.x) / CHUNK_SIZE,
            static_cast<int>(m_dims.y) / CHUNK_SIZE,
            static_cast<int>(m_dims.z) / CHUNK_SIZE
        );
        const glm::ivec3 currentMaxChunk = currentMinChunk + currentExtentChunks - glm::ivec3(1);
        const bool meshesFitCurrentBounds =
            minChunk.x >= currentMinChunk.x && minChunk.y >= currentMinChunk.y &&
            minChunk.z >= currentMinChunk.z && maxChunk.x <= currentMaxChunk.x &&
            maxChunk.y <= currentMaxChunk.y && maxChunk.z <= currentMaxChunk.z;
        if (meshesFitCurrentBounds) {
            boundsMinChunk = currentMinChunk;
            boundsMaxChunk = currentMaxChunk;
        }
    }

    const glm::ivec3 minBlocks = boundsMinChunk * CHUNK_SIZE;
    const glm::ivec3 maxBlocksExclusive = (boundsMaxChunk + glm::ivec3(1)) * CHUNK_SIZE;
    const glm::ivec3 dimsI = maxBlocksExclusive - minBlocks;
    if (dimsI.x <= 0 || dimsI.y <= 0 || dimsI.z <= 0) {
        resetActiveState();
        if (hadActiveState) {
            ++m_contentVersion;
        }
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
        resetActiveState();
        if (hadActiveState) {
            ++m_contentVersion;
        }
        return false;
    }
    const uint32_t voxelCount = static_cast<uint32_t>(voxelCount64);
    const uint32_t wordCount = (voxelCount + 31u) >> 5u;
    const bool topologyChanged =
        !m_valid || m_minBlocks != minBlocks || m_dims != dims || m_wordCount != wordCount;
    const bool signatureChanged =
        !m_valid || m_signatureXor != signatureXor || m_signatureSum != signatureSum ||
        m_chunkCount != signatureCount;
    if (!topologyChanged && !signatureChanged) {
        return true;
    }

    if (m_hostMaterialIds.size() != static_cast<size_t>(voxelCount)) {
        m_hostMaterialIds.assign(static_cast<size_t>(voxelCount), 0u);
    }
    if (m_hostOccupancyWords.size() != static_cast<size_t>(wordCount)) {
        m_hostOccupancyWords.assign(static_cast<size_t>(wordCount), 0u);
    }
    if (topologyChanged) {
        std::fill(m_hostMaterialIds.begin(), m_hostMaterialIds.end(), 0u);
        std::fill(m_hostOccupancyWords.begin(), m_hostOccupancyWords.end(), 0u);
        m_chunkSnapshotCache.clear();
    }

    auto applyChunkToHost = [&](const glm::ivec3 &chunkPos,
                                const std::array<BlockID, CHUNK_VOLUME> &chunkBlocks) {
        const glm::ivec3 chunkWorldBase = chunkPos * CHUNK_SIZE;
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    const glm::ivec3 worldPos = chunkWorldBase + glm::ivec3(x, y, z);
                    const glm::ivec3 local = worldPos - minBlocks;
                    const uint32_t linear = static_cast<uint32_t>(local.x) +
                                            dims.x * (static_cast<uint32_t>(local.y) +
                                                      dims.y * static_cast<uint32_t>(local.z));
                    const BlockID id = chunkBlocks[x + CHUNK_SIZE * (y + CHUNK_SIZE * z)];
                    m_hostMaterialIds[linear] = static_cast<uint32_t>(id);
                    const uint32_t wordIndex = linear >> 5u;
                    const uint32_t bitMask = (1u << (linear & 31u));
                    if (isTraceSolid(id)) {
                        m_hostOccupancyWords[wordIndex] |= bitMask;
                    } else {
                        m_hostOccupancyWords[wordIndex] &= ~bitMask;
                    }
                }
            }
        }
    };

    auto clearChunkInHost = [&](const glm::ivec3 &chunkPos) {
        const glm::ivec3 chunkWorldBase = chunkPos * CHUNK_SIZE;
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                for (int x = 0; x < CHUNK_SIZE; ++x) {
                    const glm::ivec3 worldPos = chunkWorldBase + glm::ivec3(x, y, z);
                    const glm::ivec3 local = worldPos - minBlocks;
                    const uint32_t linear = static_cast<uint32_t>(local.x) +
                                            dims.x * (static_cast<uint32_t>(local.y) +
                                                      dims.y * static_cast<uint32_t>(local.z));
                    m_hostMaterialIds[linear] = 0u;
                    const uint32_t wordIndex = linear >> 5u;
                    const uint32_t bitMask = (1u << (linear & 31u));
                    m_hostOccupancyWords[wordIndex] &= ~bitMask;
                }
            }
        }
    };

    std::vector<glm::ivec3> dirtyChunks;
    dirtyChunks.reserve(chunkMeshes.size());
    if (topologyChanged) {
        for (const auto &[chunkPos, cached] : chunkMeshes) {
            const auto chunkIt = chunks.find(chunkPos);
            if (chunkIt == chunks.end()) {
                auto snapshotIt = m_chunkSnapshotCache.find(chunkPos);
                if (snapshotIt != m_chunkSnapshotCache.end()) {
                    clearChunkInHost(chunkPos);
                    m_chunkSnapshotCache.erase(snapshotIt);
                    dirtyChunks.push_back(chunkPos);
                }
                continue;
            }
            auto blocks = std::make_shared<std::array<BlockID, CHUNK_VOLUME>>();
            chunkIt->second.copyBlocks(*blocks);
            CachedChunkSnapshot &snapshot = m_chunkSnapshotCache[chunkPos];
            snapshot.revision = cached.revision;
            snapshot.blocks = std::move(blocks);
            applyChunkToHost(chunkPos, *snapshot.blocks);
            dirtyChunks.push_back(chunkPos);
        }
    } else {
        for (auto it = m_chunkSnapshotCache.begin(); it != m_chunkSnapshotCache.end();) {
            if (chunkMeshes.find(it->first) == chunkMeshes.end()) {
                clearChunkInHost(it->first);
                dirtyChunks.push_back(it->first);
                it = m_chunkSnapshotCache.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto &[chunkPos, cached] : chunkMeshes) {
            const auto chunkIt = chunks.find(chunkPos);
            if (chunkIt == chunks.end()) {
                continue;
            }
            CachedChunkSnapshot &snapshot = m_chunkSnapshotCache[chunkPos];
            if (snapshot.blocks != nullptr && snapshot.revision == cached.revision) {
                continue;
            }
            if (snapshot.blocks == nullptr) {
                snapshot.blocks = std::make_shared<std::array<BlockID, CHUNK_VOLUME>>();
            }
            chunkIt->second.copyBlocks(*snapshot.blocks);
            snapshot.revision = cached.revision;
            applyChunkToHost(chunkPos, *snapshot.blocks);
            dirtyChunks.push_back(chunkPos);
        }
    }

    if (!topologyChanged && dirtyChunks.empty()) {
        m_signatureXor = signatureXor;
        m_signatureSum = signatureSum;
        m_chunkCount = signatureCount;
        return true;
    }

    const vk::DeviceSize occupancyBytes = static_cast<vk::DeviceSize>(wordCount) * sizeof(uint32_t);
    const vk::DeviceSize materialBytes = static_cast<vk::DeviceSize>(voxelCount) * sizeof(uint32_t);
    if (m_bufferSlots.empty()) {
        m_bufferSlots.resize(kGiBufferSlotCount);
    }
    size_t slotIndex = static_cast<size_t>(-1);
    const size_t start = m_nextBufferSlot % m_bufferSlots.size();
    for (size_t i = 0; i < m_bufferSlots.size(); ++i) {
        const size_t candidate = (start + i) % m_bufferSlots.size();
        if (candidate == m_activeBufferSlot) {
            continue;
        }
        const GiBufferSlot &slot = m_bufferSlots[candidate];
        if (slot.safeReuseAfterFrame > frameCounter) {
            continue;
        }
        slotIndex = candidate;
        break;
    }
    if (slotIndex == static_cast<size_t>(-1)) {
        m_bufferSlots.emplace_back();
        slotIndex = m_bufferSlots.size() - 1u;
    }
    m_nextBufferSlot = (slotIndex + 1u) % m_bufferSlots.size();
    GiBufferSlot &slot = m_bufferSlots[slotIndex];
    ensureBufferSlotCapacity(slot, context, frameCounter, occupancyBytes, materialBytes);

    {
        void *mapped = slot.occupancyBufferMemory.mapMemory(0, occupancyBytes);
        std::memcpy(mapped, m_hostOccupancyWords.data(), static_cast<size_t>(occupancyBytes));
        slot.occupancyBufferMemory.unmapMemory();
    }

    {
        void *mapped = slot.traceMaterialBufferMemory.mapMemory(0, materialBytes);
        std::memcpy(mapped, m_hostMaterialIds.data(), static_cast<size_t>(materialBytes));
        slot.traceMaterialBufferMemory.unmapMemory();
    }

    const glm::ivec3 maxBlocksExclusiveNow =
        minBlocks + glm::ivec3(static_cast<int>(dims.x), static_cast<int>(dims.y), static_cast<int>(dims.z));
    m_minBlocks = minBlocks;
    m_dims = dims;
    m_wordCount = wordCount;
    m_worldBoundsXy =
        glm::ivec4(minBlocks.x, maxBlocksExclusiveNow.x - 1, minBlocks.y, maxBlocksExclusiveNow.y - 1);
    m_worldBoundsZ = glm::ivec4(minBlocks.z, maxBlocksExclusiveNow.z - 1, 0, 0);
    m_signatureXor = signatureXor;
    m_signatureSum = signatureSum;
    m_chunkCount = signatureCount;
    m_lastRebuildFrame = frameCounter;
    m_activeBufferSlot = slotIndex;
    slot.safeReuseAfterFrame = frameCounter + kGiSlotReuseLagFrames;
    m_valid = true;
    ++m_contentVersion;
    return true;
}

void VulkanGiSceneBuffers::ensureBufferSlotCapacity(
    GiBufferSlot &slot,
    VulkanContext &context,
    uint64_t frameCounter,
    vk::DeviceSize occupancyBytes,
    vk::DeviceSize materialBytes
) {
    const vk::raii::Device &device = context.getDevice();
    const vk::raii::PhysicalDevice &physicalDevice = context.getPhysicalDevice();

    if (slot.occupancyBuffer == nullptr || slot.occupancyBufferMemory == nullptr ||
        slot.occupancyCapacityBytes < occupancyBytes) {
        if (slot.occupancyBuffer != nullptr || slot.occupancyBufferMemory != nullptr) {
            RetiredBuffers retired{};
            retired.occupancyBuffer = std::move(slot.occupancyBuffer);
            retired.occupancyBufferMemory = std::move(slot.occupancyBufferMemory);
            retired.occupancyCapacityBytes = slot.occupancyCapacityBytes;
            retired.retireFrame = frameCounter + kGiRetireDelayFrames;
            m_retiredBuffers.push_back(std::move(retired));
            slot.occupancyCapacityBytes = 0;
        }
        slot.occupancyCapacityBytes = roundGiBufferCapacity(occupancyBytes);
        VulkanUtils::createBuffer(
            device,
            physicalDevice,
            slot.occupancyCapacityBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            slot.occupancyBuffer,
            slot.occupancyBufferMemory
        );
    }

    if (slot.traceMaterialBuffer == nullptr || slot.traceMaterialBufferMemory == nullptr ||
        slot.materialCapacityBytes < materialBytes) {
        if (slot.traceMaterialBuffer != nullptr || slot.traceMaterialBufferMemory != nullptr) {
            RetiredBuffers retired{};
            retired.traceMaterialBuffer = std::move(slot.traceMaterialBuffer);
            retired.traceMaterialBufferMemory = std::move(slot.traceMaterialBufferMemory);
            retired.materialCapacityBytes = slot.materialCapacityBytes;
            retired.retireFrame = frameCounter + kGiRetireDelayFrames;
            m_retiredBuffers.push_back(std::move(retired));
            slot.materialCapacityBytes = 0;
        }
        slot.materialCapacityBytes = roundGiBufferCapacity(materialBytes);
        VulkanUtils::createBuffer(
            device,
            physicalDevice,
            slot.materialCapacityBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            slot.traceMaterialBuffer,
            slot.traceMaterialBufferMemory
        );
    }
}

bool VulkanGiSceneBuffers::consumeCompletedBuild(
    VulkanContext &context,
    uint64_t frameCounter
) {
    (void)frameCounter;
    GiBuildResult latest{};
    bool hasLatest = false;
    {
        std::lock_guard<std::mutex> lock(m_buildStateMutex);
        while (!m_completedBuilds.empty()) {
            GiBuildResult result = std::move(m_completedBuilds.front());
            m_completedBuilds.pop_front();
            if (result.submitId <= m_lastAppliedSubmitId) {
                continue;
            }
            if (!hasLatest || result.submitId > latest.submitId) {
                latest = std::move(result);
                hasLatest = true;
            }
        }
    }
    if (!hasLatest) {
        return false;
    }

    const uint64_t signatureXor = latest.signatureXor;
    const uint64_t signatureSum = latest.signatureSum;
    const size_t signatureCount = latest.signatureCount;
    const glm::ivec3 minBlocks = latest.minBlocks;
    const glm::uvec3 dims = latest.dims;
    const uint32_t wordCount = latest.wordCount;
    const uint32_t voxelCount = latest.voxelCount;

    const vk::DeviceSize occupancyBytes =
        static_cast<vk::DeviceSize>(wordCount) * sizeof(uint32_t);
    const vk::DeviceSize materialBytes =
        static_cast<vk::DeviceSize>(voxelCount) * sizeof(uint32_t);

    if (m_bufferSlots.empty()) {
        m_bufferSlots.resize(kGiBufferSlotCount);
    }
    size_t slotIndex = static_cast<size_t>(-1);
    if (!m_bufferSlots.empty()) {
        const size_t start = m_nextBufferSlot % m_bufferSlots.size();
        for (size_t i = 0; i < m_bufferSlots.size(); ++i) {
            const size_t candidate = (start + i) % m_bufferSlots.size();
            if (candidate == m_activeBufferSlot) {
                continue;
            }
            const GiBufferSlot &slot = m_bufferSlots[candidate];
            if (slot.safeReuseAfterFrame > frameCounter) {
                continue;
            }
            slotIndex = candidate;
            break;
        }
    }
    if (slotIndex == static_cast<size_t>(-1)) {
        m_bufferSlots.emplace_back();
        slotIndex = m_bufferSlots.size() - 1u;
    }
    m_nextBufferSlot = (slotIndex + 1u) % m_bufferSlots.size();
    GiBufferSlot &slot = m_bufferSlots[slotIndex];
    ensureBufferSlotCapacity(slot, context, frameCounter, occupancyBytes, materialBytes);

    if (occupancyBytes > 0 && !latest.occupancyWords.empty()) {
        void *mapped = slot.occupancyBufferMemory.mapMemory(0, occupancyBytes);
        std::memcpy(mapped, latest.occupancyWords.data(), static_cast<size_t>(occupancyBytes));
        slot.occupancyBufferMemory.unmapMemory();
    }

    if (materialBytes > 0 && !latest.materialIds.empty()) {
        void *mapped = slot.traceMaterialBufferMemory.mapMemory(0, materialBytes);
        std::memcpy(mapped, latest.materialIds.data(), static_cast<size_t>(materialBytes));
        slot.traceMaterialBufferMemory.unmapMemory();
    }

    m_minBlocks = minBlocks;
    m_dims = dims;
    m_wordCount = wordCount;
    const glm::ivec3 maxBlocksExclusive =
        minBlocks + glm::ivec3(static_cast<int>(dims.x), static_cast<int>(dims.y), static_cast<int>(dims.z));
    m_worldBoundsXy =
        glm::ivec4(minBlocks.x, maxBlocksExclusive.x - 1, minBlocks.y, maxBlocksExclusive.y - 1);
    m_worldBoundsZ = glm::ivec4(minBlocks.z, maxBlocksExclusive.z - 1, 0, 0);
    m_signatureXor = signatureXor;
    m_signatureSum = signatureSum;
    m_chunkCount = signatureCount;
    m_lastRebuildFrame = frameCounter;
    m_lastAppliedSubmitId = latest.submitId;
    m_activeBufferSlot = slotIndex;
    slot.safeReuseAfterFrame = frameCounter + kGiSlotReuseLagFrames;
    m_valid = true;
    ++m_contentVersion;
    return true;
}

void VulkanGiSceneBuffers::enqueueBuildJob(GiBuildJob &&job) {
    m_buildWorkerPool.enqueue([this, job = std::move(job)]() mutable {
        try {
            GiBuildResult result{};
            result.submitId = job.submitId;
            result.signatureXor = job.signatureXor;
            result.signatureSum = job.signatureSum;
            result.signatureCount = job.signatureCount;
            result.minBlocks = job.minBlocks;
            result.dims = job.dims;
            result.wordCount = job.wordCount;
            result.voxelCount = job.voxelCount;
            result.occupancyWords.assign(job.wordCount, 0u);
            result.materialIds.assign(job.voxelCount, 0u);

            for (const GiBuildChunkSnapshotRef &snapshot : job.chunks) {
                if (!snapshot.blocks) {
                    continue;
                }
                const std::array<BlockID, CHUNK_VOLUME> &chunkBlocks = *snapshot.blocks;
                for (int z = 0; z < CHUNK_SIZE; ++z) {
                    for (int y = 0; y < CHUNK_SIZE; ++y) {
                        for (int x = 0; x < CHUNK_SIZE; ++x) {
                            const BlockID id =
                                chunkBlocks[x + CHUNK_SIZE * (y + CHUNK_SIZE * z)];
                            if (id == BlockID::Air) {
                                continue;
                            }
                            const glm::ivec3 worldPos = snapshot.chunkWorldBase + glm::ivec3(x, y, z);
                            const glm::ivec3 local = worldPos - job.minBlocks;
                            const uint32_t linear = static_cast<uint32_t>(local.x) +
                                                    job.dims.x * (static_cast<uint32_t>(local.y) +
                                                                  job.dims.y * static_cast<uint32_t>(local.z));
                            result.materialIds[linear] = static_cast<uint32_t>(id);
                            if (isTraceSolid(id)) {
                                result.occupancyWords[linear >> 5u] |= (1u << (linear & 31u));
                            }
                        }
                    }
                }
            }

            std::lock_guard<std::mutex> lock(m_buildStateMutex);
            if (m_inFlightSubmitId == job.submitId) {
                m_inFlightSubmitId = 0;
            }
            m_buildInFlight = false;
            if (m_acceptBuildJobs) {
                m_completedBuilds.push_back(std::move(result));
                while (m_completedBuilds.size() > 3u) {
                    m_completedBuilds.pop_front();
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(m_buildStateMutex);
            if (m_inFlightSubmitId == job.submitId) {
                m_inFlightSubmitId = 0;
            }
            m_buildInFlight = false;
        }
    });
}

void VulkanGiSceneBuffers::applyToLighting(GiLightingData &lighting) const {
    if (!m_valid || m_activeBufferSlot >= m_bufferSlots.size()) {
        return;
    }
    const GiBufferSlot &slot = m_bufferSlots[m_activeBufferSlot];
    if (slot.occupancyBuffer == nullptr || slot.traceMaterialBuffer == nullptr) {
        return;
    }

    lighting.shadowOccupancyBuffer = *slot.occupancyBuffer;
    lighting.traceMaterialBuffer = *slot.traceMaterialBuffer;
    lighting.shadowOccupancyMinBlocks = m_minBlocks;
    lighting.shadowOccupancyDims = m_dims;
    lighting.shadowOccupancyWordCount = m_wordCount;
    lighting.shadowWorldBoundsXy = m_worldBoundsXy;
    lighting.shadowWorldBoundsZ = m_worldBoundsZ;
}

bool VulkanGiSceneBuffers::hasPendingBuildWork() const {
    std::lock_guard<std::mutex> lock(m_buildStateMutex);
    return m_buildInFlight || !m_completedBuilds.empty();
}

void VulkanGiSceneBuffers::collectRetiredBuffers(uint64_t frameCounter) {
    for (auto it = m_retiredBuffers.begin(); it != m_retiredBuffers.end();) {
        if (it->retireFrame > frameCounter) {
            ++it;
            continue;
        }
        it->occupancyBuffer.clear();
        it->occupancyBufferMemory.clear();
        it->occupancyCapacityBytes = 0;
        it->traceMaterialBuffer.clear();
        it->traceMaterialBufferMemory.clear();
        it->materialCapacityBytes = 0;
        it = m_retiredBuffers.erase(it);
    }
}

void VulkanGiSceneBuffers::resetActiveState() {
    m_activeBufferSlot = static_cast<size_t>(-1);
    m_nextBufferSlot = 0;
    m_minBlocks = glm::ivec3(0);
    m_dims = glm::uvec3(0u);
    m_worldBoundsXy = glm::ivec4(0);
    m_worldBoundsZ = glm::ivec4(0);
    m_wordCount = 0;
    m_signatureXor = 0;
    m_signatureSum = 0;
    m_lastRebuildFrame = 0;
    m_chunkCount = 0;
    m_valid = false;
    m_lastAppliedSubmitId = 0;
}

void VulkanGiSceneBuffers::cleanup() {
    {
        std::lock_guard<std::mutex> lock(m_buildStateMutex);
        m_acceptBuildJobs = false;
    }
    {
        auto drainPromise = std::make_shared<std::promise<void>>();
        std::future<void> drainFuture = drainPromise->get_future();
        m_buildWorkerPool.enqueue([drainPromise]() { drainPromise->set_value(); });
        drainFuture.wait();
    }
    {
        std::lock_guard<std::mutex> lock(m_buildStateMutex);
        m_completedBuilds.clear();
        m_buildInFlight = false;
        m_inFlightSubmitId = 0;
        m_lastAppliedSubmitId = 0;
        m_acceptBuildJobs = true;
    }

    resetActiveState();
    for (GiBufferSlot &slot : m_bufferSlots) {
        slot.occupancyBuffer.clear();
        slot.occupancyBufferMemory.clear();
        slot.occupancyCapacityBytes = 0;
        slot.traceMaterialBuffer.clear();
        slot.traceMaterialBufferMemory.clear();
        slot.materialCapacityBytes = 0;
    }
    m_bufferSlots.clear(); 
    for (RetiredBuffers &retired : m_retiredBuffers) {
        retired.occupancyBuffer.clear();
        retired.occupancyBufferMemory.clear();
        retired.occupancyCapacityBytes = 0;
        retired.traceMaterialBuffer.clear();
        retired.traceMaterialBufferMemory.clear();
        retired.materialCapacityBytes = 0;
        retired.retireFrame = 0;
    }
    m_retiredBuffers.clear();
    m_chunkSnapshotCache.clear();
    m_hostOccupancyWords.clear();
    m_hostMaterialIds.clear();
}
