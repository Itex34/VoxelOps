#pragma once

#include "../../../Shared/gamemode/GameModeWorldConfig.hpp"
#include "../voxels/ServerChunk.hpp"

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class ServerChunkGrid {
public:
    explicit ServerChunkGrid(ChunkWorldBounds bounds)
        : m_bounds(bounds)
        , m_slots(bounds.slotCount())
        , m_loadedIndexBySlot(bounds.slotCount(), kInvalidLoadedIndex) {}

    [[nodiscard]] const ChunkWorldBounds &bounds() const noexcept {
        return m_bounds;
    }

    [[nodiscard]] std::size_t slotCount() const noexcept {
        return m_slots.size();
    }

    [[nodiscard]] std::size_t loadedCount() const noexcept {
        return m_loadedSlots.size();
    }

    [[nodiscard]] bool containsPosition(const glm::ivec3 &pos) const noexcept {
        return m_bounds.contains(pos);
    }

    [[nodiscard]] bool containsLoaded(const glm::ivec3 &pos) const noexcept {
        const std::optional<std::size_t> index = indexOf(pos);
        return index.has_value() && static_cast<bool>(m_slots[*index]);
    }

    [[nodiscard]] ServerChunk *get(const glm::ivec3 &pos) noexcept {
        const std::optional<std::size_t> index = indexOf(pos);
        return index.has_value() ? m_slots[*index].get() : nullptr;
    }

    [[nodiscard]] ServerChunk *get(const glm::ivec3 &pos) const noexcept {
        const std::optional<std::size_t> index = indexOf(pos);
        return index.has_value() ? m_slots[*index].get() : nullptr;
    }

    std::pair<ServerChunk *, bool>
    emplaceIfEmpty(const glm::ivec3 &pos, std::unique_ptr<ServerChunk> chunk) {
        const std::optional<std::size_t> index = indexOf(pos);
        if (!index.has_value()) {
            return {nullptr, false};
        }

        std::unique_ptr<ServerChunk> &slot = m_slots[*index];
        if (slot) {
            return {slot.get(), false};
        }

        slot = std::move(chunk);
        m_loadedIndexBySlot[*index] = m_loadedSlots.size();
        m_loadedSlots.push_back(*index);
        return {slot.get(), true};
    }

    void erase(const glm::ivec3 &pos) {
        const std::optional<std::size_t> index = indexOf(pos);
        if (!index.has_value()) {
            return;
        }

        std::unique_ptr<ServerChunk> &slot = m_slots[*index];
        if (slot) {
            slot.reset();
            removeLoadedSlot(*index);
        }
    }

    template <typename Fn> void forEachLoaded(Fn &&fn) {
        for (std::size_t index : m_loadedSlots) {
            std::unique_ptr<ServerChunk> &chunk = m_slots[index];
            if (chunk) {
                fn(positionOf(index), *chunk);
            }
        }
    }

    template <typename Fn> void forEachLoaded(Fn &&fn) const {
        for (std::size_t index : m_loadedSlots) {
            const std::unique_ptr<ServerChunk> &chunk = m_slots[index];
            if (chunk) {
                fn(positionOf(index), *chunk);
            }
        }
    }

private:
    [[nodiscard]] std::optional<std::size_t> indexOf(const glm::ivec3 &pos) const noexcept {
        if (!m_bounds.contains(pos)) {
            return std::nullopt;
        }

        const int x = pos.x - m_bounds.minChunk.x;
        const int y = pos.y - m_bounds.minChunk.y;
        const int z = pos.z - m_bounds.minChunk.z;

        return static_cast<std::size_t>(x + m_bounds.sizeX() * (z + m_bounds.sizeZ() * y));
    }

    [[nodiscard]] glm::ivec3 positionOf(std::size_t index) const noexcept {
        const int sizeX = m_bounds.sizeX();
        const int sizeZ = m_bounds.sizeZ();
        const int layerSize = sizeX * sizeZ;
        const int y = static_cast<int>(index / static_cast<std::size_t>(layerSize));
        const int layerIndex = static_cast<int>(index % static_cast<std::size_t>(layerSize));
        const int z = layerIndex / sizeX;
        const int x = layerIndex % sizeX;
        return glm::ivec3(
            m_bounds.minChunk.x + x, m_bounds.minChunk.y + y, m_bounds.minChunk.z + z
        );
    }

    void removeLoadedSlot(std::size_t slotIndex) noexcept {
        const std::size_t loadedIndex = m_loadedIndexBySlot[slotIndex];
        if (loadedIndex == kInvalidLoadedIndex) {
            return;
        }

        const std::size_t lastSlotIndex = m_loadedSlots.back();
        m_loadedSlots[loadedIndex] = lastSlotIndex;
        m_loadedIndexBySlot[lastSlotIndex] = loadedIndex;
        m_loadedSlots.pop_back();
        m_loadedIndexBySlot[slotIndex] = kInvalidLoadedIndex;
    }

    static constexpr std::size_t kInvalidLoadedIndex = std::numeric_limits<std::size_t>::max();

    ChunkWorldBounds m_bounds;
    std::vector<std::unique_ptr<ServerChunk>> m_slots;
    std::vector<std::size_t> m_loadedSlots;
    std::vector<std::size_t> m_loadedIndexBySlot;
};
