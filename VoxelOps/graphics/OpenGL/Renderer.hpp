#pragma once
#include <glad/glad.h>
#include <cstddef>
#include <memory>
#include <string_view>

#include "Backend.hpp"
#include "OpenGLChunkScene.hpp"
#include "OpenGLRemotePlayerRenderer.hpp"
#include "OpenGLTextureAtlas.hpp"
#include "passes/OpenGLSkyPass.hpp"
#include "passes/OpenGLWorldPass.hpp"
#include "../../render/RenderScene.hpp"
#include "../ISkyBackend.hpp"
#include "../OpenGL/Shader.hpp"

constexpr int FACE_VERTICES = 6; // two triangles
constexpr int FACE_INDICES = 6;  // two triangles

constexpr int MAX_FACES_PER_CHUNK = 4096; // worst case : one face for every voxel

constexpr int MAX_VERTICES_PER_CHUNK = MAX_FACES_PER_CHUNK * FACE_VERTICES;
constexpr int MAX_INDICES_PER_CHUNK = MAX_FACES_PER_CHUNK * FACE_INDICES;

constexpr int MAX_CHUNKS_LOADED = 1024;

constexpr int MAX_VERTEX_BUFFER_BYTES = 256 * 1024 * 1024;

// MAX_VERTICES_PER_CHUNK * MAX_CHUNKS_LOADED * 8;

constexpr int MAX_INDEX_BUFFER_BYTES = 128 * 1024 * 1024;

// MAX_INDICES_PER_CHUNK * MAX_CHUNKS_LOADED * sizeof(unsigned short);

struct VoxelVertex;

struct GpuMeshStats {
    size_t totalVertexCapacity;
    size_t totalIndexCapacity;

    size_t usedVertexCount;
    size_t usedIndexCount;

    size_t freeVertexCount;
    size_t freeIndexCount;

    size_t largestFreeVertexBlock;
    size_t largestFreeIndexBlock;
};

struct BufferRange;
struct ChunkMesh;

class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    const Backend &getBackend() const noexcept;
    GraphicsBackend getActiveBackend() const noexcept;
    std::string_view getActiveBackendName() const noexcept;
    bool isMDIUsable() const noexcept;
    bool initializeFrameResources();
    void shutdown();

    void beginFrame();
    void endFrame();
    void renderFrame(RenderScene &scene);

private:
    bool ensureChunkAndSkyResources();

    Backend m_ActiveBackend;
    OpenGLChunkScene m_chunkScene;
    OpenGLRemotePlayerRenderer m_remotePlayerRenderer;
    OpenGLSkyPass m_skyPass;
    OpenGLWorldPass m_worldPass;
    std::unique_ptr<Shader> m_ChunkShader;
    std::unique_ptr<Shader> m_DebugShader;
    std::unique_ptr<ISkyBackend> m_SkyBackend;
    OpenGLTextureAtlas m_textureAtlas;
    bool m_ChunkUniformsInitialized = false;
};
