#include "RmlUiSystem.hpp"

#include "../../application/AppHelpers.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <SDL3/SDL.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>

namespace {
    class OpenGlRenderInterface final : public Rml::RenderInterface {
    public:
        OpenGlRenderInterface() = default;
        ~OpenGlRenderInterface() override {
            clearResources();
        }

        void setViewportSize(int width, int height) {
            m_viewportWidth = (width > 0) ? width : 1;
            m_viewportHeight = (height > 0) ? height : 1;
        }

        bool initialize() {
            if (m_shaderProgram != 0) {
                return true;
            }

            constexpr const char *kVertexShaderSrc = R"(
#version 330 core
layout (location = 0) in vec2 in_position;
layout (location = 1) in vec4 in_color;
layout (location = 2) in vec2 in_texcoord;

uniform mat4 u_projection;
uniform mat4 u_transform;
uniform vec2 u_translation;

out vec4 v_color;
out vec2 v_texcoord;

void main() {
    vec4 position = vec4(in_position + u_translation, 0.0, 1.0);
    gl_Position = u_projection * u_transform * position;
    v_color = in_color;
    v_texcoord = in_texcoord;
}
)";

            constexpr const char *kFragmentShaderSrc = R"(
#version 330 core
in vec4 v_color;
in vec2 v_texcoord;

uniform sampler2D u_texture;
uniform bool u_use_texture;

out vec4 out_color;

void main() {
    vec4 tex = u_use_texture ? texture(u_texture, v_texcoord) : vec4(1.0);
    out_color = v_color * tex;
}
)";

