#include "RegionMeshBuffer.hpp"
#include <algorithm>
#include <iostream>

static bool allocFromList(
    std::vector<BufferRange>& list,
    size_t count,
    BufferRange& out)
{
    for (size_t i = 0; i < list.size(); ++i) {
        auto& r = list[i];
        if (r.count >= count) {
            out.offset = r.offset;
            out.count = count;

            r.offset += count;
            r.count -= count;

            if (r.count == 0)
                list.erase(list.begin() + i);

            return true;
        }
    }
    return false;
}

static void freeAndMerge(
    std::vector<BufferRange>& list,
    BufferRange range)
{
    list.push_back(range);

    std::sort(list.begin(), list.end(),
        [](const BufferRange& a, const BufferRange& b) {
            return a.offset < b.offset;
        });

    for (size_t i = 0; i + 1 < list.size();) {
        auto& a = list[i];
        auto& b = list[i + 1];

        if (a.offset + a.count == b.offset) {
            a.count += b.count;
            list.erase(list.begin() + i + 1);
        }
        else {
            ++i;
        }
    }
}

RegionMeshBuffer::RegionMeshBuffer(
    size_t maxVertexBytes,
    size_t maxIndexBytes)
{
    vertexCapacity = maxVertexBytes / sizeof(VoxelVertex);
    indexCapacity = maxIndexBytes / sizeof(uint16_t);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, maxVertexBytes, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, maxIndexBytes, nullptr, GL_DYNAMIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(
        0, 1, GL_UNSIGNED_INT,
        sizeof(VoxelVertex),
        (void*)offsetof(VoxelVertex, low));

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(
        1, 1, GL_UNSIGNED_INT,
        sizeof(VoxelVertex),
        (void*)offsetof(VoxelVertex, high));

    glBindVertexArray(0);

    freeVertexRanges.push_back({ 0, vertexCapacity });
    freeIndexRanges.push_back({ 0, indexCapacity });
}

RegionMeshBuffer::~RegionMeshBuffer()
{
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteVertexArrays(1, &vao);
}

bool RegionMeshBuffer::allocVertices(size_t count, BufferRange& out)
{
    return allocFromList(freeVertexRanges, count, out);
}

bool RegionMeshBuffer::allocIndices(size_t count, BufferRange& out)
{
    return allocFromList(freeIndexRanges, count, out);
}

void RegionMeshBuffer::freeVertices(BufferRange range)
{
    freeAndMerge(freeVertexRanges, range);
}

void RegionMeshBuffer::freeIndices(BufferRange range)
{
    freeAndMerge(freeIndexRanges, range);
}

ChunkMesh RegionMeshBuffer::createChunkMesh(
    const std::vector<VoxelVertex>& vertices,
    const std::vector<uint16_t>& indices)
{
    ChunkMesh mesh;

    if (!allocVertices(vertices.size(), mesh.vertexRange)) {
        std::cerr << "[RegionMeshBuffer] vertex alloc failed\n";
        return mesh;
    }

    if (!allocIndices(indices.size(), mesh.indexRange)) {
        freeVertices(mesh.vertexRange);
        std::cerr << "[RegionMeshBuffer] index alloc failed\n";
        return mesh;
    }

    mesh.indexCount = (uint32_t)indices.size();
    mesh.valid = true;

    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(
        GL_ARRAY_BUFFER,
        mesh.vertexRange.offset * sizeof(VoxelVertex),
        mesh.vertexRange.count * sizeof(VoxelVertex),
        vertices.data());

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferSubData(
        GL_ELEMENT_ARRAY_BUFFER,
        mesh.indexRange.offset * sizeof(uint16_t),
        mesh.indexRange.count * sizeof(uint16_t),
        indices.data());

    return mesh;
}

void RegionMeshBuffer::destroyChunkMesh(ChunkMesh& mesh)
{
    if (!mesh.valid) return;

    freeVertices(mesh.vertexRange);
    freeIndices(mesh.indexRange);
    mesh.valid = false;
}

void RegionMeshBuffer::drawChunkMesh(const ChunkMesh& mesh) const
{
    if (!mesh.valid) return;

    glBindVertexArray(vao);
    glDrawElementsBaseVertex(
        GL_TRIANGLES,
        mesh.indexCount,
        GL_UNSIGNED_SHORT,
        (void*)(mesh.indexRange.offset * sizeof(uint16_t)),
        mesh.vertexRange.offset
    );
}
