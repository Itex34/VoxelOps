#include "NativeUiSystem.hpp"

#include "../text/FontAtlasBuilder.hpp"
#include "../../graphics/AtlasLayout.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <SDL3/SDL.h>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
    constexpr NativeUiTextureHandle kFontAtlasTextureHandle = 1;
    constexpr TextureHandle kFirstUserTextureHandle = 2;
    constexpr int kBuiltinIconSize = 32;

    struct ActiveClip {
        bool enabled = false;
        NativeUiClipRect rect{};
    };

    NativeUiClipRect ToNativeClip(Rect rect) {
        return NativeUiClipRect{rect.x, rect.y, rect.w, rect.h};
    }

    NativeUiClipRect IntersectClips(NativeUiClipRect a, NativeUiClipRect b) {
        const float x0 = std::max(a.x, b.x);
        const float y0 = std::max(a.y, b.y);
        const float x1 = std::min(a.x + a.w, b.x + b.w);
        const float y1 = std::min(a.y + a.h, b.y + b.h);
        return NativeUiClipRect{x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }

    bool ClipsEqual(NativeUiClipRect a, NativeUiClipRect b) {
        return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }

    void AppendBatch(
        std::vector<NativeUiDrawBatch> &batches,
        NativeUiTextureHandle texture,
        NativeUiTextureMode mode,
        ActiveClip clip,
        std::uint32_t indexCount
    ) {
        if (indexCount == 0) {
            return;
        }
        if (!batches.empty()) {
            NativeUiDrawBatch &last = batches.back();
            if (last.texture == texture && last.textureMode == mode && last.clipEnabled == clip.enabled &&
                (!clip.enabled || ClipsEqual(last.clip, clip.rect))) {
                last.indexCount += indexCount;
                return;
            }
        }

        NativeUiDrawBatch batch;
        batch.texture = texture;
        batch.textureMode = mode;
        batch.clipEnabled = clip.enabled;
        batch.clip = clip.rect;
        batch.indexOffset = batches.empty() ? 0 : batches.back().indexOffset + batches.back().indexCount;
        batch.indexCount = indexCount;
        batches.push_back(batch);
    }

    void AddQuad(
        std::vector<NativeUiVertex> &vertices,
        std::vector<std::uint32_t> &indices,
        Rect rect,
        float u0,
        float v0,
        float u1,
        float v1,
        Color color
    ) {
        const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back(NativeUiVertex{rect.x, rect.y, u0, v0, color.r, color.g, color.b, color.a});
        vertices.push_back(
            NativeUiVertex{rect.x + rect.w, rect.y, u1, v0, color.r, color.g, color.b, color.a}
        );
        vertices.push_back(
            NativeUiVertex{rect.x + rect.w, rect.y + rect.h, u1, v1, color.r, color.g, color.b, color.a}
        );
        vertices.push_back(
            NativeUiVertex{rect.x, rect.y + rect.h, u0, v1, color.r, color.g, color.b, color.a}
        );

        indices.push_back(base);
        indices.push_back(base + 1u);
        indices.push_back(base + 2u);
        indices.push_back(base);
        indices.push_back(base + 2u);
        indices.push_back(base + 3u);
    }

    bool IsPasteShortcut(const SDL_KeyboardEvent &event) {
        const SDL_Keymod mod = event.mod;
        const bool ctrlOrGui = (mod & SDL_KMOD_CTRL) != 0 || (mod & SDL_KMOD_GUI) != 0;
        const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
        return (ctrlOrGui && event.scancode == SDL_SCANCODE_V) ||
               (shift && event.scancode == SDL_SCANCODE_INSERT);
    }

    std::string ReadClipboardText() {
        char *clipboardText = SDL_GetClipboardText();
        if (clipboardText == nullptr || clipboardText[0] == '\0') {
            if (clipboardText != nullptr) {
                SDL_free(clipboardText);
            }
            return {};
        }

        std::string text = clipboardText;
        SDL_free(clipboardText);
        return text;
    }

    std::size_t BytesPerPixel(NativeUiTextureFormat format) {
        switch (format) {
        case NativeUiTextureFormat::Rgba8:
            return 4;
        case NativeUiTextureFormat::R8:
        default:
            return 1;
        }
    }

    void SetPixel(
        std::vector<std::uint8_t> &pixels,
        int width,
        int height,
        int x,
        int y,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint8_t a
    ) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const std::size_t offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) *
                                   4u;
        pixels[offset + 0u] = r;
        pixels[offset + 1u] = g;
        pixels[offset + 2u] = b;
        pixels[offset + 3u] = std::max(pixels[offset + 3u], a);
    }

    void DrawDisc(
        std::vector<std::uint8_t> &pixels,
        int width,
        int height,
        int cx,
        int cy,
        int radius,
        std::uint8_t r,
        std::uint8_t g,
        std::uint8_t b,
        std::uint8_t a
    ) {
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                const int dx = x - cx;
                const int dy = y - cy;
                if ((dx * dx + dy * dy) <= (radius * radius)) {
                    SetPixel(pixels, width, height, x, y, r, g, b, a);
                }
            }
        }
    }

    void DrawLine(
        std::vector<std::uint8_t> &pixels,
        int width,
        int height,
        int x0,
        int y0,
        int x1,
        int y1,
        int thickness,
        std::uint8_t r = 255,
        std::uint8_t g = 255,
        std::uint8_t b = 255,
        std::uint8_t a = 255
    ) {
        const int dx = x1 - x0;
        const int dy = y1 - y0;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        if (steps == 0) {
            DrawDisc(pixels, width, height, x0, y0, thickness, r, g, b, a);
            return;
        }
        for (int i = 0; i <= steps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(steps);
            const int x = static_cast<int>(std::round(static_cast<float>(x0) + static_cast<float>(dx) * t));
            const int y = static_cast<int>(std::round(static_cast<float>(y0) + static_cast<float>(dy) * t));
            DrawDisc(pixels, width, height, x, y, thickness, r, g, b, a);
        }
    }

    void DrawRectOutline(
        std::vector<std::uint8_t> &pixels,
        int width,
        int height,
        int x,
        int y,
        int w,
        int h,
        int thickness
    ) {
        DrawLine(pixels, width, height, x, y, x + w, y, thickness);
        DrawLine(pixels, width, height, x, y + h, x + w, y + h, thickness);
        DrawLine(pixels, width, height, x, y, x, y + h, thickness);
        DrawLine(pixels, width, height, x + w, y, x + w, y + h, thickness);
    }

    std::vector<std::uint8_t> BuildIconPixels(NativeUiIcon icon) {
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kBuiltinIconSize * kBuiltinIconSize * 4), 0);

        switch (icon) {
        case NativeUiIcon::Close:
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 9, 9, 23, 23, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 23, 9, 9, 23, 2);
            break;
        case NativeUiIcon::Paste:
            DrawRectOutline(pixels, kBuiltinIconSize, kBuiltinIconSize, 9, 10, 14, 16, 1);
            DrawRectOutline(pixels, kBuiltinIconSize, kBuiltinIconSize, 12, 7, 8, 5, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 12, 16, 20, 16, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 12, 20, 19, 20, 1);
            break;
        case NativeUiIcon::Connect:
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 8, 16, 22, 16, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 18, 10, 24, 16, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 18, 22, 24, 16, 2);
            break;
        case NativeUiIcon::Use:
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 8, 17, 14, 23, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 14, 23, 24, 9, 2);
            break;
        case NativeUiIcon::DropOne:
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 16, 7, 16, 21, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 10, 16, 16, 22, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 22, 16, 16, 22, 2);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 10, 25, 22, 25, 1);
            break;
        case NativeUiIcon::DropStack:
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 12, 7, 12, 17, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 8, 14, 12, 18, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 16, 14, 12, 18, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 20, 7, 20, 17, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 16, 14, 20, 18, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 24, 14, 20, 18, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 9, 23, 23, 23, 1);
            DrawLine(pixels, kBuiltinIconSize, kBuiltinIconSize, 11, 26, 21, 26, 1);
            break;
        case NativeUiIcon::Count:
            break;
        }

        return pixels;
    }
}

