#include "ShaderSkyBackend.hpp"

#include "../Camera.hpp"

#include <utility>

ShaderSkyBackend::ShaderSkyBackend(std::string vertexShaderPath, std::string fragmentShaderPath)
    : m_VertexShaderPath(std::move(vertexShaderPath)),
      m_FragmentShaderPath(std::move(fragmentShaderPath)) {}

void ShaderSkyBackend::initialize() {
    m_Sky.initialize(m_VertexShaderPath.c_str(), m_FragmentShaderPath.c_str());
}

void ShaderSkyBackend::shutdown() {
    m_Sky.shutdown();
}

void ShaderSkyBackend::resize(int width, int height) {
    (void)width;
    (void)height;
}

void ShaderSkyBackend::setCameraFromActiveCamera(const Camera &activeCamera) {
    (void)activeCamera;
}

void ShaderSkyBackend::setViewFovYDegrees(float fovYDegrees) {
    (void)fovYDegrees;
}

void ShaderSkyBackend::render(const glm::mat4 &projection, const glm::mat4 &view) const {
    m_Sky.render(projection, view);
}

void ShaderSkyBackend::setSunDir(const glm::vec3 &sunDir) {
    m_Sky.setSunDir(sunDir);
}

const glm::vec3 &ShaderSkyBackend::getSunDir() const noexcept {
    return m_Sky.getSunDir();
}

void ShaderSkyBackend::setExposure(float exposure) {
    m_Sky.setExposure(exposure);
}

float ShaderSkyBackend::getExposure() const noexcept {
    return m_Sky.getExposure();
}

bool ShaderSkyBackend::encodesOutputToSrgb() const noexcept {
    return false;
}

bool ShaderSkyBackend::requiresExternalSceneTextures() const noexcept {
    return false;
}

void ShaderSkyBackend::setExternalSceneTextures(unsigned int sceneColorTex,
                                                unsigned int sceneLinearDepthTex) {
    (void)sceneColorTex;
    (void)sceneLinearDepthTex;
}

void ShaderSkyBackend::clearExternalSceneTextures() {}

bool ShaderSkyBackend::supportsExternalShadowMap() const noexcept {
    return false;
}

void ShaderSkyBackend::setExternalShadowMap(unsigned int shadowDepthCompareTex,
                                            const glm::mat4 &shadowViewProj) {
    (void)shadowDepthCompareTex;
    (void)shadowViewProj;
}

void ShaderSkyBackend::clearExternalShadowMap() {}

bool ShaderSkyBackend::supportsAtmospherePresets() const noexcept {
    return false;
}

void ShaderSkyBackend::setAtmospherePreset(SkyAtmospherePreset preset) {
    (void)preset;
}

SkyAtmospherePreset ShaderSkyBackend::getAtmospherePreset() const noexcept {
    return SkyAtmospherePreset::Clear;
}
