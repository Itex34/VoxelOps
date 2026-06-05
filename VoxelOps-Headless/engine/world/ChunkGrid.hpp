#pragma once

#include "../../../Shared/gamemode/GameModeWorldConfig.hpp"
#include "../voxels/ServerChunk.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class ServerChunkGrid {
public:
    explicit ServerChunkGrid(ChunkWorldBounds bounds)
        : m_bounds(bounds)
        , m_slots(bounds.slotCount()) {}

    [[nodiscard]] const ChunkWorldBounds &bounds() const noexcept {
        return m_bounds;
    }

    [[nodiscard]] std::size_t slotCount() const noexcept {
        return m_slots.size();
    }

    [[nodiscard]] std::size_t loadedCount() const noexcept {
        return m_loadedCount;
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

    std::pair<ServerChunk *, bool> emplaceIfEmpty(
        const glm::ivec3 &pos, std::unique_ptr<ServerChunk> chunk
    ) {
        const std::optional<std::size_t> index = indexOf(pos);
        if (!index.has_value()) {
            return {nullptr, false};
        }

        std::unique_ptr<ServerChunk> &slot = m_slots[*index];
        if (slot) {
            return {slot.get(), false};
        }

        slot = std::move(chunk);
        ++m_loadedCount;
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
            --m_loadedCount;
        }
    }

    template <typename Fn>
    void forEachLoaded(Fn &&fn) {
        for (int y = 0; y < m_bounds.sizeY(); ++y) {
            for (int z = 0; z < m_bounds.sizeZ(); ++z) {
                for (int x = 0; x < m_bounds.sizeX(); ++x) {
                    const std::size_t index = static_cast<std::size_t>(
                        x + m_bounds.sizeX() * (z + m_bounds.sizeZ() * y)
                    );
                    std::unique_ptr<ServerChunk> &chunk = m_slots[index];
                    if (chunk) {
                        fn(
                            glm::ivec3(
                                m_bounds.minChunk.x + x,
                                m_bounds.minChunk.y + y,
                                m_bounds.minChunk.z + z
                            ),
                            *chunk
                        );
                    }
                }
            }
        }
    }

    template <typename Fn>
    void forEachLoaded(Fn &&fn) const {
        for (int y = 0; y < m_bounds.sizeY(); ++y) {
            for (int z = 0; z < m_bounds.sizeZ(); ++z) {
                for (int x = 0; x < m_bounds.sizeX(); ++x) {
                    const std::size_t index = static_cast<std::size_t>(
                        x + m_bounds.sizeX() * (z + m_bounds.sizeZ() * y)
                    );
                    const std::unique_ptr<ServerChunk> &chunk = m_slots[index];
                    if (chunk) {
                        fn(
                            glm::ivec3(
                                m_bounds.minChunk.x + x,
                                m_bounds.minChunk.y + y,
                                m_bounds.minChunk.z + z
                            ),
                            *chunk
                        );
                    }
                }
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

        return static_cast<std::size_t>(
            x + m_bounds.sizeX() * (z + m_bounds.sizeZ() * y)
        );
    }

    ChunkWorldBounds m_bounds;
    std::vector<std::unique_ptr<ServerChunk>> m_slots;
    std::size_t m_loadedCount = 0;
};