class NativeUiSystem::Impl {
    struct UiTexture {
        TextureHandle handle = 0;
        NativeUiTextureFormat format = NativeUiTextureFormat::Rgba8;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> pixels;
    };

public:
    bool initialize(SDL_Window *window, RenderApi api) {
        if (window == nullptr) {
            return false;
        }

        (void)api;
        m_window = window;
        (void)SDL_StartTextInput(window);

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        onWindowResized(width, height);

        const std::string fontPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("Assets/fonts/SF/SF-Pro-Text-Medium.otf").generic_string();
        try {
            BuiltFontAtlas atlas = FontAtlasBuilder::buildFromFile(fontPath, 16u);
            m_font = std::move(atlas.font);
            m_fontAtlasPixels = std::move(atlas.pixelsR8);
            m_fontAtlasTexture = kFontAtlasTextureHandle;
        } catch (const std::exception &err) {
            std::cerr << "[NativeUI] Failed to build font atlas: " << err.what() << "\n";
            shutdown();
            return false;
        }

        if (m_fontAtlasTexture == 0) {
            std::cerr << "[NativeUI] Failed to upload font atlas.\n";
            shutdown();
            return false;
        }

        m_vertices.reserve(4096);
        m_indices.reserve(8192);
        m_batches.reserve(64);
        m_initialized = true;
        return true;
    }

