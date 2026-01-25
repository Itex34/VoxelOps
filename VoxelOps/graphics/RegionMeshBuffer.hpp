#pragma once

#include <vector>
#include <cstdint>
#include <glad/glad.h>
#include "Mesh.hpp"



class RegionMeshBuffer {
public:
    RegionMeshBuffer(
        size_t maxVertexBytes,
        size_t maxIndexBytes);

    ~RegionMeshBuffer();

    ChunkMesh createChunkMesh(
        const std::vector<VoxelVertex>& vertices,
        const std::vector<uint16_t>& indices);

    void destroyChunkMesh(ChunkMesh& mesh);
    void drawChunkMesh(const ChunkMesh& mesh) const;

    void uploadSubData(
        const ChunkMesh& mesh,
        const std::vector<VoxelVertex>& vertices,
        const std::vector<uint16_t>& indices
    );

	void orphanBuffers();
private:
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;

    size_t vertexCapacity = 0;
    size_t indexCapacity = 0;

    std::vector<BufferRange> freeVertexRanges;
    std::vector<BufferRange> freeIndexRanges;

    bool allocVertices(size_t count, BufferRange& out);
    bool allocIndices(size_t count, BufferRange& out);

    void freeVertices(BufferRange range);
    void freeIndices(BufferRange range);
};
