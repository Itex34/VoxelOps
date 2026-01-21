#pragma once

#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "Renderer.hpp"
#include "Shader.hpp"

struct Vertex { 
    glm::vec3 position; 
    glm::vec3 normal; 
    glm::vec2 texCoords; 
    glm::vec3 color;
};




struct VertexPacked {
    uint16_t px, py, pz;   // quantized position in chunk (0..65535)
    uint32_t normal;       // packed 10_10_10_2 signed normal
    uint16_t u, v;         // quantized texcoord (0..65535)   
    uint32_t color;        // RGBA8 (0xAABBGGRR or 0xRRGGBBAA depending on packing below)
};


struct VoxelVertex {
    uint32_t low;   // bitpacked payload
    uint32_t high;  // low 16 bits used; remaining bits reserved
};


struct Texture {
    unsigned int id;
    aiTextureType type;
    std::string path;
};


class Mesh {
public:
    Mesh(std::vector<Vertex> vertices,
        std::vector<unsigned int> indices,
        std::vector<Texture> textures);
    ~Mesh();
    void draw() const;
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&&) = default;
    Mesh& operator=(Mesh&&) = default;



    size_t vertexCount() const { return vertexCount_; }
    size_t indexCount() const { return static_cast<size_t>(static_cast<long long>(indexCount_)); }
    size_t cpuSideMemoryBytes() const { return 0; } // nothing stored CPU-side
private:
    unsigned int VAO = 0, VBO = 0, EBO = 0;
    std::vector<Texture> textures;
    GLsizei indexCount_ = 0;  

    size_t vertexCount_ = 0;

};






class ChunkMesh {
public:
    ChunkMesh(Renderer& renderer, std::vector<VoxelVertex> packedVertices,
        std::vector<unsigned short> indices);

    ~ChunkMesh();
    void draw(Renderer& renderer) const;
    ChunkMesh(const ChunkMesh&) = delete;
    ChunkMesh& operator=(const ChunkMesh&) = delete;
    ChunkMesh(ChunkMesh&&) = default;
    ChunkMesh& operator=(ChunkMesh&&) = default;



    size_t vertexCount() const { return vertexCount_; }
    size_t indexCount() const { return static_cast<size_t>(static_cast<long long>(indexCount_)); }
    size_t cpuSideMemoryBytes() const { return 0; } // nothing stored CPU-side

    std::vector<Texture> textures;

    size_t vertexOffset_ = 0;
    size_t indexOffset_ = 0;
    size_t vertexCount_ = 0;
    GLsizei indexCount_ = 0;

    size_t baseVertex_ = 0;


    GLuint chunkID_;
private:

};
