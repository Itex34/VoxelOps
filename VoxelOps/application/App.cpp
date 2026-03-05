#include <glad/glad.h>

#include "App.hpp"


#include "../data/GameData.hpp"
#include "../graphics/Camera.hpp"
#include "../graphics/ChunkManager.hpp"
#include "../graphics/Frustum.hpp"
#include "../graphics/Renderer.hpp"
#include "../graphics/Shader.hpp"
#include "../graphics/Sky.hpp"
#include "../gun/Gun.hpp"
#include "../input/InputCallbacks.hpp"
#include "../network/ClientNetwork.hpp"
#include "../physics/RayManager.hpp"
#include "../physics/Raycast.hpp"
#include "../player/Player.hpp"
#include "../ui/debug/DebugUi.hpp"
#include "../../Shared/gun/GunType.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <deque>

namespace {
struct CallbackContext {
    InputCallbacks* inputCallbacks = nullptr;
    bool* useDebugCamera = nullptr;
};

bool IsImGuiTextInputActive() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput || io.WantCaptureKeyboard;
}

constexpr bool kEnableChunkDiagnostics = false;
constexpr bool kDefaultPlayerModelYawInvert = true;
constexpr float kDefaultPlayerModelYawOffsetDeg = 0.0f;
constexpr GunType kDefaultGunType = GunType::Pistol;

struct GunDefinition {
    GunType type = GunType::Pistol;
    std::string_view displayName{};
    std::string_view modelPath{};
    float fireIntervalSeconds = 1.0f / 8.0f;
    float reloadTimeSeconds = 2.0f;
    unsigned int maxAmmo = 6;
    glm::vec3 viewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
    glm::vec3 viewScale = glm::vec3(0.10f);
    glm::vec3 viewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
};

const std::array<GunDefinition, 2> kGunDefinitions{ {
    {
        .type = GunType::Pistol,
        .displayName = "Revolver",
        .modelPath = "revolver.fbx",
        .fireIntervalSeconds = 1.0f / 6.0f,
        .reloadTimeSeconds = 2.1f,
        .maxAmmo = 7,
        .viewOffset = glm::vec3(3.20f, -3.5f, 5.0f),
        .viewScale = glm::vec3(0.01f),
        .viewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f)
    },
    {
        .type = GunType::Sniper,
        .displayName = "Sniper",
        .modelPath = "Sniper/M1.obj",
        .fireIntervalSeconds = 0.85f,
        .reloadTimeSeconds = 2.8f,
        .maxAmmo = 5,
        .viewOffset = glm::vec3(1.8f, -1.5f, 4.56f),
        .viewScale = glm::vec3(1.00f),
        .viewEulerDeg = glm::vec3(-2.0f, 90.0f, 0.0f)
    }
} };

const GunDefinition* FindGunDefinition(GunType gunType) {
    for (const GunDefinition& definition : kGunDefinitions) {
        if (definition.type == gunType) {
            return &definition;
        }
    }
    return nullptr;
}

std::string ResolveGunModelPath(const GunDefinition& definition) {
    return Shared::RuntimePaths::ResolveModelsPath(
        std::string(definition.modelPath)
    ).generic_string();
}

std::unique_ptr<Gun> BuildGunFromDefinition(const GunDefinition& definition) {
    auto gun = std::make_unique<Gun>(
        definition.type,
        definition.fireIntervalSeconds,
        definition.reloadTimeSeconds,
        definition.maxAmmo
    );

    const std::string modelPath = ResolveGunModelPath(definition);
    if (!gun->loadModel(modelPath)) {
        std::cerr
            << "[gun] failed to load model for " << definition.displayName
            << " from " << modelPath
            << "\n";
    }
    return gun;
}

float NormalizeYawDegrees(float yawDegrees) {
    if (!std::isfinite(yawDegrees)) {
        return 0.0f;
    }
    float y = std::fmod(yawDegrees, 360.0f);
    if (y >= 180.0f) y -= 360.0f;
    if (y < -180.0f) y += 360.0f;
    return y;
}

float ToModelYawDegrees(float lookYawDegrees, bool invertYaw, float yawOffsetDeg) {
    const float signedYaw = invertYaw ? -lookYawDegrees : lookYawDegrees;
    return NormalizeYawDegrees(signedYaw + yawOffsetDeg);
}

inline bool IsNewerU32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline bool IsAckedU32(uint32_t sequence, uint32_t ack) {
    return static_cast<int32_t>(sequence - ack) <= 0;
}

struct LaunchOptions {
    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 27015;
    std::string requestedUsername;
    bool showHelp = false;
};

void PrintUsage() {
    std::cout
        << "VoxelOps client options:\n"
        << "  --server-ip <ipv4>   (default: 127.0.0.1)\n"
        << "  --server-port <port> (default: 27015)\n"
        << "  --name <username>    (optional, max 32 chars)\n"
        << "  --help\n";
}

bool ParsePort(std::string_view text, uint16_t& outPort) {
    if (text.empty()) {
        return false;
    }

    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }

    unsigned long value = 0;
    try {
        value = std::stoul(std::string(text));
    }
    catch (...) {
        return false;
    }

    if (value == 0 || value > 65535) {
        return false;
    }
    outPort = static_cast<uint16_t>(value);
    return true;
}

