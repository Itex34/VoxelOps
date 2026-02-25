#include <glad/glad.h>

#include "App.hpp"


#include "../data/GameData.hpp"
#include "../graphics/Camera.hpp"
#include "../graphics/ChunkManager.hpp"
#include "../graphics/Frustum.hpp"
#include "../graphics/Renderer.hpp"
#include "../graphics/Shader.hpp"
#include "../graphics/Sky.hpp"
#include "../input/InputCallbacks.hpp"
#include "../network/ClientNetwork.hpp"
#include "../physics/RayManager.hpp"
#include "../physics/Raycast.hpp"
#include "../player/Player.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>

namespace {
struct CallbackContext {
    InputCallbacks* inputCallbacks = nullptr;
    bool* useDebugCamera = nullptr;
};
}

struct App::Runtime {
    Renderer renderer;
    std::unique_ptr<ChunkManager> chunkManager;
    std::unique_ptr<Player> player;
    std::unique_ptr<InputCallbacks> inputCallbacks;

    RayManager rayManager;
    ClientNetwork clientNet;

    std::unique_ptr<Shader> chunkShader;
    std::unique_ptr<Shader> dbgShader;
    Sky sky;

    Frustum frustum;
    Camera debugCamera{ glm::vec3(0.0f, 100.0f, 0.0f) };

    bool supportsGL43Shaders = false;
    bool chunkUniformsInitialized = false;

    uint32_t netSeq = 0;
    double lastNetSendTime = 0.0;
    static constexpr double NetSendInterval = 0.1; // 10 Hz
    static constexpr size_t MaxChunkDataApplyPerFrame = 2;
    static constexpr size_t MaxChunkDeltaApplyPerFrame = 32;
    static constexpr size_t MaxChunkUnloadApplyPerFrame = 64;
    static constexpr int64_t ChunkApplyBudgetUs = 4000;
    double lastChunkCoverageLogTime = 0.0;

    double lastX = 0.0;
    double lastY = 0.0;
    double xpos = 0.0;
    double ypos = 0.0;
    float yaw = 0.0f;
    float pitch = 0.0f;

    CallbackContext callbackContext;
};

void App::updateFPSCounter() {
    GameData::frameCount++;
    const double currentTime = glfwGetTime();
    const double elapsedTime = currentTime - GameData::fpsTime;

    if (elapsedTime >= 1.0) {
        const double fps = GameData::frameCount / elapsedTime;
        std::stringstream ss;
        ss << "Voxel Ops - FPS: " << fps;
        glfwSetWindowTitle(m_Window, ss.str().c_str());

        GameData::frameCount = 0;
        GameData::fpsTime = currentTime;
    }
}

void App::Exit() {
    if (m_Window) {
        glfwSetWindowShouldClose(m_Window, GLFW_TRUE);
    }
}

bool App::initWindowAndContext() {
    if (!glfwInit()) {
        return false;
    }

    auto createWindowForVersion = [](int major, int minor) -> GLFWwindow* {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, major);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        return glfwCreateWindow(GameData::screenWidth, GameData::screenHeight, "Voxel Ops", nullptr, nullptr);
    };

    m_Window = createWindowForVersion(4, 3);
    if (!m_Window) {
        std::cerr << "OpenGL 4.3 context creation failed, retrying with OpenGL 3.3.\n";
        m_Window = createWindowForVersion(3, 3);
    }
    if (!m_Window) {
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(m_Window);
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD.\n";
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
        glfwTerminate();
        return false;
    }

    printf("OpenGL version: %s\n", glGetString(GL_VERSION));
    printf("GLSL version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    return true;
}

void App::configureBackendPolicy(Runtime& runtime) {
    const Backend& backendInfo = runtime.renderer.getBackend();
    const GraphicsBackend backendTier = runtime.renderer.getActiveBackend();

    runtime.supportsGL43Shaders =
        (backendInfo.getOpenGLVersionMajor() > 4) ||
        (backendInfo.getOpenGLVersionMajor() == 4 && backendInfo.getOpenGLVersionMinor() >= 3);

    switch (backendTier) {
    case GraphicsBackend::Realistic:
        runtime.chunkManager->enableAO = true;
        runtime.chunkManager->enableShadows = true;
        break;
    case GraphicsBackend::Performance:
        runtime.chunkManager->enableAO = true;
        runtime.chunkManager->enableShadows = false;
        break;
    case GraphicsBackend::Potato:
    default:
        runtime.chunkManager->enableAO = false;
        runtime.chunkManager->enableShadows = false;
        break;
    }

    std::cout
        << "[App] Backend tier: " << runtime.renderer.getActiveBackendName()
        << " | MDI usable: " << (runtime.renderer.isMDIUsable() ? "yes" : "no")
        << " | AO: " << (runtime.chunkManager->enableAO ? "on" : "off")
        << " | Shadows: " << (runtime.chunkManager->enableShadows ? "on" : "off")
        << " | Chunk shader profile: " << (runtime.supportsGL43Shaders ? "GL43" : "GL33")
        << "\n";
}

