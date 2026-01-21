#include "Player.hpp"

#include "../graphics/ChunkManager.hpp"
#include "../graphics/Shader.hpp"

#include <GLFW/glfw3.h> // only in .cpp
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>

// Tweakables
static constexpr float GRAVITY = -20.0f;
static constexpr float TERMINAL_VELOCITY = 50.0f;
static constexpr float GROUND_ACCEL = 60.0f;
static constexpr float AIR_ACCEL = 10.0f;
static constexpr float GROUND_FRICTION = 10.0f;
static constexpr float MAX_STEP_HEIGHT = 1.0f;
static constexpr float EYE_HEIGHT = 2.44f;

namespace {
    inline float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }
    inline int ifloor(float v) {
        return static_cast<int>(std::floor(v));
    }
}


Player::Player(const glm::vec3& startPos, ChunkManager& inChunkManager, const std::string& playerModelPath)
    : position(startPos),
    velocity(0.0f),
    front(0.0f, 0.0f, -1.0f),
    currentFov(walkFov),
    camera(startPos),
    chunkManager(inChunkManager),
    onGround(false),
    yaw(-90.0f),
    pitch(0.0f)
{
    // load model
    try {
        playerModel = std::make_shared<Model>(playerModelPath);
    }
    catch (const std::exception& e) {
        std::cerr << "Model load exception: " << e.what() << "\n";
        playerModel.reset();
    }

    // set camera pos after model load
    camera.position = position + glm::vec3(0.0f, EYE_HEIGHT, 0.0f);
    front = camera.front;
    updateModelMatrix();

    // load shader (catch exceptions if your Shader throws)
    try {
        playerShader = std::make_shared<Shader>("../../../../VoxelOps/shaders/player.vert",
            "../../../../VoxelOps/shaders/player.frag");
    }
    catch (const std::exception& e) {
        std::cerr << "Shader load exception: " << e.what() << "\n";
        playerShader.reset();
    }

    // sanity checks
    if (!playerModel)  std::cerr << "Warning: playerModel not loaded\n";
    if (!playerShader) std::cerr << "Warning: playerShader not created\n";
}

// ------------------------------------------------------------------
// Accessors - implemented here
// ------------------------------------------------------------------
const std::vector<Hitbox>& Player::getHitboxes() const noexcept {
    return m_hitboxes;
}

const glm::mat4& Player::getModelMatrix() const noexcept {
    return m_modelMatrix;
}

// update the cached model matrix used for hitbox / rendering transforms.
// We translate to the player's position (feet position) and rotate around Y by yaw so
// local hitboxes oriented with player's facing direction are transformed correctly.
void Player::updateModelMatrix() noexcept {
    glm::mat4 model(1.0f);
    // translate to feet; if your hitboxes are defined relative to feet, this is correct.
    model = glm::translate(model, position);

    // rotate by yaw so hitboxes follow player facing. yaw is degrees in this class.
    float yawDeg = static_cast<float>(yaw);
    model = glm::rotate(model, glm::radians(yawDeg), glm::vec3(0.0f, 1.0f, 0.0f));

    // If you want to offset model so that hitboxes use eye origin, add +vec3(0, eyeOffset, 0)
    // model = glm::translate(model, glm::vec3(0.0f, eyeOffset, 0.0f));

    m_modelMatrix = model;
}

// ------------------------------------------------------------------
// collision query
// ------------------------------------------------------------------
bool Player::checkCollision(const glm::vec3& pos) const {
    if (flyMode) return false; // [FLY MODE] disables collisions

    float minX = pos.x - playerRadius;
    float maxX = pos.x + playerRadius;
    float minY = pos.y;
    float maxY = pos.y + playerHeight;
    float minZ = pos.z - playerRadius;
    float maxZ = pos.z + playerRadius;

    int ix0 = ifloor(minX);
    int iy0 = ifloor(minY);
    int iz0 = ifloor(minZ);
    int ix1 = ifloor(maxX);
    int iy1 = ifloor(maxY);
    int iz1 = ifloor(maxZ);

    for (int x = ix0; x <= ix1; ++x) {
        for (int y = iy0; y <= iy1; ++y) {
            for (int z = iz0; z <= iz1; ++z) {
                if (chunkManager.getBlockGlobal(x, y, z) != BlockID::Air) {
                    return true;
                }
            }
        }
    }
    return false;
}


