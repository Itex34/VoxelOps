#pragma once

#include "ISkyBackend.hpp"

#include <PbrSkyLibOpenGL/SkyAtmosphereRenderer.h>

class RealisticSkyBackend final : public ISkyBackend {
public:
    RealisticSkyBackend() = default;
    ~RealisticSkyBackend() override = default;

    void initialize() override;
    void shutdown() override;
    void resize(int width, int height) override;
    void setCameraFromActiveCamera(const Camera& activeCamera) override;
    void setViewFovYDegrees(float fovYDegrees) override;
    void render(const glm::mat4& projection, const glm::mat4& view) const override;

    void setSunDir(const glm::vec3& sunDir) override;
    const glm::vec3& getSunDir() const noexcept override;
    void setExposure(float exposure) override;
    float getExposure() const noexcept override;
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
    static pbrsky::Vec3 toPbrPositionKm(const glm::vec3& voxelPositionMeters);
    static void toPbrYawPitch(const glm::vec3& voxelDir, float& outYawRad, float& outPitchRad);

    void applySunToRenderer();
    void applyAtmospherePresetToRenderer();

    mutable pbrsky::SkyAtmosphereRenderer m_Renderer;
    glm::vec3 m_SunDir = glm::normalize(glm::vec3(1.0f, 0.00f, 0.0f));
    float m_Exposure = 4.2f;
    pbrsky::AtmosphereInfo m_BaseAtmosphereInfo = {};
    SkyAtmospherePreset m_AtmospherePreset = SkyAtmospherePreset::Hazy;
    bool m_Initialized = false;
};
