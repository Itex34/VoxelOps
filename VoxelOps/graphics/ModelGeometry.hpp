#pragma once

#include "ModelTypes.hpp"

#include <array>
#include <limits>
#include <string>
#include <vector>

#include <assimp/scene.h>
#include <glm/vec3.hpp>

class ModelGeometry {
public:
    bool loadFromFile(const std::string &path, std::string *outError = nullptr);

    [[nodiscard]] float getLocalMinY() const noexcept;
    [[nodiscard]] float getLocalHeight() const noexcept;
    [[nodiscard]] glm::vec3 getLocalMinBounds() const noexcept;
    [[nodiscard]] glm::vec3 getLocalMaxBounds() const noexcept;
    [[nodiscard]] const ModelRegionAabb &getLocalRegionAabb(ModelRegion region) const noexcept;
    [[nodiscard]] const std::vector<ModelLocalTriangle> &getLocalTriangles() const noexcept;

private:
    void reset();
    void processNode(aiNode *node, const aiScene *scene, const aiMatrix4x4 &parentTransform);
    void processMesh(aiMesh *mesh, const aiMatrix4x4 &nodeTransform);
    void finalizeRegionAabbs();

    glm::vec3 m_localMinBounds{std::numeric_limits<float>::max()};
    glm::vec3 m_localMaxBounds{std::numeric_limits<float>::lowest()};
    bool m_hasLocalBounds = false;
    std::vector<glm::vec3> m_localVertices;
    std::vector<ModelLocalTriangle> m_localTriangles;
    std::array<ModelRegionAabb, 3> m_localRegionAabbs{};
};
