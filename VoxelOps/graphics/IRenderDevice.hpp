#pragma once

#include <string_view>

#include "Backend.hpp"

struct RenderFrameParams;

class IRenderDevice {
public:
    virtual ~IRenderDevice() = default;

    virtual int getOpenGLVersionMajor() const noexcept = 0;
    virtual int getOpenGLVersionMinor() const noexcept = 0;
    virtual GraphicsBackend getActiveBackend() const noexcept = 0;
    virtual std::string_view getActiveBackendName() const noexcept = 0;
    virtual bool isMDIUsable() const noexcept = 0;

    virtual void renderFrame(RenderFrameParams& params) = 0;
    virtual void shutdown() = 0;

    virtual std::string_view getApiName() const noexcept = 0;
};
