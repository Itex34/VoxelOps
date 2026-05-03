#include "OpenGLGunRenderer.hpp"

#include "OpenGLModel.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <glad/glad.h>

#include <exception>
#include <iostream>

OpenGLGunRenderer::~OpenGLGunRenderer() = default;

bool OpenGLGunRenderer::initialize() {
    if (m_shader) {
        return true;
    }

    try {
        const std::string playerVertPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.vert").generic_string();
        const std::string playerFragPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.frag").generic_string();
        m_shader = std::make_unique<Shader>(playerVertPath.c_str(), playerFragPath.c_str());
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[gun] shader load exception: " << e.what() << "\n";
        m_shader.reset();
        return false;
    }
}

void OpenGLGunRenderer::shutdown() {
    m_models.clear();
    m_shader.reset();
}

bool OpenGLGunRenderer::loadWeaponModel(uint16_t weaponId, const std::string &modelPath) {
    try {
        m_models[weaponId] = std::make_unique<OpenGLModel>(modelPath);
        return true;
    } catch (const std::exception &e) {
        std::cerr << "[gun] failed to load model for weaponId=" << weaponId << " from " << modelPath
                  << ": " << e.what() << "\n";
        m_models.erase(weaponId);
        return false;
    }
}

bool OpenGLGunRenderer::hasWeaponModel(uint16_t weaponId) const {
    return m_models.find(weaponId) != m_models.end();
}

OpenGLModel *OpenGLGunRenderer::findWeaponModel(uint16_t weaponId) {
    auto it = m_models.find(weaponId);
    if (it == m_models.end() || !it->second) {
        return nullptr;
    }
    return it->second.get();
}

bool OpenGLGunRenderer::bindSharedLighting(
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::vec3 &lightDir,
    const glm::vec3 &lightColor,
    const glm::vec3 &ambientColor
) {
    if (!m_shader) {
        return false;
    }

    m_shader->use();
    m_shader->setInt("diffuseTexture", 0);
    m_shader->setVec3("lightDir", lightDir);
    m_shader->setVec3("lightColor", lightColor);
    m_shader->setVec3("ambientColor", ambientColor);
    m_shader->setMat4("view", view);
    m_shader->setMat4("projection", projection);
    return true;
}

bool OpenGLGunRenderer::renderWorldWeapon(
    uint16_t weaponId,
    const glm::vec3 &position,
    const glm::quat &rotation,
    const glm::vec3 &scale,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::vec3 &lightDir,
    const glm::vec3 &lightColor,
    const glm::vec3 &ambientColor
) {
    OpenGLModel *model = findWeaponModel(weaponId);
    if (model == nullptr) {
        return false;
    }
    if (!bindSharedLighting(view, projection, lightDir, lightColor, ambientColor)) {
        return false;
    }

    model->draw(position, rotation, scale, *m_shader);
    return true;
}

bool OpenGLGunRenderer::renderViewWeapon(
    uint16_t weaponId,
    const glm::vec3 &position,
    const glm::quat &rotation,
    const glm::vec3 &scale,
    const glm::mat4 &view,
    const glm::mat4 &projection,
    const glm::vec3 &lightDir,
    const glm::vec3 &lightColor,
    const glm::vec3 &ambientColor
) {
    OpenGLModel *model = findWeaponModel(weaponId);
    if (model == nullptr) {
        return false;
    }
    if (!bindSharedLighting(view, projection, lightDir, lightColor, ambientColor)) {
        return false;
    }

    const bool cullFaceWasEnabled = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint previousCullFaceMode = GL_BACK;
    GLint previousFrontFace = GL_CCW;
    GLint previousDepthFunc = GL_LESS;
    GLboolean previousDepthMask = GL_TRUE;
    glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFaceMode);
    glGetIntegerv(GL_FRONT_FACE, &previousFrontFace);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_LEQUAL);

    model->draw(position, rotation, scale, *m_shader);

    {
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
            static int s_heldGunErrorLogCount = 0;
            if (s_heldGunErrorLogCount < 24) {
                std::cerr << "[gun] GL error(s) during held-gun render: count=" << count
                          << " first=0x" << std::hex << firstError << std::dec
                          << " weapon=" << weaponId << "\n";
                ++s_heldGunErrorLogCount;
            }
        }
    }

    glDepthFunc(static_cast<GLenum>(previousDepthFunc));
    glDepthMask(previousDepthMask);
    glCullFace(static_cast<GLenum>(previousCullFaceMode));
    glFrontFace(static_cast<GLenum>(previousFrontFace));
    if (cullFaceWasEnabled) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    return true;
}
