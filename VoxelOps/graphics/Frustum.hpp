#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtx/string_cast.hpp>
#include <vector>
#include <glad/glad.h>
#include "../voxels/Chunk.hpp" // For AABB
#include "Shader.hpp"

struct Plane {
    glm::vec3 normal;
    float d;
};

class Frustum {
public:
    Plane planes[6];

    void extractPlanes(const glm::mat4& viewProj) {
        glm::vec4 rowX = glm::row(viewProj, 0);
        glm::vec4 rowY = glm::row(viewProj, 1);
        glm::vec4 rowZ = glm::row(viewProj, 2);
        glm::vec4 rowW = glm::row(viewProj, 3);

        planes[0] = normalizePlane(rowW + rowX); // Left
        planes[1] = normalizePlane(rowW - rowX); // Right
        planes[2] = normalizePlane(rowW + rowY); // Bottom
        planes[3] = normalizePlane(rowW - rowY); // Top
        planes[4] = normalizePlane(rowW + rowZ); // Near
        planes[5] = normalizePlane(rowW - rowZ); // Far
    }

    bool isBoxVisible(const glm::vec3& min, const glm::vec3& max) const {
        for (const Plane& p : planes) {
            glm::vec3 positiveVertex = min;
            if (p.normal.x >= 0) positiveVertex.x = max.x;
            if (p.normal.y >= 0) positiveVertex.y = max.y;
            if (p.normal.z >= 0) positiveVertex.z = max.z;

            if (glm::dot(p.normal, positiveVertex) + p.d < 0.0f)
                return false;
        }
        return true;
    }



    void drawFrustumFaces(
        Shader& shader,
        const glm::mat4& frustumViewProj,
        const glm::mat4& view,
        const glm::mat4& projection,
        bool toggleWireframe
    ) const {
        std::vector<glm::vec3> corners = getFrustumCorners(frustumViewProj);

        // Corner index mapping:
        // 0: Near Bottom Left
        // 1: Near Bottom Right
        // 2: Near Top Left
        // 3: Near Top Right
        // 4: Far Bottom Left
        // 5: Far Bottom Right
        // 6: Far Top Left
        // 7: Far Top Right

        const GLuint indices[] = {
            // Near
            0, 1, 2, 1, 3, 2,
            // Far
            4, 6, 5, 5, 6, 7,
            // Left
            0, 2, 4, 4, 2, 6,
            // Right
            1, 5, 3, 3, 5, 7,
            // Top
            2, 3, 6, 6, 3, 7,
            // Bottom
            0, 4, 1, 1, 4, 5
        };

        static GLuint VAO = 0, VBO = 0, EBO = 0;
        if (VAO == 0) {
            glGenVertexArrays(1, &VAO);
            glGenBuffers(1, &VBO);
            glGenBuffers(1, &EBO);

            glBindVertexArray(VAO);

            glBindBuffer(GL_ARRAY_BUFFER, VBO);
            glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * 8, nullptr, GL_DYNAMIC_DRAW);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, EBO);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);

            glBindVertexArray(0);
        }

        glBindBuffer(GL_ARRAY_BUFFER, VBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(glm::vec3) * 8, corners.data());

        shader.use();
        shader.setMat4("model", glm::mat4(1.0f));
        shader.setMat4("view", view);
        shader.setMat4("projection", projection);
        shader.setVec3("color", glm::vec3(1.0f, 0.0f, 0.0f)); 

        glDisable(GL_CULL_FACE);
        if (toggleWireframe)
            glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

        glBindVertexArray(VAO);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);

        if (toggleWireframe)

            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
    }

private:
    Plane normalizePlane(const glm::vec4& p) const {
        float length = glm::length(glm::vec3(p));
        return { glm::vec3(p) / length, p.w / length };
    }

    std::vector<glm::vec3> getFrustumCorners(const glm::mat4& viewProj) const {
        glm::mat4 inv = glm::inverse(viewProj);
        std::vector<glm::vec3> corners;
        for (int z = 0; z <= 1; ++z) {
            for (int y = 0; y <= 1; ++y) {
                for (int x = 0; x <= 1; ++x) {
                    glm::vec4 clip = glm::vec4(
                        x ? 1.0f : -1.0f,
                        y ? 1.0f : -1.0f,
                        z ? 1.0f : -1.0f,
                        1.0f
                    );
                    glm::vec4 world = inv * clip;
                    corners.push_back(glm::vec3(world) / world.w);
                }
            }
        }

        return {
            corners[0], // -1 -1 -1  Near Bottom Left
            corners[1], //  1 -1 -1  Near Bottom Right
            corners[2], // -1  1 -1  Near Top Left
            corners[3], //  1  1 -1  Near Top Right
            corners[4], // -1 -1  1  Far Bottom Left
            corners[5], //  1 -1  1  Far Bottom Right
            corners[6], // -1  1  1  Far Top Left
            corners[7]  //  1  1  1  Far Top Right
        };
    }
};