std::string TrimAscii(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

bool ParseIPv4(std::string_view text, std::string& outIp) {
    std::array<uint32_t, 4> octets{ 0, 0, 0, 0 };
    size_t octetIndex = 0;
    size_t pos = 0;

    while (pos < text.size() && octetIndex < octets.size()) {
        const size_t start = pos;
        while (pos < text.size() && text[pos] != '.') {
            const char c = text[pos];
            if (c < '0' || c > '9') {
                return false;
            }
            ++pos;
        }

        if (start == pos) {
            return false;
        }

        unsigned long value = 0;
        try {
            value = std::stoul(std::string(text.substr(start, pos - start)));
        }
        catch (...) {
            return false;
        }
        if (value > 255) {
            return false;
        }

        octets[octetIndex++] = static_cast<uint32_t>(value);
        if (pos < text.size() && text[pos] == '.') {
            ++pos;
        }
    }

    if (octetIndex != octets.size() || pos != text.size()) {
        return false;
    }

    outIp =
        std::to_string(octets[0]) + "." +
        std::to_string(octets[1]) + "." +
        std::to_string(octets[2]) + "." +
        std::to_string(octets[3]);
    return true;
}

bool ParseServerEndpoint(std::string_view text, std::string& outIp, uint16_t& outPort) {
    const std::string endpoint = TrimAscii(text);
    if (endpoint.empty()) {
        return false;
    }

    const size_t colonPos = endpoint.find(':');
    if (colonPos == std::string::npos || endpoint.find(':', colonPos + 1) != std::string::npos) {
        return false;
    }

    const std::string_view ipPart(endpoint.data(), colonPos);
    const std::string_view portPart(endpoint.data() + colonPos + 1, endpoint.size() - colonPos - 1);
    std::string parsedIp;
    uint16_t parsedPort = 0;
    if (!ParseIPv4(ipPart, parsedIp) || !ParsePort(portPart, parsedPort)) {
        return false;
    }

    outIp = parsedIp;
    outPort = parsedPort;
    return true;
}

bool ParseLaunchOptions(int argc, char** argv, LaunchOptions& outOptions) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = (argv[i] != nullptr) ? std::string_view(argv[i]) : std::string_view();
        if (arg.empty()) {
            continue;
        }

        if (arg == "--help" || arg == "-h") {
            outOptions.showHelp = true;
            continue;
        }

        if (arg == "--server-ip" || arg == "-s") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::cerr << "Missing value for " << arg << "\n";
                return false;
            }
            outOptions.serverIp = argv[++i];
            continue;
        }

        if (arg == "--server-port" || arg == "-p") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::cerr << "Missing value for " << arg << "\n";
                return false;
            }
            uint16_t parsedPort = 0;
            if (!ParsePort(argv[++i], parsedPort)) {
                std::cerr << "Invalid server port: " << argv[i] << "\n";
                return false;
            }
            outOptions.serverPort = parsedPort;
            continue;
        }

        if (arg == "--name" || arg == "-n") {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::cerr << "Missing value for " << arg << "\n";
                return false;
            }
            outOptions.requestedUsername = argv[++i];
            if (outOptions.requestedUsername.size() > 32) {
                std::cerr << "Username too long (max 32 chars)\n";
                return false;
            }
            continue;
        }

        std::cerr << "Unknown option: " << arg << "\n";
        return false;
    }

    return true;
}
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
    std::unique_ptr<Shader> gunShader;
    Sky sky;
    std::unique_ptr<DebugUi> debugUi;

    Frustum frustum;
    Camera debugCamera{ glm::vec3(0.0f, 100.0f, 0.0f) };
    Camera interpolatedPlayerCamera{ glm::vec3(0.0f) };

    bool supportsGL43Shaders = false;
    bool chunkUniformsInitialized = false;

    uint32_t netSeq = 1;
    uint32_t lastAckedInputSeq = 0;
    uint32_t lastAppliedServerTick = 0;
    bool hasAppliedServerTick = false;
    uint32_t lastReceivedSelfSnapshotTick = 0;
    bool hasReceivedSelfSnapshotTick = false;
    double localSimAccumulator = 0.0;
    struct PendingInputEntry {
        PlayerInput packet{};
        double deltaSeconds = 0.0;
    };
    std::deque<PendingInputEntry> pendingInputs;
    static constexpr size_t MaxPendingInputs = 256;
    double lastInputSendTime = 0.0;
    double lastChunkRequestSendTime = 0.0;
    double lastShootSendTime = 0.0;
    double nextReconnectAttemptTime = 0.0;
    double reconnectBackoffSeconds = 1.0;
    std::string lastConnectionStatus = "disconnected";
    std::array<char, 32> pendingServerEndpointInput{};
    std::array<char, kMaxConnectUsernameChars + 1> pendingUsernameInput{};
    std::string usernamePromptError;
    uint32_t nextClientShotId = 1;
    double shootSendInterval = 1.0 / 8.0;
    GunType equippedGunType = kDefaultGunType;
    std::unordered_map<uint16_t, std::unique_ptr<Gun>> preloadedGuns;
    Gun* equippedGun = nullptr;
    glm::vec3 equippedGunViewOffset = glm::vec3(0.20f, -0.20f, -0.45f);
    glm::vec3 equippedGunViewScale = glm::vec3(0.10f);
    glm::vec3 equippedGunViewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
    static constexpr double InputSendInterval = 1.0 / 60.0; // 60 Hz
    static constexpr double LocalPredictionStep = 1.0 / 60.0; // match authoritative server tick for replay parity
    static constexpr size_t MaxLocalPredictionStepsPerFrame = 8;
    static constexpr float BasicAuthReconcileDeadzone = 0.20f;
    static constexpr float BasicAuthReconcileTeleportDistance = 2.0f;
    static constexpr float BasicAuthCorrectionSpeedGround = 7.5f;
    static constexpr float BasicAuthCorrectionSpeedAir = 2.0f;
    static constexpr float BasicAuthGroundYDeadzone = 0.30f;
    static constexpr float BasicAuthGroundYCorrectionScale = 0.25f;
    static constexpr float BasicAuthAirYDeadzone = 0.14f;
    static constexpr float BasicAuthAirYCorrectionScale = 0.12f;
    static constexpr float BasicAuthStepTransitionYApplyScale = 0.12f;
    static constexpr float BasicAuthAirYApplyScale = 0.35f;
    static constexpr float BasicAuthMaxPendingCorrection = 0.75f;
    static constexpr size_t InputRedundancyCopies = 2; // resend latest unacked states to mask packet loss
    static constexpr double ChunkRequestSendInterval = 0.5; // 2 Hz baseline + immediate on center changes
    static constexpr double ChunkRequestCenterChangeMinInterval = 1.0 / 30.0; // up to 30 Hz on border crossings
    static constexpr size_t MaxChunkDataApplyPerFrame = 4;
    static constexpr size_t MaxChunkDeltaApplyPerFrame = 24;
    static constexpr size_t MaxChunkUnloadApplyPerFrame = 48;
    static constexpr int64_t ChunkApplyBudgetUs = 3000;
    static constexpr int64_t ChunkApplyBudgetUsUnderInputPressure = 750;
    static constexpr size_t MaxChunkMeshBuildsPerFrame = 3;
    static constexpr size_t MaxChunkMeshBuildsPerFrameUnderInputPressure = 1;
    static constexpr int64_t ChunkMeshBuildBudgetUs = 2000;
    static constexpr int64_t ChunkMeshBuildBudgetUsUnderInputPressure = 500;
    double lastChunkCoverageLogTime = 0.0;
    glm::ivec3 lastChunkRequestCenter{ 0 };
    bool hasLastChunkRequestCenter = false;
    glm::vec3 pendingAuthoritativeCorrection{ 0.0f };

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

    if (elapsedTime >= 2.0) {
        const double fps = GameData::frameCount / elapsedTime;
        std::stringstream ss;
        ss << "Voxel Ops - FPS: " << fps;
        glfwSetWindowTitle(m_Window, ss.str().c_str());

        GameData::frameCount = 0;
        GameData::fpsTime = currentTime;
    }
}

void App::applyMouseInputModes() {
    if (!m_Window) {
        return;
    }

    const bool cursorEnabled = GameData::cursorEnabled;
    glfwSetInputMode(m_Window, GLFW_CURSOR, cursorEnabled ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_DISABLED);

    if (glfwRawMouseMotionSupported()) {
        const int rawEnabled = (!cursorEnabled && m_EnableRawMouseInput) ? GLFW_TRUE : GLFW_FALSE;
        glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, rawEnabled);
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
    // Default to VSync for stable frame pacing and smoother perceived camera motion.
    glfwSwapInterval(1);
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

    const std::string playerModelPath =
        Shared::RuntimePaths::ResolveModelsPath("MinecraftPlayer/Player.fbx").generic_string();

    runtime.player = std::make_unique<Player>(
        glm::vec3(0.0f, 60.0f, 0.0f),
        *runtime.chunkManager,
        playerModelPath
    );
    runtime.interpolatedPlayerCamera = runtime.player->getCamera();
    runtime.inputCallbacks = std::make_unique<InputCallbacks>(*runtime.player);
    preloadGuns(runtime);
    (void)equipGun(runtime, kDefaultGunType);
}

void App::preloadGuns(Runtime& runtime) {
    runtime.preloadedGuns.clear();
    runtime.equippedGun = nullptr;

    for (const GunDefinition& definition : kGunDefinitions) {
        const uint16_t weaponId = ToWeaponId(definition.type);
        runtime.preloadedGuns[weaponId] = BuildGunFromDefinition(definition);
        std::cout
            << "[gun] preloaded " << definition.displayName
            << " (weaponId=" << weaponId << ")"
            << " model=" << ResolveGunModelPath(definition)
            << "\n";
    }
}

