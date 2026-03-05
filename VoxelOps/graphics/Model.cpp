#define STB_IMAGE_IMPLEMENTATION
#include "Model.hpp"

#include "Mesh.hpp"
#include <assimp/matrix3x3.h>
#include <algorithm>
#include <limits>

Model::Model(const std::string& path) {
    loadModel(path);
}

float Model::getLocalMinY() const noexcept {
    return m_hasLocalBounds ? m_localMinBounds.y : 0.0f;
}

float Model::getLocalHeight() const noexcept {
    if (!m_hasLocalBounds) {
        return 1.0f;
    }
    return std::max(m_localMaxBounds.y - m_localMinBounds.y, 1e-4f);
}

glm::vec3 Model::getLocalSize() const noexcept {
    if (!m_hasLocalBounds) {
        return glm::vec3(1.0f);
    }
    return glm::max(m_localMaxBounds - m_localMinBounds, glm::vec3(1e-4f));
}

glm::vec3 Model::getLocalMinBounds() const noexcept {
    return m_hasLocalBounds ? m_localMinBounds : glm::vec3(0.0f);
}

glm::vec3 Model::getLocalMaxBounds() const noexcept {
    return m_hasLocalBounds ? m_localMaxBounds : glm::vec3(0.0f);
}

const ModelRegionAabb& Model::getLocalRegionAabb(ModelRegion region) const noexcept {
    return m_localRegionAabbs[static_cast<size_t>(region)];
}

const std::vector<ModelLocalTriangle>& Model::getLocalTriangles() const noexcept {
    return m_localTriangles;
}

void Model::loadModel(const std::string& path) {
    meshes.clear();
    textures_loaded.clear();
    m_localVertices.clear();
    m_localTriangles.clear();
    m_localMinBounds = glm::vec3(std::numeric_limits<float>::max());
    m_localMaxBounds = glm::vec3(std::numeric_limits<float>::lowest());
    m_hasLocalBounds = false;
    for (ModelRegionAabb& region : m_localRegionAabbs) {
        region = ModelRegionAabb{};
    }

    Assimp::Importer importer;
    // Keep model texture orientation consistent by flipping UVs at import time.
    // Texture decoding itself stays unflipped (see TextureFromFile/TextureFromAssimp).
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) {
        std::cout << "ERROR::ASSIMP::" << importer.GetErrorString() << std::endl;
        return;
    }

    directory = std::filesystem::path(path).parent_path().string();

    processNode(scene->mRootNode, scene, aiMatrix4x4());
    finalizeRegionAabbsFromVertices();
}

void Model::processNode(aiNode* node, const aiScene* scene, const aiMatrix4x4& parentTransform) {
    const aiMatrix4x4 currentTransform = parentTransform * node->mTransformation;

    for (unsigned int i = 0; i < node->mNumMeshes; i++) {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        meshes.push_back(processMesh(mesh, scene, currentTransform));
    }

    for (unsigned int i = 0; i < node->mNumChildren; i++) {
        processNode(node->mChildren[i], scene, currentTransform);
    }
}

