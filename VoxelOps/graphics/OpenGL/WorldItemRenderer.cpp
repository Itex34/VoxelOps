#include <glad/glad.h>
#include <SDL3/SDL.h>

#include "WorldItemRenderer.hpp"
#include "../../application/AppHelpers.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include "../Camera.hpp"
#include "../../runtime/Runtime.hpp"
#include "../../../Shared/items/Items.hpp"
#include "../../../Shared/player/Inventory.hpp"

#include <cmath>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

namespace {
    glm::vec3 ColorForItem(uint16_t itemId) {
        if (!Inventory::IsValidItemId(itemId)) {
            return glm::vec3(0.85f, 0.85f, 0.85f);
        }

        const ItemType itemType = Items::ItemDatabase[itemId].type;
        if (itemType == ItemType::Ammo) {
            return glm::vec3(0.92f, 0.78f, 0.30f);
        }
        if (itemType == ItemType::Consumable) {
            return glm::vec3(0.94f, 0.42f, 0.42f);
        }
        if (itemType == ItemType::Gun) {
            return glm::vec3(0.38f, 0.64f, 0.95f);
        }
        return glm::vec3(0.85f, 0.85f, 0.85f);
    }
} // namespace

void WorldItemRenderer::render(const Runtime &runtime, const Camera &activeCamera) {
    if (runtime.world.worldItems.empty()) {
        return;
    }
    if (!ensureDebugShaderLoaded() || !m_debugShader) {
        return;
    }

    const float aspect =
        static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight);
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return;
    }

    ensureCubeMesh();
    if (m_worldItemVao == 0) {
        return;
    }

    const glm::mat4 projection =
        glm::perspective(glm::radians(GameData::FOV), aspect, GameData::nearPlane, GameData::farPlane);
    const glm::mat4 view = activeCamera.getViewMatrix();
    const float now = static_cast<float>(AppHelpers::GetTimeSeconds());

    m_debugShader->use();
    m_debugShader->setMat4("view", view);
    m_debugShader->setMat4("projection", projection);

    const bool cullFaceWasEnabled = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint previousCullFaceMode = GL_BACK;
    GLint previousFrontFace = GL_CCW;
    glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFaceMode);
    glGetIntegerv(GL_FRONT_FACE, &previousFrontFace);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(static_cast<GLuint>(m_worldItemVao));
    for (const auto &[_, item] : runtime.world.worldItems) {
        if (!Inventory::IsValidItemId(item.itemId)) {
            continue;
        }

        const glm::vec3 color = ColorForItem(item.itemId);
        const float phase = static_cast<float>(item.id % 1024) * 0.173f;
        const float bob = std::sin((now * 2.6f) + phase) * 0.08f;
        const float spin = (now * 90.0f) + static_cast<float>(item.id % 360);
        const float itemScale = 0.18f;
        const glm::vec3 drawPos = item.position + glm::vec3(0.0f, 0.20f + bob, 0.0f);

        glm::mat4 model(1.0f);
        model = glm::translate(model, drawPos);
        model = glm::rotate(model, glm::radians(spin), glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(itemScale));

        m_debugShader->setMat4("model", model);
        m_debugShader->setVec3("color", color);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    }
    glBindVertexArray(0);

    if (cullFaceWasEnabled) {
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glCullFace(static_cast<GLenum>(previousCullFaceMode));
    glFrontFace(static_cast<GLenum>(previousFrontFace));
}

void WorldItemRenderer::shutdown() {
    m_debugShader.reset();

    if (m_worldItemVbo != 0) {
        const GLuint vbo = static_cast<GLuint>(m_worldItemVbo);
        glDeleteBuffers(1, &vbo);
        m_worldItemVbo = 0;
    }
    if (m_worldItemVao != 0) {
        const GLuint vao = static_cast<GLuint>(m_worldItemVao);
        glDeleteVertexArrays(1, &vao);
        m_worldItemVao = 0;
    }
}

bool WorldItemRenderer::ensureDebugShaderLoaded() {
    if (m_debugShader) {
        return true;
    }

    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();
    m_debugShader = std::make_unique<Shader>(debugVertPath.c_str(), debugFragPath.c_str());
    return m_debugShader != nullptr;
}

void WorldItemRenderer::ensureCubeMesh() {
    if (m_worldItemVao != 0 && m_worldItemVbo != 0) {
        return;
    }

    static constexpr float kCubeVertices[] = {
        -0.5f, -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, 0.5f,  0.5f,  -0.5f,
        0.5f,  0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f, -0.5f,

        -0.5f, -0.5f, 0.5f,  0.5f,  -0.5f, 0.5f,  0.5f,  0.5f,  0.5f,
        0.5f,  0.5f,  0.5f,  -0.5f, 0.5f,  0.5f,  -0.5f, -0.5f, 0.5f,

        -0.5f, 0.5f,  0.5f,  -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f, -0.5f,
        -0.5f, -0.5f, -0.5f, -0.5f, -0.5f, 0.5f,  -0.5f, 0.5f,  0.5f,

        0.5f,  0.5f,  0.5f,  0.5f,  0.5f,  -0.5f, 0.5f,  -0.5f, -0.5f,
        0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, 0.5f,  0.5f,  0.5f,  0.5f,

        -0.5f, -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, 0.5f,
        0.5f,  -0.5f, 0.5f,  -0.5f, -0.5f, 0.5f,  -0.5f, -0.5f, -0.5f,

        -0.5f, 0.5f,  -0.5f, 0.5f,  0.5f,  -0.5f, 0.5f,  0.5f,  0.5f,
        0.5f,  0.5f,  0.5f,  -0.5f, 0.5f,  0.5f,  -0.5f, 0.5f,  -0.5f
    };

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kCubeVertices), kCubeVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 3, reinterpret_cast<void *>(0));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    m_worldItemVao = static_cast<unsigned int>(vao);
    m_worldItemVbo = static_cast<unsigned int>(vbo);
}

