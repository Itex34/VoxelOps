#pragma once

#include "../IGunRenderer.hpp"
#include "OpenGLModel.hpp"
#include "Shader.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

class OpenGLGunRenderer final : public IGunRenderer {
  public:
    OpenGLGunRenderer() = default;
    ~OpenGLGunRenderer() override;

    bool initialize() override;
    void shutdown() override;

    bool loadWeaponModel(uint16_t weaponId, const std::string &modelPath) override;
    [[nodiscard]] bool hasWeaponModel(uint16_t weaponId) const override;

    bool renderWorldWeapon(uint16_t weaponId, const glm::vec3 &position, const glm::quat &rotation,
                           const glm::vec3 &scale, const glm::mat4 &view,
                           const glm::mat4 &projection, const glm::vec3 &lightDir,
                           const glm::vec3 &lightColor,
                           const glm::vec3 &ambientColor) override;

    bool renderViewWeapon(uint16_t weaponId, const glm::vec3 &position, const glm::quat &rotation,
                          const glm::vec3 &scale, const glm::mat4 &view,
                          const glm::mat4 &projection, const glm::vec3 &lightDir,
                          const glm::vec3 &lightColor, const glm::vec3 &ambientColor) override;

  private:
    OpenGLModel *findWeaponModel(uint16_t weaponId);
    bool bindSharedLighting(const glm::mat4 &view, const glm::mat4 &projection,
                            const glm::vec3 &lightDir, const glm::vec3 &lightColor,
                            const glm::vec3 &ambientColor);

    std::unique_ptr<Shader> m_shader;
    std::unordered_map<uint16_t, std::unique_ptr<OpenGLModel>> m_models;
};
