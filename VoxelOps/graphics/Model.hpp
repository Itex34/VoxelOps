#ifndef MODEL_HPP
#define MODEL_HPP

#define GLM_ENABLE_EXPERIMENTAL

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <array>
#include <cstdint>
#include <limits>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <stb_image.h>

#include "Shader.hpp"  


struct Texture;
class Mesh;

struct ModelRegionAabb {
    glm::vec3 min{ 0.0f };
    glm::vec3 max{ 0.0f };
    bool valid = false;
};

enum class ModelRegion : uint8_t {
    Legs = 0,
    Body = 1,
    Head = 2
};

struct ModelLocalTriangle {
    glm::vec3 a{ 0.0f };
    glm::vec3 b{ 0.0f };
    glm::vec3 c{ 0.0f };
    ModelRegion region = ModelRegion::Body;
};

class Model {
public:
    std::vector<Texture> textures_loaded;
    std::vector<Mesh> meshes;
    std::string directory;

    Model(const std::string& path);
    void draw(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale, Shader& shader);
    [[nodiscard]] float getLocalMinY() const noexcept;
    [[nodiscard]] float getLocalHeight() const noexcept;
    [[nodiscard]] glm::vec3 getLocalSize() const noexcept;
    [[nodiscard]] glm::vec3 getLocalMinBounds() const noexcept;
    [[nodiscard]] glm::vec3 getLocalMaxBounds() const noexcept;
    [[nodiscard]] const ModelRegionAabb& getLocalRegionAabb(ModelRegion region) const noexcept;
    [[nodiscard]] const std::vector<ModelLocalTriangle>& getLocalTriangles() const noexcept;


private:
    void loadModel(const std::string& path);
    void processNode(aiNode* node, const aiScene* scene, const aiMatrix4x4& parentTransform);
    Mesh processMesh(aiMesh* mesh, const aiScene* scene, const aiMatrix4x4& nodeTransform);
    std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, const aiScene* scene);
    unsigned int TextureFromFile(const char* path, const std::string& directory);
    unsigned int TextureFromAssimp(const aiTexture* aiTex);
    void finalizeRegionAabbsFromVertices();

    glm::vec3 m_localMinBounds{ std::numeric_limits<float>::max() };
    glm::vec3 m_localMaxBounds{ std::numeric_limits<float>::lowest() };
    bool m_hasLocalBounds = false;
    std::vector<glm::vec3> m_localVertices;
    std::vector<ModelLocalTriangle> m_localTriangles;
    std::array<ModelRegionAabb, 3> m_localRegionAabbs{};
};

#endif // MODEL_HPP