Mesh Model::processMesh(aiMesh* mesh, const aiScene* scene, const aiMatrix4x4& nodeTransform) {
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    std::vector<Texture> textures;
    aiMatrix3x3 normalTransform(nodeTransform);
    normalTransform.Inverse().Transpose();

    // Pick the UV set that carries meaningful variation (some FBX files keep valid UVs in channel 1+).
    unsigned int uvChannel = AI_MAX_NUMBER_OF_TEXTURECOORDS;
    float bestUvScore = -1.0f;
    for (unsigned int c = 0; c < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++c) {
        if (mesh->mTextureCoords[c] == nullptr) {
            continue;
        }
        float minU = std::numeric_limits<float>::max();
        float minV = std::numeric_limits<float>::max();
        float maxU = std::numeric_limits<float>::lowest();
        float maxV = std::numeric_limits<float>::lowest();
        for (unsigned int i = 0; i < mesh->mNumVertices; ++i) {
            const aiVector3D& uv = mesh->mTextureCoords[c][i];
            minU = std::min(minU, uv.x);
            maxU = std::max(maxU, uv.x);
            minV = std::min(minV, uv.y);
            maxV = std::max(maxV, uv.y);
        }
        const float score = (maxU - minU) + (maxV - minV);
        if (score > bestUvScore) {
            bestUvScore = score;
            uvChannel = c;
        }
    }

    for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
        Vertex vertex;
        const aiVector3D transformedPos = nodeTransform * mesh->mVertices[i];
        vertex.position = { transformedPos.x, transformedPos.y, transformedPos.z };
        m_localVertices.push_back(vertex.position);
        m_localMinBounds = glm::min(m_localMinBounds, vertex.position);
        m_localMaxBounds = glm::max(m_localMaxBounds, vertex.position);
        m_hasLocalBounds = true;
        if (mesh->HasNormals()) {
            aiVector3D transformedNormal = normalTransform * mesh->mNormals[i];
            transformedNormal.Normalize();
            vertex.normal = { transformedNormal.x, transformedNormal.y, transformedNormal.z };
        }
        else {
            vertex.normal = { 0.0f, 1.0f, 0.0f };
        }

        if (uvChannel < AI_MAX_NUMBER_OF_TEXTURECOORDS) {
            vertex.texCoords = { mesh->mTextureCoords[uvChannel][i].x, mesh->mTextureCoords[uvChannel][i].y };
        }
        else {
            vertex.texCoords = { 0.0f, 0.0f };
        }
        vertex.color = { 1.0f, 1.0f, 1.0f };

        vertices.push_back(vertex);
    }

    for (unsigned int i = 0; i < mesh->mNumFaces; i++) {
        aiFace face = mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; j++) {
            indices.push_back(face.mIndices[j]);
        }
    }

    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const unsigned int ia = indices[i];
        const unsigned int ib = indices[i + 1];
        const unsigned int ic = indices[i + 2];
        if (ia >= vertices.size() || ib >= vertices.size() || ic >= vertices.size()) {
            continue;
        }
        ModelLocalTriangle tri;
        tri.a = vertices[ia].position;
        tri.b = vertices[ib].position;
        tri.c = vertices[ic].position;
        tri.region = ModelRegion::Body;
        m_localTriangles.push_back(tri);
    }

    if (mesh->mMaterialIndex >= 0) {
        aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
        std::vector<Texture> diffuseMaps = loadMaterialTextures(material, aiTextureType_BASE_COLOR, scene);
        if (diffuseMaps.empty()) {
            diffuseMaps = loadMaterialTextures(material, aiTextureType_DIFFUSE, scene);
        }
        textures.insert(textures.end(), diffuseMaps.begin(), diffuseMaps.end());
    }

    return Mesh(vertices, indices, textures);
}

std::vector<Texture> Model::loadMaterialTextures(aiMaterial* mat, aiTextureType type, const aiScene* scene) {
    std::vector<Texture> textures;
    for (unsigned int i = 0; i < mat->GetTextureCount(type); i++) {
        aiString str;
        mat->GetTexture(type, i, &str);
        std::cout << "Trying to load texture: " << str.C_Str() << " from directory: " << directory << std::endl;
        const aiTexture* aiTex = scene->GetEmbeddedTexture(str.C_Str());
        if (aiTex) {
            Texture texture;
            texture.id = TextureFromAssimp(aiTex);
            texture.type = type;
            texture.path = str.C_Str();
            textures.push_back(texture);
            textures_loaded.push_back(texture);
        }
        else {
            bool skip = false;
            for (const auto& loadedTexture : textures_loaded) {
                if (std::strcmp(loadedTexture.path.data(), str.C_Str()) == 0) {
                    textures.push_back(loadedTexture);
                    skip = true;
                    break;
                }
            }

            if (!skip) {
                Texture texture;
                texture.id = TextureFromFile(str.C_Str(), directory);
                texture.type = type;
                texture.path = str.C_Str();
                textures.push_back(texture);
                textures_loaded.push_back(texture);
            }
        }
    }
    return textures;
}