            const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, kVertexShaderSrc);
            if (vertexShader == 0) {
                return false;
            }
            const GLuint fragmentShader = compileShader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);
            if (fragmentShader == 0) {
                glDeleteShader(vertexShader);
                return false;
            }

            m_shaderProgram = glCreateProgram();
            glAttachShader(m_shaderProgram, vertexShader);
            glAttachShader(m_shaderProgram, fragmentShader);
            glLinkProgram(m_shaderProgram);

            GLint linkStatus = GL_FALSE;
            glGetProgramiv(m_shaderProgram, GL_LINK_STATUS, &linkStatus);
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            if (linkStatus != GL_TRUE) {
                GLint logLength = 0;
                glGetProgramiv(m_shaderProgram, GL_INFO_LOG_LENGTH, &logLength);
                std::string log;
                if (logLength > 1) {
                    log.resize(static_cast<size_t>(logLength));
                    glGetProgramInfoLog(
                        m_shaderProgram, logLength, nullptr, log.data()
                    );
                }
                std::cerr << "[RmlUi] OpenGL shader link failed: " << log << "\n";
                glDeleteProgram(m_shaderProgram);
                m_shaderProgram = 0;
                return false;
            }

            m_projectionLoc = glGetUniformLocation(m_shaderProgram, "u_projection");
            m_transformLoc = glGetUniformLocation(m_shaderProgram, "u_transform");
            m_translationLoc = glGetUniformLocation(m_shaderProgram, "u_translation");
            m_useTextureLoc = glGetUniformLocation(m_shaderProgram, "u_use_texture");
            m_textureLoc = glGetUniformLocation(m_shaderProgram, "u_texture");
            return true;
        }

        Rml::CompiledGeometryHandle
        CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override {
            if (vertices.empty() || indices.empty()) {
                return 0;
            }

            GlGeometry geometry{};
            glGenVertexArrays(1, &geometry.vao);
            glGenBuffers(1, &geometry.vbo);
            glGenBuffers(1, &geometry.ebo);

            glBindVertexArray(geometry.vao);

            glBindBuffer(GL_ARRAY_BUFFER, geometry.vbo);
            glBufferData(
                GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(vertices.size() * sizeof(Rml::Vertex)),
                vertices.data(),
                GL_STATIC_DRAW
            );

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, geometry.ebo);
            glBufferData(
                GL_ELEMENT_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(indices.size() * sizeof(int)),
                indices.data(),
                GL_STATIC_DRAW
            );

            constexpr GLsizei stride = static_cast<GLsizei>(sizeof(Rml::Vertex));
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(
                0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(offsetof(Rml::Vertex, position))
            );
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(
                1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<const void *>(offsetof(Rml::Vertex, colour))
            );
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(
                2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(offsetof(Rml::Vertex, tex_coord))
            );

            glBindVertexArray(0);
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

            geometry.indexCount = static_cast<GLsizei>(indices.size());
            const Rml::CompiledGeometryHandle handle = m_nextGeometryHandle++;
            m_geometries.emplace(handle, geometry);
            return handle;
        }

        void RenderGeometry(
            Rml::CompiledGeometryHandle geometryHandle, Rml::Vector2f translation, Rml::TextureHandle textureHandle
        ) override {
            const auto geometryIt = m_geometries.find(geometryHandle);
            if (geometryIt == m_geometries.end()) {
                return;
            }

            const GLuint textureId = findTexture(textureHandle);
            renderGeometry(geometryIt->second, translation, textureId, textureHandle != 0);
        }

        void ReleaseGeometry(Rml::CompiledGeometryHandle geometryHandle) override {
            const auto it = m_geometries.find(geometryHandle);
            if (it == m_geometries.end()) {
                return;
            }
            const GlGeometry &geometry = it->second;
            glDeleteBuffers(1, &geometry.vbo);
            glDeleteBuffers(1, &geometry.ebo);
            glDeleteVertexArrays(1, &geometry.vao);
            m_geometries.erase(it);
        }

        Rml::TextureHandle LoadTexture(Rml::Vector2i &texture_dimensions, const Rml::String &source) override {
            (void)source;
            texture_dimensions = Rml::Vector2i(0, 0);
            return 0;
        }

        Rml::TextureHandle
        GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override {
            if (source.empty() || source_dimensions.x <= 0 || source_dimensions.y <= 0) {
                return 0;
            }

            GLuint textureId = 0;
            glGenTextures(1, &textureId);
            glBindTexture(GL_TEXTURE_2D, textureId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                GL_RGBA8,
                source_dimensions.x,
                source_dimensions.y,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                source.data()
            );
            glBindTexture(GL_TEXTURE_2D, 0);

            const Rml::TextureHandle handle = m_nextTextureHandle++;
            m_textures.emplace(handle, textureId);
            return handle;
        }

        void ReleaseTexture(Rml::TextureHandle textureHandle) override {
            const auto it = m_textures.find(textureHandle);
            if (it == m_textures.end()) {
                return;
            }
            GLuint textureId = it->second;
            glDeleteTextures(1, &textureId);
            m_textures.erase(it);
        }

        void EnableScissorRegion(bool enable) override {
            if (enable) {
                glEnable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
        }

        void SetScissorRegion(Rml::Rectanglei region) override {
            const int x = region.Left();
            const int yTop = region.Top();
            const int width = region.Width();
            const int height = region.Height();
            const int y = m_viewportHeight - (yTop + height);
            glScissor(x, y, width, height);
        }

        void SetTransform(const Rml::Matrix4f *transform) override {
            if (transform != nullptr) {
                const float *data = transform->data();
                std::copy(data, data + 16, m_transformMatrix.begin());
                m_hasTransform = true;
            } else {
                m_transformMatrix = kIdentityMatrix;
                m_hasTransform = false;
            }
        }

        void clearResources() {
            for (auto &[_, geometry] : m_geometries) {
                glDeleteBuffers(1, &geometry.vbo);
                glDeleteBuffers(1, &geometry.ebo);
                glDeleteVertexArrays(1, &geometry.vao);
            }
            m_geometries.clear();

            for (auto &[_, textureId] : m_textures) {
                GLuint id = textureId;
                glDeleteTextures(1, &id);
            }
            m_textures.clear();

            if (m_shaderProgram != 0) {
                glDeleteProgram(m_shaderProgram);
                m_shaderProgram = 0;
            }
        }

    private:
        struct GlGeometry {
            GLuint vao = 0;
            GLuint vbo = 0;
            GLuint ebo = 0;
            GLsizei indexCount = 0;
        };

        GLuint compileShader(GLenum shaderType, const char *source) {
            const GLuint shader = glCreateShader(shaderType);
            glShaderSource(shader, 1, &source, nullptr);
            glCompileShader(shader);

            GLint status = GL_FALSE;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
            if (status == GL_TRUE) {
                return shader;
            }

            GLint logLength = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
            std::string log;
            if (logLength > 1) {
                log.resize(static_cast<size_t>(logLength));
                glGetShaderInfoLog(shader, logLength, nullptr, log.data());
            }
            std::cerr << "[RmlUi] OpenGL shader compile failed: " << log << "\n";
            glDeleteShader(shader);
            return 0;
        }

        GLuint findTexture(Rml::TextureHandle textureHandle) const {
            if (textureHandle == 0) {
                return 0;
            }
            const auto it = m_textures.find(textureHandle);
            return (it != m_textures.end()) ? it->second : 0;
        }

        void renderGeometry(
            const GlGeometry &geometry, Rml::Vector2f translation, GLuint textureId, bool useTexture
        ) {
            if (m_shaderProgram == 0) {
                return;
            }

            GLint prevProgram = 0;
            GLint prevVertexArray = 0;
            GLint prevArrayBuffer = 0;
            GLint prevElementArrayBuffer = 0;
            GLint prevTexture = 0;
            GLint prevActiveTexture = 0;
            GLint prevBlendSrcRGB = 0;
            GLint prevBlendDstRGB = 0;
            GLint prevBlendSrcAlpha = 0;
            GLint prevBlendDstAlpha = 0;
            GLboolean blendEnabled = GL_FALSE;
            GLboolean cullEnabled = GL_FALSE;
            GLboolean depthEnabled = GL_FALSE;
            GLboolean scissorEnabled = GL_FALSE;

            glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVertexArray);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevArrayBuffer);
            glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &prevElementArrayBuffer);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActiveTexture);
            glActiveTexture(GL_TEXTURE0);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTexture);
            glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
            glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
            glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcAlpha);
            glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstAlpha);
            blendEnabled = glIsEnabled(GL_BLEND);
            cullEnabled = glIsEnabled(GL_CULL_FACE);
            depthEnabled = glIsEnabled(GL_DEPTH_TEST);
            scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

            glDisable(GL_CULL_FACE);
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            if (scissorEnabled == GL_FALSE) {
                glDisable(GL_SCISSOR_TEST);
            }

            glUseProgram(m_shaderProgram);
            glUniform1i(m_textureLoc, 0);
            glUniform1i(m_useTextureLoc, useTexture ? 1 : 0);
            glUniform2f(m_translationLoc, translation.x, translation.y);
            glUniformMatrix4fv(m_projectionLoc, 1, GL_FALSE, projectionMatrix().data());
            glUniformMatrix4fv(
                m_transformLoc, 1, GL_FALSE, m_hasTransform ? m_transformMatrix.data() : kIdentityMatrix.data()
            );

            glBindVertexArray(geometry.vao);
            glBindTexture(GL_TEXTURE_2D, textureId);
            glDrawElements(GL_TRIANGLES, geometry.indexCount, GL_UNSIGNED_INT, nullptr);

            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTexture));
            glBindVertexArray(static_cast<GLuint>(prevVertexArray));
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(prevArrayBuffer));
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(prevElementArrayBuffer));
            glUseProgram(static_cast<GLuint>(prevProgram));
            glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcAlpha, prevBlendDstAlpha);
            if (blendEnabled == GL_FALSE) {
                glDisable(GL_BLEND);
            }
            if (cullEnabled != GL_FALSE) {
                glEnable(GL_CULL_FACE);
            } else {
                glDisable(GL_CULL_FACE);
            }
            if (depthEnabled != GL_FALSE) {
                glEnable(GL_DEPTH_TEST);
            } else {
                glDisable(GL_DEPTH_TEST);
            }
            if (scissorEnabled != GL_FALSE) {
                glEnable(GL_SCISSOR_TEST);
            } else {
                glDisable(GL_SCISSOR_TEST);
            }
            glActiveTexture(static_cast<GLenum>(prevActiveTexture));
        }

        std::array<float, 16> projectionMatrix() const {
            const float width = static_cast<float>((m_viewportWidth > 0) ? m_viewportWidth : 1);
            const float height = static_cast<float>((m_viewportHeight > 0) ? m_viewportHeight : 1);
            return std::array<float, 16>{
                2.0f / width, 0.0f,         0.0f, 0.0f,
                0.0f,         -2.0f / height, 0.0f, 0.0f,
                0.0f,         0.0f,         -1.0f, 0.0f,
                -1.0f,        1.0f,         0.0f, 1.0f
            };
        }

        static constexpr std::array<float, 16> kIdentityMatrix{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        };

        std::unordered_map<Rml::CompiledGeometryHandle, GlGeometry> m_geometries;
        std::unordered_map<Rml::TextureHandle, GLuint> m_textures;
        Rml::CompiledGeometryHandle m_nextGeometryHandle = 1;
        Rml::TextureHandle m_nextTextureHandle = 1;

        GLuint m_shaderProgram = 0;
        GLint m_projectionLoc = -1;
        GLint m_transformLoc = -1;
        GLint m_translationLoc = -1;
        GLint m_useTextureLoc = -1;
        GLint m_textureLoc = -1;
        int m_viewportWidth = 1;
        int m_viewportHeight = 1;
        bool m_hasTransform = false;
        std::array<float, 16> m_transformMatrix = kIdentityMatrix;
    };

    class SdlSystemInterface final : public Rml::SystemInterface {
    public:
        explicit SdlSystemInterface(SDL_Window *window)
            : m_window(window) {}

        double GetElapsedTime() override {
            return AppHelpers::GetTimeSeconds();
        }

        bool LogMessage(Rml::Log::Type type, const Rml::String &message) override {
            switch (type) {
            case Rml::Log::Type::LT_ERROR:
                std::cerr << "[RmlUi][error] " << message << "\n";
                break;
            case Rml::Log::Type::LT_WARNING:
                std::cerr << "[RmlUi][warn] " << message << "\n";
                break;
            default:
                std::cout << "[RmlUi] " << message << "\n";
                break;
            }
            return true;
        }

        void SetClipboardText(const Rml::String &text) override {
            if (m_window != nullptr) {
                SDL_SetClipboardText(text.c_str());
            }
        }

        void GetClipboardText(Rml::String &text) override {
            text.clear();
            if (m_window == nullptr) {
                return;
            }
            char *clipboardText = SDL_GetClipboardText();
            if (clipboardText != nullptr) {
                text = clipboardText;
                SDL_free(clipboardText);
            }
        }

    private:
        SDL_Window *m_window = nullptr;
    };

    int ToRmlModifiers(SDL_Keymod mod) {
        int value = 0;
        if ((mod & SDL_KMOD_CTRL) != 0) {
            value |= Rml::Input::KM_CTRL;
        }
        if ((mod & SDL_KMOD_SHIFT) != 0) {
            value |= Rml::Input::KM_SHIFT;
        }
        if ((mod & SDL_KMOD_ALT) != 0) {
            value |= Rml::Input::KM_ALT;
        }
        if ((mod & SDL_KMOD_GUI) != 0) {
            value |= Rml::Input::KM_META;
        }
        if ((mod & SDL_KMOD_CAPS) != 0) {
            value |= Rml::Input::KM_CAPSLOCK;
        }
        if ((mod & SDL_KMOD_NUM) != 0) {
            value |= Rml::Input::KM_NUMLOCK;
        }
        if ((mod & SDL_KMOD_SCROLL) != 0) {
            value |= Rml::Input::KM_SCROLLLOCK;
        }
        return value;
    }

    int ToRmlMouseButtonIndex(uint8_t sdlButton) {
        switch (sdlButton) {
        case SDL_BUTTON_LEFT:
            return 0;
        case SDL_BUTTON_RIGHT:
            return 1;
        case SDL_BUTTON_MIDDLE:
            return 2;
        default:
            return static_cast<int>(sdlButton) - 1;
        }
    }

    Rml::Input::KeyIdentifier ToRmlKey(SDL_Scancode scancode) {
        using KI = Rml::Input::KeyIdentifier;
        switch (scancode) {
        case SDL_SCANCODE_SPACE:
            return KI::KI_SPACE;
        case SDL_SCANCODE_A:
            return KI::KI_A;
        case SDL_SCANCODE_B:
            return KI::KI_B;
        case SDL_SCANCODE_C:
            return KI::KI_C;
        case SDL_SCANCODE_D:
            return KI::KI_D;
        case SDL_SCANCODE_E:
            return KI::KI_E;
        case SDL_SCANCODE_F:
            return KI::KI_F;
        case SDL_SCANCODE_G:
            return KI::KI_G;
        case SDL_SCANCODE_H:
            return KI::KI_H;
        case SDL_SCANCODE_I:
            return KI::KI_I;
        case SDL_SCANCODE_J:
            return KI::KI_J;
        case SDL_SCANCODE_K:
            return KI::KI_K;
        case SDL_SCANCODE_L:
            return KI::KI_L;
        case SDL_SCANCODE_M:
            return KI::KI_M;
        case SDL_SCANCODE_N:
            return KI::KI_N;
        case SDL_SCANCODE_O:
            return KI::KI_O;
        case SDL_SCANCODE_P:
            return KI::KI_P;
        case SDL_SCANCODE_Q:
            return KI::KI_Q;
        case SDL_SCANCODE_R:
            return KI::KI_R;
        case SDL_SCANCODE_S:
            return KI::KI_S;
        case SDL_SCANCODE_T:
            return KI::KI_T;
        case SDL_SCANCODE_U:
            return KI::KI_U;
        case SDL_SCANCODE_V:
            return KI::KI_V;
        case SDL_SCANCODE_W:
            return KI::KI_W;
        case SDL_SCANCODE_X:
            return KI::KI_X;
        case SDL_SCANCODE_Y:
            return KI::KI_Y;
        case SDL_SCANCODE_Z:
            return KI::KI_Z;
        case SDL_SCANCODE_0:
            return KI::KI_0;
        case SDL_SCANCODE_1:
            return KI::KI_1;
        case SDL_SCANCODE_2:
            return KI::KI_2;
        case SDL_SCANCODE_3:
            return KI::KI_3;
        case SDL_SCANCODE_4:
            return KI::KI_4;
        case SDL_SCANCODE_5:
            return KI::KI_5;
        case SDL_SCANCODE_6:
            return KI::KI_6;
        case SDL_SCANCODE_7:
            return KI::KI_7;
        case SDL_SCANCODE_8:
            return KI::KI_8;
        case SDL_SCANCODE_9:
            return KI::KI_9;
        case SDL_SCANCODE_BACKSPACE:
            return KI::KI_BACK;
        case SDL_SCANCODE_TAB:
            return KI::KI_TAB;
        case SDL_SCANCODE_RETURN:
            return KI::KI_RETURN;
        case SDL_SCANCODE_ESCAPE:
            return KI::KI_ESCAPE;
        case SDL_SCANCODE_INSERT:
            return KI::KI_INSERT;
        case SDL_SCANCODE_DELETE:
            return KI::KI_DELETE;
        case SDL_SCANCODE_HOME:
            return KI::KI_HOME;
        case SDL_SCANCODE_END:
            return KI::KI_END;
        case SDL_SCANCODE_PAGEUP:
            return KI::KI_PRIOR;
        case SDL_SCANCODE_PAGEDOWN:
            return KI::KI_NEXT;
        case SDL_SCANCODE_LEFT:
            return KI::KI_LEFT;
        case SDL_SCANCODE_RIGHT:
            return KI::KI_RIGHT;
        case SDL_SCANCODE_UP:
            return KI::KI_UP;
        case SDL_SCANCODE_DOWN:
            return KI::KI_DOWN;
        case SDL_SCANCODE_F1:
            return KI::KI_F1;
        case SDL_SCANCODE_F2:
            return KI::KI_F2;
        case SDL_SCANCODE_F3:
            return KI::KI_F3;
        case SDL_SCANCODE_F4:
            return KI::KI_F4;
        case SDL_SCANCODE_F5:
            return KI::KI_F5;
        case SDL_SCANCODE_F6:
            return KI::KI_F6;
        case SDL_SCANCODE_F7:
            return KI::KI_F7;
        case SDL_SCANCODE_F8:
            return KI::KI_F8;
        case SDL_SCANCODE_F9:
            return KI::KI_F9;
        case SDL_SCANCODE_F10:
            return KI::KI_F10;
        case SDL_SCANCODE_F11:
            return KI::KI_F11;
        case SDL_SCANCODE_F12:
            return KI::KI_F12;
        case SDL_SCANCODE_LSHIFT:
            return KI::KI_LSHIFT;
        case SDL_SCANCODE_RSHIFT:
            return KI::KI_RSHIFT;
        case SDL_SCANCODE_LCTRL:
            return KI::KI_LCONTROL;
        case SDL_SCANCODE_RCTRL:
            return KI::KI_RCONTROL;
        case SDL_SCANCODE_LALT:
            return KI::KI_LMENU;
        case SDL_SCANCODE_RALT:
            return KI::KI_RMENU;
        case SDL_SCANCODE_LGUI:
            return KI::KI_LMETA;
        case SDL_SCANCODE_RGUI:
            return KI::KI_RMETA;
        case SDL_SCANCODE_MINUS:
            return KI::KI_OEM_MINUS;
        case SDL_SCANCODE_EQUALS:
            return KI::KI_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET:
            return KI::KI_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET:
            return KI::KI_OEM_6;
        case SDL_SCANCODE_BACKSLASH:
            return KI::KI_OEM_5;
        case SDL_SCANCODE_SEMICOLON:
            return KI::KI_OEM_1;
        case SDL_SCANCODE_APOSTROPHE:
            return KI::KI_OEM_7;
        case SDL_SCANCODE_GRAVE:
            return KI::KI_OEM_3;
        case SDL_SCANCODE_COMMA:
            return KI::KI_OEM_COMMA;
        case SDL_SCANCODE_PERIOD:
            return KI::KI_OEM_PERIOD;
        case SDL_SCANCODE_SLASH:
            return KI::KI_OEM_2;
        case SDL_SCANCODE_KP_0:
            return KI::KI_NUMPAD0;
        case SDL_SCANCODE_KP_1:
            return KI::KI_NUMPAD1;
        case SDL_SCANCODE_KP_2:
            return KI::KI_NUMPAD2;
        case SDL_SCANCODE_KP_3:
            return KI::KI_NUMPAD3;
        case SDL_SCANCODE_KP_4:
            return KI::KI_NUMPAD4;
        case SDL_SCANCODE_KP_5:
            return KI::KI_NUMPAD5;
        case SDL_SCANCODE_KP_6:
            return KI::KI_NUMPAD6;
        case SDL_SCANCODE_KP_7:
            return KI::KI_NUMPAD7;
        case SDL_SCANCODE_KP_8:
            return KI::KI_NUMPAD8;
        case SDL_SCANCODE_KP_9:
            return KI::KI_NUMPAD9;
        case SDL_SCANCODE_KP_PLUS:
            return KI::KI_ADD;
        case SDL_SCANCODE_KP_MINUS:
            return KI::KI_SUBTRACT;
        case SDL_SCANCODE_KP_MULTIPLY:
            return KI::KI_MULTIPLY;
        case SDL_SCANCODE_KP_DIVIDE:
            return KI::KI_DIVIDE;
        case SDL_SCANCODE_KP_DECIMAL:
            return KI::KI_DECIMAL;
        case SDL_SCANCODE_KP_ENTER:
            return KI::KI_NUMPADENTER;
        default:
            return KI::KI_UNKNOWN;
        }
    }
} // namespace