    void shutdown() {
        if (m_window != nullptr) {
            (void)SDL_StopTextInput(m_window);
            m_window = nullptr;
        }

        m_font = Font{};
        m_fontAtlasTexture = 0;
        m_fontAtlasPixels.clear();
        m_uiTextures.clear();
        m_iconTextures.fill(0);
        m_atlasTileTextures.clear();
        m_atlasPixels.clear();
        m_atlasWidth = 0;
        m_atlasHeight = 0;
        m_nextTextureHandle = kFirstUserTextureHandle;

        m_vertices.clear();
        m_indices.clear();
        m_batches.clear();
        m_initialized = false;
    }

    void onWindowResized(int width, int height) {
        m_width = std::max(width, 1);
        m_height = std::max(height, 1);
    }

    void processEvent(const SDL_Event &event) {
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            m_input.mousePos = glm::vec2(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_input.mouseDown = true;
                m_input.mousePressed = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                m_input.mouseDown = false;
                m_input.mouseReleased = true;
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (event.text.text != nullptr) {
                m_input.textInput += event.text.text;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            m_input.ctrlDown = (event.key.mod & SDL_KMOD_CTRL) != 0 ||
                               (event.key.mod & SDL_KMOD_GUI) != 0;
            if (IsPasteShortcut(event.key)) {
                m_input.pasteText += ReadClipboardText();
                break;
            }
            switch (event.key.scancode) {
            case SDL_SCANCODE_BACKSPACE:
                m_input.backspacePressed = true;
                break;
            case SDL_SCANCODE_DELETE:
                m_input.deletePressed = true;
                break;
            case SDL_SCANCODE_LEFT:
                m_input.leftPressed = true;
                break;
            case SDL_SCANCODE_RIGHT:
                m_input.rightPressed = true;
                break;
            case SDL_SCANCODE_HOME:
                m_input.homePressed = true;
                break;
            case SDL_SCANCODE_END:
                m_input.endPressed = true;
                break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER:
                m_input.enterPressed = true;
                break;
            case SDL_SCANCODE_TAB:
                m_input.tabPressed = true;
                break;
            default:
                break;
            }
            break;
        case SDL_EVENT_KEY_UP:
            m_input.ctrlDown = (event.key.mod & SDL_KMOD_CTRL) != 0 ||
                               (event.key.mod & SDL_KMOD_GUI) != 0;
            break;
        default:
            break;
        }
    }

    void beginFrame(float dt) {
        m_context.beginFrame(glm::vec2(static_cast<float>(m_width), static_cast<float>(m_height)), m_input, dt);
        m_input.mousePressed = false;
        m_input.mouseReleased = false;
        m_input.backspacePressed = false;
        m_input.deletePressed = false;
        m_input.leftPressed = false;
        m_input.rightPressed = false;
        m_input.homePressed = false;
        m_input.endPressed = false;
        m_input.enterPressed = false;
        m_input.tabPressed = false;
        m_input.textInput.clear();
        m_input.pasteText.clear();
    }

    void endFrame() {
        m_context.endFrame();
        m_wantsMouseCapture = m_context.wantsMouseCapture();
        m_wantsKeyboardCapture = m_context.wantsKeyboardCapture();
        buildDrawData();
    }

    bool isInitialized() const noexcept {
        return m_initialized;
    }

    bool hasRenderer() const noexcept {
        return m_initialized;
    }

    bool isUsingOpenGlBackend() const noexcept {
        return false;
    }

    bool wantsMouseCapture() const noexcept {
        return m_wantsMouseCapture;
    }

    bool wantsKeyboardCapture() const noexcept {
        return m_wantsKeyboardCapture;
    }

    UIContext &context() noexcept {
        return m_context;
    }

    const UIContext &context() const noexcept {
        return m_context;
    }

    const NativeUiDrawData *drawData() const noexcept {
        return &m_drawData;
    }

    TextureHandle createTexture2D(int width, int height, const void *pixels, NativeUiTextureFormat format) {
        if (width <= 0 || height <= 0 || pixels == nullptr) {
            return 0;
        }
        const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        const std::size_t byteCount = pixelCount * BytesPerPixel(format);
        if (byteCount == 0 || m_nextTextureHandle == 0) {
            return 0;
        }

        UiTexture texture;
        texture.handle = m_nextTextureHandle++;
        texture.format = format;
        texture.width = width;
        texture.height = height;
        const auto *source = static_cast<const std::uint8_t *>(pixels);
        texture.pixels.assign(source, source + byteCount);

        const TextureHandle handle = texture.handle;
        m_uiTextures.push_back(std::move(texture));
        return handle;
    }

    TextureHandle loadTextureRgbaFile(std::string_view path) {
        if (path.empty()) {
            return 0;
        }

        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char *pixels = stbi_load(std::string(path).c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            std::cerr << "[NativeUI] Failed to load image: " << path << "\n";
            return 0;
        }

        const TextureHandle handle = createTexture2D(width, height, pixels, NativeUiTextureFormat::Rgba8);
        stbi_image_free(pixels);
        return handle;
    }

    TextureHandle atlasTile(std::string_view tileName) {
        if (tileName.empty()) {
            return 0;
        }

        const std::string key(tileName);
        const auto cached = m_atlasTileTextures.find(key);
        if (cached != m_atlasTileTextures.end()) {
            return cached->second;
        }

        const auto tileIt = m_atlasLayout.tileMap.find(key);
        if (tileIt == m_atlasLayout.tileMap.end() || !ensureAtlasPixelsLoaded()) {
            return 0;
        }

        const glm::ivec2 tile = tileIt->second;
        if (tile.x < 0 || tile.y < 0 || tile.x >= TEXTURE_ATLAS_SIZE || tile.y >= TEXTURE_ATLAS_SIZE) {
            return 0;
        }

        std::vector<std::uint8_t> tilePixels(
            static_cast<std::size_t>(TILE_RESOLUTION * TILE_RESOLUTION * 4),
            0
        );
        const int sourceX = tile.x * TILE_RESOLUTION;
        const int sourceY = (TEXTURE_ATLAS_SIZE - 1 - tile.y) * TILE_RESOLUTION;
        for (int row = 0; row < TILE_RESOLUTION; ++row) {
            const std::size_t srcOffset =
                (static_cast<std::size_t>(sourceY + row) * static_cast<std::size_t>(m_atlasWidth) +
                 static_cast<std::size_t>(sourceX)) *
                4u;
            const std::size_t dstOffset = static_cast<std::size_t>(row * TILE_RESOLUTION) * 4u;
            std::copy_n(
                m_atlasPixels.data() + srcOffset,
                static_cast<std::size_t>(TILE_RESOLUTION * 4),
                tilePixels.data() + dstOffset
            );
        }

        const TextureHandle handle =
            createTexture2D(TILE_RESOLUTION, TILE_RESOLUTION, tilePixels.data(), NativeUiTextureFormat::Rgba8);
        if (handle != 0) {
            m_atlasTileTextures.emplace(key, handle);
        }
        return handle;
    }

    TextureHandle icon(NativeUiIcon icon) {
        const std::size_t index = static_cast<std::size_t>(icon);
        if (index >= m_iconTextures.size()) {
            return 0;
        }

        TextureHandle &handle = m_iconTextures[index];
        if (handle != 0) {
            return handle;
        }

        const std::vector<std::uint8_t> pixels = BuildIconPixels(icon);
        handle = createTexture2D(
            kBuiltinIconSize, kBuiltinIconSize, pixels.data(), NativeUiTextureFormat::Rgba8
        );
        return handle;
    }

private:
    bool ensureAtlasPixelsLoaded() {
        if (!m_atlasPixels.empty()) {
            return true;
        }

        const std::string atlasPath =
            Shared::RuntimePaths::ResolveVoxelOpsPath("assets/textures/textureAtlas.png").generic_string();
        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char *pixels = stbi_load(atlasPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            std::cerr << "[NativeUI] Failed to load texture atlas: " << atlasPath << "\n";
            return false;
        }

        if (width != TEXTURE_ATLAS_SIZE * TILE_RESOLUTION || height != TEXTURE_ATLAS_SIZE * TILE_RESOLUTION) {
            std::cerr << "[NativeUI] Unexpected texture atlas size: " << width << "x" << height << "\n";
            stbi_image_free(pixels);
            return false;
        }

        m_atlasWidth = width;
        m_atlasHeight = height;
        const std::size_t byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
        m_atlasPixels.assign(pixels, pixels + byteCount);
        stbi_image_free(pixels);
        return true;
    }

    void buildDrawData() {
        m_vertices.clear();
        m_indices.clear();
        m_batches.clear();

        if (m_fontAtlasTexture == 0) {
            updateDrawDataView();
            return;
        }

        std::vector<ActiveClip> clipStack;
        ActiveClip currentClip;

        for (const UICommand &command : m_context.getCommands()) {
            if (const auto *rect = std::get_if<RectCommand>(&command)) {
                const std::uint32_t before = static_cast<std::uint32_t>(m_indices.size());
                AddQuad(m_vertices, m_indices, rect->rect, 0.0f, 0.0f, 0.0f, 0.0f, rect->color);
                AppendBatch(
                    m_batches,
                    0,
                    NativeUiTextureMode::Solid,
                    currentClip,
                    static_cast<std::uint32_t>(m_indices.size()) - before
                );
            } else if (const auto *image = std::get_if<ImageCommand>(&command)) {
                const std::uint32_t before = static_cast<std::uint32_t>(m_indices.size());
                AddQuad(m_vertices, m_indices, image->rect, 0.0f, 0.0f, 1.0f, 1.0f, image->tint);
                AppendBatch(
                    m_batches,
                    image->texture,
                    NativeUiTextureMode::Rgba,
                    currentClip,
                    static_cast<std::uint32_t>(m_indices.size()) - before
                );
            } else if (const auto *text = std::get_if<TextCommand>(&command)) {
                appendText(*text, currentClip);
            } else if (const auto *clip = std::get_if<ClipPushCommand>(&command)) {
                ActiveClip pushed;
                pushed.enabled = true;
                pushed.rect = currentClip.enabled
                    ? IntersectClips(currentClip.rect, ToNativeClip(clip->rect))
                    : ToNativeClip(clip->rect);
                clipStack.push_back(pushed);
                currentClip = pushed;
            } else if (std::holds_alternative<ClipPopCommand>(command)) {
                if (!clipStack.empty()) {
                    clipStack.pop_back();
                }
                currentClip = clipStack.empty() ? ActiveClip{} : clipStack.back();
            }
        }

        updateDrawDataView();
    }

    void appendText(const TextCommand &text, ActiveClip clip) {
        TextCommand alignedText = text;
        if (text.bounded) {
            alignedText.text = fitTextToWidth(text.text, text.rect.w);
            alignedText.pos = alignedTextPosition(alignedText);
        }

        const std::uint32_t before = static_cast<std::uint32_t>(m_indices.size());
        float x = alignedText.pos.x;
        float baselineY = alignedText.pos.y + static_cast<float>(m_font.ascender);
        const float startX = x;
        const float lineAdvance = static_cast<float>(std::max(m_font.lineHeight, static_cast<int>(m_font.pixelSize)));

        for (const char ch : alignedText.text) {
            if (ch == '\n') {
                x = startX;
                baselineY += lineAdvance;
                continue;
            }

            const char32_t codepoint = static_cast<unsigned char>(ch);
            auto it = m_font.glyphs.find(codepoint);
            if (it == m_font.glyphs.end()) {
                it = m_font.glyphs.find(U'?');
            }
            if (it == m_font.glyphs.end()) {
                continue;
            }

            const GlyphBitmap &glyph = it->second;
            if (glyph.width > 0 && glyph.height > 0) {
                Rect rect{
                    x + static_cast<float>(glyph.bearingX),
                    baselineY - static_cast<float>(glyph.bearingY),
                    static_cast<float>(glyph.width),
                    static_cast<float>(glyph.height)
                };
                AddQuad(m_vertices, m_indices, rect, glyph.u0, glyph.v0, glyph.u1, glyph.v1, text.color);
            }
            x += static_cast<float>(glyph.advanceX) / 64.0f;
        }

        AppendBatch(
            m_batches,
            m_fontAtlasTexture,
            NativeUiTextureMode::AlphaR8,
            clip,
            static_cast<std::uint32_t>(m_indices.size()) - before
        );
    }

    glm::vec2 measureText(std::string_view text) const {
        float maxWidth = 0.0f;
        float lineWidth = 0.0f;
        int lineCount = 1;
        for (const char ch : text) {
            if (ch == '\n') {
                maxWidth = std::max(maxWidth, lineWidth);
                lineWidth = 0.0f;
                ++lineCount;
                continue;
            }

            const char32_t codepoint = static_cast<unsigned char>(ch);
            auto it = m_font.glyphs.find(codepoint);
            if (it == m_font.glyphs.end()) {
                it = m_font.glyphs.find(U'?');
            }
            if (it == m_font.glyphs.end()) {
                continue;
            }
            lineWidth += static_cast<float>(it->second.advanceX) / 64.0f;
        }

        maxWidth = std::max(maxWidth, lineWidth);
        const float lineAdvance = static_cast<float>(std::max(m_font.lineHeight, static_cast<int>(m_font.pixelSize)));
        return glm::vec2(maxWidth, static_cast<float>(lineCount) * lineAdvance);
    }

    float measureLineWidth(std::string_view text) const {
        float width = 0.0f;
        for (const char ch : text) {
            if (ch == '\n') {
                break;
            }

            const char32_t codepoint = static_cast<unsigned char>(ch);
            auto it = m_font.glyphs.find(codepoint);
            if (it == m_font.glyphs.end()) {
                it = m_font.glyphs.find(U'?');
            }
            if (it == m_font.glyphs.end()) {
                continue;
            }
            width += static_cast<float>(it->second.advanceX) / 64.0f;
        }
        return width;
    }

    std::string fitTextToWidth(std::string_view text, float maxWidth) const {
        if (text.empty() || maxWidth <= 0.0f) {
            return {};
        }

        std::string singleLine;
        singleLine.reserve(text.size());
        for (const char ch : text) {
            if (ch == '\n' || ch == '\r' || ch == '\t') {
                singleLine.push_back(' ');
            } else {
                singleLine.push_back(ch);
            }
        }

        if (measureLineWidth(singleLine) <= maxWidth) {
            return singleLine;
        }

        constexpr std::string_view ellipsis = "...";
        const float ellipsisWidth = measureLineWidth(ellipsis);
        if (ellipsisWidth > maxWidth) {
            return {};
        }

        std::string fitted;
        fitted.reserve(singleLine.size());
        for (const char ch : singleLine) {
            fitted.push_back(ch);
            if (measureLineWidth(fitted) + ellipsisWidth > maxWidth) {
                fitted.pop_back();
                break;
            }
        }

        while (!fitted.empty() && fitted.back() == ' ') {
            fitted.pop_back();
        }
        fitted += ellipsis;
        return fitted;
    }

    glm::vec2 alignedTextPosition(const TextCommand &text) const {
        const glm::vec2 textSize = measureText(text.text);
        float x = text.rect.x;
        if (text.align == TextAlign::Center) {
            x = text.rect.x + std::max(0.0f, text.rect.w - textSize.x) * 0.5f;
        } else if (text.align == TextAlign::End) {
            x = text.rect.x + std::max(0.0f, text.rect.w - textSize.x);
        }

        float y = text.rect.y;
        if (text.verticalAlign == TextVerticalAlign::Center) {
            y = text.rect.y + std::max(0.0f, text.rect.h - textSize.y) * 0.5f;
        } else if (text.verticalAlign == TextVerticalAlign::Bottom) {
            y = text.rect.y + std::max(0.0f, text.rect.h - textSize.y);
        }

        return glm::vec2(x, y);
    }

    void updateDrawDataView() {
        m_drawData.width = m_width;
        m_drawData.height = m_height;
        if (m_fontAtlasTexture != 0 && !m_fontAtlasPixels.empty()) {
            m_textureUploads.clear();
            m_textureUploads.push_back(
                NativeUiTextureUpload{
                    .handle = m_fontAtlasTexture,
                    .format = NativeUiTextureFormat::R8,
                    .width = m_font.atlasWidth,
                    .height = m_font.atlasHeight,
                    .pixels = m_fontAtlasPixels.data(),
                    .byteCount = m_fontAtlasPixels.size()
                }
            );
        } else {
            m_textureUploads.clear();
        }
        for (const UiTexture &texture : m_uiTextures) {
            if (texture.handle == 0 || texture.pixels.empty()) {
                continue;
            }
            m_textureUploads.push_back(
                NativeUiTextureUpload{
                    .handle = texture.handle,
                    .format = texture.format,
                    .width = texture.width,
                    .height = texture.height,
                    .pixels = texture.pixels.data(),
                    .byteCount = texture.pixels.size()
                }
            );
        }
        m_drawData.textureUploads = m_textureUploads.empty() ? nullptr : m_textureUploads.data();
        m_drawData.textureUploadCount = m_textureUploads.size();
        m_drawData.vertices = m_vertices.empty() ? nullptr : m_vertices.data();
        m_drawData.vertexCount = m_vertices.size();
        m_drawData.indices = m_indices.empty() ? nullptr : m_indices.data();
        m_drawData.indexCount = m_indices.size();
        m_drawData.batches = m_batches.empty() ? nullptr : m_batches.data();
        m_drawData.batchCount = m_batches.size();
    }

    bool m_initialized = false;
    SDL_Window *m_window = nullptr;
    int m_width = 1;
    int m_height = 1;
    bool m_wantsMouseCapture = false;
    bool m_wantsKeyboardCapture = false;

    UIContext m_context;
    InputState m_input;
    Font m_font;
    NativeUiTextureHandle m_fontAtlasTexture = 0;
    std::vector<std::uint8_t> m_fontAtlasPixels;
    TextureHandle m_nextTextureHandle = kFirstUserTextureHandle;
    std::vector<UiTexture> m_uiTextures;
    std::array<TextureHandle, static_cast<std::size_t>(NativeUiIcon::Count)> m_iconTextures{};
    AtlasLayout m_atlasLayout;
    std::unordered_map<std::string, TextureHandle> m_atlasTileTextures;
    std::vector<std::uint8_t> m_atlasPixels;
    int m_atlasWidth = 0;
    int m_atlasHeight = 0;

    std::vector<NativeUiTextureUpload> m_textureUploads;
    std::vector<NativeUiVertex> m_vertices;
    std::vector<std::uint32_t> m_indices;
    std::vector<NativeUiDrawBatch> m_batches;
    NativeUiDrawData m_drawData;
};

NativeUiSystem::NativeUiSystem()
    : m_impl(std::make_unique<Impl>()) {}

NativeUiSystem::~NativeUiSystem() {
    shutdown();
}

bool NativeUiSystem::initialize(SDL_Window *window, RenderApi api) {
    return m_impl->initialize(window, api);
}

void NativeUiSystem::shutdown() {
    if (m_impl) {
        m_impl->shutdown();
    }
}

void NativeUiSystem::onWindowResized(int width, int height) {
    if (m_impl) {
        m_impl->onWindowResized(width, height);
    }
}

void NativeUiSystem::processEvent(const SDL_Event &event) {
    if (m_impl) {
        m_impl->processEvent(event);
    }
}

void NativeUiSystem::beginFrame(float dt) {
    if (m_impl) {
        m_impl->beginFrame(dt);
    }
}

void NativeUiSystem::endFrame() {
    if (m_impl) {
        m_impl->endFrame();
    }
}

bool NativeUiSystem::isInitialized() const noexcept {
    return m_impl && m_impl->isInitialized();
}

bool NativeUiSystem::hasBackendRenderer() const noexcept {
    return m_impl && m_impl->hasRenderer();
}

bool NativeUiSystem::isUsingOpenGlBackend() const noexcept {
    return m_impl && m_impl->isUsingOpenGlBackend();
}

bool NativeUiSystem::wantsMouseCapture() const noexcept {
    return m_impl && m_impl->wantsMouseCapture();
}

bool NativeUiSystem::wantsKeyboardCapture() const noexcept {
    return m_impl && m_impl->wantsKeyboardCapture();
}

UIContext &NativeUiSystem::context() noexcept {
    return m_impl->context();
}

const UIContext &NativeUiSystem::context() const noexcept {
    return m_impl->context();
}

const NativeUiDrawData *NativeUiSystem::drawData() const noexcept {
    return m_impl ? m_impl->drawData() : nullptr;
}

TextureHandle NativeUiSystem::createTexture2D(
    int width,
    int height,
    const void *pixels,
    NativeUiTextureFormat format
) {
    return m_impl ? m_impl->createTexture2D(width, height, pixels, format) : 0;
}

TextureHandle NativeUiSystem::loadTextureRgbaFile(std::string_view path) {
    return m_impl ? m_impl->loadTextureRgbaFile(path) : 0;
}

TextureHandle NativeUiSystem::atlasTile(std::string_view tileName) {
    return m_impl ? m_impl->atlasTile(tileName) : 0;
}

TextureHandle NativeUiSystem::icon(NativeUiIcon icon) {
    return m_impl ? m_impl->icon(icon) : 0;
}
