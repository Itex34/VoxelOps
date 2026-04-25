#include "graphics/Vulkan/graphics/Model.hpp"

#include "graphics/Vulkan/vulkan/UploadContext.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/material.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
constexpr float kWClipEpsilon = 1.0e-6f;

std::string buildAssimpErrorMessage(const std::string &path, const Assimp::Importer &importer) {
    std::string message = "Failed to load model '" + path + "'";
    const char *importerError = importer.GetErrorString();
    if (importerError && importerError[0] != '\0') {
        message += ": ";
        message += importerError;
    }
    return message;
}
} // namespace

void VkModel::initGpuResources(const vk::raii::Device &device,
                               const vk::raii::PhysicalDevice &physicalDevice,
                               UploadContext &uploadContext) {
    for (VkMesh &mesh : m_meshes) {
        mesh.init(device, physicalDevice, uploadContext);
    }
}

void VkModel::cleanupGpuResources() {
    for (VkMesh &mesh : m_meshes) {
        mesh.cleanup();
    }
}

void VkModel::loadModel(const std::string &path) {
    m_meshes.clear();
    m_meshTexturePaths.clear();
    m_localVertices.clear();
    m_localTriangles.clear();
    m_localMinBounds = glm::vec3(std::numeric_limits<float>::max());
    m_localMaxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    m_hasLocalBounds = false;
    m_directory = std::filesystem::path(path).parent_path().string();

    Assimp::Importer importer;
    const aiScene *scene =
        importer.ReadFile(path, aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
                                    aiProcess_ImproveCacheLocality | aiProcess_FlipUVs);

    if (!scene || !scene->mRootNode || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0) {
        throw std::runtime_error(buildAssimpErrorMessage(path, importer));
    }

    processNode(scene->mRootNode, scene, aiMatrix4x4{});
    finalizeRegionAabbsFromVertices();

    if (m_meshes.empty()) {
        throw std::runtime_error("VkModel '" + path + "' did not contain any renderable meshes.");
    }
}

void VkModel::processNode(aiNode *node, const aiScene *scene, const aiMatrix4x4 &parentTransform) {
    const aiMatrix4x4 nodeTransform = parentTransform * node->mTransformation;

    for (uint32_t i = 0; i < node->mNumMeshes; ++i) {
        aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
        if (!mesh || !mesh->HasPositions()) {
            continue;
        }

        VkMesh processedMesh = processMesh(mesh, nodeTransform);
        if (processedMesh.hasGeometry()) {
            m_meshes.emplace_back(std::move(processedMesh));
            m_meshTexturePaths.emplace_back(resolveMeshTexturePath(mesh, scene));
        }
    }

    for (uint32_t i = 0; i < node->mNumChildren; ++i) {
        processNode(node->mChildren[i], scene, nodeTransform);
    }
}

VkMesh VkModel::processMesh(aiMesh *mesh, const aiMatrix4x4 &nodeTransform) {
    std::vector<VkMesh::Vertex> vertices;
    vertices.reserve(mesh->mNumVertices);

    for (uint32_t i = 0; i < mesh->mNumVertices; ++i) {
        VkMesh::Vertex vertex{};
        vertex.position = transformPoint(nodeTransform, mesh->mVertices[i]);

        if (mesh->HasTextureCoords(0)) {
            vertex.uv = glm::vec2(mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y);
        } else {
            vertex.uv = glm::vec2(0.0f);
        }

        vertices.emplace_back(vertex);
        m_localVertices.emplace_back(vertex.position);
    }

    std::vector<uint32_t> indices;
    indices.reserve(mesh->mNumFaces * 3u);

    for (uint32_t i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace &face = mesh->mFaces[i];
        if (face.mNumIndices < 3) {
            continue;
        }

        for (uint32_t j = 0; j < face.mNumIndices; ++j) {
            indices.emplace_back(face.mIndices[j]);
        }
    }

    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const uint32_t i0 = indices[i];
        const uint32_t i1 = indices[i + 1];
        const uint32_t i2 = indices[i + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        VkModelLocalTriangle triangle{};
        triangle.a = vertices[i0].position;
        triangle.b = vertices[i1].position;
        triangle.c = vertices[i2].position;
        m_localTriangles.emplace_back(triangle);
    }

    return VkMesh(std::move(vertices), std::move(indices));
}

std::string VkModel::resolveMeshTexturePath(const aiMesh *mesh, const aiScene *scene) const {
    if (!mesh || !scene || mesh->mMaterialIndex >= scene->mNumMaterials) {
        return {};
    }

    const aiMaterial *material = scene->mMaterials[mesh->mMaterialIndex];
    if (!material) {
        return {};
    }

    aiString aiTexturePath;
    aiReturn result = material->GetTexture(aiTextureType_BASE_COLOR, 0, &aiTexturePath);
    if (result != aiReturn_SUCCESS) {
        result = material->GetTexture(aiTextureType_DIFFUSE, 0, &aiTexturePath);
    }
    if (result != aiReturn_SUCCESS) {
        return {};
    }

    const std::string texturePath = aiTexturePath.C_Str();
    if (texturePath.empty()) {
        return {};
    }

    // Embedded textures (*0, *1...) are not resolved to files in this loader.
    if (!texturePath.empty() && texturePath.front() == '*') {
        return {};
    }

    const std::filesystem::path sourcePath(texturePath);
    if (sourcePath.is_absolute()) {
        return sourcePath.string();
    }

    const std::filesystem::path combinedPath = std::filesystem::path(m_directory) / sourcePath;
    return combinedPath.lexically_normal().string();
}

void VkModel::finalizeRegionAabbsFromVertices() {
    if (m_localVertices.empty()) {
        m_hasLocalBounds = false;
        return;
    }

    for (const glm::vec3 &v : m_localVertices) {
        m_localMinBounds.x = std::min(m_localMinBounds.x, v.x);
        m_localMinBounds.y = std::min(m_localMinBounds.y, v.y);
        m_localMinBounds.z = std::min(m_localMinBounds.z, v.z);

        m_localMaxBounds.x = std::max(m_localMaxBounds.x, v.x);
        m_localMaxBounds.y = std::max(m_localMaxBounds.y, v.y);
        m_localMaxBounds.z = std::max(m_localMaxBounds.z, v.z);
    }

    m_hasLocalBounds = true;
}

glm::vec3 VkModel::transformPoint(const aiMatrix4x4 &transform, const aiVector3D &point) {
    const float x = (transform.a1 * point.x) + (transform.a2 * point.y) + (transform.a3 * point.z) +
                    transform.a4;
    const float y = (transform.b1 * point.x) + (transform.b2 * point.y) + (transform.b3 * point.z) +
                    transform.b4;
    const float z = (transform.c1 * point.x) + (transform.c2 * point.y) + (transform.c3 * point.z) +
                    transform.c4;
    const float w = (transform.d1 * point.x) + (transform.d2 * point.y) + (transform.d3 * point.z) +
                    transform.d4;

    if (std::abs(w) > kWClipEpsilon) {
        return glm::vec3(x / w, y / w, z / w);
    }
    return glm::vec3(x, y, z);
}
