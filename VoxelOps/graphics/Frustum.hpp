#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <vector>
#include <glad/glad.h>
#include "../voxels/Chunk.hpp" // For AABB

struct Plane {
    glm::vec3 normal;
    float d;
};

class Frustum {
  public:
    Plane planes[6];

    void extractPlanes(const glm::mat4 &viewProj, bool depthZeroToOne = false) {
        glm::vec4 rowX = glm::row(viewProj, 0);
        glm::vec4 rowY = glm::row(viewProj, 1);
        glm::vec4 rowZ = glm::row(viewProj, 2);
        glm::vec4 rowW = glm::row(viewProj, 3);

        planes[0] = normalizePlane(rowW + rowX); // Left
        planes[1] = normalizePlane(rowW - rowX); // Right
        planes[2] = normalizePlane(rowW + rowY); // Bottom
        planes[3] = normalizePlane(rowW - rowY); // Top
        // Near/Far extraction depends on clip-space depth convention:
        // OpenGL: z in [-w, +w], Vulkan/D3D: z in [0, +w].
        planes[4] = depthZeroToOne ? normalizePlane(rowZ) : normalizePlane(rowW + rowZ); // Near
        planes[5] = normalizePlane(rowW - rowZ);                                         // Far
    }

    bool isBoxVisible(const glm::vec3 &min, const glm::vec3 &max) const {
        for (const Plane &p : planes) {
            glm::vec3 positiveVertex = min;
            if (p.normal.x >= 0)
                positiveVertex.x = max.x;
            if (p.normal.y >= 0)
                positiveVertex.y = max.y;
            if (p.normal.z >= 0)
                positiveVertex.z = max.z;

            if (glm::dot(p.normal, positiveVertex) + p.d < 0.0f)
                return false;
        }
        return true;
    }


  private:
    Plane normalizePlane(const glm::vec4 &p) const {
        float length = glm::length(glm::vec3(p));
        return {glm::vec3(p) / length, p.w / length};
    }

    std::vector<glm::vec3> getFrustumCorners(const glm::mat4 &viewProj) const {
        glm::mat4 inv = glm::inverse(viewProj);
        std::vector<glm::vec3> corners;
        for (int z = 0; z <= 1; ++z) {
            for (int y = 0; y <= 1; ++y) {
                for (int x = 0; x <= 1; ++x) {
                    glm::vec4 clip =
                        glm::vec4(x ? 1.0f : -1.0f, y ? 1.0f : -1.0f, z ? 1.0f : -1.0f, 1.0f);
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
