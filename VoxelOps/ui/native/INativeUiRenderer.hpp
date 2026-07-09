#pragma once

#include "NativeUiDrawData.hpp"

struct SDL_Window;

class INativeUiRenderer {
public:
    virtual ~INativeUiRenderer() = default;

    virtual bool initialize(SDL_Window *window) = 0;
    virtual void shutdown() = 0;
    virtual void onWindowResized(int width, int height) = 0;

    virtual void uploadTexture2D(
        NativeUiTextureHandle handle,
        int width,
        int height,
        const void *pixels,
        NativeUiTextureFormat format
    ) = 0;
    virtual void destroyTexture(NativeUiTextureHandle texture) = 0;

    virtual void render(const NativeUiDrawData &drawData) = 0;
    virtual bool isInitialized() const noexcept = 0;
};
