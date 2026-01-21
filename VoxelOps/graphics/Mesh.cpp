#include "Mesh.hpp"
#include <iostream>

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

}




//--ChunkMesh--

ChunkMesh::ChunkMesh(Renderer& renderer,
    const std::vector<VoxelVertex> packedVertices,
    const std::vector<unsigned short> indices)
{
    vertexCount_ = packedVertices.size();
    indexCount_ = indices.size();



    



    // Offsets (in vertices and indices)
    renderer.allocateMesh(vertexCount_, indexCount_, vertexOffset_, indexOffset_);


    const size_t vertexBytesReq = vertexCount_ * sizeof(VoxelVertex);
    const size_t indexBytesReq = indexCount_ * sizeof(unsigned short);






    // Store base vertex (in vertices)
    baseVertex_ = vertexOffset_;

    glBindVertexArray(renderer.worldVAO);

    // Upload vertices
    glBindBuffer(GL_ARRAY_BUFFER, renderer.worldVBO);

    glBufferSubData(GL_ARRAY_BUFFER,
        vertexOffset_ * sizeof(VoxelVertex),
        vertexCount_ * sizeof(VoxelVertex),
        packedVertices.data());



    // Upload indices
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, renderer.worldEBO);


    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
        indexOffset_ * sizeof(unsigned short),
        indexCount_ * sizeof(unsigned short),
        indices.data());


}






void ChunkMesh::draw(Renderer& renderer) const {

    intptr_t indexByteOffset = (intptr_t)indexOffset_ * (intptr_t)sizeof(unsigned short);

    glDrawElementsBaseVertex(
        GL_TRIANGLES,
        indexCount_,
        GL_UNSIGNED_SHORT,
        (void*)indexByteOffset,
        (GLint)baseVertex_
    );
}



ChunkMesh::~ChunkMesh() {

}