bool App::equipGun(Runtime& runtime, GunType gunType) {
    const GunDefinition* definition = FindGunDefinition(gunType);
    if (definition == nullptr) {
        std::cerr << "[gun] missing definition for weapon id=" << ToWeaponId(gunType) << "\n";
        return false;
    }

    if (runtime.equippedGun && runtime.equippedGunType == gunType) {
        return true;
    }

    const uint16_t weaponId = ToWeaponId(definition->type);
    auto it = runtime.preloadedGuns.find(weaponId);
    if (it == runtime.preloadedGuns.end()) {
        std::cerr
            << "[gun] preloaded entry missing for weaponId=" << weaponId
            << ", loading on-demand.\n";
        runtime.preloadedGuns[weaponId] = BuildGunFromDefinition(*definition);
        it = runtime.preloadedGuns.find(weaponId);
        if (it == runtime.preloadedGuns.end() || !it->second) {
            std::cerr << "[gun] unable to equip weaponId=" << weaponId << "\n";
            return false;
        }
    }

    runtime.equippedGun = it->second.get();
    runtime.equippedGunType = definition->type;
    runtime.shootSendInterval = std::max(0.03, static_cast<double>(definition->fireIntervalSeconds));
    runtime.equippedGunViewOffset = definition->viewOffset;
    runtime.equippedGunViewScale = definition->viewScale;
    runtime.equippedGunViewEulerDeg = definition->viewEulerDeg;

    std::cout
        << "[gun] equipped " << definition->displayName
        << " (weaponId=" << weaponId << ")"
        << " [preloaded]"
        << "\n";
    return true;
}

void App::renderHeldGun(Runtime& runtime, const Camera& activeCamera) {
    if (m_UseDebugCamera) {
        return;
    }
    if (!runtime.gunShader || !runtime.equippedGun) {
        return;
    }

    const float frontLenSq = glm::dot(activeCamera.front, activeCamera.front);
    if (!std::isfinite(frontLenSq) || frontLenSq < 1e-8f) {
        return;
    }
    glm::vec3 forward = glm::normalize(activeCamera.front);
    glm::vec3 up = activeCamera.up;
    const float upLenSq = glm::dot(up, up);
    if (!std::isfinite(upLenSq) || upLenSq < 1e-8f) {
        up = glm::vec3(0.0f, 1.0f, 0.0f);
    }
    else {
        up = glm::normalize(up);
    }
    glm::vec3 right = glm::cross(forward, up);
    const float rightLenSq = glm::dot(right, right);
    if (!std::isfinite(rightLenSq) || rightLenSq < 1e-8f) {
        right = glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f));
        const float fallbackLenSq = glm::dot(right, right);
        if (!std::isfinite(fallbackLenSq) || fallbackLenSq < 1e-8f) {
            return;
        }
    }
    right = glm::normalize(right);
    up = glm::normalize(glm::cross(right, forward));

    const glm::vec3 gunPos =
        activeCamera.position +
        right * runtime.equippedGunViewOffset.x +
        up * runtime.equippedGunViewOffset.y +
        forward * runtime.equippedGunViewOffset.z;

    const glm::mat4 lookBasis = glm::inverse(glm::lookAt(glm::vec3(0.0f), forward, up));
    glm::quat gunRot = glm::normalize(glm::quat_cast(glm::mat3(lookBasis)));
    const glm::quat yawOffset = glm::angleAxis(
        glm::radians(runtime.equippedGunViewEulerDeg.y),
        glm::vec3(0.0f, 1.0f, 0.0f)
    );
    const glm::quat pitchOffset = glm::angleAxis(
        glm::radians(runtime.equippedGunViewEulerDeg.x),
        glm::vec3(1.0f, 0.0f, 0.0f)
    );
    const glm::quat rollOffset = glm::angleAxis(
        glm::radians(runtime.equippedGunViewEulerDeg.z),
        glm::vec3(0.0f, 0.0f, 1.0f)
    );
    gunRot = glm::normalize(gunRot * yawOffset * pitchOffset * rollOffset);

    const float aspect = static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight);
    const glm::mat4 projection = glm::perspective(glm::radians(GameData::FOV), aspect, 0.02f, 200.0f);
    const glm::mat4 view = activeCamera.getViewMatrix();

    const bool cullFaceWasEnabled = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
    GLint previousCullFaceMode = GL_BACK;
    GLint previousFrontFace = GL_CCW;
    GLint previousDepthFunc = GL_LESS;
    GLboolean previousDepthMask = GL_TRUE;
    glGetIntegerv(GL_CULL_FACE_MODE, &previousCullFaceMode);
    glGetIntegerv(GL_FRONT_FACE, &previousFrontFace);
    glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunc);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask);

    // Viewmodel should self-occlude like a regular opaque mesh.
    // Clear only depth after world pass so it still renders in front of terrain.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDepthFunc(GL_LEQUAL);

    runtime.gunShader->use();
    runtime.gunShader->setInt("diffuseTexture", 0);
    runtime.gunShader->setVec3("lightDir", glm::normalize(runtime.sky.getSunDir()));
    runtime.gunShader->setVec3("lightColor", glm::vec3(1.0f, 0.98f, 0.96f));
    runtime.gunShader->setVec3("ambientColor", glm::vec3(0.42f, 0.44f, 0.47f));
    runtime.gunShader->setMat4("view", view);
    runtime.gunShader->setMat4("projection", projection);
    runtime.equippedGun->render(gunPos, gunRot, runtime.equippedGunViewScale, *runtime.gunShader);

    glDepthFunc(static_cast<GLenum>(previousDepthFunc));
    glDepthMask(previousDepthMask);
    glCullFace(static_cast<GLenum>(previousCullFaceMode));
    glFrontFace(static_cast<GLenum>(previousFrontFace));
    if (cullFaceWasEnabled) {
        glEnable(GL_CULL_FACE);
    }
    else {
        glDisable(GL_CULL_FACE);
    }
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
    applyMouseInputModes();
}

void App::initRenderResources(Runtime& runtime) {
    const std::string chunkVertPath = runtime.supportsGL43Shaders
        ? Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack.vert").generic_string()
        : Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack33.vert").generic_string();
    const std::string chunkFragPath = runtime.supportsGL43Shaders
        ? Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack.frag").generic_string()
        : Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/allLightingPack33.frag").generic_string();
    const std::string debugVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugVert.vert").generic_string();
    const std::string debugFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/debugFrag.frag").generic_string();
    const std::string skyVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/sky.vert").generic_string();
    const std::string skyFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/sky_simple.frag").generic_string();
    const std::string playerVertPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.vert").generic_string();
    const std::string playerFragPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("shaders/player.frag").generic_string();

    runtime.chunkShader = std::make_unique<Shader>(chunkVertPath.c_str(), chunkFragPath.c_str());
    runtime.dbgShader = std::make_unique<Shader>(
        debugVertPath.c_str(),
        debugFragPath.c_str()
    );
    runtime.gunShader = std::make_unique<Shader>(
        playerVertPath.c_str(),
        playerFragPath.c_str()
    );
    runtime.sky.initialize(
        skyVertPath.c_str(),
        skyFragPath.c_str()
    );

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

}

void App::initUi(Runtime& runtime) {
    runtime.debugUi = std::make_unique<DebugUi>();
    if (!runtime.debugUi->initialize(m_Window, "#version 330")) {
        std::cerr << "Failed to initialize ImGui debug UI.\n";
        runtime.debugUi.reset();
        return;
    }

    runtime.debugUi->setVisible(m_ShowDebugUi);
    if (m_ShowDebugUi) {
        GameData::cursorEnabled = true;
    }
    applyMouseInputModes();
}

