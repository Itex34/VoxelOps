#pragma once

#include <cstddef>
#include <cstdint>

using NativeUiTextureHandle = std::uint64_t;

enum class NativeUiTextureFormat : std::uint8_t {
    R8 = 1,
    Rgba8 = 2
};

enum class NativeUiTextureMode : std::uint8_t {
    Solid = 0,
    AlphaR8 = 1,
    Rgba = 2
};

struct NativeUiVertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct NativeUiClipRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct NativeUiDrawBatch {
    NativeUiTextureHandle texture = 0;
    NativeUiTextureMode textureMode = NativeUiTextureMode::Solid;
    bool clipEnabled = false;
    NativeUiClipRect clip{};
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
};

struct NativeUiTextureUpload {
    NativeUiTextureHandle handle = 0;
    NativeUiTextureFormat format = NativeUiTextureFormat::R8;
    int width = 0;
    int height = 0;
    const void *pixels = nullptr;
    std::size_t byteCount = 0;
};

struct NativeUiDrawData {
    int width = 1;
    int height = 1;
    const NativeUiTextureUpload *textureUploads = nullptr;
    std::size_t textureUploadCount = 0;
    const NativeUiVertex *vertices = nullptr;
    std::size_t vertexCount = 0;
    const std::uint32_t *indices = nullptr;
    std::size_t indexCount = 0;
    const NativeUiDrawBatch *batches = nullptr;
    std::size_t batchCount = 0;
};
