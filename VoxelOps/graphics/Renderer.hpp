#pragma once
#include <glad/glad.h>
#include <iostream>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <functional>
#include <array>

#include "Model.hpp"
#include "Mesh.hpp"
#include "Backend.hpp"

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
class Shader;
class ChunkManager;
class Frustum;
class Player;
class Camera;
class ISkyBackend;

struct RenderFrameParams {
    Shader &chunkShader;
    Shader &debugShader;
    ChunkManager &chunkManager;
    Frustum &frustum;
    Player &player;
    const Camera &activeCamera;
    const Camera *cullingCamera = nullptr;
    ISkyBackend &sky;
    bool toggleWireframe = false;
    bool toggleChunkBorders = false;
    bool toggleDebugFrustum = false;
    glm::vec3 sunShadowDirectionalBias = glm::vec3(0.0f); // x=+Y, y=side, z=-Y
    float sunShadowLowSunBiasBoost = 1.0f;
    bool sunShadowFrontFaceCullAtLowSun = false;
    float sunShadowFrontFaceCullGrazingThreshold = 0.78f;
    bool *chunkUniformsInitialized = nullptr;
    std::function<void()> renderOpaqueOverlayPasses;
};

class Renderer {
  public:
    Renderer() = default;

    GLuint loadTexture(const char *path);
    const Backend &getBackend() const noexcept;
    GraphicsBackend getActiveBackend() const noexcept;
    std::string_view getActiveBackendName() const noexcept;
    bool isMDIUsable() const noexcept;
    void shutdown();

    void beginFrame();
    void endFrame();
    void renderFrame(RenderFrameParams &params);

  private:
    bool ensureSceneCaptureResources(int width, int height);
    void releaseSceneCaptureResources();
    bool ensureDepthLinearizeProgram();
    void runDepthLinearizePass(const glm::mat4 &projection);
    bool ensureSunShadowResources();
    void releaseSunShadowResources();
    bool runSunShadowMomentsBlurPass();
    bool renderSunShadowMaps(RenderFrameParams &params, const glm::mat4 &projection,
                             const glm::mat4 &view, glm::mat4 &outShadowViewProj);

    Backend m_ActiveBackend;
    GLuint m_SceneFbo = 0;
    GLuint m_SceneColorTex = 0;
    GLuint m_SceneDepthTex = 0;
    GLuint m_SceneLinearDepthTex = 0;
    GLuint m_DepthLinearizeProgram = 0;
    GLuint m_FullscreenTriangleVao = 0;
    static constexpr int kSunShadowCascadeCount = 2;
    std::array<GLuint, kSunShadowCascadeCount> m_SunShadowFbo = {};
    std::array<GLuint, kSunShadowCascadeCount> m_SunShadowDepthTex = {};
    GLuint m_SunShadowMomentsTex = 0;
    GLuint m_SunShadowMomentsTempTex = 0;
    GLuint m_SunShadowMomentsFbo = 0;
    GLuint m_SunShadowProgram = 0;
    GLuint m_SunShadowMomentsBlurProgram = 0;
    std::array<glm::mat4, kSunShadowCascadeCount> m_LastSunShadowViewProjVoxel = {glm::mat4(1.0f),
                                                                                  glm::mat4(1.0f)};
    std::array<float, kSunShadowCascadeCount> m_SunShadowCascadeFarMeters = {0.0f, 0.0f};
    int m_SceneWidth = 0;
    int m_SceneHeight = 0;
    int m_SunShadowMapSize = 2048;
};
