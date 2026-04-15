#pragma once


struct Vertex {
    glm::vec3 position;
    glm::vec2 uv;

    static vk::VertexInputBindingDescription getBindingDescription();
    static std::array<vk::VertexInputAttributeDescription, 2> getAttributeDescriptions();
};