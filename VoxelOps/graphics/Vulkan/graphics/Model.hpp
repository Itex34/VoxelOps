#pragma once
#include <glm/glm.hpp>
#include <assimp/scene.h>

#include <limits>
#include <string>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

#include "graphics/Vulkan/graphics/Mesh.hpp"

struct VkModelLocalTriangle {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    glm::vec3 c{0.0f};
};

class UploadContext;

class VkModel {
public:
    void loadModel(const std::string &path);

    void initGpuResources(
        const vk::raii::Device &device,
        const vk::raii::PhysicalDevice &physicalDevice,
        UploadContext &uploadContext
    );
    void cleanupGpuResources();

    const std::vector<VkMesh> &getMeshes() const {
        return m_meshes;
    }
    std::vector<VkMesh> &getMeshes() {
        return m_meshes;
    }
    const std::vector<std::string> &getMeshTexturePaths() const {
        return m_meshTexturePaths;
    }

    bool hasLocalBounds() const {
        return m_hasLocalBounds;
    }
    const glm::vec3 &getLocalMinBounds() const {
        return m_localMinBounds;
    }
    const glm::vec3 &getLocalMaxBounds() const {
        return m_localMaxBounds;
    }
    const std::vector<VkModelLocalTriangle> &getLocalTriangles() const {
        return m_localTriangles;
    }

private:
    void processNode(aiNode *node, const aiScene *scene, const aiMatrix4x4 &parentTransform);
    VkMesh processMesh(aiMesh *mesh, const aiMatrix4x4 &nodeTransform);
    std::string resolveMeshTexturePath(const aiMesh *mesh, const aiScene *scene) const;
    void finalizeRegionAabbsFromVertices();
    static glm::vec3 transformPoint(const aiMatrix4x4 &transform, const aiVector3D &point);

    std::vector<VkMesh> m_meshes;
    std::vector<std::string> m_meshTexturePaths;
    std::string m_directory;
    glm::vec3 m_localMinBounds{std::numeric_limits<float>::max()};
    glm::vec3 m_localMaxBounds{std::numeric_limits<float>::lowest()};
    bool m_hasLocalBounds = false;
    std::vector<glm::vec3> m_localVertices;
    std::vector<VkModelLocalTriangle> m_localTriangles;
};