void App::initNetworking(Runtime& runtime) {
    const double now = glfwGetTime();
    runtime.lastInputSendTime = now;
    runtime.lastChunkRequestSendTime = now;
    runtime.lastShootSendTime = now - runtime.shootSendInterval;
    runtime.nextReconnectAttemptTime = now;
    runtime.reconnectBackoffSeconds = 1.0;
    runtime.hasLastChunkRequestCenter = false;
    runtime.usernamePromptError.clear();
    runtime.pendingServerEndpointInput.fill('\0');
    runtime.pendingUsernameInput.fill('\0');
    {
        const std::string endpoint = m_ServerIp + ":" + std::to_string(m_ServerPort);
        const size_t copyLen = std::min(endpoint.size(), runtime.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        runtime.pendingServerEndpointInput[copyLen] = '\0';
    }
    if (!m_RequestedUsername.empty()) {
        const size_t copyLen = std::min(m_RequestedUsername.size(), runtime.pendingUsernameInput.size() - 1);
        std::memcpy(runtime.pendingUsernameInput.data(), m_RequestedUsername.data(), copyLen);
        runtime.pendingUsernameInput[copyLen] = '\0';
    }

    if (!runtime.clientNet.Start()) {
        std::cerr << "Failed to start networking\n";
        runtime.lastConnectionStatus = runtime.clientNet.GetConnectionStatusText();
        return;
    }

    runtime.lastConnectionStatus = runtime.clientNet.GetConnectionStatusText();
}

bool App::beginConnectionAttempt(Runtime& runtime) {
    if (!runtime.clientNet.ConnectTo(m_ServerIp.c_str(), m_ServerPort)) {
        std::cerr << "ConnectTo(" << m_ServerIp << ":" << m_ServerPort << ") failed\n";
        return false;
    }
    if (!runtime.clientNet.SendConnectRequest(m_RequestedUsername)) {
        std::cerr << "Failed to send connect request\n";
        return false;
    }
    return true;
}

void App::drawConnectionPrompt(Runtime& runtime) {
    if (!runtime.debugUi || runtime.clientNet.IsConnected()) {
        return;
    }

    GameData::cursorEnabled = true;

    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 windowSize(460.0f, 0.0f);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f)
    );

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove;

    if (!ImGui::Begin("Connect", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Server (x.x.x.x:XXXXX)");
    ImGui::SetNextItemWidth(-1.0f);
    const ClientNetwork::ConnectionState connState = runtime.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);

    bool submit = false;
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText(
        "##server_endpoint_input",
        runtime.pendingServerEndpointInput.data(),
        runtime.pendingServerEndpointInput.size(),
        ImGuiInputTextFlags_EnterReturnsTrue
    )) {
        submit = true;
    }
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Enter username");
    ImGui::SetNextItemWidth(-1.0f);
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText(
        "##username_input",
        runtime.pendingUsernameInput.data(),
        runtime.pendingUsernameInput.size(),
        ImGuiInputTextFlags_EnterReturnsTrue
    )) {
        submit = true;
    }
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    if (!isConnecting) {
        if (ImGui::Button("Connect")) {
            submit = true;
        }
    }
    else {
        ImGui::BeginDisabled();
        ImGui::Button("Connecting...");
        ImGui::EndDisabled();
    }

    if (submit) {
        std::string desiredEndpoint = runtime.pendingServerEndpointInput.data();
        std::string parsedIp;
        uint16_t parsedPort = 0;
        if (!ParseServerEndpoint(desiredEndpoint, parsedIp, parsedPort)) {
            runtime.usernamePromptError = "Server must be x.x.x.x:XXXXX (example: 127.0.0.1:27015).";
            ImGui::End();
            applyMouseInputModes();
            return;
        }

        std::string desiredUsername = runtime.pendingUsernameInput.data();
        size_t begin = 0;
        while (begin < desiredUsername.size() && std::isspace(static_cast<unsigned char>(desiredUsername[begin])) != 0) {
            ++begin;
        }
        size_t end = desiredUsername.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(desiredUsername[end - 1])) != 0) {
            --end;
        }
        desiredUsername = desiredUsername.substr(begin, end - begin);

        if (desiredUsername.empty()) {
            runtime.usernamePromptError = "Please enter a username.";
        }
        else {
            if (desiredUsername.size() > kMaxConnectUsernameChars) {
                desiredUsername.resize(kMaxConnectUsernameChars);
            }

            m_ServerIp = parsedIp;
            m_ServerPort = parsedPort;
            const std::string endpoint = m_ServerIp + ":" + std::to_string(m_ServerPort);
            std::memset(runtime.pendingServerEndpointInput.data(), 0, runtime.pendingServerEndpointInput.size());
            std::memcpy(runtime.pendingServerEndpointInput.data(), endpoint.data(), endpoint.size());

            m_RequestedUsername = desiredUsername;
            std::memset(runtime.pendingUsernameInput.data(), 0, runtime.pendingUsernameInput.size());
            std::memcpy(runtime.pendingUsernameInput.data(), m_RequestedUsername.data(), m_RequestedUsername.size());

            runtime.usernamePromptError.clear();
            if (!beginConnectionAttempt(runtime)) {
                runtime.usernamePromptError = "Failed to start connection. Check server reachability and retry.";
            }
            runtime.nextReconnectAttemptTime = glfwGetTime() + 1.0;
        }
    }

    if (!runtime.usernamePromptError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", runtime.usernamePromptError.c_str());
    }

    ImGui::Spacing();
    ImGui::TextWrapped("If that username is already taken, enter a different username and retry.");
    ImGui::Text("Status: %s", runtime.clientNet.GetConnectionStatusText().c_str());

    ImGui::End();
    applyMouseInputModes();
}

void App::updateDebugCamera(Runtime& runtime) {
    glfwGetCursorPos(m_Window, &runtime.xpos, &runtime.ypos);

    const bool keyboardBlockedByUi = IsImGuiTextInputActive();
    glm::vec3 moveDir(0.0f);
    if (!keyboardBlockedByUi) {
        if (glfwGetKey(m_Window, GLFW_KEY_U) == GLFW_PRESS) moveDir += runtime.debugCamera.XZfront;
        if (glfwGetKey(m_Window, GLFW_KEY_J) == GLFW_PRESS) moveDir -= runtime.debugCamera.XZfront;
        if (glfwGetKey(m_Window, GLFW_KEY_H) == GLFW_PRESS) moveDir -= glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
        if (glfwGetKey(m_Window, GLFW_KEY_K) == GLFW_PRESS) moveDir += glm::normalize(glm::cross(runtime.debugCamera.front, runtime.debugCamera.up));
        if (glfwGetKey(m_Window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS) moveDir += runtime.debugCamera.up;
        if (glfwGetKey(m_Window, GLFW_KEY_V) == GLFW_PRESS) moveDir -= runtime.debugCamera.up;
    }

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

void App::updateToggleStates(Runtime& runtime) {
    const bool keyboardBlockedByUi = IsImGuiTextInputActive();

    const bool isF1Pressed = glfwGetKey(m_Window, GLFW_KEY_F1) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF1Pressed && !m_WasF1Pressed) {
        m_UseDebugCamera = !m_UseDebugCamera;
    }
    m_WasF1Pressed = isF1Pressed;

    const bool isTPressed = glfwGetKey(m_Window, GLFW_KEY_T) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isTPressed && !m_WasTPressed) {
        m_ToggleWireframe = !m_ToggleWireframe;
    }
    m_WasTPressed = isTPressed;

    const bool isF2Pressed = glfwGetKey(m_Window, GLFW_KEY_F2) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF2Pressed && !m_WasF2Pressed) {
        m_ToggleChunkBorders = !m_ToggleChunkBorders;
    }
    m_WasF2Pressed = isF2Pressed;

    const bool isF3Pressed = glfwGetKey(m_Window, GLFW_KEY_F3) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF3Pressed && !m_WasF3Pressed) {
        m_ToggleDebugFrustum = !m_ToggleDebugFrustum;
    }
    m_WasF3Pressed = isF3Pressed;

    const bool isF10Pressed = glfwGetKey(m_Window, GLFW_KEY_F10) == GLFW_PRESS;
    if (!keyboardBlockedByUi && isF10Pressed && !m_WasF10Pressed) {
        m_ShowDebugUi = !m_ShowDebugUi;
        if (runtime.debugUi) {
            runtime.debugUi->setVisible(m_ShowDebugUi);
        }

        if (m_ShowDebugUi) {
            GameData::cursorEnabled = true;
        }
        else {
            GameData::cursorEnabled = false;
        }
        applyMouseInputModes();
    }
    m_WasF10Pressed = isF10Pressed;
}

