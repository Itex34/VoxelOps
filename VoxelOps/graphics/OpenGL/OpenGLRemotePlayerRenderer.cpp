#include "OpenGLRemotePlayerRenderer.hpp"

#include "OpenGLModel.hpp"
#include "Shader.hpp"
#include "../../../Shared/player/PlayerData.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <SDL3/SDL.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <iostream>

OpenGLRemotePlayerRenderer::~OpenGLRemotePlayerRenderer() = default;

bool OpenGLRemotePlayerRenderer::ensureResources() {
    if (m_playerShader && m_playerModel) {
        return true;
    }
    if (SDL_GL_GetCurrentContext() == nullptr) {
        return false;
    }

    try {
        const std::string playerModelPath =
            Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();
        const std::string playerVertPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.vert").generic_string();
        const std::string playerFragPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.frag").generic_string();

        m_playerModel = std::make_unique<OpenGLModel>(playerModelPath);
        m_playerShader = std::make_unique<Shader>(playerVertPath.c_str(), playerFragPath.c_str());
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[OpenGLRemotePlayerRenderer] Failed to initialize resources: " << e.what()
                  << "\n";
        m_playerModel.reset();
        m_playerShader.reset();
        return false;
    }
}

void OpenGLRemotePlayerRenderer::shutdown() {
    m_playerModel.reset();
    m_playerShader.reset();
}

void OpenGLRemotePlayerRenderer::render(
    const glm::vec3 &localPlayerPosition,
    std::span<const RenderRemotePlayerState> remotePlayers,
    const glm::mat4 &viewMat,
    const glm::mat4 &projMat,
    const glm::vec3 &lightDir,
    const glm::vec3 &lightColor,
    const glm::vec3 &ambientColor
) {
    if (!ensureResources() || !m_playerShader || !m_playerModel ||
        remotePlayers.empty()) {
        return;
    }

    constexpr float kLocalGhostRejectDistance = 2.0f;
    const float localGhostRejectDistanceSq = kLocalGhostRejectDistance * kLocalGhostRejectDistance;

    glDisable(GL_CULL_FACE);

    m_playerShader->use();
    m_playerShader->setInt("diffuseTexture", 0);
    m_playerShader->setVec3("lightDir", lightDir);
    m_playerShader->setVec3("lightColor", lightColor);
    m_playerShader->setVec3("ambientColor", ambientColor);
    m_playerShader->setMat4("view", viewMat);
    m_playerShader->setMat4("projection", projMat);

    const float collisionHeight = Shared::PlayerData::GetMovementSettings().collisionHeight;
    const glm::vec3 modelSize = m_playerModel->getLocalSize();
    const float modelMinY = m_playerModel->getLocalMinY();
    const float uniformFitToCollision =
        std::max(collisionHeight, 0.01f) / std::max(modelSize.y, 1e-4f);

    for (const RenderRemotePlayerState &state : remotePlayers) {
        const glm::vec3 toLocal = state.position - localPlayerPosition;
        const float localDistSq = glm::dot(toLocal, toLocal);
        if (!std::isfinite(localDistSq) || localDistSq < localGhostRejectDistanceSq) {
            continue;
        }

        const glm::vec3 scaled = state.scale * uniformFitToCollision;
        const glm::vec3 anchoredPos = state.position + glm::vec3(0.0f, -modelMinY * scaled.y, 0.0f);
        m_playerModel->draw(anchoredPos, state.rotation, scaled, *m_playerShader);
    }

    glEnable(GL_CULL_FACE);
}