void App::initGameplay(Runtime& runtime) {
    runtime.chunkManager = std::make_unique<ChunkManager>(runtime.renderer);
    configureBackendPolicy(runtime);

    runtime.player = std::make_unique<Player>(
        glm::vec3(0.0f, 60.0f, 0.0f),
        *runtime.chunkManager,
        "../../../../Models/sniper.fbx"
    );
    runtime.inputCallbacks = std::make_unique<InputCallbacks>(*runtime.player);
}

void App::initCallbacks(Runtime& runtime) {
    runtime.callbackContext = CallbackContext{
        .inputCallbacks = runtime.inputCallbacks.get(),
        .useDebugCamera = &m_UseDebugCamera
    };

    glfwSetWindowUserPointer(m_Window, &runtime.callbackContext);
    glfwSetFramebufferSizeCallback(m_Window, [](GLFWwindow* w, int width, int height) {
        auto* context = static_cast<CallbackContext*>(glfwGetWindowUserPointer(w));
        context->inputCallbacks->framebuffer_size_callback(w, width, height);
    });
    glfwSetCursorPosCallback(m_Window, [](GLFWwindow* w, double x, double y) {
        auto* context = static_cast<CallbackContext*>(glfwGetWindowUserPointer(w));
        context->inputCallbacks->mouse_callback(w, x, y, *context->useDebugCamera);
    });
    glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* w, int button, int action, int mods) {
        auto* context = static_cast<CallbackContext*>(glfwGetWindowUserPointer(w));
        context->inputCallbacks->mouse_button_callback(w, button, action, mods);
    });
    glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
}

void App::initRenderResources(Runtime& runtime) {
    const char* chunkVertPath = runtime.supportsGL43Shaders
        ? "../../../../VoxelOps/shaders/allLightingPack.vert"
        : "../../../../VoxelOps/shaders/allLightingPack33.vert";
    const char* chunkFragPath = runtime.supportsGL43Shaders
        ? "../../../../VoxelOps/shaders/allLightingPack.frag"
        : "../../../../VoxelOps/shaders/allLightingPack33.frag";

    runtime.chunkShader = std::make_unique<Shader>(chunkVertPath, chunkFragPath);
    runtime.dbgShader = std::make_unique<Shader>(
        "../../../../VoxelOps/shaders/debugVert.vert",
        "../../../../VoxelOps/shaders/debugFrag.frag"
    );
    runtime.sky.initialize(
        "../../../../VoxelOps/shaders/sky.vert",
        "../../../../VoxelOps/shaders/sky_simple.frag"
    );

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

}

void App::initNetworking(Runtime& runtime) {
    runtime.lastNetSendTime = glfwGetTime();

    if (!runtime.clientNet.Start()) {
        std::cerr << "Failed to start networking\n";
        return;
    }

    const char serverIp[] = "127.0.0.1";
    const uint16_t serverPort = 27015;
    if (!runtime.clientNet.ConnectTo(serverIp, serverPort)) {
        std::cerr << "ConnectTo(" << serverIp << ":" << serverPort << ") failed\n";
        return;
    }

    runtime.clientNet.SendConnectRequest("player1");
}

