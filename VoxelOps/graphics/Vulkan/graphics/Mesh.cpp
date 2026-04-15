#include "graphics/Vulkan/graphics/Mesh.hpp"

#include "graphics/Vulkan/vulkan/UploadContext.hpp"
#include "graphics/Vulkan/vulkan/VulkanUtils.hpp"

#include <cstddef>
#include <stdexcept>
#include <utility>

vk::VertexInputBindingDescription VkMesh::PackedVoxelVertex::getBindingDescription() {
    vk::VertexInputBindingDescription d{};
    d.binding = 0;
    d.stride = static_cast<uint32_t>(sizeof(PackedVoxelVertex));
    d.inputRate = vk::VertexInputRate::eVertex;
    return d;
}

std::array<vk::VertexInputAttributeDescription, 2> VkMesh::PackedVoxelVertex::getAttributeDescriptions() {
    std::array<vk::VertexInputAttributeDescription, 2> a{};
    a[0].binding = 0;
    a[0].location = 0;
    a[0].format = vk::Format::eR32Uint;
    a[0].offset = static_cast<uint32_t>(offsetof(PackedVoxelVertex, low));

    a[1].binding = 0;
    a[1].location = 1;
    a[1].format = vk::Format::eR32Uint;
    a[1].offset = static_cast<uint32_t>(offsetof(PackedVoxelVertex, high));
    return a;
}

vk::VertexInputBindingDescription VkMesh::Vertex::getBindingDescription() {
    vk::VertexInputBindingDescription d{};
    d.binding = 0;
    d.stride = static_cast<uint32_t>(sizeof(Vertex));
    d.inputRate = vk::VertexInputRate::eVertex;
    return d;
}

std::array<vk::VertexInputAttributeDescription, 2> VkMesh::Vertex::getAttributeDescriptions() {
    std::array<vk::VertexInputAttributeDescription, 2> a{};
    a[0].binding = 0;
    a[0].location = 0;
    a[0].format = vk::Format::eR32G32B32Sfloat;
    a[0].offset = static_cast<uint32_t>(offsetof(Vertex, position));

    a[1].binding = 0;
    a[1].location = 1;
    a[1].format = vk::Format::eR32G32Sfloat;
    a[1].offset = static_cast<uint32_t>(offsetof(Vertex, uv));
    return a;
}

VkMesh::VkMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices) {
    setGeometry(std::move(vertices), std::move(indices));
}

void VkMesh::setGeometry(std::vector<Vertex> vertices, std::vector<uint32_t> indices) {
    m_geometryFormat = GeometryFormat::Float32;
    m_vertices = std::move(vertices);
    m_indices = std::move(indices);
    m_packedVertices.clear();
    m_packedIndices.clear();
    m_indexType = vk::IndexType::eUint32;
    m_indexCount = static_cast<uint32_t>(m_indices.size());
}

void VkMesh::setPackedVoxelGeometry(std::vector<PackedVoxelVertex> vertices, std::vector<uint16_t> indices) {
    m_geometryFormat = GeometryFormat::PackedVoxel;
    m_packedVertices = std::move(vertices);
    m_packedIndices = std::move(indices);
    m_vertices.clear();
    m_indices.clear();
    m_indexType = vk::IndexType::eUint16;
    m_indexCount = static_cast<uint32_t>(m_packedIndices.size());
}

bool VkMesh::hasGeometry() const {
    if (m_geometryFormat == GeometryFormat::PackedVoxel) {
        return !m_packedVertices.empty() && !m_packedIndices.empty();
    }
    return !m_vertices.empty() && !m_indices.empty();
}

