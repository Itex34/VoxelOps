#include "RealisticSkyBackend.hpp"

#include "Camera.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
constexpr float kMetersToKilometers = 0.001f;

float clampPositive(float v, float minValue)
{
    return (v < minValue) ? minValue : v;
}

void applyVisibilityKmPreset(
    pbrsky::AtmosphereInfo& ioInfo,
    float visibilityKm,
    float singleScatteringAlbedo,
    float miePhaseG)
{
    const float vKm = clampPositive(visibilityKm, 0.2f);
    const float betaExt = 3.912f / vKm; // Koschmieder law, km^-1.
    const float albedo = std::clamp(singleScatteringAlbedo, 0.01f, 0.99f);
    const float betaSca = betaExt * albedo;
    const float betaAbs = betaExt - betaSca;

    ioInfo.mie_extinction = { betaExt, betaExt, betaExt };
    ioInfo.mie_scattering = { betaSca, betaSca, betaSca };
    ioInfo.mie_absorption = { betaAbs, betaAbs, betaAbs };
    ioInfo.mie_phase_g = std::clamp(miePhaseG, 0.0f, 0.995f);
}

const char* presetName(SkyAtmospherePreset preset)
{
    switch (preset) {
    case SkyAtmospherePreset::Clear: return "clear";
    case SkyAtmospherePreset::Hazy: return "hazy";
    case SkyAtmospherePreset::Foggy: return "foggy";
    default: return "unknown";
    }
}
}

void RealisticSkyBackend::initialize()
{
    if (!m_Renderer.initialise()) {
        std::cerr << "[Sky] Failed to initialize realistic PBR sky backend.\n";
        m_Initialized = false;
        return;
    }

    m_Initialized = true;

    // VoxelOps renders terrain/scene into external textures, so disable internal terrain pass.
    m_Renderer.setRenderTerrain(false);
    m_Renderer.setShadowMapsEnabled(true);
    m_Renderer.setFastSky(false);
    m_Renderer.setFastAerialPerspective(false);
    m_Renderer.setAerialPerspectiveQualityPreset(pbrsky::AerialPerspectiveQualityPreset::High);
    m_Renderer.setAerialPerspectiveSampleCountScale(1.5f);
    m_Renderer.setRayMarchMinSpp(8);
    m_Renderer.setRayMarchMaxSpp(24);
    m_Renderer.setColoredTransmittance(true);
    m_Renderer.setAutoExposureEnabled(true);
    m_Renderer.setUseHistogramAutoExposure(true);
    m_Renderer.setAutoExposureHistogramLowPercent(60.0f);
    m_Renderer.setAutoExposureHistogramHighPercent(96.0f);
    m_Renderer.setAutoExposureKey(0.115f);
    m_Renderer.setSunAngleExposureBiasEnabled(true);
    m_Renderer.setSunAngleExposureBiasAtHorizonEv(-0.70f);
    m_Renderer.setSunAngleExposureBiasAtNoonEv(0.70f);

    m_BaseAtmosphereInfo = m_Renderer.getAtmosphereInfo();
    applyAtmospherePresetToRenderer();
    std::cout << "[Sky] Realistic atmosphere preset: " << presetName(m_AtmospherePreset) << ".\n";
    applySunToRenderer();
}

void RealisticSkyBackend::shutdown()
{
    if (m_Initialized) {
        m_Renderer.clearExternalSceneTextures();
        m_Renderer.clearExternalShadowMapTexture();
    }
    m_Renderer.shutdown();
    m_Initialized = false;
}

void RealisticSkyBackend::resize(int width, int height)
{
    if (!m_Initialized) {
        return;
    }
    m_Renderer.resize(width, height);
}

void RealisticSkyBackend::setCameraFromActiveCamera(const Camera& activeCamera)
{
    if (!m_Initialized) {
        return;
    }

    m_Renderer.setCameraOffset(toPbrPositionKm(activeCamera.position));

    float yawRad = 0.0f;
    float pitchRad = 0.0f;
    toPbrYawPitch(activeCamera.front, yawRad, pitchRad);
    m_Renderer.setViewYaw(yawRad);
    m_Renderer.setViewPitch(pitchRad);
}

void RealisticSkyBackend::setViewFovYDegrees(float fovYDegrees)
{
    if (!m_Initialized) {
        return;
    }
    m_Renderer.setMainCameraFovYDegrees(fovYDegrees);
}

void RealisticSkyBackend::render(const glm::mat4& projection, const glm::mat4& view) const
{
    (void)projection;
    (void)view;
    if (!m_Initialized) {
        return;
    }
    m_Renderer.render();
}

