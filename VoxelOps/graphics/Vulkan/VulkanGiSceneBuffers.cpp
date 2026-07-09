#include "VulkanGiSceneBuffers.hpp"

#include "../../voxels/Voxel.hpp"
#include "vulkan/VulkanContext.hpp"
#include "vulkan/VulkanUtils.hpp"

#include <algorithm>
#include <array>
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

    using BlockSolidTable = std::array<uint8_t, static_cast<size_t>(BlockID::COUNT)>;

    const BlockSolidTable &traceSolidTable() {
        static const BlockSolidTable table = [] {
            BlockSolidTable result{};
            result.fill(1u);
            for (size_t i = 0; i < result.size(); ++i) {
                const BlockID id = static_cast<BlockID>(i);
                const auto it = blockTypes.find(id);
                result[i] = (it == blockTypes.end()) ? uint8_t{id != BlockID::Air}
                                                     : uint8_t{it->second.isSolid};
            }
            result[static_cast<size_t>(BlockID::Air)] = 0u;
            return result;
        }();
        return table;
    }

    bool isTraceSolid(BlockID id) {
        const uint32_t index = static_cast<uint32_t>(id);
        const BlockSolidTable &table = traceSolidTable();
        if (index >= table.size()) {
            return id != BlockID::Air;
        }
        return table[index] != 0u;
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

    uint32_t packedMaterialWordCount(uint32_t voxelCount) {
        return (voxelCount + 3u) >> 2u;
    }

    uint32_t packMaterialWord(BlockID b0, BlockID b1, BlockID b2, BlockID b3) {
        return (static_cast<uint32_t>(b0) & 0xFFu) |
               ((static_cast<uint32_t>(b1) & 0xFFu) << 8u) |
               ((static_cast<uint32_t>(b2) & 0xFFu) << 16u) |
               ((static_cast<uint32_t>(b3) & 0xFFu) << 24u);
    }

    uint32_t traceSolidNibble(
        const BlockSolidTable &solidTable,
        BlockID b0,
        BlockID b1,
        BlockID b2,
        BlockID b3
    ) {
        auto solidBit = [&](BlockID id, uint32_t bit) {
            const uint32_t index = static_cast<uint32_t>(id);
            const bool solid = (index < solidTable.size()) ? (solidTable[index] != 0u)
                                                           : (id != BlockID::Air);
            return solid ? bit : 0u;
        };
        return solidBit(b0, 1u) | solidBit(b1, 2u) | solidBit(b2, 4u) | solidBit(b3, 8u);
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
        m_hostMaterialWords.clear();
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
    const uint32_t materialWordCount = packedMaterialWordCount(voxelCount);
    const bool topologyChanged =
        !m_valid || m_minBlocks != minBlocks || m_dims != dims || m_wordCount != wordCount;
    const bool signatureChanged =
        !m_valid || m_signatureXor != signatureXor || m_signatureSum != signatureSum ||
        m_chunkCount != signatureCount;
    if (!topologyChanged && !signatureChanged) {
        return true;
    }

    if (m_hostMaterialWords.size() != static_cast<size_t>(materialWordCount)) {
        m_hostMaterialWords.assign(static_cast<size_t>(materialWordCount), 0u);
    }
    if (m_hostOccupancyWords.size() != static_cast<size_t>(wordCount)) {
        m_hostOccupancyWords.assign(static_cast<size_t>(wordCount), 0u);
    }
    if (topologyChanged) {
        std::fill(m_hostMaterialWords.begin(), m_hostMaterialWords.end(), 0u);
        std::fill(m_hostOccupancyWords.begin(), m_hostOccupancyWords.end(), 0u);
        m_chunkSnapshotCache.clear();
    }

    const uint32_t yStride = dims.x;
    const uint32_t zStride = dims.x * dims.y;
    const BlockSolidTable &solidTable = traceSolidTable();

    auto applyChunkToHost = [&](const glm::ivec3 &chunkPos,
                                const std::array<BlockID, CHUNK_VOLUME> &chunkBlocks) {
        const glm::ivec3 baseLocal = (chunkPos * CHUNK_SIZE) - minBlocks;
        const uint32_t baseLinear = static_cast<uint32_t>(baseLocal.x) +
                                    yStride * static_cast<uint32_t>(baseLocal.y) +
                                    zStride * static_cast<uint32_t>(baseLocal.z);
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            const uint32_t zLinear = baseLinear + static_cast<uint32_t>(z) * zStride;
            const uint32_t zSource = static_cast<uint32_t>(z * CHUNK_SIZE * CHUNK_SIZE);
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                const uint32_t rowLinear = zLinear + static_cast<uint32_t>(y) * yStride;
                const uint32_t rowSource = zSource + static_cast<uint32_t>(y * CHUNK_SIZE);
                for (int x = 0; x < CHUNK_SIZE; x += 4) {
                    const uint32_t linear = rowLinear + static_cast<uint32_t>(x);
                    const uint32_t source = rowSource + static_cast<uint32_t>(x);
                    const BlockID b0 = chunkBlocks[source + 0u];
                    const BlockID b1 = chunkBlocks[source + 1u];
                    const BlockID b2 = chunkBlocks[source + 2u];
                    const BlockID b3 = chunkBlocks[source + 3u];

                    m_hostMaterialWords[linear >> 2u] = packMaterialWord(b0, b1, b2, b3);

                    const uint32_t occupancyShift = linear & 31u;
                    const uint32_t occupancyMask = 0xFu << occupancyShift;
                    const uint32_t occupancyBits =
                        traceSolidNibble(solidTable, b0, b1, b2, b3) << occupancyShift;
                    uint32_t &occupancyWord = m_hostOccupancyWords[linear >> 5u];
                    occupancyWord = (occupancyWord & ~occupancyMask) | occupancyBits;
                }
            }
        }
    };

    auto clearChunkInHost = [&](const glm::ivec3 &chunkPos) {
        const glm::ivec3 baseLocal = (chunkPos * CHUNK_SIZE) - minBlocks;
        const uint32_t baseLinear = static_cast<uint32_t>(baseLocal.x) +
                                    yStride * static_cast<uint32_t>(baseLocal.y) +
                                    zStride * static_cast<uint32_t>(baseLocal.z);
        for (int z = 0; z < CHUNK_SIZE; ++z) {
            const uint32_t zLinear = baseLinear + static_cast<uint32_t>(z) * zStride;
            for (int y = 0; y < CHUNK_SIZE; ++y) {
                const uint32_t rowLinear = zLinear + static_cast<uint32_t>(y) * yStride;
                for (int x = 0; x < CHUNK_SIZE; x += 4) {
                    const uint32_t linear = rowLinear + static_cast<uint32_t>(x);
                    m_hostMaterialWords[linear >> 2u] = 0u;

                    const uint32_t occupancyShift = linear & 31u;
                    m_hostOccupancyWords[linear >> 5u] &= ~(0xFu << occupancyShift);
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
    const vk::DeviceSize materialBytes =
        static_cast<vk::DeviceSize>(materialWordCount) * sizeof(uint32_t);
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
        void *mapped = VulkanUtils::mapAllocation(slot.occupancyBuffer);
        std::memcpy(mapped, m_hostOccupancyWords.data(), static_cast<size_t>(occupancyBytes));
        VulkanUtils::unmapAllocation(slot.occupancyBuffer);
    }

    {
        void *mapped = VulkanUtils::mapAllocation(slot.traceMaterialBuffer);
        std::memcpy(mapped, m_hostMaterialWords.data(), static_cast<size_t>(materialBytes));
        VulkanUtils::unmapAllocation(slot.traceMaterialBuffer);
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
    const VmaAllocator allocator = context.getVmaAllocator();

    if (slot.occupancyBuffer == nullptr || slot.occupancyCapacityBytes < occupancyBytes) {
        if (slot.occupancyBuffer != nullptr) {
            RetiredBuffers retired{};
            retired.occupancyBuffer = std::move(slot.occupancyBuffer);
            retired.occupancyCapacityBytes = slot.occupancyCapacityBytes;
            retired.retireFrame = frameCounter + kGiRetireDelayFrames;
            m_retiredBuffers.push_back(std::move(retired));
            slot.occupancyCapacityBytes = 0;
        }
        slot.occupancyCapacityBytes = roundGiBufferCapacity(occupancyBytes);
        VulkanUtils::createBuffer(
            allocator,
            slot.occupancyCapacityBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            slot.occupancyBuffer
        );
    }

    if (slot.traceMaterialBuffer == nullptr || slot.materialCapacityBytes < materialBytes) {
        if (slot.traceMaterialBuffer != nullptr) {
            RetiredBuffers retired{};
            retired.traceMaterialBuffer = std::move(slot.traceMaterialBuffer);
            retired.materialCapacityBytes = slot.materialCapacityBytes;
            retired.retireFrame = frameCounter + kGiRetireDelayFrames;
            m_retiredBuffers.push_back(std::move(retired));
            slot.materialCapacityBytes = 0;
        }
        slot.materialCapacityBytes = roundGiBufferCapacity(materialBytes);
        VulkanUtils::createBuffer(
            allocator,
            slot.materialCapacityBytes,
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
            slot.traceMaterialBuffer
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
        static_cast<vk::DeviceSize>(packedMaterialWordCount(voxelCount)) * sizeof(uint32_t);

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
        void *mapped = VulkanUtils::mapAllocation(slot.occupancyBuffer);
        std::memcpy(mapped, latest.occupancyWords.data(), static_cast<size_t>(occupancyBytes));
        VulkanUtils::unmapAllocation(slot.occupancyBuffer);
    }

    if (materialBytes > 0 && !latest.materialWords.empty()) {
        void *mapped = VulkanUtils::mapAllocation(slot.traceMaterialBuffer);
        std::memcpy(mapped, latest.materialWords.data(), static_cast<size_t>(materialBytes));
        VulkanUtils::unmapAllocation(slot.traceMaterialBuffer);
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
            result.materialWords.assign(packedMaterialWordCount(job.voxelCount), 0u);

            const uint32_t yStride = job.dims.x;
            const uint32_t zStride = job.dims.x * job.dims.y;
            const BlockSolidTable &solidTable = traceSolidTable();
            for (const GiBuildChunkSnapshotRef &snapshot : job.chunks) {
                if (!snapshot.blocks) {
                    continue;
                }
                const std::array<BlockID, CHUNK_VOLUME> &chunkBlocks = *snapshot.blocks;
                const glm::ivec3 baseLocal = snapshot.chunkWorldBase - job.minBlocks;
                const uint32_t baseLinear = static_cast<uint32_t>(baseLocal.x) +
                                            yStride * static_cast<uint32_t>(baseLocal.y) +
                                            zStride * static_cast<uint32_t>(baseLocal.z);
                for (int z = 0; z < CHUNK_SIZE; ++z) {
                    const uint32_t zLinear = baseLinear + static_cast<uint32_t>(z) * zStride;
                    const uint32_t zSource = static_cast<uint32_t>(z * CHUNK_SIZE * CHUNK_SIZE);
                    for (int y = 0; y < CHUNK_SIZE; ++y) {
                        const uint32_t rowLinear = zLinear + static_cast<uint32_t>(y) * yStride;
                        const uint32_t rowSource = zSource + static_cast<uint32_t>(y * CHUNK_SIZE);
                        for (int x = 0; x < CHUNK_SIZE; x += 4) {
                            const uint32_t linear = rowLinear + static_cast<uint32_t>(x);
                            const uint32_t source = rowSource + static_cast<uint32_t>(x);
                            const BlockID b0 = chunkBlocks[source + 0u];
                            const BlockID b1 = chunkBlocks[source + 1u];
                            const BlockID b2 = chunkBlocks[source + 2u];
                            const BlockID b3 = chunkBlocks[source + 3u];

                            result.materialWords[linear >> 2u] =
                                packMaterialWord(b0, b1, b2, b3);

                            const uint32_t occupancyShift = linear & 31u;
                            const uint32_t occupancyBits =
                                traceSolidNibble(solidTable, b0, b1, b2, b3) << occupancyShift;
                            result.occupancyWords[linear >> 5u] |= occupancyBits;
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
        VulkanUtils::destroyBuffer(it->occupancyBuffer);
        it->occupancyCapacityBytes = 0;
        VulkanUtils::destroyBuffer(it->traceMaterialBuffer);
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
        VulkanUtils::destroyBuffer(slot.occupancyBuffer);
        slot.occupancyCapacityBytes = 0;
        VulkanUtils::destroyBuffer(slot.traceMaterialBuffer);
        slot.materialCapacityBytes = 0;
    }
    m_bufferSlots.clear(); 
    for (RetiredBuffers &retired : m_retiredBuffers) {
        VulkanUtils::destroyBuffer(retired.occupancyBuffer);
        retired.occupancyCapacityBytes = 0;
        VulkanUtils::destroyBuffer(retired.traceMaterialBuffer);
        retired.materialCapacityBytes = 0;
        retired.retireFrame = 0;
    }
    m_retiredBuffers.clear();
    m_chunkSnapshotCache.clear();
    m_hostOccupancyWords.clear();
    m_hostMaterialWords.clear();
}