void Player::moveAndCollide(const glm::vec3& delta) {
    // Fly mode: ignore collisions entirely
    if (flyMode) {
        position += delta;
        camera.position = position + glm::vec3(0.0f, EYE_HEIGHT, 0.0f);
        front = camera.front;
        updateModelMatrix();
        return;
    }

    glm::vec3 tryPos = position;

    // --- X axis ---
    tryPos.x += delta.x;
    if (checkCollision(tryPos)) {
        // Attempt to step up if we're on ground and not already stepping
        bool stepped = false;
        if (onGround && !stepActive && delta.x != 0.0f) {
            constexpr float STEP_INCREMENT = 0.05f; // small increments
            for (float step = STEP_INCREMENT; step <= MAX_STEP_HEIGHT + 1e-6f; step += STEP_INCREMENT) {
                glm::vec3 testPos = position;
                testPos.y += step;
                testPos.x += delta.x;
                if (!checkCollision(testPos)) {
                    // Accept horizontal move but start a smooth step animation (do NOT snap Y yet)
                    tryPos.x = testPos.x;
                    // prepare step animation state
                    stepActive = true;
                    stepStartY = position.y;
                    stepTargetY = testPos.y;
                    stepTimer = 0.0f;
                    velocity.y = 0.0f; // cancel vertical velocity so step is clean
                    stepped = true;
                    break;
                }
            }
        }

        if (!stepped) {
            tryPos.x = position.x;
            velocity.x = 0.0f;
        }
    }

    // --- Z axis ---
    tryPos.z += delta.z;
    if (checkCollision(tryPos)) {
        bool stepped = false;
        if (onGround && !stepActive && delta.z != 0.0f) {
            constexpr float STEP_INCREMENT = 0.05f;
            for (float step = STEP_INCREMENT; step <= MAX_STEP_HEIGHT + 1e-6f; step += STEP_INCREMENT) {
                glm::vec3 testPos = position;
                testPos.y += step;
                testPos.z += delta.z;
                if (!checkCollision(testPos)) {
                    // Accept horizontal move, start smooth step
                    tryPos.z = testPos.z;
                    // if we already planned a step on X, choose the higher target
                    if (!stepActive) {
                        stepActive = true;
                        stepStartY = position.y;
                        stepTargetY = testPos.y;
                        stepTimer = 0.0f;
                    }
                    else {
                        // we already set stepTargetY from X; keep the higher target so we clear both
                        if (testPos.y > stepTargetY) stepTargetY = testPos.y;
                    }
                    velocity.y = 0.0f;
                    stepped = true;
                    break;
                }
            }
        }

        if (!stepped) {
            tryPos.z = position.z;
            velocity.z = 0.0f;
        }
    }

    // --- Y axis (vertical) ---
    bool wasOnGround = onGround;
    onGround = false;

    if (!stepActive) {
        glm::vec3 tryPosY = tryPos;
        tryPosY.y += delta.y;
        if (!checkCollision(tryPosY)) {
            // no collision -> apply Y movement
            tryPos = tryPosY;
        }
        else {
            if (delta.y < 0.0f) {
                onGround = true;
                velocity.y = 0.0f;

                float foot = position.y;
                int baseY = ifloor(foot);
                bool placed = false;
                for (int y = baseY; y <= baseY + 3; ++y) {
                    glm::vec3 testPos = tryPos;
                    testPos.y = static_cast<float>(y);
                    if (!checkCollision(testPos)) {
                        tryPos.y = testPos.y;
                        placed = true;
                        break;
                    }
                }
                if (!placed) {
                    tryPos.y = position.y;
                }
            }
            else {
                // moving up hit ceiling
                velocity.y = 0.0f;
                tryPos.y = position.y;
            }
        }
    }
    else {
        // If stepActive, keep Y unchanged here (we animate it in update)
        tryPos.y = position.y;
    }

    // Assign resolved position (X and Z applied; Y may be animated later)
    position = tryPos;

    // Keep camera/sync front
    camera.position = position + glm::vec3(0.0f, EYE_HEIGHT, 0.0f);
    front = camera.front;

    // keep hitbox/model transform up to date
    updateModelMatrix();
}




