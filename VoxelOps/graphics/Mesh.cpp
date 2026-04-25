#include "Mesh.hpp"
#include <iostream>
#include <SDL3/SDL.h>
#include <algorithm>

namespace {
bool CanDeleteGlObjects() noexcept {
    return SDL_GL_GetCurrentContext() != nullptr;
}
} // namespace

// Constructor
Mesh::Mesh(std::vector<Vertex> vertices, std::vector<unsigned int> indices,
           std::vector<Texture> textures)
    : textures(std::move(textures)), indexCount_(static_cast<GLsizei>(indices.size())),
      vertexCount_(vertices.size()) {
    glGenVertexArrays(1, &VAO);
    glGenBuffers(1, &VBO);
    glGenBuffers(1, &EBO);

    glBindVertexArray(VAO);

    // vertices
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), nullptr, GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, vertices.size() * sizeof(Vertex), vertices.data());

    // indices
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), nullptr,
                 GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices.size() * sizeof(unsigned int),
                    indices.data());

    // attributes
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, texCoords));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, color));

    glBindVertexArray(0);
}

Mesh::Mesh(Mesh &&other) noexcept
    : VAO(other.VAO), VBO(other.VBO), EBO(other.EBO), textures(std::move(other.textures)),
      indexCount_(other.indexCount_), vertexCount_(other.vertexCount_) {
    other.VAO = 0;
    other.VBO = 0;
    other.EBO = 0;
    other.indexCount_ = 0;
    other.vertexCount_ = 0;
}

Mesh &Mesh::operator=(Mesh &&other) noexcept {
    if (this == &other) {
        return *this;
    }

    if (CanDeleteGlObjects() && VAO != 0) {
        glDeleteVertexArrays(1, &VAO);
    }
    if (CanDeleteGlObjects() && VBO != 0) {
        glDeleteBuffers(1, &VBO);
    }
    if (CanDeleteGlObjects() && EBO != 0) {
        glDeleteBuffers(1, &EBO);
    }

    VAO = other.VAO;
    VBO = other.VBO;
    EBO = other.EBO;
    textures = std::move(other.textures);
    indexCount_ = other.indexCount_;
    vertexCount_ = other.vertexCount_;

    other.VAO = 0;
    other.VBO = 0;
    other.EBO = 0;
    other.indexCount_ = 0;
    other.vertexCount_ = 0;

    return *this;
}

void Mesh::draw() const {
    if (VAO == 0 || EBO == 0 || indexCount_ <= 0) {
        return;
    }

    if (glIsVertexArray(VAO) != GL_TRUE) {
        static int s_invalidVaoLogCount = 0;
        if (s_invalidVaoLogCount < 8) {
            std::cerr << "[mesh] skipped draw: invalid VAO=" << VAO << " EBO=" << EBO
                      << " indices=" << indexCount_ << "\n";
            ++s_invalidVaoLogCount;
        }
        return;
    }

    GLint maxTextureUnits = 0;
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTextureUnits);
    const size_t safeMaxTextureUnits =
        (maxTextureUnits > 0) ? static_cast<size_t>(maxTextureUnits) : 16u;
    const size_t bindCount = std::min(textures.size(), safeMaxTextureUnits);

    if (textures.size() > bindCount) {
        static int s_textureClampLogCount = 0;
        if (s_textureClampLogCount < 8) {
            std::cerr << "[mesh] clamped bound textures from " << textures.size() << " to "
                      << bindCount << " (max units=" << safeMaxTextureUnits << ")\n";
            ++s_textureClampLogCount;
        }
    }

    for (size_t i = 0; i < bindCount; ++i) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
        glBindTexture(GL_TEXTURE_2D, textures[i].id);
    }
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(VAO);
    GLint elementBufferBinding = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBufferBinding);
    if (elementBufferBinding == 0) {
        static int s_missingEboBindingLogCount = 0;
        if (s_missingEboBindingLogCount < 8) {
            std::cerr << "[mesh] skipped draw: VAO has no element buffer binding"
                      << " (VAO=" << VAO << ", indices=" << indexCount_ << ")\n";
            ++s_missingEboBindingLogCount;
        }
        glBindVertexArray(0);
        return;
    }
    glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

Mesh::~Mesh() {
    if (CanDeleteGlObjects() && VAO != 0) {
        glDeleteVertexArrays(1, &VAO);
    }
    if (CanDeleteGlObjects() && VBO != 0) {
        glDeleteBuffers(1, &VBO);
    }
    if (CanDeleteGlObjects() && EBO != 0) {
        glDeleteBuffers(1, &EBO);
    }
}
