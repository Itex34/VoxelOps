


#include "Renderer.hpp"
#include "Mesh.hpp"
#include "Shader.hpp"
#include "ChunkManager.hpp"
#include "Frustum.hpp"
#include "../player/Player.hpp"
#include "../data/GameData.hpp"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

GLuint Renderer::loadTexture(const char* path) {
    int width, height, nrChannels;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* data = stbi_load(path, &width, &height, &nrChannels, 0);
    if (!data) { std::cerr << "Failed to load texture: " << path << std::endl; return 0; }

    GLenum internalFormat, format;
    if (nrChannels == 1) {
        internalFormat = GL_R8;
        format = GL_RED;
    }
    else if (nrChannels == 3) {
        internalFormat = GL_SRGB8;      // sRGB internal for correct sampling -> linear data in shader
        format = GL_RGB;
    }
    else if (nrChannels == 4) {
        internalFormat = GL_SRGB8_ALPHA8;
        format = GL_RGBA;
    }
    else {
        std::cerr << "Unsupported channel count: " << nrChannels << std::endl;
        stbi_image_free(data);
        return 0;
    }

    GLuint textureID;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);


    glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, data);

    //glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);

    GLfloat aniso = 2.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);

    glEnable(GL_FRAMEBUFFER_SRGB);

    stbi_image_free(data);

    glBindTexture(GL_TEXTURE_2D, 0);
    return textureID;
}

const Backend& Renderer::getBackend() const noexcept
{
    return m_ActiveBackend;
}

GraphicsBackend Renderer::getActiveBackend() const noexcept
{
    return m_ActiveBackend.getActiveBackend();
}

std::string_view Renderer::getActiveBackendName() const noexcept
{
    return m_ActiveBackend.getActiveBackendName();
}

bool Renderer::isMDIUsable() const noexcept
{
    return m_ActiveBackend.isMDIUsable();
}



void Renderer::beginFrame()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::endFrame()
{
    // swap buffers (outside if handled elsewhere)
}

void Renderer::renderFrame(RenderFrameParams& params)
{
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    beginFrame();

    glm::mat4 projection = glm::perspective(
        glm::radians(GameData::FOV),
        static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight),
        0.1f,
        100000.0f
    );
    glm::mat4 view = params.activeCamera.getViewMatrix();

    const glm::mat4 invSkyProj = glm::inverse(projection);
    const glm::mat4 invSkyView = glm::inverse(view);

    // Sky pass
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    params.skyShader.use();
    params.skyShader.setMat4("uInvProj", invSkyProj);
    params.skyShader.setMat4("uInvView", invSkyView);
    params.skyShader.setVec3("uSunDir", params.skySunDir);
    params.skyShader.setFloat("uExposure", 1.0f);
    glBindVertexArray(params.skyVAO);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);

    // World pass
    const glm::mat4 playerCamView = params.player.getCamera().getViewMatrix();
    const glm::mat4 viewProjection = projection * view;
    const glm::mat4 playerCamViewProjection = projection * playerCamView;
    params.frustum.extractPlanes(playerCamViewProjection);

    const glm::vec3 lightDir = params.skySunDir;
    const glm::vec3 lightColor = glm::vec3(1.0f, 0.98f, 0.96f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, params.chunkManager.atlas.atlasTextureID);

    params.chunkShader.use();
    params.chunkShader.setMat4("viewProj", viewProjection);

    bool localUniformState = false;
    bool& uniformsConfigured = params.chunkUniformsInitialized ? *params.chunkUniformsInitialized : localUniformState;
    if (!uniformsConfigured) {
        params.chunkShader.setVec3("lightDir", lightDir);
        params.chunkShader.setVec3("lightColor", lightColor);

        if (params.chunkManager.enableAO) {
            params.chunkShader.setVec3("skyColorTop", glm::vec3(0.58f, 0.73f, 0.95f));
            params.chunkShader.setVec3("skyColorBottom", glm::vec3(0.86f, 0.91f, 0.98f));

            params.chunkShader.setFloat("ambientStrength", 0.89f);
            params.chunkShader.setFloat("diffuseStrength", 0.85f);
            params.chunkShader.setFloat("minAmbient", 0.01f);

            params.chunkShader.setFloat("hemiTint", 0.5f);
            params.chunkShader.setFloat("contrast", 1.0f);
            params.chunkShader.setFloat("satBoost", 1.17f);
            params.chunkShader.setVec3("warmth", glm::vec3(1.03f, 1.00f, 0.97f));

            params.chunkShader.setFloat("aoPow", 0.8f);
            params.chunkShader.setFloat("aoMin", 0.6f);
            params.chunkShader.setFloat("aoApplyAfterTone", 0.8f);

            params.chunkShader.setFloat("shadowDarkness", 0.3f);
            params.chunkShader.setFloat("shadowContrast", 1.3f);
        }

        uniformsConfigured = true;
    }

    // Camera position changes every frame; keep this outside one-time static uniforms.
    params.chunkShader.setVec3("cameraPos", params.player.getCamera().position);

    params.chunkShader.setInt("texture1", 0);
    glPolygonMode(GL_FRONT_AND_BACK, params.toggleWireframe ? GL_LINE : GL_FILL);

    params.chunkManager.renderChunks(
        params.chunkShader,
        params.frustum,
        params.player,
        params.player.renderDistance
    );

    // Debug passes
    if (params.toggleChunkBorders) {
        params.chunkManager.renderChunkBorders(view, projection);
    }

    if (params.toggleDebugFrustum) {
        params.frustum.drawFrustumFaces(
            params.debugShader,
            projection * params.player.getViewMatrix(),
            view,
            projection,
            params.toggleWireframe
        );
    }
}

void Renderer::drawMesh(const ChunkMesh& mesh)
{
    // Intentionally empty: draw call is owned by RegionMeshBuffer
    // Renderer should NOT know which VAO to bind
}
