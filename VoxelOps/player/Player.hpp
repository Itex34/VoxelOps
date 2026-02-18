#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>

#include "../graphics/Camera.hpp"
#include "../graphics/Model.hpp"
#include "../physics/RayManager.hpp"

#include "Hitbox.hpp"



using PlayerID = uint32_t;

class Shader;
class ChunkManager;
struct GLFWwindow; // forward-declare to avoid including GLFW in header



struct PlayerState {
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale = glm::vec3(1.0f);
};


enum class BlockMode : uint8_t {
    Block = 0,
    Wall,
    Stair,
    Floor,
};



class Player {
public:



    // explicit to avoid accidental conversions
    explicit Player(const glm::vec3& startPos, ChunkManager& chunkManager, const std::string& playerModelPath);

    ~Player() = default;

    // non-copyable: players reference ChunkManager; enable/make copy semantics explicit if needed
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Update movement, physics, and camera (window pointer is opaque here)
    void update(GLFWwindow* window, double deltaTime);

    void render(const glm::mat4& projMat, const glm::vec3& lightDir, const glm::vec3& lightColor, const glm::vec3& ambientColor);

    // Mouse look (dbgCam toggles debug camera mode)
    void processMouse(bool dbgCam, double xpos, double ypos) noexcept;

    // Simple check if player is standing on a block
    [[nodiscard]] bool isGrounded() const noexcept;

    // Access view matrix for rendering
    [[nodiscard]] glm::mat4 getViewMatrix() const noexcept;

    // Hitboxes for raycasting / collision queries
    [[nodiscard]] const std::vector<Hitbox>& getHitboxes() const noexcept;

    // Model transform for rendering & hitbox transforms
    [[nodiscard]] const glm::mat4& getModelMatrix() const noexcept;

    //returns the position of the player's feet
    [[nodiscard]] glm::vec3 getPosition() const noexcept { return position; }

    void setPosition(const glm::vec3& p) noexcept { position = p; updateModelMatrix(); }


    [[nodiscard]] glm::vec3 getVelocity() const noexcept { return velocity; }
    void setVelocity(const glm::vec3& v) noexcept { velocity = v; }

    [[nodiscard]] glm::vec3 getFront() const noexcept { return front; }

    // Camera is exposed read-only; you can provide further accessors if necessary
    const Camera& getCamera() const noexcept { return camera; }
    Camera& getCameraMutable() noexcept { return camera; } // if you need to adjust it externally

    void placeBlock(BlockMode blockMode);
    void breakBlock();








    // Public gameplay tunables
    float currentFov;
    int8_t renderDistance = 100;
    bool flyMode = true;
    int8_t maxReach = 8; //blocks



    std::unordered_map<PlayerID, PlayerState> connectedPlayers;

    void onPlayerJoined(PlayerID id, const PlayerState& initialState) {
        connectedPlayers[id] = initialState;
    }


private:
    RayManager rayManager;
    // Reference to the world
    ChunkManager& chunkManager;

    std::shared_ptr<Shader> playerShader;
    std::shared_ptr<Model> playerModel;



    // Core state
    glm::vec3 position{ 0.0f };
    glm::vec3 velocity{ 0.0f };
    glm::vec3 front{ 0.0f, 0.0f, -1.0f };

    Camera camera;

    // Movement settings (tweakable)
    float moveSpeed = 4.0f;
    float runSpeed = 7.0f;
    float jumpVelocity = 8.5f;
    float mouseSensitivity = 0.1f;

    // Player dimensions
    float playerHeight = 2.56f;
    float playerRadius = 0.3f; // half width

    // Mouse look state
    bool firstMouse = true;
    double lastX = 0.0;
    double lastY = 0.0;
    double yaw = -90.0;
    float pitch = 0.0f;

    // Internal helper
    bool onGround = false;

    // Step-up smoothing
    bool stepActive = false;
    float stepStartY = 0.0f;
    float stepTargetY = 0.0f;
    float stepTimer = 0.0f;
    float stepDuration = 0.20f;

    // Cached model matrix & hitboxes
    glm::mat4 m_modelMatrix{ 1.0f };
    std::vector<Hitbox> m_hitboxes;

    // Movement / collisions
    void moveAndCollide(const glm::vec3& delta);
    bool checkCollision(const glm::vec3& pos) const;

    // Internal helpers
    void updateModelMatrix() noexcept;



    float walkFov = 80.0f;
    float runningFov = 83.0f; 
    float runningFovMultiplier = 1.0f;
};
