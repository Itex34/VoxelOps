#include "OpenGLWorldPass.hpp"

#include "../OpenGLChunkScene.hpp"
#include "../OpenGLRemotePlayerRenderer.hpp"
#include "../../Frustum.hpp"
#include "../../OpenGL/Shader.hpp"
#include "../../Camera.hpp"
#include "../../../player/Player.hpp"
#include <glad/glad.h>

#include <iostream>

namespace {
void drainStageErrors(const char *stageTag) {
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
        static int loggedCount = 0;
        if (loggedCount < 16) {
            std::cerr << "[OpenGLWorldPass] Cleared " << count << " GL error(s) after " << stageTag
                      << " (first=0x" << std::hex << firstError << std::dec << ").\n";
            ++loggedCount;
        }
    }
}
} // namespace

void OpenGLWorldPass::execute(const OpenGLWorldPassInput &in) const {
    in.frameParams.frustum.extractPlanes(in.cullingViewProjection);

    glPolygonMode(GL_FRONT_AND_BACK, in.toggleWireframe ? GL_LINE : GL_FILL);
    drainStageErrors("pre world draw");

    in.chunkScene.renderChunks(in.chunkShader, in.frameParams.frustum, in.frameParams.cullingCamera
                                                                ? in.frameParams.cullingCamera
                                                                      ->position
                                                                : in.frameParams.activeCamera.position,
                               in.frameParams.player.renderDistance);
    drainStageErrors("chunk world pass");

    const glm::vec3 ambientColor = glm::vec3(0.36f, 0.40f, 0.46f);
    in.remotePlayerRenderer.render(in.frameParams.player, in.view, in.projection, in.lightDir,
                                   in.lightColor, ambientColor);
    drainStageErrors("remote player world pass");

    if (in.toggleChunkBorders) {
        in.chunkScene.renderChunkBorders(in.view, in.projection);
        drainStageErrors("chunk border pass");
    }


    if (in.frameParams.renderOpaqueOverlayPasses) {
        in.frameParams.renderOpaqueOverlayPasses();
        drainStageErrors("opaque overlay pass");
    }
}