void VkMesh::init(
    const vk::raii::Device& device,
    const vk::raii::PhysicalDevice& physicalDevice,
    UploadContext& uploadContext
) {
    cleanup();

    if (m_geometryFormat == GeometryFormat::Float32 && (m_vertices.empty() || m_indices.empty())) {
        m_vertices = createDefaultCubeVertices();
        m_indices = createDefaultCubeIndices();
        m_indexType = vk::IndexType::eUint32;
    }

    if (!hasGeometry()) {
        throw std::runtime_error("VkMesh::init called with empty geometry.");
    }

    const void* vertexData = nullptr;
    size_t vertexCount = 0;
    vk::DeviceSize vertexStride = 0;
    const void* indexData = nullptr;
    size_t indexCount = 0;
    vk::DeviceSize indexStride = 0;

    if (m_geometryFormat == GeometryFormat::PackedVoxel) {
        vertexData = m_packedVertices.data();
        vertexCount = m_packedVertices.size();
        vertexStride = static_cast<vk::DeviceSize>(sizeof(PackedVoxelVertex));
        indexData = m_packedIndices.data();
        indexCount = m_packedIndices.size();
        indexStride = static_cast<vk::DeviceSize>(sizeof(uint16_t));
        m_indexType = vk::IndexType::eUint16;
    }
    else {
        vertexData = m_vertices.data();
        vertexCount = m_vertices.size();
        vertexStride = static_cast<vk::DeviceSize>(sizeof(Vertex));
        indexData = m_indices.data();
        indexCount = m_indices.size();
        indexStride = static_cast<vk::DeviceSize>(sizeof(uint32_t));
        m_indexType = vk::IndexType::eUint32;
    }

    m_indexCount = static_cast<uint32_t>(indexCount);
    const vk::DeviceSize vertexBufferSize = vertexStride * static_cast<vk::DeviceSize>(vertexCount);
    const vk::DeviceSize indexBufferSize = indexStride * static_cast<vk::DeviceSize>(indexCount);

    VulkanUtils::createBuffer(
        device,
        physicalDevice,
        vertexBufferSize,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eVertexBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        m_vertexBuffer,
        m_vertexBufferMemory
    );

    VulkanUtils::createBuffer(
        device,
        physicalDevice,
        indexBufferSize,
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndexBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        m_indexBuffer,
        m_indexBufferMemory
    );

    std::vector<UploadContext::BufferCopyUpload> uploads;
    uploads.reserve(2);
    uploads.push_back(UploadContext::BufferCopyUpload{
        uploadContext.createStagingBuffer(physicalDevice, vertexData, vertexBufferSize),
        *m_vertexBuffer,
        vertexBufferSize
    });
    uploads.push_back(UploadContext::BufferCopyUpload{
        uploadContext.createStagingBuffer(physicalDevice, indexData, indexBufferSize),
        *m_indexBuffer,
        indexBufferSize
    });
    uploadContext.submitCopyBufferBatch(std::move(uploads));
}

void VkMesh::cleanup() {
    m_vertexBuffer.clear();
    m_vertexBufferMemory.clear();
    m_indexBuffer.clear();
    m_indexBufferMemory.clear();
    m_indexCount = 0;
}

void VkMesh::bind(const vk::raii::CommandBuffer& commandBuffer) const {
    const std::array<vk::Buffer, 1> vertexBuffers = { *m_vertexBuffer };
    const std::array<vk::DeviceSize, 1> offsets = { 0 };
    commandBuffer.bindVertexBuffers(0, vertexBuffers, offsets);
    commandBuffer.bindIndexBuffer(*m_indexBuffer, 0, m_indexType);
}

std::vector<VkMesh::Vertex> VkMesh::createDefaultCubeVertices() {
    return {
        { {-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f} }, { { 0.5f, 0.5f, 0.5f}, {1.0f, 0.0f} },
        { { 0.5f, 1.5f, 0.5f}, {1.0f, 1.0f} }, { {-0.5f, 1.5f, 0.5f}, {0.0f, 1.0f} },

        { { 0.5f, 0.5f, -0.5f}, {0.0f, 0.0f} }, { {-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f} },
        { {-0.5f, 1.5f, -0.5f}, {1.0f, 1.0f} }, { { 0.5f, 1.5f, -0.5f}, {0.0f, 1.0f} },

        { {-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f} }, { {-0.5f, 0.5f, 0.5f}, {1.0f, 0.0f} },
        { {-0.5f, 1.5f, 0.5f}, {1.0f, 1.0f} }, { {-0.5f, 1.5f, -0.5f}, {0.0f, 1.0f} },

        { {0.5f, 0.5f, 0.5f}, {0.0f, 0.0f} }, { {0.5f, 0.5f, -0.5f}, {1.0f, 0.0f} },
        { {0.5f, 1.5f, -0.5f}, {1.0f, 1.0f} }, { {0.5f, 1.5f, 0.5f}, {0.0f, 1.0f} },

        { {-0.5f, 1.5f, 0.5f}, {0.0f, 0.0f} }, { { 0.5f, 1.5f, 0.5f}, {1.0f, 0.0f} },
        { { 0.5f, 1.5f, -0.5f}, {1.0f, 1.0f} }, { {-0.5f, 1.5f, -0.5f}, {0.0f, 1.0f} },

        { {-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f} }, { { 0.5f, 0.5f, -0.5f}, {1.0f, 0.0f} },
        { { 0.5f, 0.5f, 0.5f}, {1.0f, 1.0f} }, { {-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f} }
    };
}

std::vector<uint32_t> VkMesh::createDefaultCubeIndices() {
    return {
        0, 1, 2, 2, 3, 0, 4, 5, 6, 6, 7, 4,
        8, 9, 10, 10, 11, 8, 12, 13, 14, 14, 15, 12,
        16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20
    };
}


