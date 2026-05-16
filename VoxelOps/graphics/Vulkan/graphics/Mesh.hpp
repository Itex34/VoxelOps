#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <vector>

class UploadContext;

class VkMesh {
public:
    struct PackedVoxelVertex {
        uint32_t low = 0;
        uint32_t high = 0;

        static vk::VertexInputBindingDescription getBindingDescription();
        static std::array<vk::VertexInputAttributeDescription, 2> getAttributeDescriptions();
    };

    struct Vertex {
        glm::vec3 position;
        glm::vec2 uv;

        static vk::VertexInputBindingDescription getBindingDescription();
        static std::array<vk::VertexInputAttributeDescription, 2> getAttributeDescriptions();
    };

    VkMesh() = default;
    VkMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
    ~VkMesh();

    VkMesh(const VkMesh &) = delete;
    VkMesh &operator=(const VkMesh &) = delete;
    VkMesh(VkMesh &&other) noexcept;
    VkMesh &operator=(VkMesh &&other) noexcept;

    void setGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
    void
    setPackedVoxelGeometry(std::vector<PackedVoxelVertex> vertices, std::vector<uint16_t> indices);

    void init(
        const vk::raii::Device &device,
        const vk::raii::PhysicalDevice &physicalDevice,
        VmaAllocator allocator,
        UploadContext &uploadContext
    );
    void cleanup();

    bool hasGeometry() const;
    const std::vector<Vertex> &getVertices() const {
        return m_vertices;
    }
    const std::vector<uint32_t> &getIndices() const {
        return m_indices;
    }
    const std::vector<PackedVoxelVertex> &getPackedVertices() const {
        return m_packedVertices;
    }
    const std::vector<uint16_t> &getPackedIndices() const {
        return m_packedIndices;
    }
    bool isPackedVoxelGeometry() const {
        return m_geometryFormat == GeometryFormat::PackedVoxel;
    }

    void bind(const vk::raii::CommandBuffer &commandBuffer) const;
    uint32_t getIndexCount() const {
        return m_indexCount;
    }

private:
    enum class GeometryFormat : uint8_t { Float32, PackedVoxel };

    static std::vector<Vertex> createDefaultCubeVertices();
    static std::vector<uint32_t> createDefaultCubeIndices();
    void moveFrom(VkMesh &&other) noexcept;
    void destroyGpuBuffers();

    GeometryFormat m_geometryFormat = GeometryFormat::Float32;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<PackedVoxelVertex> m_packedVertices;
    std::vector<uint16_t> m_packedIndices;

    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_vertexAllocation = VK_NULL_HANDLE;
    vk::DeviceSize m_vertexCapacityBytes = 0;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VmaAllocation m_indexAllocation = VK_NULL_HANDLE;
    vk::DeviceSize m_indexCapacityBytes = 0;
    vk::IndexType m_indexType = vk::IndexType::eUint32;
    uint32_t m_indexCount = 0;
};
