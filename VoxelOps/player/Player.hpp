#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <utility>

#include "../graphics/Camera.hpp"
#include "../graphics/Model.hpp"
#include "../physics/RayManager.hpp"

#include "Hitbox.hpp"



using PlayerID = uint64_t;

class Shader;
class ChunkManager;
struct GLFWwindow; // forward-declare to avoid including GLFW in header



struct PlayerState {
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale = glm::vec3(1.0f);
    uint16_t weaponId = 0;
};

struct NetworkInputState {
    float moveX = 0.0f;
    float moveZ = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    uint8_t flags = 0;
    bool flyMode = false;
};


enum class BlockMode : uint8_t {
    Block = 0,
    Wall,
    Stair,
    Floor,
};



class Player {
public:
    struct SimulationState {
        glm::vec3 position{ 0.0f };
        glm::vec3 velocity{ 0.0f };
        glm::vec3 front{ 0.0f, 0.0f, -1.0f };
        double yaw = -90.0;
        float pitch = 0.0f;
        bool onGround = false;
        bool flyMode = false;
        bool jumpPressedLastTick = false;
        float timeSinceGrounded = 0.0f;
        float jumpBufferTimer = 0.0f;
        float currentFov = 80.0f;
        float stepUpVisualOffset = 0.0f;
    };



    // explicit to avoid accidental conversions
    explicit Player(const glm::vec3& startPos, ChunkManager& chunkManager, const std::string& playerModelPath);

    ~Player() = default;

    // non-copyable: players reference ChunkManager; enable/make copy semantics explicit if needed
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Update movement, physics, and camera (window pointer is opaque here)
    void update(GLFWwindow* window, double deltaTime);

    void renderRemotePlayers(const glm::mat4& viewMat, const glm::mat4& projMat,
        const glm::vec3& lightDir, const glm::vec3& lightColor, const glm::vec3& ambientColor) const;

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

    void setPosition(const glm::vec3& p) noexcept;


    [[nodiscard]] glm::vec3 getVelocity() const noexcept { return velocity; }
    void setVelocity(const glm::vec3& v) noexcept { velocity = v; }
    [[nodiscard]] SimulationState captureSimulationState() const noexcept;
    void restoreSimulationState(const SimulationState& state) noexcept;

    [[nodiscard]] glm::vec3 getFront() const noexcept { return front; }
    [[nodiscard]] const NetworkInputState& getNetworkInputState() const noexcept { return m_networkInput; }
    [[nodiscard]] NetworkInputState captureCurrentInput(GLFWwindow* window) const noexcept;  // Capture fresh input directly (no 1-frame delay)
    void simulateFromNetworkInput(const NetworkInputState& input, double deltaTime, bool updateFov = false);
    void setFlyModeAllowed(bool allowed) noexcept;
    [[nodiscard]] bool isFlyModeAllowed() const noexcept { return m_flyModeAllowed; }

    // Camera is exposed read-only; you can provide further accessors if necessary
    const Camera& getCamera() const noexcept { return camera; }
    Camera& getCameraMutable() noexcept { return camera; } // if you need to adjust it externally
    [[nodiscard]] float getStepUpVisualOffset() const noexcept { return m_stepUpVisualOffset; }

    void placeBlock(BlockMode blockMode);
    void breakBlock();





    float currentFov;
    uint16_t renderDistance = 12;
    bool flyMode = false;
    int8_t maxReach = 8; //blocks



    std::unordered_map<PlayerID, PlayerState> connectedPlayers;

    void onPlayerJoined(PlayerID id, const PlayerState& initialState) {
        connectedPlayers[id] = initialState;
    }
    void setConnectedPlayers(const std::unordered_map<PlayerID, PlayerState>& players);
    void clearConnectedPlayers();
    void updateRemotePlayers(float deltaTime);


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

    float moveSpeed = 8.0f;
    float runSpeed = 16.0f;
    float jumpVelocity = 8.5f;
    float mouseSensitivity = 0.05f;

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
    bool m_flyModeAllowed = false;

    bool m_jumpPressedLastTick = false;
    float m_timeSinceGrounded = 0.0f;
    float m_jumpBufferTimer = 0.0f;

    // Cached model matrix & hitboxes
    glm::mat4 m_modelMatrix{ 1.0f };
    std::vector<Hitbox> m_hitboxes;

    // Movement / collisions
    void moveAndCollide(const glm::vec3& delta, bool allowStepUp, float* outStepUpHeight = nullptr);
    bool checkCollision(const glm::vec3& pos) const;
    void simulateMovement(const NetworkInputState& input, float dt, bool updateFov);

    // Internal helpers
    void updateModelMatrix() noexcept;
    void syncCameraToBody() noexcept;
    void decayStepUpOffset(float dt) noexcept;



    float walkFov = 80.0f;
    float runningFov = 83.0f; 
    float runningFovMultiplier = 1.0f;
    float m_stepUpVisualOffset = 0.0f;
    float m_stepUpSmoothingSpeed = 7.5f;
    float m_stepUpOffsetMax = 0.70f;
    float m_stepUpVisualScale = 0.48f;

    NetworkInputState m_networkInput{};
    std::unordered_map<PlayerID, PlayerState> m_remotePlayerTargets;
};