void App::updateDebugCamera(Runtime& runtime) {
    glfwGetCursorPos(m_Window, &runtime.xpos, &runtime.ypos);

    glm::vec3 moveDir(0.0f);
    if (glfwGetKey(m_Window, GLFW_KEY_U) == GLFW_PRESS) moveDir += runtime.debugCamera.XZfront;
    if (glfwGetKey(m_Window, GLFW_KEY_J) == GLFW_PRESS) moveDir -= runtime.debugCamera.XZfront;
    if (glfwGetKey(m_Window, GLFW_KEY_H) == GLFW_PRESS) moveDir -= glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
    if (glfwGetKey(m_Window, GLFW_KEY_K) == GLFW_PRESS) moveDir += glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
    if (glfwGetKey(m_Window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) moveDir += runtime.debugCamera.up;
    if (glfwGetKey(m_Window, GLFW_KEY_V) == GLFW_PRESS) moveDir -= runtime.debugCamera.up;

    if (glm::length(moveDir) > 0.0f) {
        moveDir = glm::normalize(moveDir);
    }
    runtime.debugCamera.position += moveDir * 10.0f * static_cast<float>(GameData::deltaTime);

    if (m_UseDebugCamera) {
        const double xoffset = runtime.xpos - runtime.lastX;
        const double yoffset = runtime.ypos - runtime.lastY;
        runtime.lastX = runtime.xpos;
        runtime.lastY = runtime.ypos;

        runtime.yaw += static_cast<float>(xoffset * 0.1);
        runtime.pitch -= static_cast<float>(yoffset * 0.1);
        runtime.pitch = glm::clamp(runtime.pitch, -89.0f, 89.0f);
    }

    runtime.debugCamera.updateRotation(runtime.yaw, runtime.pitch);
}

void App::updateToggleStates() {
    const bool isF1Pressed = glfwGetKey(m_Window, GLFW_KEY_F1) == GLFW_PRESS;
    if (isF1Pressed && !m_WasF1Pressed) {
        m_UseDebugCamera = !m_UseDebugCamera;
    }
    m_WasF1Pressed = isF1Pressed;

    const bool isTPressed = glfwGetKey(m_Window, GLFW_KEY_T) == GLFW_PRESS;
    if (isTPressed && !m_WasTPressed) {
        m_ToggleWireframe = !m_ToggleWireframe;
    }
    m_WasTPressed = isTPressed;

    const bool isF2Pressed = glfwGetKey(m_Window, GLFW_KEY_F2) == GLFW_PRESS;
    if (isF2Pressed && !m_WasF2Pressed) {
        m_ToggleChunkBorders = !m_ToggleChunkBorders;
    }
    m_WasF2Pressed = isF2Pressed;

    const bool isF3Pressed = glfwGetKey(m_Window, GLFW_KEY_F3) == GLFW_PRESS;
    if (isF3Pressed && !m_WasF3Pressed) {
        m_ToggleDebugFrustum = !m_ToggleDebugFrustum;
    }
    m_WasF3Pressed = isF3Pressed;
}

void App::processWorldInteraction(Runtime& runtime) {
    if (glfwGetKey(m_Window, GLFW_KEY_H) == GLFW_PRESS) {
        Ray ray(runtime.player->getCamera().position, runtime.player->getCamera().front);
        const auto hitResult = runtime.rayManager.rayHasBlockIntersectSingle(
            ray, *runtime.chunkManager, runtime.player->maxReach
        );
        if (hitResult.hit) {
            runtime.chunkManager->playerBreakBlockAt(hitResult.hitBlockWorld);
        }
    }

    if (glfwGetKey(m_Window, GLFW_KEY_G) == GLFW_PRESS) {
        Ray ray(runtime.player->getCamera().position, runtime.player->getCamera().front);
        const auto hitResult = runtime.rayManager.rayHasBlockIntersectSingle(
            ray, *runtime.chunkManager, runtime.player->maxReach + 100.05f
        );
        if (hitResult.hit) {
            runtime.chunkManager->playerPlaceBlockAt(hitResult.hitBlockWorld, 0, BlockID::Dirt);
        }
    }
}

void App::processNetworking(Runtime& runtime) {
    runtime.clientNet.Poll();

    const auto chunkApplyStart = std::chrono::steady_clock::now();
    const auto withinChunkApplyBudget = [&]() -> bool {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - chunkApplyStart
        ).count();
        return elapsedUs < Runtime::ChunkApplyBudgetUs;
    };

    ChunkData chunkData;
    size_t chunkDataApplied = 0;
    while (
        chunkDataApplied < Runtime::MaxChunkDataApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkData(chunkData)
    ) {
        runtime.chunkManager->applyNetworkChunkData(chunkData);
        if (!runtime.clientNet.SendChunkDataAck(chunkData)) {
            std::cerr
                << "[chunk/ack] app failed to ACK applied chunk ("
                << chunkData.chunkX << "," << chunkData.chunkY << "," << chunkData.chunkZ << ")\n";
        }
        ++chunkDataApplied;
    }

    ChunkDelta chunkDelta;
    size_t chunkDeltaApplied = 0;
    while (
        chunkDeltaApplied < Runtime::MaxChunkDeltaApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkDelta(chunkDelta)
    ) {
        runtime.chunkManager->applyNetworkChunkDelta(chunkDelta);
        ++chunkDeltaApplied;
    }

    ChunkUnload chunkUnload;
    size_t chunkUnloadApplied = 0;
    while (
        chunkUnloadApplied < Runtime::MaxChunkUnloadApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkUnload(chunkUnload)
    ) {
        runtime.chunkManager->applyNetworkChunkUnload(chunkUnload);
        ++chunkUnloadApplied;
    }

    const double now = glfwGetTime();
    if (now - runtime.lastChunkCoverageLogTime >= 1.0) {
        runtime.lastChunkCoverageLogTime = now;

        const glm::vec3 pos = runtime.player->getPosition();
        const glm::ivec3 worldPos(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
        const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
        const int viewDistance = std::max<int>(2, runtime.player->renderDistance);
        const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
        const int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

        const auto& chunks = runtime.chunkManager->getChunks();
        size_t desired = 0;
        size_t loaded = 0;
        std::vector<glm::ivec3> missingSamples;
        missingSamples.reserve(8);
        for (int x = centerChunk.x - viewDistance; x <= centerChunk.x + viewDistance; ++x) {
            for (int z = centerChunk.z - viewDistance; z <= centerChunk.z + viewDistance; ++z) {
                for (int y = minChunkY; y <= maxChunkY; ++y) {
                    const glm::ivec3 cp(x, y, z);
                    if (!runtime.chunkManager->inBounds(cp)) continue;
                    ++desired;
                    if (chunks.find(cp) != chunks.end()) {
                        ++loaded;
                    }
                    else if (missingSamples.size() < 8) {
                        missingSamples.push_back(cp);
                    }
                }
            }
        }

        std::cerr
            << "[chunk/client] coverage center=("
            << centerChunk.x << "," << centerChunk.y << "," << centerChunk.z << ")"
            << " viewDist=" << viewDistance
            << " desired=" << desired
            << " loaded=" << loaded
            << " missing=" << (desired - loaded) << "\n";

        if (!missingSamples.empty()) {
            std::cerr << "[chunk/client] missing samples:";
            for (const glm::ivec3& cp : missingSamples) {
                std::cerr << " (" << cp.x << "," << cp.y << "," << cp.z << ")";
            }
            std::cerr << "\n";
        }
    }

    if (now - runtime.lastNetSendTime >= Runtime::NetSendInterval) {
        runtime.lastNetSendTime = now;
        const glm::vec3 pos = runtime.player->getPosition();
        const glm::vec3 vel(0.0f);
        (void)runtime.clientNet.SendPosition(runtime.netSeq++, pos, vel);

        const glm::ivec3 worldPos(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
        const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
        const uint16_t viewDistance = static_cast<uint16_t>(std::max<int>(2, runtime.player->renderDistance));
        (void)runtime.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
}

void App::processFrame(Runtime& runtime) {
    updateDebugCamera(runtime);
    updateToggleStates();

    const Camera& activeCamera = m_UseDebugCamera ? runtime.debugCamera : runtime.player->getCamera();

    runtime.inputCallbacks->processInput(m_Window);
    runtime.player->update(m_Window, GameData::deltaTime);

    RenderFrameParams frameParams{
        .chunkShader = *runtime.chunkShader,
        .debugShader = *runtime.dbgShader,
        .chunkManager = *runtime.chunkManager,
        .frustum = runtime.frustum,
        .player = *runtime.player,
        .activeCamera = activeCamera,
        .sky = runtime.sky,
        .toggleWireframe = m_ToggleWireframe,
        .toggleChunkBorders = m_ToggleChunkBorders,
        .toggleDebugFrustum = m_ToggleDebugFrustum,
        .chunkUniformsInitialized = &runtime.chunkUniformsInitialized
    };
    runtime.renderer.renderFrame(frameParams);

    processWorldInteraction(runtime);
    processNetworking(runtime);

    updateFPSCounter();
    glfwSwapBuffers(m_Window);
    glfwPollEvents();
}

void App::shutdown(Runtime& runtime) {
    runtime.clientNet.Shutdown();

    runtime.sky.shutdown();

    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    glfwTerminate();
}

int App::Run() {
    if (!initWindowAndContext()) {
        return -1;
    }

    Runtime runtime;
    initGameplay(runtime);
    initCallbacks(runtime);
    initRenderResources(runtime);
    initNetworking(runtime);

    while (!glfwWindowShouldClose(m_Window)) {
        processFrame(runtime);
    }

    shutdown(runtime);
    return 0;
}
