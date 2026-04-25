#pragma once

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

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

    VkMesh(const VkMesh &) = delete;
    VkMesh &operator=(const VkMesh &) = delete;
    VkMesh(VkMesh &&) noexcept = default;
    VkMesh &operator=(VkMesh &&) noexcept = default;

    void setGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices);
    void setPackedVoxelGeometry(std::vector<PackedVoxelVertex> vertices,
                                std::vector<uint16_t> indices);

    void init(const vk::raii::Device &device, const vk::raii::PhysicalDevice &physicalDevice,
              UploadContext &uploadContext);
    void cleanup();

    bool hasGeometry() const;
    const std::vector<Vertex> &getVertices() const {
        return m_vertices;
    }
    const std::vector<uint32_t> &getIndices() const {
        return m_indices;
    }

    void bind(const vk::raii::CommandBuffer &commandBuffer) const;
    uint32_t getIndexCount() const {
        return m_indexCount;
    }

  private:
    enum class GeometryFormat : uint8_t { Float32, PackedVoxel };

    static std::vector<Vertex> createDefaultCubeVertices();
    static std::vector<uint32_t> createDefaultCubeIndices();

    GeometryFormat m_geometryFormat = GeometryFormat::Float32;
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<PackedVoxelVertex> m_packedVertices;
    std::vector<uint16_t> m_packedIndices;

    vk::raii::Buffer m_vertexBuffer{nullptr};
    vk::raii::DeviceMemory m_vertexBufferMemory{nullptr};
    vk::raii::Buffer m_indexBuffer{nullptr};
    vk::raii::DeviceMemory m_indexBufferMemory{nullptr};
    vk::IndexType m_indexType = vk::IndexType::eUint32;
    uint32_t m_indexCount = 0;
};
