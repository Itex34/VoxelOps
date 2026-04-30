#pragma once

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <stb_image.h>

#include "../ModelTypes.hpp"
#include "Shader.hpp"
#include "OpenGLMesh.hpp"

class OpenGLModel {
  public:
    std::vector<OpenGLModelTexture> textures_loaded;
    std::vector<OpenGLMesh> meshes;
    std::string directory;

    explicit OpenGLModel(const std::string &path);
    void draw(const glm::vec3 &position, const glm::quat &rotation, const glm::vec3 &scale,
              Shader &shader);
    [[nodiscard]] float getLocalMinY() const noexcept;
    [[nodiscard]] float getLocalHeight() const noexcept;
    [[nodiscard]] glm::vec3 getLocalSize() const noexcept;
    [[nodiscard]] glm::vec3 getLocalMinBounds() const noexcept;
    [[nodiscard]] glm::vec3 getLocalMaxBounds() const noexcept;
    [[nodiscard]] const ModelRegionAabb &getLocalRegionAabb(ModelRegion region) const noexcept;
    [[nodiscard]] const std::vector<ModelLocalTriangle> &getLocalTriangles() const noexcept;

  private:
    void loadModel(const std::string &path);
    void processNode(aiNode *node, const aiScene *scene, const aiMatrix4x4 &parentTransform);
    OpenGLMesh processMesh(aiMesh *mesh, const aiScene *scene, const aiMatrix4x4 &nodeTransform);
    std::vector<OpenGLModelTexture> loadMaterialTextures(aiMaterial *mat, aiTextureType type,
                                                          const aiScene *scene);
    unsigned int TextureFromFile(const char *path, const std::string &directory);
    unsigned int TextureFromAssimp(const aiTexture *aiTex);
    void finalizeRegionAabbsFromVertices();

    glm::vec3 m_localMinBounds{std::numeric_limits<float>::max()};
    glm::vec3 m_localMaxBounds{std::numeric_limits<float>::lowest()};
    bool m_hasLocalBounds = false;
    std::vector<glm::vec3> m_localVertices;
    std::vector<ModelLocalTriangle> m_localTriangles;
    std::array<ModelRegionAabb, 3> m_localRegionAabbs{};
};
