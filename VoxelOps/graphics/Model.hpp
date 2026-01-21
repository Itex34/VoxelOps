#ifndef MODEL_HPP
#define MODEL_HPP

#define GLM_ENABLE_EXPERIMENTAL

#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
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

class Model {
public:
    std::vector<Texture> textures_loaded;
    std::vector<Mesh> meshes;
    std::string directory;

    Model(const std::string& path);
    void draw(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale, Shader& shader);


private:
    void loadModel(const std::string& path);
    void processNode(aiNode* node, const aiScene* scene);
    Mesh processMesh(aiMesh* mesh, const aiScene* scene);
    std::vector<Texture> loadMaterialTextures(aiMaterial* mat, aiTextureType type, const aiScene* scene);
    unsigned int TextureFromFile(const char* path, const std::string& directory);
    unsigned int TextureFromAssimp(const aiTexture* aiTex);
};

#endif // MODEL_HPP
