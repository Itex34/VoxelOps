#include "graphics/Vulkan/renderer/NrdBootstrap.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#ifndef VOXELOPS_NRD_HEADERS
#define VOXELOPS_NRD_HEADERS 0
#endif

#ifndef VOXELOPS_NRD_DLL_PATH
#define VOXELOPS_NRD_DLL_PATH ""
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

#if VOXELOPS_NRD_HEADERS
#include <NRD.h>
#endif

namespace {
constexpr uint32_t kNrdDenoiserId = 0x564F5855u; // "VOXU" as a stable ID.

inline uint16_t clampDimToU16(uint32_t v) {
    return static_cast<uint16_t>(std::min<uint32_t>(v, std::numeric_limits<uint16_t>::max()));
}

inline void copyMat4(const glm::mat4& src, float* dst16) {
    std::memcpy(dst16, glm::value_ptr(src), sizeof(float) * 16u);
}

inline glm::mat4 makeIdentity() {
    return glm::mat4(1.0f);
}
}

struct NrdBootstrap::Impl {
#if defined(_WIN32) && VOXELOPS_NRD_HEADERS
    using PfnCreateInstance = nrd::Result(NRD_CALL*)(const nrd::InstanceCreationDesc&, nrd::Instance*&);
    using PfnDestroyInstance = void(NRD_CALL*)(nrd::Instance&);
    using PfnGetLibraryDesc = const nrd::LibraryDesc* (NRD_CALL*)();
    using PfnGetInstanceDesc = const nrd::InstanceDesc* (NRD_CALL*)(const nrd::Instance&);
    using PfnSetCommonSettings = nrd::Result(NRD_CALL*)(nrd::Instance&, const nrd::CommonSettings&);
    using PfnSetDenoiserSettings = nrd::Result(NRD_CALL*)(nrd::Instance&, nrd::Identifier, const void*);
    using PfnGetComputeDispatches = nrd::Result(NRD_CALL*)(
        nrd::Instance&,
        const nrd::Identifier*,
        uint32_t,
        const nrd::DispatchDesc*&,
        uint32_t&
    );

    HMODULE module = nullptr;
    nrd::Instance* instance = nullptr;
    glm::mat4 prevWorldToView = makeIdentity();
    glm::mat4 prevViewToClip = makeIdentity();
    bool hasPrevMatrices = false;
    bool loggedDispatchInfo = false;

    PfnCreateInstance createInstance = nullptr;
    PfnDestroyInstance destroyInstance = nullptr;
    PfnGetLibraryDesc getLibraryDesc = nullptr;
    PfnGetInstanceDesc getInstanceDesc = nullptr;
    PfnSetCommonSettings setCommonSettings = nullptr;
    PfnSetDenoiserSettings setDenoiserSettings = nullptr;
    PfnGetComputeDispatches getComputeDispatches = nullptr;
#endif
};

NrdBootstrap::NrdBootstrap()
    : m_impl(std::make_unique<Impl>()) {
}

NrdBootstrap::~NrdBootstrap() noexcept {
    shutdown();
}

