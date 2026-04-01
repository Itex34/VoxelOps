#pragma once

#include <glm/glm.hpp>

class Camera;

enum class SkyAtmospherePreset : unsigned char {
    Clear = 0,
    Hazy,
    Foggy
};

class ISkyBackend {
public:
    virtual ~ISkyBackend() = default;

    virtual void initialize() = 0;
    virtual void shutdown() = 0;
    virtual void resize(int width, int height) = 0;
    virtual void setCameraFromActiveCamera(const Camera& activeCamera) = 0;
    virtual void setViewFovYDegrees(float fovYDegrees) = 0;
    virtual void render(const glm::mat4& projection, const glm::mat4& view) const = 0;

    virtual void setSunDir(const glm::vec3& sunDir) = 0;
    virtual const glm::vec3& getSunDir() const noexcept = 0;
    virtual bool encodesOutputToSrgb() const noexcept = 0;
    virtual bool requiresExternalSceneTextures() const noexcept = 0;
    virtual void setExternalSceneTextures(unsigned int sceneColorTex, unsigned int sceneLinearDepthTex) = 0;
    virtual void clearExternalSceneTextures() = 0;
    virtual bool supportsExternalShadowMap() const noexcept = 0;
    virtual void setExternalShadowMap(unsigned int shadowDepthCompareTex, const glm::mat4& shadowViewProj) = 0;
    virtual void clearExternalShadowMap() = 0;
    virtual bool supportsAtmospherePresets() const noexcept = 0;
    virtual void setAtmospherePreset(SkyAtmospherePreset preset) = 0;
    virtual SkyAtmospherePreset getAtmospherePreset() const noexcept = 0;
};
