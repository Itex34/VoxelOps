#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>

#include <memory>

class Shader;

class Sky {
public:
    Sky() = default;
    ~Sky();

    Sky(const Sky&) = delete;
    Sky& operator=(const Sky&) = delete;

    void initialize(const char* vertexShaderPath, const char* fragmentShaderPath);
    void shutdown();
    void render(const glm::mat4& projection, const glm::mat4& view) const;

    void setSunDir(const glm::vec3& sunDir);
    const glm::vec3& getSunDir() const noexcept;

private:
    std::unique_ptr<Shader> m_Shader;
    glm::vec3 m_SunDir = glm::normalize(glm::vec3(1.0f, 0.01f, 0.0f));
    float m_Exposure = 1.0f;
    GLuint m_VAO = 0;
    GLuint m_VBO = 0;
};
