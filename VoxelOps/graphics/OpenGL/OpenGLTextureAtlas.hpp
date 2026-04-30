#pragma once

#include <glad/glad.h>

class OpenGLTextureAtlas {
  public:
    OpenGLTextureAtlas() = default;
    ~OpenGLTextureAtlas();

    OpenGLTextureAtlas(const OpenGLTextureAtlas &) = delete;
    OpenGLTextureAtlas &operator=(const OpenGLTextureAtlas &) = delete;
    OpenGLTextureAtlas(OpenGLTextureAtlas &&) = delete;
    OpenGLTextureAtlas &operator=(OpenGLTextureAtlas &&) = delete;

    bool initialize();
    void cleanup();

    [[nodiscard]] GLuint getArrayTextureId() const noexcept { return m_arrayTextureId; }

  private:
    GLuint m_arrayTextureId = 0;
};