class RmlUiSystem::Impl {
public:
    bool initialize(SDL_Window *window, RenderApi api) {
        if (window == nullptr) {
            return false;
        }
        m_window = window;
        m_api = api;
        if (m_api != RenderApi::OpenGL) {
            return true;
        }

        m_systemInterface = std::make_unique<SdlSystemInterface>(window);
        m_renderInterface = std::make_unique<OpenGlRenderInterface>();

        Rml::SetSystemInterface(m_systemInterface.get());
        if (!Rml::Initialise()) {
            std::cerr << "[RmlUi] Failed to initialize RmlUi core.\n";
            m_renderInterface.reset();
            m_systemInterface.reset();
            return false;
        }
        m_coreInitialized = true;

        const std::string fontPath = Shared::RuntimePaths::ResolveVoxelOpsPath("Assets/fonts/SF/SF-Pro-Text-Medium.otf").generic_string();

        if (!Rml::LoadFontFace(fontPath)) {
            std::cerr << "[RmlUi] Failed to load font: " << fontPath << "\n";
        }

        if (!m_renderInterface->initialize()) {
            std::cerr << "[RmlUi] Failed to initialize OpenGL render interface.\n";
            shutdown();
            return false;
        }

        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(window, &width, &height);
        m_renderInterface->setViewportSize(width, height);

        m_context = Rml::CreateContext(
            "voxelops_main", Rml::Vector2i(width, height), m_renderInterface.get()
        );
        if (m_context == nullptr) {
            std::cerr << "[RmlUi] Failed to create RmlUi context.\n";
            shutdown();
            return false;
        }

        (void)SDL_StartTextInput(window);
        return true;
    }

