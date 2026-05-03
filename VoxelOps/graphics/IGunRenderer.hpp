#pragma once

#include <cstdint>
#include <string>

#include <glm/mat4x4.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

class IGunRenderer {
public:
    virtual ~IGunRenderer() = default;

    virtual bool initialize() = 0;
    virtual void shutdown() = 0;

    virtual bool loadWeaponModel(uint16_t weaponId, const std::string &modelPath) = 0;
    [[nodiscard]] virtual bool hasWeaponModel(uint16_t weaponId) const = 0;

    virtual bool renderWorldWeapon(
        uint16_t weaponId,
        const glm::vec3 &position,
        const glm::quat &rotation,
        const glm::vec3 &scale,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::vec3 &lightDir,
        const glm::vec3 &lightColor,
        const glm::vec3 &ambientColor
    ) = 0;

    virtual bool renderViewWeapon(
        uint16_t weaponId,
        const glm::vec3 &position,
        const glm::quat &rotation,
        const glm::vec3 &scale,
        const glm::mat4 &view,
        const glm::mat4 &projection,
        const glm::vec3 &lightDir,
        const glm::vec3 &lightColor,
        const glm::vec3 &ambientColor
    ) = 0;
};
