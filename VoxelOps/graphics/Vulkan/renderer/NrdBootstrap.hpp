#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>

#include "graphics/Vulkan/renderer/RenderFrameData.hpp"

class NrdBootstrap final {
public:
    NrdBootstrap();
    ~NrdBootstrap() noexcept;

    NrdBootstrap(const NrdBootstrap &) = delete;
    NrdBootstrap &operator=(const NrdBootstrap &) = delete;
    NrdBootstrap(NrdBootstrap &&) = delete;
    NrdBootstrap &operator=(NrdBootstrap &&) = delete;

    void init();
    void shutdown();
    void updateFrame(
        const glm::mat4 &viewMatrix,
        const glm::mat4 &projectionMatrix,
        const glm::mat4 &prevViewMatrix,
        const glm::mat4 &prevProjectionMatrix,
        bool hasPrevMatrices,
        const FrameRenderData &frameData,
        uint32_t renderWidth,
        uint32_t renderHeight
    );

    bool isActive() const noexcept {
        return m_active;
    }
    uint32_t lastDispatchCount() const noexcept {
        return m_lastDispatchCount;
    }
    const void *instanceDescData() const noexcept {
        return m_instanceDescData;
    }
    const void *libraryDescData() const noexcept {
        return m_libraryDescData;
    }
    const void *dispatchDescData() const noexcept {
        return m_dispatchDescData;
    }
    uint32_t normalEncoding() const noexcept {
        return m_normalEncoding;
    }
    uint32_t roughnessEncoding() const noexcept {
        return m_roughnessEncoding;
    }

private:
    struct Impl;

    std::unique_ptr<Impl> m_impl;
    bool m_initialized = false;
    bool m_active = false;
    bool m_loggedUnavailable = false;
    uint32_t m_lastDispatchCount = 0;
    uint32_t m_frameIndex = 0;
    uint32_t m_normalEncoding = 2;   // nrd::NormalEncoding::R10_G10_B10_A2_UNORM
    uint32_t m_roughnessEncoding = 1; // nrd::RoughnessEncoding::LINEAR
    uint32_t m_prevRenderWidth = 0;
    uint32_t m_prevRenderHeight = 0;
    const void *m_instanceDescData = nullptr;
    const void *m_libraryDescData = nullptr;
    const void *m_dispatchDescData = nullptr;
};
