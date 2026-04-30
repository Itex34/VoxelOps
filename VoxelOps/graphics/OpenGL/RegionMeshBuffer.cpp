#include "RegionMeshBuffer.hpp"
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <limits>
#include <utility>

static bool allocFromList(std::vector<BufferRange> &list, size_t count, BufferRange &out) {
    for (size_t i = 0; i < list.size(); ++i) {
        auto &r = list[i];
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

static void freeAndMerge(std::vector<BufferRange> &list, BufferRange range) {
    list.push_back(range);

    std::sort(list.begin(), list.end(),
              [](const BufferRange &a, const BufferRange &b) { return a.offset < b.offset; });

    for (size_t i = 0; i + 1 < list.size();) {
        auto &a = list[i];
        auto &b = list[i + 1];

        if (a.offset + a.count == b.offset) {
            a.count += b.count;
            list.erase(list.begin() + i + 1);
        } else {
            ++i;
        }
    }
}

RegionMeshBuffer::RegionMeshBuffer(size_t maxVertexBytes, size_t maxIndexBytes) {
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
    glVertexAttribIPointer(0, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex),
                           (void *)offsetof(VoxelVertex, low));

    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex),
                           (void *)offsetof(VoxelVertex, high));

    glBindVertexArray(0);

    freeVertexRanges.push_back({0, vertexCapacity});
    freeIndexRanges.push_back({0, indexCapacity});
}

RegionMeshBuffer::~RegionMeshBuffer() {
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &ebo);
    glDeleteVertexArrays(1, &vao);
}

bool RegionMeshBuffer::allocVertices(size_t count, BufferRange &out) {
    return allocFromList(freeVertexRanges, count, out);
}

bool RegionMeshBuffer::allocIndices(size_t count, BufferRange &out) {
    return allocFromList(freeIndexRanges, count, out);
}

void RegionMeshBuffer::freeVertices(BufferRange range) {
    freeAndMerge(freeVertexRanges, range);
}

void RegionMeshBuffer::freeIndices(BufferRange range) {
    freeAndMerge(freeIndexRanges, range);
}

ChunkMesh RegionMeshBuffer::createChunkMesh(const std::vector<VoxelVertex> &vertices,
                                            const std::vector<uint16_t> &indices) {
    ChunkMesh mesh;

    if (!allocVertices(vertices.size(), mesh.vertexRange)) {
        mesh.status = ChunkMeshStatus::OutOfMemory;
        return mesh;
    }

    if (!allocIndices(indices.size(), mesh.indexRange)) {
        freeVertices(mesh.vertexRange);
        mesh.status = ChunkMeshStatus::OutOfMemory;
        return mesh;
    }

    mesh.indexCount = (uint32_t)indices.size();
    mesh.valid = true;
    mesh.status = ChunkMeshStatus::Ok;

    uploadSubData(mesh, vertices, indices);
    return mesh;
}

void RegionMeshBuffer::destroyChunkMesh(ChunkMesh &mesh) {
    if (!mesh.valid)
        return;

    freeVertices(mesh.vertexRange);
    freeIndices(mesh.indexRange);
    mesh.valid = false;
}

