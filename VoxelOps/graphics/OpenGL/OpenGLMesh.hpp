#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <assimp/scene.h>
#include <glad/glad.h>
#include <glm/glm.hpp>

struct OpenGLModelVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 texCoords;
    glm::vec3 color;
};

struct OpenGLModelTexture {
    unsigned int id;
    aiTextureType type;
    std::string path;
};

class OpenGLMesh {
public:
    OpenGLMesh(
        std::vector<OpenGLModelVertex> vertices,
        std::vector<unsigned int> indices,
        std::vector<OpenGLModelTexture> textures
    );
    ~OpenGLMesh();

    OpenGLMesh(const OpenGLMesh &) = delete;
    OpenGLMesh &operator=(const OpenGLMesh &) = delete;
    OpenGLMesh(OpenGLMesh &&other) noexcept;
    OpenGLMesh &operator=(OpenGLMesh &&other) noexcept;

    void draw() const;

    size_t vertexCount() const {
        return m_vertexCount;
    }

    size_t indexCount() const {
        return static_cast<size_t>(static_cast<long long>(m_indexCount));
    }

private:
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    std::vector<OpenGLModelTexture> m_textures;
    GLsizei m_indexCount = 0;
    size_t m_vertexCount = 0;
};