    void shutdown() {
        if (m_window != nullptr) {
            (void)SDL_StopTextInput(m_window);
        }

        if (m_context != nullptr) {
            Rml::RemoveContext(m_context->GetName());
            m_context = nullptr;
        }

        if (m_coreInitialized) {
            Rml::Shutdown();
            m_coreInitialized = false;
        }

        if (m_renderInterface) {
            m_renderInterface->clearResources();
            m_renderInterface.reset();
        }
        m_systemInterface.reset();
        m_window = nullptr;
    }

    void onWindowResized(int width, int height) {
        if (m_renderInterface) {
            m_renderInterface->setViewportSize(width, height);
        }
        if (m_context != nullptr) {
            m_context->SetDimensions(Rml::Vector2i(width, height));
        }
    }

    void processEvent(const SDL_Event &event) {
        if (m_context == nullptr || m_api != RenderApi::OpenGL) {
            return;
        }

        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            m_context->ProcessMouseMove(
                static_cast<int>(event.motion.x),
                static_cast<int>(event.motion.y),
                ToRmlModifiers(SDL_GetModState())
            );
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            m_context->ProcessMouseButtonDown(
                ToRmlMouseButtonIndex(event.button.button),
                ToRmlModifiers(SDL_GetModState())
            );
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            m_context->ProcessMouseButtonUp(
                ToRmlMouseButtonIndex(event.button.button),
                ToRmlModifiers(SDL_GetModState())
            );
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            m_context->ProcessMouseWheel(
                Rml::Vector2f(event.wheel.x, event.wheel.y),
                ToRmlModifiers(SDL_GetModState())
            );
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            m_context->ProcessMouseLeave();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                m_context->ProcessKeyDown(
                    ToRmlKey(event.key.scancode), ToRmlModifiers(event.key.mod)
                );
            }
            break;
        case SDL_EVENT_KEY_UP:
            m_context->ProcessKeyUp(
                ToRmlKey(event.key.scancode), ToRmlModifiers(event.key.mod)
            );
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (event.text.text != nullptr) {
                m_context->ProcessTextInput(Rml::String(event.text.text));
            }
            break;
        default:
            break;
        }
    }

    void update() {
        if (m_context == nullptr || m_api != RenderApi::OpenGL) {
            return;
        }
        (void)m_context->Update();
    }

    void render() {
        if (m_context == nullptr || m_api != RenderApi::OpenGL) {
            return;
        }
        (void)m_context->Render();
    }

    bool isInitialized() const noexcept {
        return m_api == RenderApi::OpenGL ? (m_context != nullptr) : true;
    }

    bool wantsMouseCapture() const noexcept {
        return (m_context != nullptr) && m_context->IsMouseInteracting();
    }

    bool wantsKeyboardCapture() const noexcept {
        if (m_context == nullptr) {
            return false;
        }
        Rml::Element *focus = m_context->GetFocusElement();
        if (focus == nullptr) {
            return false;
        }
        const Rml::String &tag = focus->GetTagName();
        return tag == "input" || tag == "textarea" || tag == "select";
    }

    bool isUsingOpenGlBackend() const noexcept {
        return m_api == RenderApi::OpenGL && m_context != nullptr;
    }

    Rml::Context *context() const noexcept {
        return m_context;
    }

