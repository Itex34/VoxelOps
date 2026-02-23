#ifndef SHADER_HPP
#define SHADER_HPP

#include <string>
#include <unordered_map>
#include <glad/glad.h>
#include <glm/glm.hpp>

class Shader {
public:
    unsigned int ID;

    Shader(const char* vertexPath, const char* fragmentPath, unsigned int extraFragmentShader = 0);
    void use() const;

    void setBool(const std::string& name, bool value) const;
    void setInt(const std::string& name, int value) const;
    void setFloat(const std::string& name, float value) const;
    void setMat4(const std::string& name, const glm::mat4& mat) const;
    void setMat3(const std::string& name, const glm::mat3& mat) const;

    void setVec4v(const std::string& name, GLsizei count, const glm::vec4* v) const;
    void setVec3(const std::string& name, const glm::vec3& value) const;
	void setVec2(const std::string& name, const glm::vec2& value) const;

private:
    GLint getUniformLocation(const std::string& name) const;

    std::string loadFile(const char* path);
    void checkCompileErrors(unsigned int shader, const std::string& type);

    mutable std::unordered_map<std::string, GLint> m_uniformLocationCache;
};

#endif