unsigned int Model::TextureFromFile(const char* path, const std::string& directory) {
    std::filesystem::path filename = std::filesystem::path(directory) / path;
    unsigned int textureID;
    glGenTextures(1, &textureID);

    int width, height, nrComponents;
    // Model assets use their authored UV orientation; avoid inheriting global flip state.
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data = stbi_load(filename.string().c_str(), &width, &height, &nrComponents, 0);
    if (data) {
        GLenum format;
        if (nrComponents == 1)
            format = GL_RED;
        else if (nrComponents == 3)
            format = GL_RGB;
        else if (nrComponents == 4)
            format = GL_RGBA;
        else
            format = GL_RGB;

        glBindTexture(GL_TEXTURE_2D, textureID);
        glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        // Avoid bleeding from transparent atlas regions in small skinned textures.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        stbi_image_free(data);
    }
    else {
        std::cout << "Texture failed to load at path: " << path << std::endl;
        stbi_image_free(data);
    }

    return textureID;
}

unsigned int Model::TextureFromAssimp(const aiTexture* aiTex) {
    unsigned int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    if (aiTex->mHeight == 0) {    // compressed texture
        int width, height, nrChannels;
        // Keep embedded model textures consistent with non-embedded model texture orientation.
        stbi_set_flip_vertically_on_load(false);
        unsigned char* data = stbi_load_from_memory(
            reinterpret_cast<unsigned char*>(aiTex->pcData),
            aiTex->mWidth, &width, &height, &nrChannels, 0
        );

        if (data) {
            GLenum format = (nrChannels == 4) ? GL_RGBA : GL_RGB;
            glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
        }
        else {
            std::cout << "Failed to load embedded texture from memory." << std::endl;
        }
    }
    else {
        // aiTexel is stored BGRA8888 for uncompressed embedded textures.
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            static_cast<GLsizei>(aiTex->mWidth),
            static_cast<GLsizei>(aiTex->mHeight),
            0,
            GL_BGRA,
            GL_UNSIGNED_BYTE,
            aiTex->pcData
        );
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    return textureID;
}

void Model::finalizeRegionAabbsFromVertices() {
    if (!m_hasLocalBounds || m_localVertices.empty()) {
        return;
    }

    constexpr float kLegsRatio = 0.45f;
    constexpr float kBodyRatio = 0.37f;
    constexpr float kHeadRatio = 0.18f;
    static_assert(kLegsRatio + kBodyRatio + kHeadRatio > 0.99f, "Region ratios must cover full height.");

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
    std::array<bool, 3> hasRegion{ false, false, false };

    for (const glm::vec3& v : m_localVertices) {
        size_t regionIdx = static_cast<size_t>(ModelRegion::Head);
        if (v.y <= legsTop) {
            regionIdx = static_cast<size_t>(ModelRegion::Legs);
        }
        else if (v.y <= bodyTop) {
            regionIdx = static_cast<size_t>(ModelRegion::Body);
        }

        regionMin[regionIdx] = glm::min(regionMin[regionIdx], v);
        regionMax[regionIdx] = glm::max(regionMax[regionIdx], v);
        hasRegion[regionIdx] = true;
    }

    const std::array<float, 4> yCuts{ minY, legsTop, bodyTop, maxY };
    for (size_t i = 0; i < 3; ++i) {
        ModelRegionAabb& out = m_localRegionAabbs[i];
        if (hasRegion[i]) {
            out.min = regionMin[i];
            out.max = regionMax[i];
        }
        else {
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

    for (ModelLocalTriangle& tri : m_localTriangles) {
        const float centerY = (tri.a.y + tri.b.y + tri.c.y) / 3.0f;
        if (centerY <= legsTop) {
            tri.region = ModelRegion::Legs;
        }
        else if (centerY <= bodyTop) {
            tri.region = ModelRegion::Body;
        }
        else {
            tri.region = ModelRegion::Head;
        }
    }
}


void Model::draw(const glm::vec3& position, const glm::quat& rotation, const glm::vec3& scale, Shader& shader) {
    glm::mat4 scaleMat = glm::scale(glm::mat4(1.0f), scale);
    glm::mat4 rotationMat = glm::toMat4(rotation);
    glm::mat4 translationMat = glm::translate(glm::mat4(1.0f), position);

    glm::mat4 model = translationMat * rotationMat * scaleMat;

    shader.setMat4("model", model);

    for (Mesh& mesh : meshes) {
        mesh.draw();
    }
}