private:
    SDL_Window *m_window = nullptr;
    RenderApi m_api = RenderApi::OpenGL;
    bool m_coreInitialized = false;
    std::unique_ptr<SdlSystemInterface> m_systemInterface;
    std::unique_ptr<OpenGlRenderInterface> m_renderInterface;
    Rml::Context *m_context = nullptr;
};

RmlUiSystem::RmlUiSystem()
    : m_impl(std::make_unique<Impl>()) {}

RmlUiSystem::~RmlUiSystem() {
    shutdown();
}

bool RmlUiSystem::initialize(SDL_Window *window, RenderApi api) {
    return m_impl->initialize(window, api);
}

void RmlUiSystem::shutdown() {
    if (m_impl) {
        m_impl->shutdown();
    }
}

void RmlUiSystem::onWindowResized(int width, int height) {
    if (m_impl) {
        m_impl->onWindowResized(width, height);
    }
}

void RmlUiSystem::processEvent(const SDL_Event &event) {
    if (m_impl) {
        m_impl->processEvent(event);
    }
}

void RmlUiSystem::update() {
    if (m_impl) {
        m_impl->update();
    }
}

void RmlUiSystem::render() {
    if (m_impl) {
        m_impl->render();
    }
}

bool RmlUiSystem::isInitialized() const noexcept {
    return m_impl && m_impl->isInitialized();
}

bool RmlUiSystem::wantsMouseCapture() const noexcept {
    return m_impl && m_impl->wantsMouseCapture();
}

bool RmlUiSystem::wantsKeyboardCapture() const noexcept {
    return m_impl && m_impl->wantsKeyboardCapture();
}

bool RmlUiSystem::isUsingOpenGlBackend() const noexcept {
    return m_impl && m_impl->isUsingOpenGlBackend();
}

Rml::Context *RmlUiSystem::context() const noexcept {
    return m_impl ? m_impl->context() : nullptr;
}
