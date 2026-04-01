#pragma once

#include "ISkyBackend.hpp"
#include "Sky.hpp"

#include <string>

class ShaderSkyBackend final : public ISkyBackend {
public:
    ShaderSkyBackend(std::string vertexShaderPath, std::string fragmentShaderPath);

    void initialize() override;
    void shutdown() override;
    void resize(int width, int height) override;
    void setCameraFromActiveCamera(const Camera& activeCamera) override;
    void setViewFovYDegrees(float fovYDegrees) override;
    void render(const glm::mat4& projection, const glm::mat4& view) const override;

    void setSunDir(const glm::vec3& sunDir) override;
    const glm::vec3& getSunDir() const noexcept override;
    bool encodesOutputToSrgb() const noexcept override;
    bool requiresExternalSceneTextures() const noexcept override;
    void setExternalSceneTextures(unsigned int sceneColorTex, unsigned int sceneLinearDepthTex) override;
    void clearExternalSceneTextures() override;
    bool supportsExternalShadowMap() const noexcept override;
    void setExternalShadowMap(unsigned int shadowDepthCompareTex, const glm::mat4& shadowViewProj) override;
    void clearExternalShadowMap() override;
    bool supportsAtmospherePresets() const noexcept override;
    void setAtmospherePreset(SkyAtmospherePreset preset) override;
    SkyAtmospherePreset getAtmospherePreset() const noexcept override;

private:
    std::string m_VertexShaderPath;
    std::string m_FragmentShaderPath;
    Sky m_Sky;
};
