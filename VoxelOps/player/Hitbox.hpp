#pragma once

#include <glm/glm.hpp>
#include <vector>

// ============================================================================
//  ENUM: HitRegion
// ============================================================================
// Represents the type of body region hit (used for damage multipliers, etc.)
enum class HitRegion {
    Head,
    Body,
    Legs,
    Unknown
};


struct Hitbox {
    glm::vec3 min;      
    glm::vec3 max;   
    HitRegion region;   
};


struct HitResult {
    bool hit = false;          
    HitRegion region = HitRegion::Unknown;
    glm::vec3 hitPointWorld;   
    float distance = 0.0f;   
};

class HitboxManager {
public:

    static std::vector<Hitbox> buildBlockyHitboxes(
        float playerHeight = 1.8f,
        float halfWidth = 0.3f,
        float halfDepth = 0.2f,
        bool originAtFeet = true
    );



    static bool rayIntersectsAABB(
        const glm::vec3& rayOrigin,
        const glm::vec3& rayDir,
        const Hitbox& box,
        const glm::mat4& modelMatrix,
        glm::vec3& outHitPointWorld,
        float& outDistance
    );



    static HitResult raycastHitboxes(
        const glm::vec3& rayOrigin,
        const glm::vec3& rayDir,
        const std::vector<Hitbox>& hitboxes,
        const glm::mat4& modelMatrix,
        float maxDistance = 100.0f
    );
};