void RealisticSkyBackend::setSunDir(const glm::vec3& sunDir)
{
    const float lenSq = glm::dot(sunDir, sunDir);
    if (lenSq <= 1e-8f) {
        return;
    }
    m_SunDir = glm::normalize(sunDir);
    if (m_Initialized) {
        applySunToRenderer();
    }
}

const glm::vec3& RealisticSkyBackend::getSunDir() const noexcept
{
    return m_SunDir;
}

bool RealisticSkyBackend::encodesOutputToSrgb() const noexcept
{
    return true;
}

bool RealisticSkyBackend::requiresExternalSceneTextures() const noexcept
{
    return true;
}

void RealisticSkyBackend::setExternalSceneTextures(unsigned int sceneColorTex, unsigned int sceneLinearDepthTex)
{
    if (!m_Initialized) {
        return;
    }
    m_Renderer.setExternalSceneTextures(sceneColorTex, sceneLinearDepthTex);
}

void RealisticSkyBackend::clearExternalSceneTextures()
{
    if (!m_Initialized) {
        return;
    }
    m_Renderer.clearExternalSceneTextures();
}

bool RealisticSkyBackend::supportsExternalShadowMap() const noexcept
{
    return true;
}

void RealisticSkyBackend::setExternalShadowMap(unsigned int shadowDepthCompareTex, const glm::mat4& shadowViewProj)
{
    if (!m_Initialized) {
        return;
    }
    m_Renderer.setExternalShadowMapTexture(shadowDepthCompareTex);
    m_Renderer.setExternalShadowViewProj(glm::value_ptr(shadowViewProj));
}

void RealisticSkyBackend::clearExternalShadowMap()
{
    if (!m_Initialized) {
        return;
    }
    m_Renderer.clearExternalShadowMapTexture();
}

bool RealisticSkyBackend::supportsAtmospherePresets() const noexcept
{
    return true;
}

void RealisticSkyBackend::setAtmospherePreset(SkyAtmospherePreset preset)
{
    m_AtmospherePreset = preset;
    if (m_Initialized) {
        applyAtmospherePresetToRenderer();
        std::cout << "[Sky] Realistic atmosphere preset switched to " << presetName(m_AtmospherePreset) << ".\n";
    }
}

SkyAtmospherePreset RealisticSkyBackend::getAtmospherePreset() const noexcept
{
    return m_AtmospherePreset;
}

pbrsky::Vec3 RealisticSkyBackend::toPbrPositionKm(const glm::vec3& voxelPositionMeters)
{
    // VoxelOps is meters + Y-up. PbrSkyLib expects kilometers + Z-up.
    return pbrsky::Vec3{
        voxelPositionMeters.x * kMetersToKilometers,
        voxelPositionMeters.z * kMetersToKilometers,
        voxelPositionMeters.y * kMetersToKilometers
    };
}

void RealisticSkyBackend::toPbrYawPitch(const glm::vec3& voxelDir, float& outYawRad, float& outPitchRad)
{
    const float lenSq = glm::dot(voxelDir, voxelDir);
    if (lenSq <= 1e-8f) {
        outYawRad = 0.0f;
        outPitchRad = 0.0f;
        return;
    }

    const glm::vec3 nd = glm::normalize(voxelDir);
    const glm::vec3 pbrDir(nd.x, nd.z, nd.y);
    const float clampedZ = std::clamp(pbrDir.z, -1.0f, 1.0f);
    outYawRad = std::atan2(pbrDir.x, pbrDir.y);
    outPitchRad = std::asin(clampedZ);
}

void RealisticSkyBackend::applySunToRenderer()
{
    float sunYawRad = 0.0f;
    float sunPitchRad = 0.0f;
    toPbrYawPitch(m_SunDir, sunYawRad, sunPitchRad);
    m_Renderer.setSunYaw(sunYawRad);
    m_Renderer.setSunPitch(sunPitchRad);
    m_Renderer.setSunIlluminanceScale(1.0f);
}

void RealisticSkyBackend::applyAtmospherePresetToRenderer()
{
    pbrsky::AtmosphereInfo info = m_BaseAtmosphereInfo;

    switch (m_AtmospherePreset) {
    case SkyAtmospherePreset::Clear:
        // Keep library defaults for clear weather.
        break;
    case SkyAtmospherePreset::Hazy:
        // Moderate haze (roughly 24km meteorological visibility).
        applyVisibilityKmPreset(info, 24.0f, 0.94f, 0.86f);
        break;
    case SkyAtmospherePreset::Foggy:
        // Dense fog/haze mix (roughly 4km visibility).
        applyVisibilityKmPreset(info, 4.0f, 0.90f, 0.90f);
        break;
    default:
        break;
    }

    m_Renderer.setAtmosphereInfo(info);
}
