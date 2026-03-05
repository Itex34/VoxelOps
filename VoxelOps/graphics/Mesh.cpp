#include "Mesh.hpp"
#include <iostream>
#include <GLFW/glfw3.h>

namespace {
bool CanDeleteGlObjects() noexcept {
    return glfwGetCurrentContext() != nullptr;
}
}

// Constructor
Mesh::Mesh(std::vector<Vertex> vertices,
    std::vector<unsigned int> indices,
    std::vector<Texture> textures)
    : textures(std::move(textures)),
    indexCount_(static_cast<GLsizei>(indices.size())),
    vertexCount_(vertices.size())
{
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
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(unsigned int), nullptr, GL_STATIC_DRAW);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indices.size() * sizeof(unsigned int), indices.data());

    // attributes
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, texCoords));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, color));

    glBindVertexArray(0);
}

Mesh::Mesh(Mesh&& other) noexcept
    : VAO(other.VAO),
    VBO(other.VBO),
    EBO(other.EBO),
    textures(std::move(other.textures)),
    indexCount_(other.indexCount_),
    vertexCount_(other.vertexCount_)
{
    other.VAO = 0;
    other.VBO = 0;
    other.EBO = 0;
    other.indexCount_ = 0;
    other.vertexCount_ = 0;
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
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
    for (unsigned int i = 0; i < textures.size(); ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, textures[i].id); 
    }
    glBindVertexArray(VAO); 
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





