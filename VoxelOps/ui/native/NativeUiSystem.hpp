#pragma once

#include "../text/Font.hpp"
#include "../widgets/UIContext.hpp"
#include "../../graphics/RenderApi.hpp"
#include "../../graphics/TextureHandle.hpp"
#include "NativeUiDrawData.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

union SDL_Event;
struct SDL_Window;

enum class NativeUiIcon : std::uint8_t {
    Close = 0,
    Paste,
    Connect,
    Use,
    DropOne,
    DropStack,
    Count
};

class NativeUiSystem final {
public:
    NativeUiSystem();
    ~NativeUiSystem();

    NativeUiSystem(const NativeUiSystem &) = delete;
    NativeUiSystem &operator=(const NativeUiSystem &) = delete;

    bool initialize(SDL_Window *window, RenderApi api);
    void shutdown();
    void onWindowResized(int width, int height);
    void processEvent(const SDL_Event &event);
    void beginFrame(float dt);
    void endFrame();

    bool isInitialized() const noexcept;
    bool hasBackendRenderer() const noexcept;
    bool isUsingOpenGlBackend() const noexcept;
    bool wantsMouseCapture() const noexcept;
    bool wantsKeyboardCapture() const noexcept;

    UIContext &context() noexcept;
    const UIContext &context() const noexcept;
    const NativeUiDrawData *drawData() const noexcept;
    TextureHandle createTexture2D(
        int width,
        int height,
        const void *pixels,
        NativeUiTextureFormat format = NativeUiTextureFormat::Rgba8
    );
    TextureHandle loadTextureRgbaFile(std::string_view path);
    TextureHandle atlasTile(std::string_view tileName);
    TextureHandle icon(NativeUiIcon icon);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};
