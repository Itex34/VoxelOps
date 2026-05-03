#include "ModelGeometry.hpp"

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <array>

namespace {
    constexpr float kLegsRatio = 0.45f;
    constexpr float kBodyRatio = 0.37f;
    constexpr float kHeadRatio = 0.18f;
    static_assert(
        kLegsRatio + kBodyRatio + kHeadRatio > 0.99f, "Region ratios must cover full height."
    );
} // namespace

bool ModelGeometry::loadFromFile(const std::string &path, std::string *outError) {
    reset();

    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        if (outError != nullptr) {
            *outError = importer.GetErrorString();
        }
        return false;
    }

    processNode(scene->mRootNode, scene, aiMatrix4x4());
    finalizeRegionAabbs();
    return m_hasLocalBounds;
}

float ModelGeometry::getLocalMinY() const noexcept {
    return m_hasLocalBounds ? m_localMinBounds.y : 0.0f;
}

float ModelGeometry::getLocalHeight() const noexcept {
    if (!m_hasLocalBounds) {
        return 1.0f;
    }
    return std::max(m_localMaxBounds.y - m_localMinBounds.y, 1e-4f);
}

glm::vec3 ModelGeometry::getLocalMinBounds() const noexcept {
    return m_hasLocalBounds ? m_localMinBounds : glm::vec3(0.0f);
}

glm::vec3 ModelGeometry::getLocalMaxBounds() const noexcept {
    return m_hasLocalBounds ? m_localMaxBounds : glm::vec3(0.0f);
}

const ModelRegionAabb &ModelGeometry::getLocalRegionAabb(ModelRegion region) const noexcept {
    return m_localRegionAabbs[static_cast<size_t>(region)];
}

const std::vector<ModelLocalTriangle> &ModelGeometry::getLocalTriangles() const noexcept {
    return m_localTriangles;
}

void ModelGeometry::reset() {
    m_localVertices.clear();
    m_localTriangles.clear();
    m_localMinBounds = glm::vec3(std::numeric_limits<float>::max());
    m_localMaxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    m_hasLocalBounds = false;
    for (ModelRegionAabb &region : m_localRegionAabbs) {
        region = ModelRegionAabb{};
    }
}

void ModelGeometry::processNode(
    aiNode *node, const aiScene *scene, const aiMatrix4x4 &parentTransform
) {
    const aiMatrix4x4 currentTransform = parentTransform * node->mTransformation;
    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        aiMesh *mesh = scene->mMeshes[node->mMeshes[i]];
        processMesh(mesh, currentTransform);
    }
    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        processNode(node->mChildren[i], scene, currentTransform);
    }
}

void ModelGeometry::processMesh(aiMesh *mesh, const aiMatrix4x4 &nodeTransform) {
    std::vector<glm::vec3> vertices;
    vertices.reserve(mesh->mNumVertices);
    for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
        const aiVector3D transformed = nodeTransform * mesh->mVertices[i];
        const glm::vec3 v(transformed.x, transformed.y, transformed.z);
        vertices.push_back(v);
        m_localVertices.push_back(v);
        m_localMinBounds = glm::min(m_localMinBounds, v);
        m_localMaxBounds = glm::max(m_localMaxBounds, v);
        m_hasLocalBounds = true;
    }

    for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
        const aiFace &face = mesh->mFaces[i];
        if (face.mNumIndices != 3) {
            continue;
        }
        const unsigned int ia = face.mIndices[0];
        const unsigned int ib = face.mIndices[1];
        const unsigned int ic = face.mIndices[2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
            continue;
        }
        ModelLocalTriangle tri;
        tri.a = vertices[ia];
        tri.b = vertices[ib];
        tri.c = vertices[ic];
        tri.region = ModelRegion::Body;
        m_localTriangles.push_back(tri);
    }
}

void ModelGeometry::finalizeRegionAabbs() {
    if (!m_hasLocalBounds || m_localVertices.empty()) {
        return;
    }

    const float minY = m_localMinBounds.y;
    const float maxY = m_localMaxBounds.y;
    const float height = std::max(maxY - minY, 1e-4f);
    const float legsTop = minY + height * kLegsRatio;
    const float bodyTop = legsTop + height * kBodyRatio;

    std::array<glm::vec3, 3> regionMin{
        glm::vec3(std::numeric_limits<float>::max()),
        glm::vec3(std::numeric_limits<float>::max()),
        glm::vec3(std::numeric_limits<float>::max())
    };
    std::array<glm::vec3, 3> regionMax{
        glm::vec3(std::numeric_limits<float>::lowest()),
        glm::vec3(std::numeric_limits<float>::lowest()),
        glm::vec3(std::numeric_limits<float>::lowest())
    };
    std::array<bool, 3> hasRegion{false, false, false};

    for (const glm::vec3 &v : m_localVertices) {
        size_t regionIdx = static_cast<size_t>(ModelRegion::Head);
        if (v.y <= legsTop) {
            regionIdx = static_cast<size_t>(ModelRegion::Legs);
        } else if (v.y <= bodyTop) {
            regionIdx = static_cast<size_t>(ModelRegion::Body);
        }
        regionMin[regionIdx] = glm::min(regionMin[regionIdx], v);
        regionMax[regionIdx] = glm::max(regionMax[regionIdx], v);
        hasRegion[regionIdx] = true;
    }

    const std::array<float, 4> yCuts{minY, legsTop, bodyTop, maxY};
    for (size_t i = 0; i < 3; ++i) {
        ModelRegionAabb &out = m_localRegionAabbs[i];
        if (hasRegion[i]) {
            out.min = regionMin[i];
            out.max = regionMax[i];
        } else {
            out.min = glm::vec3(m_localMinBounds.x, yCuts[i], m_localMinBounds.z);
            out.max = glm::vec3(m_localMaxBounds.x, yCuts[i + 1], m_localMaxBounds.z);
        }
        if (out.max.y <= out.min.y) {
            out.max.y = out.min.y + 1e-4f;
        }
        if (out.max.x <= out.min.x) {
            out.max.x = out.min.x + 1e-4f;
        }
        if (out.max.z <= out.min.z) {
            out.max.z = out.min.z + 1e-4f;
        }
        out.valid = true;
    }

    for (ModelLocalTriangle &tri : m_localTriangles) {
        const float centerY = (tri.a.y + tri.b.y + tri.c.y) / 3.0f;
        if (centerY <= legsTop) {
            tri.region = ModelRegion::Legs;
        } else if (centerY <= bodyTop) {
            tri.region = ModelRegion::Body;
        } else {
            tri.region = ModelRegion::Head;
        }
    }
}