void RegionMeshBuffer::drawChunkMesh(const ChunkMesh &mesh) const {
    if (!mesh.valid)
        return;

    const auto drainErrors = [](const char *stage, int maxLogs) {
        unsigned int firstError = GL_NO_ERROR;
        int count = 0;
        for (;;) {
            const unsigned int err = glGetError();
            if (err == GL_NO_ERROR) {
                break;
            }
            if (count == 0) {
                firstError = err;
            }
            ++count;
        }
        if (count > 0) {
            static int s_drawStateErrorLogCount = 0;
            if (s_drawStateErrorLogCount < maxLogs) {
                std::cerr << "[chunk] GL error before draw at " << stage << ": count=" << count
                          << " first=0x" << std::hex << firstError << std::dec << "\n";
                ++s_drawStateErrorLogCount;
            }
        }
        return count;
    };

    if (vao == 0 || vbo == 0 || ebo == 0) {
        return;
    }
    if (mesh.indexCount == 0) {
        return;
    }
    if (mesh.indexRange.offset + mesh.indexCount > indexCapacity) {
        static int s_invalidChunkIndexRangeLogCount = 0;
        if (s_invalidChunkIndexRangeLogCount < 24) {
            std::cerr << "[chunk] invalid index range (draw skipped): offset="
                      << mesh.indexRange.offset << " count=" << mesh.indexCount
                      << " capacity=" << indexCapacity << "\n";
            ++s_invalidChunkIndexRangeLogCount;
        }
        drainErrors("invalid index range", 24);
        return;
    }
    if (mesh.vertexRange.offset + mesh.vertexRange.count > vertexCapacity) {
        static int s_invalidChunkVertexRangeLogCount = 0;
        if (s_invalidChunkVertexRangeLogCount < 24) {
            std::cerr << "[chunk] invalid vertex range (draw skipped): offset="
                      << mesh.vertexRange.offset << " count=" << mesh.vertexRange.count
                      << " capacity=" << vertexCapacity << "\n";
            ++s_invalidChunkVertexRangeLogCount;
        }
        drainErrors("invalid vertex range", 24);
        return;
    }
    if (mesh.vertexRange.offset > static_cast<size_t>(std::numeric_limits<GLint>::max())) {
        static int s_chunkBaseVertexOverflowLogCount = 0;
        if (s_chunkBaseVertexOverflowLogCount < 24) {
            std::cerr << "[chunk] baseVertex overflow (draw skipped): baseVertex="
                      << mesh.vertexRange.offset << "\n";
            ++s_chunkBaseVertexOverflowLogCount;
        }
        drainErrors("baseVertex overflow", 24);
        return;
    }

    if (glIsVertexArray(vao) != GL_TRUE) {
        static int s_invalidChunkVaoLogCount = 0;
        if (s_invalidChunkVaoLogCount < 12) {
            std::cerr << "[chunk] invalid region VAO=" << vao << " (draw skipped)\n";
            ++s_invalidChunkVaoLogCount;
        }
        drainErrors("invalid VAO", 24);
        return;
    }

    GLint currentProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currentProgram);
    if (currentProgram == 0) {
        static int s_missingChunkProgramLogCount = 0;
        if (s_missingChunkProgramLogCount < 12) {
            std::cerr << "[chunk] no current shader program during chunk draw (draw skipped)\n";
            ++s_missingChunkProgramLogCount;
        }
        drainErrors("missing shader program", 24);
        return;
    }

    glBindVertexArray(vao);
    if (drainErrors("glBindVertexArray", 24) > 0) {
        glBindVertexArray(0);
        return;
    }
    GLint elementBufferBinding = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBufferBinding);
    if (drainErrors("query EBO binding", 24) > 0) {
        glBindVertexArray(0);
        return;
    }
    if (elementBufferBinding == 0) {
        static int s_missingChunkEboLogCount = 0;
        if (s_missingChunkEboLogCount < 12) {
            std::cerr << "[chunk] VAO missing EBO binding during chunk draw (draw skipped)\n";
            ++s_missingChunkEboLogCount;
        }
        glBindVertexArray(0);
        return;
    }

    static bool s_useChunkLegacyNoBaseVertexPath = false;
    const auto issueChunkDraw = [&]() {
        const void *indexOffsetPtr =
            reinterpret_cast<const void *>(mesh.indexRange.offset * sizeof(uint16_t));
        if (s_useChunkLegacyNoBaseVertexPath) {
            const size_t vertexByteBase = mesh.vertexRange.offset * sizeof(VoxelVertex);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glVertexAttribIPointer(
                0, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex),
                reinterpret_cast<const void *>(vertexByteBase + offsetof(VoxelVertex, low)));
            glVertexAttribIPointer(
                1, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex),
                reinterpret_cast<const void *>(vertexByteBase + offsetof(VoxelVertex, high)));
            glDrawElements(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_SHORT, indexOffsetPtr);
            return;
        }

        glDrawElementsBaseVertex(GL_TRIANGLES, mesh.indexCount, GL_UNSIGNED_SHORT, indexOffsetPtr,
                                 static_cast<GLint>(mesh.vertexRange.offset));
    };

    const auto drainDrawErrors = [&]() {
        unsigned int firstError = GL_NO_ERROR;
        int count = 0;
        for (;;) {
            const unsigned int err = glGetError();
            if (err == GL_NO_ERROR) {
                break;
            }
            if (count == 0) {
                firstError = err;
            }
            ++count;
        }
        return std::pair<int, unsigned int>(count, firstError);
    };

    issueChunkDraw();
    std::pair<int, unsigned int> drawErrors = drainDrawErrors();

    if (drawErrors.first > 0 && drawErrors.second == GL_INVALID_OPERATION &&
        !s_useChunkLegacyNoBaseVertexPath) {
        s_useChunkLegacyNoBaseVertexPath = true;
        static bool s_loggedLegacyFallback = false;
        if (!s_loggedLegacyFallback) {
            std::cerr << "[chunk] switching to legacy no-base-vertex draw path after "
                         "GL_INVALID_OPERATION.\n";
            s_loggedLegacyFallback = true;
        }

        issueChunkDraw();
        drawErrors = drainDrawErrors();
    }

    if (drawErrors.first > 0) {
        static int s_chunkDrawErrorLogCount = 0;
        if (s_chunkDrawErrorLogCount < 24) {
            std::cerr << "[chunk] GL error during draw: count=" << drawErrors.first << " first=0x"
                      << std::hex << drawErrors.second << std::dec << " vao=" << vao
                      << " eboBind=" << elementBufferBinding << " indexCount=" << mesh.indexCount
                      << " indexOffset=" << mesh.indexRange.offset
                      << " baseVertex=" << mesh.vertexRange.offset << " program=" << currentProgram
                      << "\n";
            ++s_chunkDrawErrorLogCount;
        }
    }

    glBindVertexArray(0);
}

void RegionMeshBuffer::uploadSubData(const ChunkMesh &mesh,
                                     const std::vector<VoxelVertex> &vertices,
                                     const std::vector<uint16_t> &indices) {
    glBindVertexArray(vao);

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, mesh.vertexRange.offset * sizeof(VoxelVertex),
                    vertices.size() * sizeof(VoxelVertex), vertices.data());

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, mesh.indexRange.offset * sizeof(uint16_t),
                    indices.size() * sizeof(uint16_t), indices.data());
}

void RegionMeshBuffer::orphanBuffers() {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertexCapacity * sizeof(VoxelVertex), nullptr, GL_DYNAMIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indexCapacity * sizeof(uint16_t), nullptr,
                 GL_DYNAMIC_DRAW);
}
