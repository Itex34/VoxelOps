#include "OpenGlNativeUiRenderer.hpp"

#include <SDL3/SDL.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>

namespace {
    GLuint CompileShader(GLenum shaderType, const char *source) {
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
        std::cerr << "[NativeUI][OpenGL] shader compile failed: " << log << "\n";
        glDeleteShader(shader);
        return 0;
    }

    bool LinkProgram(GLuint &program) {
        constexpr const char *kVertexShaderSrc = R"(
#version 330 core
layout (location = 0) in vec2 in_position;
layout (location = 1) in vec2 in_uv;
layout (location = 2) in vec4 in_color;

uniform mat4 u_projection;

out vec2 v_uv;
out vec4 v_color;

void main() {
    gl_Position = u_projection * vec4(in_position, 0.0, 1.0);
    v_uv = in_uv;
    v_color = in_color;
}
)";

        constexpr const char *kFragmentShaderSrc = R"(
#version 330 core
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_texture;
uniform int u_texture_mode;

out vec4 out_color;

void main() {
    if (u_texture_mode == 1) {
        float alpha = texture(u_texture, v_uv).r;
        out_color = vec4(v_color.rgb, v_color.a * alpha);
    } else if (u_texture_mode == 2) {
        out_color = v_color * texture(u_texture, v_uv);
    } else {
        out_color = v_color;
    }
}
)";

        const GLuint vertexShader = CompileShader(GL_VERTEX_SHADER, kVertexShaderSrc);
        if (vertexShader == 0) {
            return false;
        }
        const GLuint fragmentShader = CompileShader(GL_FRAGMENT_SHADER, kFragmentShaderSrc);
        if (fragmentShader == 0) {
            glDeleteShader(vertexShader);
            return false;
        }

        program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        GLint status = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &status);
        if (status == GL_TRUE) {
            return true;
        }

        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log;
        if (logLength > 1) {
            log.resize(static_cast<size_t>(logLength));
            glGetProgramInfoLog(program, logLength, nullptr, log.data());
        }
        std::cerr << "[NativeUI][OpenGL] shader link failed: " << log << "\n";
        glDeleteProgram(program);
        program = 0;
        return false;
    }

    struct ScissorRect {
        GLint x = 0;
        GLint y = 0;
        GLsizei width = 0;
        GLsizei height = 0;
    };

    bool ComputeScissor(NativeUiClipRect clip, int framebufferWidth, int framebufferHeight, ScissorRect &out) {
        const float x0 = std::max(0.0f, clip.x);
        const float y0 = std::max(0.0f, clip.y);
        const float x1 = std::min(static_cast<float>(framebufferWidth), clip.x + clip.w);
        const float y1 = std::min(static_cast<float>(framebufferHeight), clip.y + clip.h);

        const GLint left = static_cast<GLint>(std::floor(x0));
        const GLint top = static_cast<GLint>(std::floor(y0));
        const GLint right = static_cast<GLint>(std::ceil(x1));
        const GLint bottom = static_cast<GLint>(std::ceil(y1));
        if (right <= left || bottom <= top) {
            return false;
        }

        out.x = left;
        out.y = static_cast<GLint>(framebufferHeight - bottom);
        out.width = static_cast<GLsizei>(right - left);
        out.height = static_cast<GLsizei>(bottom - top);
        return out.width > 0 && out.height > 0;
    }
}

OpenGlNativeUiRenderer::~OpenGlNativeUiRenderer() {
    shutdown();
}

bool OpenGlNativeUiRenderer::initialize(SDL_Window *window) {
    if (window == nullptr) {
        return false;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    onWindowResized(width, height);

    if (!LinkProgram(m_shaderProgram)) {
        return false;
    }
    m_projectionLoc = glGetUniformLocation(m_shaderProgram, "u_projection");
    m_textureLoc = glGetUniformLocation(m_shaderProgram, "u_texture");
    m_textureModeLoc = glGetUniformLocation(m_shaderProgram, "u_texture_mode");

    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glGenBuffers(1, &m_ebo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NativeUiVertex),
        reinterpret_cast<void *>(offsetof(NativeUiVertex, x))
    );
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1,
        2,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NativeUiVertex),
        reinterpret_cast<void *>(offsetof(NativeUiVertex, u))
    );
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(
        2,
        4,
        GL_FLOAT,
        GL_FALSE,
        sizeof(NativeUiVertex),
        reinterpret_cast<void *>(offsetof(NativeUiVertex, r))
    );
    glBindVertexArray(0);

    m_initialized = true;
    return true;
}

void OpenGlNativeUiRenderer::shutdown() {
    if (m_ebo != 0) {
        GLuint buffer = m_ebo;
        glDeleteBuffers(1, &buffer);
        m_ebo = 0;
    }
    if (m_vbo != 0) {
        GLuint buffer = m_vbo;
        glDeleteBuffers(1, &buffer);
        m_vbo = 0;
    }
    if (m_vao != 0) {
        GLuint vertexArray = m_vao;
        glDeleteVertexArrays(1, &vertexArray);
        m_vao = 0;
    }
    if (m_shaderProgram != 0) {
        GLuint program = m_shaderProgram;
        glDeleteProgram(program);
        m_shaderProgram = 0;
    }
    for (auto &[_, texture] : m_textures) {
        GLuint glTexture = texture.id;
        glDeleteTextures(1, &glTexture);
    }
    m_textures.clear();
    m_initialized = false;
}

