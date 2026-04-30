#pragma once
#include <string_view>
#include "../GraphicsBackend.hpp"

class Backend {
  public:
    Backend();

    int getOpenGLVersionMajor() const noexcept;
    int getOpenGLVersionMinor() const noexcept;
    bool isMDISupported() const noexcept;
    bool isShaderDrawParametersSupported() const noexcept;
    bool isMDIUsable() const noexcept;
    GraphicsBackend getActiveBackend() const noexcept;
    std::string_view getActiveBackendName() const noexcept;
    bool isInitialized() const noexcept;

  private:
    int OpenGLVersionMajor = 0;
    int OpenGLVersionMinor = 0;
    bool supportsMDI = false;
    bool supportsShaderDrawParameters = false;
    GraphicsBackend activeBackend = GraphicsBackend::Performance;
    bool initialized = false;

    int getMaxSupportedGLVersionMajor() const;
    int getMaxSupportedGLVersionMinor() const;
    GraphicsBackend chooseBackend() const;
    static bool isAtLeast(int major, int minor, int requiredMajor, int requiredMinor) noexcept;
};
