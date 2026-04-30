#pragma once

#include <cstdint>

#include <glm/vec3.hpp>

struct ModelRegionAabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};
    bool valid = false;
};

enum class ModelRegion : uint8_t { Legs = 0, Body = 1, Head = 2 };

struct ModelLocalTriangle {
    glm::vec3 a{0.0f};
    glm::vec3 b{0.0f};
    glm::vec3 c{0.0f};
    ModelRegion region = ModelRegion::Body;
};