void App::processWorldInteraction(Runtime& runtime) {
    if (GameData::cursorEnabled || IsImGuiTextInputActive()) {
        return;
    }

    if (glfwGetKey(m_Window, GLFW_KEY_H) == GLFW_PRESS) {
        Ray ray(runtime.player->getCamera().position, runtime.player->getCamera().front);
        const RayResult hitResult = runtime.rayManager.rayHasBlockIntersectSingle(
            ray, *runtime.chunkManager, runtime.player->maxReach
        );
        if (hitResult.hit) {
            runtime.chunkManager->playerBreakBlockAt(hitResult.hitBlockWorld);
        }
    }

    if (glfwGetKey(m_Window, GLFW_KEY_G) == GLFW_PRESS) {
        Ray ray(runtime.player->getCamera().position, runtime.player->getCamera().front);
        const RayResult hitResult = runtime.rayManager.rayHasBlockIntersectSingle(
            ray, *runtime.chunkManager, runtime.player->maxReach + 100.05f
        );
        if (hitResult.hit) {
            runtime.chunkManager->playerPlaceBlockAt(hitResult.hitBlockWorld, 0, BlockID::Dirt);
        }
    }
}

void App::processShooting(Runtime& runtime) {
    const bool keyboardBlockedByUi = IsImGuiTextInputActive();

    if (!keyboardBlockedByUi && glfwGetKey(m_Window, GLFW_KEY_1) == GLFW_PRESS) {
        (void)equipGun(runtime, GunType::Pistol);
    }
    else if (!keyboardBlockedByUi && glfwGetKey(m_Window, GLFW_KEY_2) == GLFW_PRESS) {
        (void)equipGun(runtime, GunType::Sniper);
    }

    if (GameData::cursorEnabled || m_UseDebugCamera) {
        return;
    }
    if (!runtime.clientNet.IsConnected()) {
        return;
    }
    if (!runtime.equippedGun) {
        return;
    }

    const bool triggerPressed = (glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    if (!triggerPressed) {
        return;
    }

    const double now = glfwGetTime();
    if ((now - runtime.lastShootSendTime) < runtime.shootSendInterval) {
        return;
    }

    const Camera& cam = runtime.player->getCamera();
    const float dirLenSq = glm::dot(cam.front, cam.front);
    if (!std::isfinite(dirLenSq) || dirLenSq < 1e-8f) {
        return;
    }

    const glm::vec3 shootDir = glm::normalize(cam.front);
    const glm::vec3 shootPos = cam.position;
    const uint32_t shotId = runtime.nextClientShotId++;
    const uint32_t clientTick = runtime.hasAppliedServerTick ? runtime.lastAppliedServerTick : 0u;
    const uint32_t seed = shotId ^ (clientTick * 2654435761u);

    if (runtime.clientNet.SendShootRequest(
        shotId,
        clientTick,
        runtime.equippedGun->getWeaponId(),
        shootPos,
        shootDir,
        seed,
        0
    )) {
        runtime.lastShootSendTime = now;
    }
}

void App::processMovementNetworking(Runtime& runtime) {
    runtime.clientNet.Poll();

    const std::string& statusNow = runtime.clientNet.GetConnectionStatusText();
    if (statusNow != runtime.lastConnectionStatus) {
        std::cout << "[net] status: " << statusNow << "\n";
        runtime.lastConnectionStatus = statusNow;
        if (runtime.clientNet.IsConnected()) {
            runtime.usernamePromptError.clear();
        }
        else if (statusNow.find("username already taken") != std::string::npos) {
            runtime.usernamePromptError = "Username already taken. Enter a different username and retry.";
        }
    }

    const double now = glfwGetTime();
    const ClientNetwork::ConnectionState connState = runtime.clientNet.GetConnectionState();
    if (connState == ClientNetwork::ConnectionState::Disconnected) {
        if (runtime.clientNet.ShouldAutoReconnect() && now >= runtime.nextReconnectAttemptTime) {
            const bool started = beginConnectionAttempt(runtime);
            const double backoff = runtime.reconnectBackoffSeconds;
            runtime.nextReconnectAttemptTime = now + (started ? backoff : 2.0);
            runtime.reconnectBackoffSeconds = std::min(runtime.reconnectBackoffSeconds * 1.5, 8.0);
        }
    }
    else {
        runtime.reconnectBackoffSeconds = 1.0;
    }

    ShootResult shootResult{};
    while (runtime.clientNet.PopShootResult(shootResult)) {
        if (!shootResult.accepted) {
            std::cout << "[shoot] rejected shot id=" << shootResult.clientShotId << "\n";
            continue;
        }
        if (shootResult.didHit) {
            std::cout
                << "[shoot] hit id=" << shootResult.hitEntityId
                << " dmg=" << shootResult.damageApplied
                << " at=(" << shootResult.hitX << "," << shootResult.hitY << "," << shootResult.hitZ << ")\n";
        }
        else {
            std::cout
                << "[shoot] miss"
                << " at=(" << shootResult.hitX << "," << shootResult.hitY << "," << shootResult.hitZ << ")\n";
        }
    }

    bool hasNewestSelfSnapshot = false;
    uint32_t newestServerTick = 0;
    uint32_t newestAckedInputSeq = 0;
    glm::vec3 newestServerPos(0.0f);
    glm::vec3 newestServerVel(0.0f);
    bool newestServerOnGround = false;
    bool newestServerFlyMode = false;
    bool newestServerAllowFlyMode = false;
    std::vector<PlayerSnapshot> newestRemotePlayerSnapshots;
    uint64_t newestRemoteSelfPlayerId = 0;
    bool hasNewestRemotePlayers = false;

    PlayerSnapshotFrame snapshotFrame;
    while (runtime.clientNet.PopPlayerSnapshot(snapshotFrame)) {
        if (runtime.hasReceivedSelfSnapshotTick &&
            !IsNewerU32(snapshotFrame.serverTick, runtime.lastReceivedSelfSnapshotTick)) {
            continue;
        }

        const PlayerSnapshot* localSnapshot = nullptr;
        for (const PlayerSnapshot& snapshot : snapshotFrame.players) {
            if (snapshot.id == snapshotFrame.selfPlayerId) {
                localSnapshot = &snapshot;
                break;
            }
        }
        if (localSnapshot == nullptr) {
            continue;
        }

        runtime.hasReceivedSelfSnapshotTick = true;
        runtime.lastReceivedSelfSnapshotTick = snapshotFrame.serverTick;

        hasNewestSelfSnapshot = true;
        newestServerTick = snapshotFrame.serverTick;
        newestAckedInputSeq = snapshotFrame.lastProcessedInputSequence;
        newestServerPos = glm::vec3(localSnapshot->px, localSnapshot->py, localSnapshot->pz);
        newestServerVel = glm::vec3(localSnapshot->vx, localSnapshot->vy, localSnapshot->vz);
        newestServerOnGround = (localSnapshot->onGround != 0);
        newestServerFlyMode = (localSnapshot->flyMode != 0);
        newestServerAllowFlyMode = (localSnapshot->allowFlyMode != 0);

        newestRemoteSelfPlayerId = snapshotFrame.selfPlayerId;
        newestRemotePlayerSnapshots = std::move(snapshotFrame.players);
        hasNewestRemotePlayers = true;
    }

    if (hasNewestRemotePlayers) {
        std::unordered_map<PlayerID, PlayerState> newestRemotePlayers;
        newestRemotePlayers.reserve(newestRemotePlayerSnapshots.size());
        for (const PlayerSnapshot& snapshot : newestRemotePlayerSnapshots) {
            if (snapshot.id == newestRemoteSelfPlayerId) {
                continue;
            }

            PlayerState remoteState;
            remoteState.position = glm::vec3(snapshot.px, snapshot.py, snapshot.pz);
            remoteState.rotation = glm::angleAxis(
                glm::radians(ToModelYawDegrees(
                    NormalizeYawDegrees(snapshot.yaw),
                    kDefaultPlayerModelYawInvert,
                    kDefaultPlayerModelYawOffsetDeg
                )),
                glm::vec3(0.0f, 1.0f, 0.0f)
            );
            remoteState.scale = glm::vec3(1.0f);
            newestRemotePlayers[snapshot.id] = remoteState;
        }
        runtime.player->setConnectedPlayers(newestRemotePlayers);
    }

    if (hasNewestSelfSnapshot &&
        (!runtime.hasAppliedServerTick || IsNewerU32(newestServerTick, runtime.lastAppliedServerTick))) {
        runtime.hasAppliedServerTick = true;
        runtime.lastAppliedServerTick = newestServerTick;
        runtime.lastAckedInputSeq = newestAckedInputSeq;
        while (!runtime.pendingInputs.empty() &&
            IsAckedU32(runtime.pendingInputs.front().packet.sequenceNumber, runtime.lastAckedInputSeq)) {
            runtime.pendingInputs.pop_front();
        }

        runtime.player->setFlyModeAllowed(newestServerAllowFlyMode);
        const Player::SimulationState predictedState = runtime.player->captureSimulationState();
        const glm::vec3 predictedPos = predictedState.position;

        Player::SimulationState serverBaseState = predictedState;
        serverBaseState.position = newestServerPos;
        serverBaseState.velocity = newestServerVel;
        serverBaseState.onGround = newestServerOnGround;
        serverBaseState.flyMode = newestServerFlyMode;
        runtime.player->restoreSimulationState(serverBaseState);

        for (const Runtime::PendingInputEntry& pending : runtime.pendingInputs) {
            NetworkInputState replayInput{};
            replayInput.moveX = pending.packet.moveX;
            replayInput.moveZ = pending.packet.moveZ;
            replayInput.yaw = pending.packet.yaw;
            replayInput.pitch = pending.packet.pitch;
            replayInput.flags = pending.packet.inputFlags;
            replayInput.flyMode =
                newestServerAllowFlyMode &&
                (pending.packet.flyMode != 0);
            runtime.player->simulateFromNetworkInput(replayInput, pending.deltaSeconds, false);
        }

        const Player::SimulationState reconciledState = runtime.player->captureSimulationState();
        const glm::vec3 correction = reconciledState.position - predictedPos;
        const float correctionLenSq = glm::dot(correction, correction);
        const float deadzoneDistSq =
            Runtime::BasicAuthReconcileDeadzone * Runtime::BasicAuthReconcileDeadzone;
        const float teleportDistSq =
            Runtime::BasicAuthReconcileTeleportDistance * Runtime::BasicAuthReconcileTeleportDistance;

        if (correctionLenSq > teleportDistSq) {
            runtime.player->restoreSimulationState(reconciledState);
            runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
        }
        else {
            runtime.player->restoreSimulationState(predictedState);
            glm::vec3 queuedCorrection = correction;
            const bool airborne = !predictedState.onGround || !reconciledState.onGround;
            if (airborne) {
                if (std::abs(queuedCorrection.y) < Runtime::BasicAuthAirYDeadzone) {
                    queuedCorrection.y = 0.0f;
                }
                else {
                    queuedCorrection.y *= Runtime::BasicAuthAirYCorrectionScale;
                }
            }
            else {
                if (std::abs(queuedCorrection.y) < Runtime::BasicAuthGroundYDeadzone) {
                    queuedCorrection.y = 0.0f;
                }
                else {
                    queuedCorrection.y *= Runtime::BasicAuthGroundYCorrectionScale;
                }
            }

            const float queuedLenSq = glm::dot(queuedCorrection, queuedCorrection);
            if (queuedLenSq > deadzoneDistSq) {
                runtime.pendingAuthoritativeCorrection += queuedCorrection;
                const float pendingLen = glm::length(runtime.pendingAuthoritativeCorrection);
                if (pendingLen > Runtime::BasicAuthMaxPendingCorrection) {
                    runtime.pendingAuthoritativeCorrection *= Runtime::BasicAuthMaxPendingCorrection / pendingLen;
                }
            }
        }
    }

    if (!runtime.clientNet.IsConnected()) {
        runtime.pendingInputs.clear();
        runtime.player->setFlyModeAllowed(false);
        runtime.player->clearConnectedPlayers();
        runtime.hasAppliedServerTick = false;
        runtime.hasReceivedSelfSnapshotTick = false;
        runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
        return;
    }

    constexpr size_t kMaxInputSendsPerFrame = 4;
    size_t inputSendsThisFrame = 0;
    while (
        now - runtime.lastInputSendTime >= Runtime::InputSendInterval &&
        inputSendsThisFrame < kMaxInputSendsPerFrame
    ) {
        runtime.lastInputSendTime += Runtime::InputSendInterval;

        const NetworkInputState& input = runtime.player->getNetworkInputState();
        PlayerInput packet;
        packet.sequenceNumber = runtime.netSeq++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (!runtime.clientNet.SendPlayerInput(packet)) {
            break;
        }

        Runtime::PendingInputEntry entry;
        entry.packet = packet;
        entry.deltaSeconds = Runtime::InputSendInterval;
        runtime.pendingInputs.push_back(entry);
        while (runtime.pendingInputs.size() > Runtime::MaxPendingInputs) {
            runtime.pendingInputs.pop_front();
        }

        size_t resentCopies = 0;
        for (
            auto pendingIt = runtime.pendingInputs.rbegin();
            pendingIt != runtime.pendingInputs.rend() &&
            resentCopies < Runtime::InputRedundancyCopies;
            ++pendingIt
        ) {
            const PlayerInput& resendPacket = pendingIt->packet;
            if (resendPacket.sequenceNumber == packet.sequenceNumber) {
                continue;
            }
            if (IsAckedU32(resendPacket.sequenceNumber, runtime.lastAckedInputSeq)) {
                continue;
            }
            if (!runtime.clientNet.SendPlayerInput(resendPacket)) {
                break;
            }
            ++resentCopies;
        }

        ++inputSendsThisFrame;
    }
    if (now - runtime.lastInputSendTime >= Runtime::InputSendInterval) {
        // Avoid unbounded backlog after long hitches.
        runtime.lastInputSendTime = now;
    }

    const glm::vec3 requestPos = runtime.player->getPosition();
    const glm::ivec3 worldPos(
        static_cast<int>(std::floor(requestPos.x)),
        static_cast<int>(std::floor(requestPos.y)),
        static_cast<int>(std::floor(requestPos.z))
    );
    const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
    const uint16_t viewDistance = static_cast<uint16_t>(std::max<int>(2, runtime.player->renderDistance));
    const bool centerChanged =
        !runtime.hasLastChunkRequestCenter ||
        centerChunk.x != runtime.lastChunkRequestCenter.x ||
        centerChunk.y != runtime.lastChunkRequestCenter.y ||
        centerChunk.z != runtime.lastChunkRequestCenter.z;
    const bool allowBurstRequest =
        !runtime.hasLastChunkRequestCenter ||
        (now - runtime.lastChunkRequestSendTime >= Runtime::ChunkRequestCenterChangeMinInterval);

    if (centerChanged && allowBurstRequest) {
        runtime.lastChunkRequestSendTime = now;
        runtime.lastChunkRequestCenter = centerChunk;
        runtime.hasLastChunkRequestCenter = true;
        (void)runtime.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
    else if (now - runtime.lastChunkRequestSendTime >= Runtime::ChunkRequestSendInterval) {
        runtime.lastChunkRequestSendTime = now;
        runtime.lastChunkRequestCenter = centerChunk;
        runtime.hasLastChunkRequestCenter = true;
        (void)runtime.clientNet.SendChunkRequest(centerChunk, viewDistance);
    }
}

void App::processChunkStreaming(Runtime& runtime, bool prioritizeMovement) {
    const uint16_t resyncViewDistance = 2;
    constexpr double kChunkResyncCooldownSec = 0.25;
    static std::unordered_map<glm::ivec3, double, IVec3Hash> s_chunkResyncCooldownUntil;

    const auto requestChunkResync = [&](const glm::ivec3& chunkPos) {
        const double nowSec = glfwGetTime();
        auto it = s_chunkResyncCooldownUntil.find(chunkPos);
        if (it != s_chunkResyncCooldownUntil.end() && nowSec < it->second) {
            return;
        }
        s_chunkResyncCooldownUntil[chunkPos] = nowSec + kChunkResyncCooldownSec;
        if (!runtime.clientNet.SendChunkRequest(chunkPos, resyncViewDistance)) {
            std::cerr
                << "[chunk/resync] failed to request full chunk ("
                << chunkPos.x << "," << chunkPos.y << "," << chunkPos.z << ")\n";
        }
    };

    const int64_t chunkApplyBudgetUs = prioritizeMovement
        ? Runtime::ChunkApplyBudgetUsUnderInputPressure
        : Runtime::ChunkApplyBudgetUs;
    const auto chunkApplyStart = std::chrono::steady_clock::now();
    const auto withinChunkApplyBudget = [&]() -> bool {
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - chunkApplyStart
        ).count();
        return elapsedUs < chunkApplyBudgetUs;
    };

    ChunkData chunkData;
    size_t chunkDataApplied = 0;
    while (
        chunkDataApplied < Runtime::MaxChunkDataApplyPerFrame &&
        withinChunkApplyBudget() &&
        runtime.clientNet.PopChunkData(chunkData)
    ) {
        const bool accepted = runtime.chunkManager->applyNetworkChunkData(chunkData);
        if (accepted && !runtime.clientNet.SendChunkDataAck(chunkData)) {
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
        const NetworkChunkDeltaApplyResult deltaResult = runtime.chunkManager->applyNetworkChunkDelta(chunkDelta);
        if (
            deltaResult == NetworkChunkDeltaApplyResult::MissingBaseChunk ||
            deltaResult == NetworkChunkDeltaApplyResult::VersionGap
        ) {
            requestChunkResync(glm::ivec3(chunkDelta.chunkX, chunkDelta.chunkY, chunkDelta.chunkZ));
        }
        ++chunkDeltaApplied;
    }

    ChunkUnload chunkUnload;
    size_t chunkUnloadApplied = 0;
    while (
        chunkUnloadApplied < Runtime::MaxChunkUnloadApplyPerFrame &&
        runtime.clientNet.PopChunkUnload(chunkUnload)
    ) {
        runtime.chunkManager->applyNetworkChunkUnload(chunkUnload);
        ++chunkUnloadApplied;
    }

    const size_t maxChunkMeshBuilds = prioritizeMovement
        ? Runtime::MaxChunkMeshBuildsPerFrameUnderInputPressure
        : Runtime::MaxChunkMeshBuildsPerFrame;
    const int64_t chunkMeshBuildBudgetUs = prioritizeMovement
        ? Runtime::ChunkMeshBuildBudgetUsUnderInputPressure
        : Runtime::ChunkMeshBuildBudgetUs;
    runtime.chunkManager->updateDirtyChunks(maxChunkMeshBuilds, chunkMeshBuildBudgetUs);

    const double now = glfwGetTime();
    if (kEnableChunkDiagnostics && now - runtime.lastChunkCoverageLogTime >= 1.0) {
        runtime.lastChunkCoverageLogTime = now;
        const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();

        const glm::vec3 pos = runtime.player->getPosition();
        const glm::ivec3 worldPos(
            static_cast<int>(std::floor(pos.x)),
            static_cast<int>(std::floor(pos.y)),
            static_cast<int>(std::floor(pos.z))
        );
        const glm::ivec3 centerChunk = runtime.chunkManager->worldToChunkPos(worldPos);
        const int viewDistance = std::max<int>(2, runtime.player->renderDistance);
        const int64_t radius2 = static_cast<int64_t>(viewDistance) * static_cast<int64_t>(viewDistance);
        const int minChunkY = WORLD_MIN_Y / CHUNK_SIZE;
        const int maxChunkY = WORLD_MAX_Y / CHUNK_SIZE;

        const auto& chunks = runtime.chunkManager->getChunks();
        size_t desired = 0;
        size_t loaded = 0;
        std::vector<glm::ivec3> missingSamples;
        missingSamples.reserve(8);
        for (int x = centerChunk.x - viewDistance; x <= centerChunk.x + viewDistance; ++x) {
            const int64_t dx = static_cast<int64_t>(x - centerChunk.x);
            const int64_t dx2 = dx * dx;
            for (int z = centerChunk.z - viewDistance; z <= centerChunk.z + viewDistance; ++z) {
                const int64_t dz = static_cast<int64_t>(z - centerChunk.z);
                if (dx2 + dz * dz > radius2) {
                    continue;
                }
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
            << " missing=" << (desired - loaded)
            << " queue(data/delta/unload)=("
            << queueDepths.chunkData << "/"
            << queueDepths.chunkDelta << "/"
            << queueDepths.chunkUnload << ")"
            << " applied(data/delta/unload)=("
            << chunkDataApplied << "/"
            << chunkDeltaApplied << "/"
            << chunkUnloadApplied << ")\n";

        if (!missingSamples.empty()) {
            std::cerr << "[chunk/client] missing samples:";
            for (const glm::ivec3& cp : missingSamples) {
                std::cerr << " (" << cp.x << "," << cp.y << "," << cp.z << ")";
            }
            std::cerr << "\n";
        }
    }
}

void App::processFrame(Runtime& runtime) {
    const double frameNow = glfwGetTime();
    double frameDeltaSeconds = frameNow - GameData::lastFrame;
    if (!std::isfinite(frameDeltaSeconds) || frameDeltaSeconds < 0.0) {
        frameDeltaSeconds = 0.0;
    }
    // Clamp hitches so interpolation and prediction do not overreact to one bad frame.
    if (frameDeltaSeconds > 0.1) {
        frameDeltaSeconds = 0.1;
    }
    GameData::deltaTime = frameDeltaSeconds;
    GameData::lastFrame = frameNow;

    if (runtime.debugUi) {
        runtime.debugUi->beginFrame();
    }
    updateDebugCamera(runtime);
    updateToggleStates(runtime);

    if (!runtime.clientNet.IsConnected()) {
        GameData::cursorEnabled = true;
    }

    runtime.inputCallbacks->processInput(m_Window);
    applyMouseInputModes();
    runtime.localSimAccumulator += GameData::deltaTime;

    auto applyBasicAuthoritativeCorrection = [&](double dtSeconds) {
        if (dtSeconds <= 0.0) {
            return;
        }
        const float corrLenSq = glm::dot(
            runtime.pendingAuthoritativeCorrection,
            runtime.pendingAuthoritativeCorrection
        );
        if (corrLenSq <= 1e-10f) {
            runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
            return;
        }

        Player::SimulationState state = runtime.player->captureSimulationState();
        const float correctionSpeed = state.onGround
            ? Runtime::BasicAuthCorrectionSpeedGround
            : Runtime::BasicAuthCorrectionSpeedAir;
        const float maxStep = correctionSpeed * static_cast<float>(dtSeconds);
        if (maxStep <= 0.0f) {
            return;
        }

        const float corrLen = std::sqrt(corrLenSq);
        glm::vec3 step = runtime.pendingAuthoritativeCorrection;
        if (corrLen > maxStep) {
            step *= (maxStep / corrLen);
        }
        if (!state.onGround) {
            step.y *= Runtime::BasicAuthAirYApplyScale;
        }
        else if (step.y > 0.0f && runtime.player->getStepUpVisualOffset() > 0.01f) {
            // During visual step transitions, strongly damp upward authority pulls.
            step.y *= Runtime::BasicAuthStepTransitionYApplyScale;
        }

        state.position += step;
        runtime.player->restoreSimulationState(state);
        runtime.pendingAuthoritativeCorrection -= step;

        const float remainingLenSq = glm::dot(
            runtime.pendingAuthoritativeCorrection,
            runtime.pendingAuthoritativeCorrection
        );
        if (remainingLenSq <= 1e-10f) {
            runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
        }
    };

    const double maxAccumulatedTime = Runtime::LocalPredictionStep * static_cast<double>(Runtime::MaxLocalPredictionStepsPerFrame);
    if (runtime.localSimAccumulator > maxAccumulatedTime) {
        runtime.localSimAccumulator = maxAccumulatedTime;
    }

    size_t localPredictionSteps = 0;
    while (
        runtime.localSimAccumulator >= Runtime::LocalPredictionStep &&
        localPredictionSteps < Runtime::MaxLocalPredictionStepsPerFrame
    ) {
        applyBasicAuthoritativeCorrection(Runtime::LocalPredictionStep);
        runtime.player->update(m_Window, Runtime::LocalPredictionStep);
        runtime.localSimAccumulator -= Runtime::LocalPredictionStep;
        ++localPredictionSteps;
    }
    if (localPredictionSteps == 0) {
        applyBasicAuthoritativeCorrection(GameData::deltaTime);
        // Keep input sampling responsive even on very high FPS frames.
        runtime.player->update(m_Window, 0.0);
    }
    if (
        localPredictionSteps == Runtime::MaxLocalPredictionStepsPerFrame &&
        runtime.localSimAccumulator >= Runtime::LocalPredictionStep
    ) {
        runtime.localSimAccumulator = std::fmod(runtime.localSimAccumulator, Runtime::LocalPredictionStep);
    }

    processWorldInteraction(runtime);
    processMovementNetworking(runtime);
    runtime.player->updateRemotePlayers(static_cast<float>(GameData::deltaTime));
    processShooting(runtime);

    runtime.interpolatedPlayerCamera = runtime.player->getCamera();

    const Camera& activeCamera = m_UseDebugCamera
        ? runtime.debugCamera
        : runtime.interpolatedPlayerCamera;

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
    if (!m_UseDebugCamera) {
        renderHeldGun(runtime, runtime.interpolatedPlayerCamera);
    }

    if (runtime.debugUi) {
        runtime.debugUi->drawCrosshair(!GameData::cursorEnabled);
        if (runtime.debugUi->isVisible()) {
            const ClientNetwork::ChunkQueueDepths queueDepths = runtime.clientNet.GetChunkQueueDepths();
            UiFrameData frameData;
            frameData.fps = (GameData::deltaTime > 1e-6) ? static_cast<float>(1.0 / GameData::deltaTime) : 0.0f;
            frameData.frameMs = static_cast<float>(GameData::deltaTime * 1000.0);
            frameData.playerPosition = runtime.player->getPosition();
            frameData.playerVelocity = runtime.player->getVelocity();
            frameData.flyMode = runtime.player->flyMode;
            frameData.onGround = runtime.player->isGrounded();
            frameData.renderDistance = runtime.player->renderDistance;
            frameData.remotePlayerCount = runtime.player->connectedPlayers.size();
            frameData.netConnected = runtime.clientNet.IsConnected();
            frameData.netStatus = runtime.clientNet.GetConnectionStatusText();
            frameData.serverTick = runtime.lastAppliedServerTick;
            frameData.ackedInputSeq = runtime.lastAckedInputSeq;
            frameData.pendingInputCount = runtime.pendingInputs.size();
            frameData.chunkDataQueueDepth = queueDepths.chunkData;
            frameData.chunkDeltaQueueDepth = queueDepths.chunkDelta;
            frameData.chunkUnloadQueueDepth = queueDepths.chunkUnload;
            frameData.backendName = runtime.renderer.getActiveBackendName();
            frameData.mdiUsable = runtime.renderer.isMDIUsable();

            UiMutableState mutableState;
            mutableState.useDebugCamera = &m_UseDebugCamera;
            mutableState.toggleWireframe = &m_ToggleWireframe;
            mutableState.toggleChunkBorders = &m_ToggleChunkBorders;
            mutableState.toggleDebugFrustum = &m_ToggleDebugFrustum;
            mutableState.renderDistance = &runtime.player->renderDistance;
            mutableState.cursorEnabled = &GameData::cursorEnabled;
            mutableState.rawMouseInputEnabled = &m_EnableRawMouseInput;
            mutableState.rawMouseInputSupported = glfwRawMouseMotionSupported();
            mutableState.gunViewOffset = &runtime.equippedGunViewOffset;
            mutableState.gunViewScale = &runtime.equippedGunViewScale;
            mutableState.gunViewEulerDeg = &runtime.equippedGunViewEulerDeg;

            runtime.debugUi->drawMainWindow(frameData, mutableState);

            if (!runtime.debugUi->isVisible() && m_ShowDebugUi) {
                m_ShowDebugUi = false;
                GameData::cursorEnabled = false;
            }
            applyMouseInputModes();
        }

        drawConnectionPrompt(runtime);
        runtime.debugUi->render();
    }

    updateFPSCounter();
    glfwSwapBuffers(m_Window);
    glfwPollEvents();

    const bool prioritizeMovement = (localPredictionSteps > 1);
    processChunkStreaming(runtime, prioritizeMovement);
}

void App::shutdown(Runtime& runtime) {
    runtime.clientNet.Shutdown();
    if (runtime.debugUi) {
        runtime.debugUi->shutdown();
        runtime.debugUi.reset();
    }

    runtime.sky.shutdown();

    if (m_Window) {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
    glfwTerminate();
}

int App::Run(int argc, char** argv) {
    LaunchOptions options;
    if (!ParseLaunchOptions(argc, argv, options)) {
        PrintUsage();
        return 2;
    }
    if (options.showHelp) {
        PrintUsage();
        return 0;
    }

    m_ServerIp = options.serverIp;
    m_ServerPort = options.serverPort;
    m_RequestedUsername = options.requestedUsername;

    std::cout << "[App] Runtime paths: " << Shared::RuntimePaths::Describe() << "\n";
    std::cout << "[App] Network target: " << m_ServerIp << ":" << m_ServerPort;
    if (!m_RequestedUsername.empty()) {
        std::cout << " | requestedName=" << m_RequestedUsername;
    }
    std::cout << "\n";

    if (!initWindowAndContext()) {
        return -1;
    }

    Runtime runtime;
    initGameplay(runtime);
    initCallbacks(runtime);
    initRenderResources(runtime);
    initUi(runtime);
    initNetworking(runtime);

    const double startTime = glfwGetTime();
    GameData::lastFrame = startTime;
    GameData::fpsTime = startTime;
    GameData::deltaTime = 0.0;

    while (!glfwWindowShouldClose(m_Window)) {
        processFrame(runtime);
    }

    shutdown(runtime);
    return 0;
}
