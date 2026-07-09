#pragma once

#include "../../ui/native/INativeUiRenderer.hpp"

#include <array>
#include <cstdint>
#include <unordered_map>

class OpenGlNativeUiRenderer final : public INativeUiRenderer {
public:
    OpenGlNativeUiRenderer() = default;
    ~OpenGlNativeUiRenderer() override;

    bool initialize(SDL_Window *window) override;
    void shutdown() override;
    void onWindowResized(int width, int height) override;

    void uploadTexture2D(
        NativeUiTextureHandle handle,
        int width,
        int height,
        const void *pixels,
        NativeUiTextureFormat format
    ) override;
    void destroyTexture(NativeUiTextureHandle texture) override;

    void render(const NativeUiDrawData &drawData) override;
    bool isInitialized() const noexcept override;

private:
    struct TextureResource {
        unsigned int id = 0;
        int width = 0;
        int height = 0;
        NativeUiTextureFormat format = NativeUiTextureFormat::Rgba8;
    };

    std::array<float, 16> projectionMatrix(const NativeUiDrawData &drawData) const;

    bool m_initialized = false;
    int m_width = 1;
    int m_height = 1;
    unsigned int m_shaderProgram = 0;
    unsigned int m_vao = 0;
    unsigned int m_vbo = 0;
    unsigned int m_ebo = 0;
    int m_projectionLoc = -1;
    int m_textureLoc = -1;
    int m_textureModeLoc = -1;
    std::unordered_map<NativeUiTextureHandle, TextureResource> m_textures;
};