// update (called each frame)
void Player::update(GLFWwindow* window, double deltaTime) {
    float dt = static_cast<float>(deltaTime);

    // [FLY MODE TOGGLE]
    static bool f8PressedLast = false;
    bool f8Pressed = (glfwGetKey(window, GLFW_KEY_F8) == GLFW_PRESS);
    if (f8Pressed && !f8PressedLast) {
        flyMode = !flyMode;
        std::cout << (flyMode ? "Fly mode ON\n" : "Fly mode OFF\n");
        velocity = glm::vec3(0.0f);
        onGround = false;
    }
    f8PressedLast = f8Pressed;

    glm::vec3 inputDir(0.0f);

    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        inputDir += camera.XZfront;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        inputDir -= camera.XZfront;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        inputDir -= glm::normalize(glm::cross(camera.front, camera.up));
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        inputDir += glm::normalize(glm::cross(camera.front, camera.up));

    if (glm::length(inputDir) > 0.0001f)
        inputDir = glm::normalize(inputDir);

    float targetSpeed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ? runSpeed : moveSpeed;

    float targetFov = (targetSpeed == runSpeed) ? runningFov * runningFovMultiplier : walkFov;
    float fovSmoothSpeed = 10.0f;
    currentFov += (targetFov - currentFov) * fovSmoothSpeed * dt;

    // [FLY MODE BEHAVIOUR]
    if (flyMode) {
        // vertical control (SPACE = up, CTRL = down)
        glm::vec3 flyVel(0.0f);
        flyVel += inputDir * targetSpeed;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            flyVel.y += targetSpeed;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS)
            flyVel.y -= targetSpeed;

        position += flyVel * dt;
        camera.position = position + glm::vec3(0.0f, EYE_HEIGHT, 0.0f);
        camera.front = front;
        updateModelMatrix();
        return; // skip normal physics
    }

    // --- NORMAL PHYSICS ---
    glm::vec3 desiredVelXZ = glm::vec3(inputDir.x * targetSpeed, 0.0f, inputDir.z * targetSpeed);

    float accel = onGround ? GROUND_ACCEL : AIR_ACCEL;
    float alpha = std::clamp(accel * dt, 0.0f, 1.0f);
    velocity.x = lerp(velocity.x, desiredVelXZ.x, alpha);
    velocity.z = lerp(velocity.z, desiredVelXZ.z, alpha);

    // gravity
    velocity.y += GRAVITY * dt;
    if (velocity.y < -TERMINAL_VELOCITY) velocity.y = -TERMINAL_VELOCITY;

    // jumping
    static bool jumpPressedLast = false;
    bool jumpPressed = (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS);
    if (jumpPressed && !jumpPressedLast) {
        if (onGround) {
            velocity.y = jumpVelocity;
            onGround = false;
        }
    }
    jumpPressedLast = jumpPressed;

    glm::vec3 delta = velocity * dt;
    moveAndCollide(delta);


    if (stepActive) {
        stepTimer += dt;
        float t = std::clamp(stepTimer / stepDuration, 0.0f, 1.0f);

        // optional ease (smoothstep) for nicer curve:
        float easeT = t * t * (3.0f - 2.0f * t); // smoothstep

        position.y = lerp(stepStartY, stepTargetY, easeT);

        // When finished, finalize step and ensure onGround gets recomputed next physics
        if (t >= 1.0f) {
            stepActive = false;
            stepTimer = 0.0f;
            // ensure feet are exactly at target
            position.y = stepTargetY;
            // after stepping, consider the player onGround if there's ground underfoot
            if (!checkCollision(glm::vec3(position.x, position.y - 0.01f, position.z))) {
                // nothing right at feet — leave onGround as false, it'll be recomputed in next moveAndCollide
            }
            else {
                onGround = true;
            }

            // finalize model matrix now that Y is changed
            updateModelMatrix();
        }
        else {
            // still animating: update model matrix so hitboxes track animated Y
            updateModelMatrix();
        }
    }

    if (onGround && glm::length(inputDir) < 0.001f) {
        velocity.x = lerp(velocity.x, 0.0f, std::clamp(GROUND_FRICTION * dt, 0.0f, 1.0f));
        velocity.z = lerp(velocity.z, 0.0f, std::clamp(GROUND_FRICTION * dt, 0.0f, 1.0f));
    }

    camera.position = position + glm::vec3(0.0f, EYE_HEIGHT, 0.0f);
    camera.front = front;

    // keep hitbox/model transform up to date
    updateModelMatrix();
}

void Player::processMouse(bool dbgCam, double xpos, double ypos) noexcept {
    if (dbgCam) return;

    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
        return;
    }

    double xoffset = xpos - lastX;
    double yoffset = ypos - lastY;
    lastX = xpos;
    lastY = ypos;

    xoffset *= mouseSensitivity;
    yoffset *= mouseSensitivity;

    yaw += static_cast<float>(xoffset);
    pitch -= static_cast<float>(yoffset);
    pitch = glm::clamp(pitch, -89.0f, 89.0f);

    camera.updateRotation(yaw, pitch);

    // keep front consistent and update model matrix orientation
    front = camera.front;
    updateModelMatrix();
}

glm::mat4 Player::getViewMatrix() const noexcept {
    return camera.getViewMatrix();
}



void Player::render(const glm::mat4& projMat, const glm::vec3& lightDir, const glm::vec3& lightColor, const glm::vec3& ambientColor) {
    glDisable(GL_CULL_FACE);

    playerShader->use();


    playerShader->setInt("texture_diffuse0", 0);

    playerShader->setVec3("lightDir", lightDir);//normalized
    playerShader->setVec3("lightColor", lightColor);
    playerShader->setVec3("ambientColor", ambientColor);

    // also set the view/projection matrices
    playerShader->setMat4("view", camera.getViewMatrix());
    playerShader->setMat4("projection", projMat);

    playerModel->draw(position + glm::vec3(0.0f, 0.0f, 10.0f), glm::quat(glm::radians(glm::vec3(0.0f, yaw, 0.0f))), glm::vec3(1.0f), *playerShader);
    glEnable(GL_CULL_FACE);

}






//client side for now
void Player::placeBlock(BlockMode blockMode) {
    Ray ray(camera.position, camera.front);
    if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hit) {


    }
}



void Player::breakBlock() {
    Ray ray(camera.position, camera.front);
    if (rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hit) {
        glm::ivec3 hitBlock = rayManager.rayHasBlockIntersectSingle(ray, chunkManager, maxReach).hitBlockWorld;

        chunkManager.playerBreakBlockAt(hitBlock);
    }
}