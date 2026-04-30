#include "OpenGLMesh.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <utility>

namespace {
bool CanDeleteGlObjects() noexcept {
    return SDL_GL_GetCurrentContext() != nullptr;
}
} // namespace

OpenGLMesh::OpenGLMesh(std::vector<OpenGLModelVertex> vertices, std::vector<unsigned int> indices,
                       std::vector<OpenGLModelTexture> textures)
    : m_textures(std::move(textures)), m_indexCount(static_cast<GLsizei>(indices.size())),
      m_vertexCount(vertices.size()) {
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(OpenGLModelVertex), nullptr,
                 GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(OpenGLModelVertex),
                    vertices.data());

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), nullptr,
                 GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices.size() * sizeof(unsigned int),
                    indices.data());

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(OpenGLModelVertex),
                          reinterpret_cast<void *>(offsetof(OpenGLModelVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(OpenGLModelVertex),
                          reinterpret_cast<void *>(offsetof(OpenGLModelVertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(OpenGLModelVertex),
                          reinterpret_cast<void *>(offsetof(OpenGLModelVertex, texCoords)));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(OpenGLModelVertex),
                          reinterpret_cast<void *>(offsetof(OpenGLModelVertex, color)));

    glBindVertexArray(0);
}

OpenGLMesh::~OpenGLMesh() {
    if (CanDeleteGlObjects() && m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
    }
    if (CanDeleteGlObjects() && m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (CanDeleteGlObjects() && m_ebo != 0) {
        glDeleteBuffers(1, &m_ebo);
    }
}

OpenGLMesh::OpenGLMesh(OpenGLMesh &&other) noexcept
    : m_vao(other.m_vao), m_vbo(other.m_vbo), m_ebo(other.m_ebo),
      m_textures(std::move(other.m_textures)), m_indexCount(other.m_indexCount),
      m_vertexCount(other.m_vertexCount) {
    other.m_vao = 0;
    other.m_vbo = 0;
    other.m_ebo = 0;
    other.m_indexCount = 0;
    other.m_vertexCount = 0;
}

OpenGLMesh &OpenGLMesh::operator=(OpenGLMesh &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (CanDeleteGlObjects() && m_vao != 0) {
        glDeleteVertexArrays(1, &m_vao);
    }
    if (CanDeleteGlObjects() && m_vbo != 0) {
        glDeleteBuffers(1, &m_vbo);
    }
    if (CanDeleteGlObjects() && m_ebo != 0) {
        glDeleteBuffers(1, &m_ebo);
    }

    m_vao = other.m_vao;
    m_vbo = other.m_vbo;
    m_ebo = other.m_ebo;
    m_textures = std::move(other.m_textures);
    m_indexCount = other.m_indexCount;
    m_vertexCount = other.m_vertexCount;

    other.m_vao = 0;
    other.m_vbo = 0;
    other.m_ebo = 0;
    other.m_indexCount = 0;
    other.m_vertexCount = 0;

    return *this;
}

void OpenGLMesh::draw() const {
    if (m_vao == 0 || m_ebo == 0 || m_indexCount <= 0) {
        return;
    }

    if (glIsVertexArray(m_vao) != GL_TRUE) {
        static int s_invalidVaoLogCount = 0;
        if (s_invalidVaoLogCount < 8) {
            std::cerr << "[mesh] skipped draw: invalid VAO=" << m_vao << " EBO=" << m_ebo
                      << " indices=" << m_indexCount << "\n";
            ++s_invalidVaoLogCount;
        }
        return;
    }

    GLint maxTextureUnits = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
    const size_t safeMaxTextureUnits =
        (maxTextureUnits > 0) ? static_cast<size_t>(maxTextureUnits) : 16u;
    const size_t bindCount = std::min(m_textures.size(), safeMaxTextureUnits);

    if (m_textures.size() > bindCount) {
        static int s_textureClampLogCount = 0;
        if (s_textureClampLogCount < 8) {
            std::cerr << "[mesh] clamped bound textures from " << m_textures.size() << " to "
                      << bindCount << " (max units=" << safeMaxTextureUnits << ")\n";
            ++s_textureClampLogCount;
        }
    }

    for (size_t i = 0; i < bindCount; ++i) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
        glBindTexture(GL_TEXTURE_2D, m_textures[i].id);
    }
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(m_vao);
    GLint elementBufferBinding = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBufferBinding);
    if (elementBufferBinding == 0) {
        static int s_missingEboBindingLogCount = 0;
        if (s_missingEboBindingLogCount < 8) {
            std::cerr << "[mesh] skipped draw: VAO has no element buffer binding"
                      << " (VAO=" << m_vao << ", indices=" << m_indexCount << ")\n";
            ++s_missingEboBindingLogCount;
        }
        glBindVertexArray(0);
        return;
    }
    glDrawElements(GL_TRIANGLES, m_indexCount, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}
