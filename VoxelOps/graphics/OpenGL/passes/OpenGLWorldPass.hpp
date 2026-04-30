#pragma once

#include "../../RenderFrameParams.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Shader;
class OpenGLChunkScene;
class OpenGLRemotePlayerRenderer;

struct OpenGLWorldPassInput {
    Shader &chunkShader;
    Shader &debugShader;
    OpenGLChunkScene &chunkScene;
    OpenGLRemotePlayerRenderer &remotePlayerRenderer;
    RenderFrameParams &frameParams;
    const glm::mat4 &view;
    const glm::mat4 &projection;
    const glm::mat4 &cullingViewProjection;
    const glm::vec3 &lightDir;
    const glm::vec3 &lightColor;
    bool toggleWireframe = false;
    bool toggleChunkBorders = false;
    bool toggleDebugFrustum = false;
};

class OpenGLWorldPass {
  public:
    void execute(const OpenGLWorldPassInput &in) const;
};