void OpenGlNativeUiRenderer::onWindowResized(int width, int height) {
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
}

void OpenGlNativeUiRenderer::uploadTexture2D(
    NativeUiTextureHandle handle,
    int width,
    int height,
    const void *pixels,
    NativeUiTextureFormat format
) {
    if (handle == 0 || width <= 0 || height <= 0 || pixels == nullptr) {
        return;
    }

    GLenum internalFormat = GL_RGBA8;
    GLenum sourceFormat = GL_RGBA;
    if (format == NativeUiTextureFormat::R8) {
        internalFormat = GL_R8;
        sourceFormat = GL_RED;
    }

    auto existing = m_textures.find(handle);
    if (existing != m_textures.end() && existing->second.width == width &&
        existing->second.height == height && existing->second.format == format) {
        return;
    }

    TextureResource *resource = nullptr;
    if (existing != m_textures.end()) {
        resource = &existing->second;
    } else {
        TextureResource created;
        glGenTextures(1, &created.id);
        const auto [inserted, _] = m_textures.emplace(handle, created);
        resource = &inserted->second;
    }

    glBindTexture(GL_TEXTURE_2D, resource->id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        static_cast<GLint>(internalFormat),
        width,
        height,
        0,
        sourceFormat,
        GL_UNSIGNED_BYTE,
        pixels
    );
    glBindTexture(GL_TEXTURE_2D, 0);
    resource->width = width;
    resource->height = height;
    resource->format = format;
}

void OpenGlNativeUiRenderer::destroyTexture(NativeUiTextureHandle texture) {
    if (texture == 0) {
        return;
    }
    const auto it = m_textures.find(texture);
    if (it == m_textures.end()) {
        return;
    }
    GLuint glTexture = it->second.id;
    glDeleteTextures(1, &glTexture);
    m_textures.erase(it);
}

void OpenGlNativeUiRenderer::render(const NativeUiDrawData &drawData) {
    if (!m_initialized || drawData.indexCount == 0 || drawData.batchCount == 0) {
        return;
    }

    for (std::size_t i = 0; i < drawData.textureUploadCount; ++i) {
        const NativeUiTextureUpload &upload = drawData.textureUploads[i];
        uploadTexture2D(upload.handle, upload.width, upload.height, upload.pixels, upload.format);
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
    GLint prevScissorBox[4] = {0, 0, 0, 0};
    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
    GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);

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
    glGetIntegerv(GL_SCISSOR_BOX, prevScissorBox);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(m_shaderProgram);
    glUniform1i(m_textureLoc, 0);
    glUniformMatrix4fv(m_projectionLoc, 1, GL_FALSE, projectionMatrix(drawData).data());

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(drawData.vertexCount * sizeof(NativeUiVertex)),
        drawData.vertices,
        GL_STREAM_DRAW
    );
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ebo);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(drawData.indexCount * sizeof(std::uint32_t)),
        drawData.indices,
        GL_STREAM_DRAW
    );

    for (std::size_t i = 0; i < drawData.batchCount; ++i) {
        const NativeUiDrawBatch &batch = drawData.batches[i];
        if (batch.clipEnabled) {
            ScissorRect scissor;
            if (!ComputeScissor(batch.clip, std::max(drawData.width, 1), std::max(drawData.height, 1), scissor)) {
                continue;
            }
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor.x, scissor.y, scissor.width, scissor.height);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        GLuint texture = 0;
        if (batch.texture != 0) {
            const auto textureIt = m_textures.find(batch.texture);
            if (textureIt != m_textures.end()) {
                texture = textureIt->second.id;
            }
        }
        glUniform1i(m_textureModeLoc, static_cast<int>(batch.textureMode));
        glBindTexture(GL_TEXTURE_2D, texture);
        glDrawElements(
            GL_TRIANGLES,
            static_cast<GLsizei>(batch.indexCount),
            GL_UNSIGNED_INT,
            reinterpret_cast<const void *>(static_cast<uintptr_t>(batch.indexOffset * sizeof(std::uint32_t)))
        );
    }

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
    glScissor(prevScissorBox[0], prevScissorBox[1], prevScissorBox[2], prevScissorBox[3]);
    if (scissorEnabled != GL_FALSE) {
        glEnable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    glActiveTexture(static_cast<GLenum>(prevActiveTexture));
}

bool OpenGlNativeUiRenderer::isInitialized() const noexcept {
    return m_initialized;
}

std::array<float, 16> OpenGlNativeUiRenderer::projectionMatrix(const NativeUiDrawData &drawData) const {
    const float width = static_cast<float>(std::max(drawData.width, 1));
    const float height = static_cast<float>(std::max(drawData.height, 1));
    return std::array<float, 16>{
        2.0f / width, 0.0f,          0.0f, 0.0f,
        0.0f,        -2.0f / height, 0.0f, 0.0f,
        0.0f,         0.0f,         -1.0f, 0.0f,
       -1.0f,         1.0f,          0.0f, 1.0f
    };
}
