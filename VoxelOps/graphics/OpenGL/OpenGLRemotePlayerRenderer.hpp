#pragma once

#include "OpenGLModel.hpp"

#include <memory>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Player;
class Shader;

class OpenGLRemotePlayerRenderer {
  public:
    OpenGLRemotePlayerRenderer() = default;
    ~OpenGLRemotePlayerRenderer();

    bool ensureResources();
    void shutdown();
    void render(const Player &player, const glm::mat4 &viewMat, const glm::mat4 &projMat,
                const glm::vec3 &lightDir, const glm::vec3 &lightColor,
                const glm::vec3 &ambientColor);

  private:
    std::unique_ptr<Shader> m_playerShader;
    std::unique_ptr<OpenGLModel> m_playerModel;
};