void NrdBootstrap::init() {
    if (m_initialized) {
        return;
    }
    m_initialized = true;

#if !defined(_WIN32) || !VOXELOPS_NRD_HEADERS
    if (!m_loggedUnavailable) {
        std::cout << "[NRD] Bootstrap unavailable (requires Windows build and NRD headers)." << "\n";
        m_loggedUnavailable = true;
    }
    return;
#else
    auto loadProc = [this](const char* name, FARPROC& outProc) -> bool {
        outProc = GetProcAddress(m_impl->module, name);
        if (outProc == nullptr) {
            std::cerr << "[NRD] Missing export: " << name << "\n";
            return false;
        }
        return true;
    };

    std::vector<std::string> candidateDlls;
    if (const char* envPath = std::getenv("VOXELOPS_NRD_DLL"); envPath != nullptr && envPath[0] != '\0') {
        candidateDlls.emplace_back(envPath);
    }
    if (VOXELOPS_NRD_DLL_PATH[0] != '\0') {
        candidateDlls.emplace_back(VOXELOPS_NRD_DLL_PATH);
    }
    candidateDlls.emplace_back("NRD.dll");

    for (const std::string& candidate : candidateDlls) {
        m_impl->module = LoadLibraryA(candidate.c_str());
        if (m_impl->module != nullptr) {
            std::cout << "[NRD] Loaded: " << candidate << "\n";
            break;
        }
    }

    if (m_impl->module == nullptr) {
        std::cerr << "[NRD] Failed to load NRD.dll. Set VOXELOPS_NRD_DLL to the DLL path." << "\n";
        return;
    }

    FARPROC proc = nullptr;
    if (!loadProc("CreateInstance", proc)) {
        return;
    }
    m_impl->createInstance = reinterpret_cast<Impl::PfnCreateInstance>(proc);

    if (!loadProc("DestroyInstance", proc)) {
        return;
    }
    m_impl->destroyInstance = reinterpret_cast<Impl::PfnDestroyInstance>(proc);

    if (!loadProc("GetLibraryDesc", proc)) {
        return;
    }
    m_impl->getLibraryDesc = reinterpret_cast<Impl::PfnGetLibraryDesc>(proc);

    if (!loadProc("GetInstanceDesc", proc)) {
        return;
    }
    m_impl->getInstanceDesc = reinterpret_cast<Impl::PfnGetInstanceDesc>(proc);

    if (!loadProc("SetCommonSettings", proc)) {
        return;
    }
    m_impl->setCommonSettings = reinterpret_cast<Impl::PfnSetCommonSettings>(proc);

    if (!loadProc("SetDenoiserSettings", proc)) {
        return;
    }
    m_impl->setDenoiserSettings = reinterpret_cast<Impl::PfnSetDenoiserSettings>(proc);

    if (!loadProc("GetComputeDispatches", proc)) {
        return;
    }
    m_impl->getComputeDispatches = reinterpret_cast<Impl::PfnGetComputeDispatches>(proc);

    nrd::DenoiserDesc denoiserDesc{};
    denoiserDesc.identifier = kNrdDenoiserId;
    denoiserDesc.denoiser = nrd::Denoiser::REBLUR_DIFFUSE;

    nrd::InstanceCreationDesc instanceCreationDesc{};
    instanceCreationDesc.denoisers = &denoiserDesc;
    instanceCreationDesc.denoisersNum = 1u;

    const nrd::Result createResult = m_impl->createInstance(instanceCreationDesc, m_impl->instance);
    if (createResult != nrd::Result::SUCCESS || m_impl->instance == nullptr) {
        std::cerr << "[NRD] CreateInstance failed with code " << static_cast<uint32_t>(createResult) << "\n";
        return;
    }

    if (const nrd::LibraryDesc* libraryDesc = m_impl->getLibraryDesc(); libraryDesc != nullptr) {
        m_libraryDescData = libraryDesc;
        std::cout
            << "[NRD] API ready. Version "
            << static_cast<uint32_t>(libraryDesc->versionMajor) << "."
            << static_cast<uint32_t>(libraryDesc->versionMinor) << "."
            << static_cast<uint32_t>(libraryDesc->versionBuild)
            << " (normalEncoding=" << static_cast<uint32_t>(libraryDesc->normalEncoding)
            << ", roughnessEncoding=" << static_cast<uint32_t>(libraryDesc->roughnessEncoding)
            << ")\n";
    }
    if (const nrd::InstanceDesc* instanceDesc = m_impl->getInstanceDesc(*m_impl->instance); instanceDesc != nullptr) {
        m_instanceDescData = instanceDesc;
    }

    m_active = true;
#endif
}

void NrdBootstrap::shutdown() {
    if (!m_initialized) {
        return;
    }

#if defined(_WIN32) && VOXELOPS_NRD_HEADERS
    if (m_impl != nullptr) {
        if (m_impl->instance != nullptr && m_impl->destroyInstance != nullptr) {
            m_impl->destroyInstance(*m_impl->instance);
            m_impl->instance = nullptr;
        }
        if (m_impl->module != nullptr) {
            FreeLibrary(m_impl->module);
            m_impl->module = nullptr;
        }
        m_impl->hasPrevMatrices = false;
        m_impl->loggedDispatchInfo = false;
    }
#endif

    m_active = false;
    m_initialized = false;
    m_lastDispatchCount = 0;
    m_frameIndex = 0;
    m_instanceDescData = nullptr;
    m_libraryDescData = nullptr;
    m_dispatchDescData = nullptr;
}

