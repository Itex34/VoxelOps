#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/mat4x4.hpp>

#include "atmosphere/model.h"

#include <memory>
#include <string>

class Sky {
public:
    enum class Backend {
        Simple33,
        Advanced43
    };

    Sky() = default;
    ~Sky();

    bool initialize(const glm::vec3& sunDir);
    void destroy();
    void prepareAdvancedLuts(
        const glm::mat4& invProj,
        const glm::mat4& invView,
        const glm::vec3& cameraPos,
        const glm::vec3& sunDir);

    void bindForSkyPass(GLuint program, GLuint transmittanceUnit, GLuint scatteringUnit,
        GLuint irradianceUnit, GLuint singleMieUnit) const;
    void bindAdvancedCompositeTextures(
        GLuint program,
        GLuint skyViewUnit,
        GLuint cameraScatteringUnit,
        GLuint cameraTransmittanceUnit) const;

    [[nodiscard]] bool isReady() const { return initialized_; }
    [[nodiscard]] GLuint atmosphereShader() const;
    [[nodiscard]] Backend backend() const { return backend_; }
    [[nodiscard]] bool isAdvancedBackend() const { return backend_ == Backend::Advanced43; }
    [[nodiscard]] const char* fragmentShaderPath() const;
    [[nodiscard]] glm::vec3 earthCenter() const { return earthCenter_; }
    [[nodiscard]] glm::vec2 sunSize() const { return sunSize_; }
    [[nodiscard]] float lengthUnitInMeters() const { return lengthUnitInMeters_; }

private:
    bool initAdvancedProgramsAndTextures();
    void destroyAdvancedResources();

    std::unique_ptr<atmosphere::Model> model_;
    bool initialized_ = false;
    Backend backend_ = Backend::Simple33;

    glm::vec3 earthCenter_ = glm::vec3(0.0f);
    glm::vec2 sunSize_ = glm::vec2(0.0f);
    float lengthUnitInMeters_ = 1000.0f;

    GLuint fsTriangleVao_ = 0;
    GLuint lutFbo_ = 0;
    GLuint skyViewProgram_ = 0;
    GLuint cameraVolumeProgram_ = 0;
    GLuint multiScattProgram_ = 0;
    GLuint transmittanceProgram_ = 0;
    GLuint skyViewLutTex_ = 0;
    GLuint multiScattLutTex_ = 0;
    GLuint transmittanceLutTex_ = 0;
    GLuint cameraScatteringVolumeTex_ = 0;
    GLuint cameraTransmittanceVolumeTex_ = 0;
};