void NrdBootstrap::updateFrame(
    const glm::mat4& viewMatrix,
    const glm::mat4& projectionMatrix,
    const glm::mat4& prevViewMatrix,
    const glm::mat4& prevProjectionMatrix,
    bool hasPrevMatrices,
    const FrameRenderData& frameData,
    uint32_t renderWidth,
    uint32_t renderHeight
) {
    m_dispatchDescData = nullptr;
    m_lastDispatchCount = 0;

    if (!m_active) {
        return;
    }

#if !defined(_WIN32) || !VOXELOPS_NRD_HEADERS
    (void)viewMatrix;
    (void)projectionMatrix;
    (void)prevViewMatrix;
    (void)prevProjectionMatrix;
    (void)hasPrevMatrices;
    (void)frameData;
    (void)renderWidth;
    (void)renderHeight;
    return;
#else
    if (m_impl == nullptr || m_impl->instance == nullptr) {
        return;
    }
    if (!frameData.giLighting.enabled || renderWidth == 0 || renderHeight == 0) {
        return;
    }

    nrd::CommonSettings commonSettings{};
    copyMat4(projectionMatrix, commonSettings.viewToClipMatrix);
    copyMat4(hasPrevMatrices ? prevProjectionMatrix : projectionMatrix, commonSettings.viewToClipMatrixPrev);
    copyMat4(viewMatrix, commonSettings.worldToViewMatrix);
    copyMat4(hasPrevMatrices ? prevViewMatrix : viewMatrix, commonSettings.worldToViewMatrixPrev);
    commonSettings.resourceSize[0] = clampDimToU16(renderWidth);
    commonSettings.resourceSize[1] = clampDimToU16(renderHeight);
    commonSettings.resourceSizePrev[0] = commonSettings.resourceSize[0];
    commonSettings.resourceSizePrev[1] = commonSettings.resourceSize[1];
    commonSettings.rectSize[0] = commonSettings.resourceSize[0];
    commonSettings.rectSize[1] = commonSettings.resourceSize[1];
    commonSettings.rectSizePrev[0] = commonSettings.resourceSize[0];
    commonSettings.rectSizePrev[1] = commonSettings.resourceSize[1];
    commonSettings.motionVectorScale[0] = 1.0f / static_cast<float>(std::max(1u, renderWidth));
    commonSettings.motionVectorScale[1] = 1.0f / static_cast<float>(std::max(1u, renderHeight));
    commonSettings.motionVectorScale[2] = 0.0f;
    commonSettings.viewZScale = 1.0f;
    commonSettings.disocclusionThreshold = 0.01f;
    commonSettings.disocclusionThresholdAlternate = 0.10f;
    commonSettings.frameIndex = m_frameIndex++;
    commonSettings.accumulationMode = frameData.giLighting.resetHistory
        ? nrd::AccumulationMode::RESTART
        : nrd::AccumulationMode::CONTINUE;

    const nrd::Result commonResult = m_impl->setCommonSettings(*m_impl->instance, commonSettings);
    if (commonResult != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] SetCommonSettings failed with code " << static_cast<uint32_t>(commonResult) << "\n";
        return;
    }

    nrd::ReblurSettings reblurSettings{};
    const float temporalBlend = glm::clamp(frameData.giLighting.denoiseTemporalBlend, 0.0f, 1.0f);
    const float accumulationTimeSec = 0.10f + (temporalBlend * 0.90f);
    const uint32_t maxFrames = nrd::GetMaxAccumulatedFrameNum(accumulationTimeSec, 60.0f);
    reblurSettings.maxAccumulatedFrameNum = std::clamp(maxFrames, 1u, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
    reblurSettings.maxFastAccumulatedFrameNum = std::max(1u, reblurSettings.maxAccumulatedFrameNum / 5u);
    reblurSettings.maxStabilizedFrameNum = reblurSettings.maxAccumulatedFrameNum;
    reblurSettings.enableAntiFirefly = true;
    reblurSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;

    const nrd::Result settingsResult =
        m_impl->setDenoiserSettings(*m_impl->instance, kNrdDenoiserId, &reblurSettings);
    if (settingsResult != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] SetDenoiserSettings failed with code " << static_cast<uint32_t>(settingsResult) << "\n";
        return;
    }

    const nrd::Identifier denoiserIds[] = { kNrdDenoiserId };
    const nrd::DispatchDesc* dispatchDescs = nullptr;
    uint32_t dispatchCount = 0;
    const nrd::Result dispatchResult =
        m_impl->getComputeDispatches(*m_impl->instance, denoiserIds, 1u, dispatchDescs, dispatchCount);
    if (dispatchResult != nrd::Result::SUCCESS) {
        std::cerr << "[NRD] GetComputeDispatches failed with code " << static_cast<uint32_t>(dispatchResult) << "\n";
        return;
    }

    m_dispatchDescData = dispatchDescs;
    m_lastDispatchCount = dispatchCount;
    if (!m_impl->loggedDispatchInfo) {
        std::cout << "[NRD] REBLUR_DIFFUSE bootstrap dispatches: " << dispatchCount << "\n";
        m_impl->loggedDispatchInfo = true;
    }

    m_impl->prevWorldToView = viewMatrix;
    m_impl->prevViewToClip = projectionMatrix;
    m_impl->hasPrevMatrices = true;
#endif
}
