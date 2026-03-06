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
#include "../../Shared/player/PlayerData.hpp"
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

float LatencyCorrectionBlend(const ClientNetwork& net) {
    const int pingMs = net.GetPingMs();
    if (pingMs <= 35) {
        return 0.0f;
    }
    // Blend from 0..1 over roughly 35ms -> 155ms.
    return std::clamp((static_cast<float>(pingMs) - 35.0f) / 120.0f, 0.0f, 1.0f);
}

constexpr bool kEnableChunkDiagnostics = false;
constexpr bool kDefaultPlayerModelYawInvert = true;
constexpr float kDefaultPlayerModelYawOffsetDeg = 0.0f;
constexpr float kRemoteGunOwnerYawCorrectionDeg = -90.0f;
constexpr glm::vec3 kRemoteGunRightHandAnchorOffset(0.5f, 1.24f, 0.3f);
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
    glm::vec3 worldOffset = glm::vec3(0.25f, 1.30f, 0.10f);
    glm::vec3 worldScale = glm::vec3(0.10f);
    glm::vec3 worldEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f);
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
        .viewEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f),
        .worldOffset = glm::vec3(0.03f, -0.04f, 0.11f),
        .worldScale = glm::vec3(0.0012f),
        .worldEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f)
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
        .viewEulerDeg = glm::vec3(-2.0f, 90.0f, 0.0f),
        .worldOffset = glm::vec3(0.04f, -0.06f, 0.14f),
        .worldScale = glm::vec3(0.25f),
        .worldEulerDeg = glm::vec3(-2.0f, 90.0f, 0.0f)
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

const GunDefinition* FindGunDefinitionByWeaponId(uint16_t weaponId) {
    for (const GunDefinition& definition : kGunDefinitions) {
        if (ToWeaponId(definition.type) == weaponId) {
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
        << "  --server-ip <host>   (default: 127.0.0.1)\n"
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

bool ParseHost(std::string_view text, std::string& outHost) {
    const std::string host = TrimAscii(text);
    if (host.empty()) {
        return false;
    }
    for (char c : host) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            return false;
        }
    }
    outHost = host;
    return true;
}

bool ParseServerEndpoint(std::string_view text, std::string& outIp, uint16_t& outPort) {
    const std::string endpoint = TrimAscii(text);
    if (endpoint.empty()) {
        return false;
    }

    std::string_view hostPart;
    std::string_view portPart;
    if (endpoint.front() == '[') {
        const size_t bracketClose = endpoint.find(']');
        if (bracketClose == std::string::npos || bracketClose <= 1) {
            return false;
        }
        if (bracketClose + 1 >= endpoint.size() || endpoint[bracketClose + 1] != ':') {
            return false;
        }
        hostPart = std::string_view(endpoint.data() + 1, bracketClose - 1);
        portPart = std::string_view(
            endpoint.data() + bracketClose + 2,
            endpoint.size() - bracketClose - 2
        );
    } else {
        const size_t colonPos = endpoint.rfind(':');
        if (colonPos == std::string::npos) {
            return false;
        }
        hostPart = std::string_view(endpoint.data(), colonPos);
        portPart = std::string_view(endpoint.data() + colonPos + 1, endpoint.size() - colonPos - 1);
        if (hostPart.find(':') != std::string_view::npos) {
            return false;
        }
    }

    std::string parsedIp;
    uint16_t parsedPort = 0;
    if (!ParseHost(hostPart, parsedIp) || !ParsePort(portPart, parsedPort)) {
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
    struct KillFeedEntry {
        std::string killer;
        std::string victim;
        uint16_t weaponId = 0;
        double expiresAt = 0.0;
    };
    std::deque<PendingInputEntry> pendingInputs;
    static constexpr size_t MaxPendingInputs = 256;
    std::deque<KillFeedEntry> killFeedEntries;
    static constexpr size_t MaxKillFeedEntries = 8;
    static constexpr double KillFeedDurationSec = 5.0;
    int matchRemainingSeconds = 600;
    bool matchStarted = false;
    bool matchEnded = false;
    std::string matchWinner;
    std::vector<ClientNetwork::ScoreboardEntry> scoreboardEntries;
    bool localPlayerAlive = true;
    float localRespawnSeconds = 0.0f;
    std::string localDeathKiller;
    bool wasRespawnClickDown = false;
    double lastInputSendTime = 0.0;
    double lastChunkRequestSendTime = 0.0;
    double lastShootSendTime = 0.0;
    double nextReconnectAttemptTime = 0.0;
    double reconnectBackoffSeconds = 1.0;
    std::string lastConnectionStatus = "disconnected";
    std::array<char, 128> pendingServerEndpointInput{};
    std::array<char, kMaxConnectUsernameChars + 1> pendingUsernameInput{};
    bool wasEndpointPasteShortcutPressed = false;
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
    static constexpr float RenderLeadMaxDistance = 0.40f;
    static constexpr float RenderExtrapolationBlend = 0.60f;
    static constexpr float RenderCameraSmoothingGroundHz = 26.0f;
    static constexpr float RenderCameraSmoothingAirHz = 16.0f;
    static constexpr size_t InputRedundancyCopies = 2; // resend latest unacked states to mask packet loss
    static constexpr double ChunkRequestSendInterval = 0.5; // 2 Hz baseline + immediate on center changes
    static constexpr double ChunkRequestCenterChangeMinInterval = 1.0 / 30.0; // up to 30 Hz on border crossings
    static constexpr size_t MaxChunkDataApplyPerFrame = 12;
    static constexpr size_t MaxChunkDeltaApplyPerFrame = 48;
    static constexpr size_t MaxChunkUnloadApplyPerFrame = 64;
    static constexpr int64_t ChunkApplyBudgetUs = 9000;
    static constexpr int64_t ChunkApplyBudgetUsUnderInputPressure = 2500;
    static constexpr size_t MaxChunkMeshBuildsPerFrame = 8;
    static constexpr size_t MaxChunkMeshBuildsPerFrameUnderInputPressure = 3;
    static constexpr int64_t ChunkMeshBuildBudgetUs = 6000;
    static constexpr int64_t ChunkMeshBuildBudgetUsUnderInputPressure = 2000;
    double lastChunkCoverageLogTime = 0.0;
    glm::ivec3 lastChunkRequestCenter{ 0 };
    bool hasLastChunkRequestCenter = false;
    glm::vec3 pendingAuthoritativeCorrection{ 0.0f };
    Player::SimulationState renderPrevSimState{};
    Player::SimulationState renderCurrSimState{};
    bool hasRenderSimState = false;
    glm::vec3 smoothedPlayerCameraPos{ 0.0f };
    bool hasSmoothedPlayerCameraPos = false;

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

    if (runtime.clientNet.IsConnected() && runtime.player) {
        const NetworkInputState& input = runtime.player->getNetworkInputState();
        PlayerInput packet;
        packet.sequenceNumber = runtime.netSeq++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = weaponId;
        packet.yaw = input.yaw;
        packet.pitch = input.pitch;
        packet.moveX = input.moveX;
        packet.moveZ = input.moveZ;
        if (runtime.clientNet.SendPlayerInput(packet)) {
            Runtime::PendingInputEntry entry;
            entry.packet = packet;
            entry.deltaSeconds = Runtime::InputSendInterval;
            runtime.pendingInputs.push_back(entry);
            while (runtime.pendingInputs.size() > Runtime::MaxPendingInputs) {
                runtime.pendingInputs.pop_front();
            }
            runtime.lastInputSendTime = glfwGetTime();
        }
    }

    std::cout
        << "[gun] equipped " << definition->displayName
        << " (weaponId=" << weaponId << ")"
        << " [preloaded]"
        << "\n";
    return true;
}

void App::renderRemotePlayerGuns(Runtime& runtime, const Camera& activeCamera) {
    if (!runtime.gunShader || runtime.preloadedGuns.empty() || runtime.player->connectedPlayers.empty()) {
        return;
    }

    const float aspect = static_cast<float>(GameData::screenWidth) / static_cast<float>(GameData::screenHeight);
    if (!std::isfinite(aspect) || aspect <= 0.0f) {
        return;
    }

    const glm::mat4 projection = glm::perspective(glm::radians(GameData::FOV), aspect, 0.1f, 100000.0f);
    const glm::mat4 view = activeCamera.getViewMatrix();

    runtime.gunShader->use();
    runtime.gunShader->setInt("diffuseTexture", 0);
    runtime.gunShader->setVec3("lightDir", glm::normalize(runtime.sky.getSunDir()));
    runtime.gunShader->setVec3("lightColor", glm::vec3(1.0f, 0.98f, 0.96f));
    runtime.gunShader->setVec3("ambientColor", glm::vec3(0.36f, 0.40f, 0.46f));
    runtime.gunShader->setMat4("view", view);
    runtime.gunShader->setMat4("projection", projection);

    for (const auto& [_, remoteState] : runtime.player->connectedPlayers) {
        const uint16_t weaponId = remoteState.weaponId;
        auto gunIt = runtime.preloadedGuns.find(weaponId);
        if (gunIt == runtime.preloadedGuns.end() || !gunIt->second) {
            continue;
        }

        const GunDefinition* definition = FindGunDefinitionByWeaponId(weaponId);
        if (definition == nullptr) {
            continue;
        }

        const glm::vec3 handAnchorPos =
            remoteState.position + (remoteState.rotation * kRemoteGunRightHandAnchorOffset);
        const glm::vec3 worldOffset = definition->worldOffset * remoteState.scale;
        const glm::vec3 gunPos = handAnchorPos + (remoteState.rotation * worldOffset);

        const glm::quat yawOffset = glm::angleAxis(
            glm::radians(definition->worldEulerDeg.y),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        const glm::quat ownerYawCorrection = glm::angleAxis(
            glm::radians(kRemoteGunOwnerYawCorrectionDeg),
            glm::vec3(0.0f, 1.0f, 0.0f)
        );
        const glm::quat pitchOffset = glm::angleAxis(
            glm::radians(definition->worldEulerDeg.x),
            glm::vec3(1.0f, 0.0f, 0.0f)
        );
        const glm::quat rollOffset = glm::angleAxis(
            glm::radians(definition->worldEulerDeg.z),
            glm::vec3(0.0f, 0.0f, 1.0f)
        );
        const glm::quat gunRot = glm::normalize(
            remoteState.rotation * ownerYawCorrection * yawOffset * pitchOffset * rollOffset
        );
        const glm::vec3 gunScale = definition->worldScale * remoteState.scale;

        gunIt->second->render(gunPos, gunRot, gunScale, *runtime.gunShader);
    }
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
    if (!runtime.clientNet.ConnectTo(m_ServerIp, m_ServerPort)) {
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

    ImGui::TextUnformatted("Server (host:port)");
    const float pasteButtonWidth = ImGui::CalcTextSize("Paste").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
    const float endpointFieldWidth =
        ImGui::GetContentRegionAvail().x - pasteButtonWidth - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(endpointFieldWidth > 60.0f ? endpointFieldWidth : -1.0f);
    const ClientNetwork::ConnectionState connState = runtime.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);
    const auto pasteEndpointFromClipboard = [&]() -> bool {
        if (m_Window == nullptr) {
            return false;
        }
        const char* clipboardText = glfwGetClipboardString(m_Window);
        if (clipboardText == nullptr || clipboardText[0] == '\0') {
            return false;
        }
        const std::string endpoint = TrimAscii(clipboardText);
        if (endpoint.empty()) {
            return false;
        }
        std::memset(runtime.pendingServerEndpointInput.data(), 0, runtime.pendingServerEndpointInput.size());
        const size_t copyLen = std::min(endpoint.size(), runtime.pendingServerEndpointInput.size() - 1);
        std::memcpy(runtime.pendingServerEndpointInput.data(), endpoint.data(), copyLen);
        return true;
    };

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
    const bool endpointFieldActive = ImGui::IsItemActive();
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    bool pasteShortcutPressed = false;
    if (endpointFieldActive && !isConnecting) {
        const bool ctrlDown =
            (glfwGetKey(m_Window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) ||
            (glfwGetKey(m_Window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS) ||
            (glfwGetKey(m_Window, GLFW_KEY_LEFT_SUPER) == GLFW_PRESS) ||
            (glfwGetKey(m_Window, GLFW_KEY_RIGHT_SUPER) == GLFW_PRESS);
        const bool shiftDown =
            (glfwGetKey(m_Window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) ||
            (glfwGetKey(m_Window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        const bool pasteCtrlV = ctrlDown && (glfwGetKey(m_Window, GLFW_KEY_V) == GLFW_PRESS);
        const bool pasteShiftInsert = shiftDown && (glfwGetKey(m_Window, GLFW_KEY_INSERT) == GLFW_PRESS);
        pasteShortcutPressed = pasteCtrlV || pasteShiftInsert;
        if (pasteShortcutPressed && !runtime.wasEndpointPasteShortcutPressed) {
            if (!pasteEndpointFromClipboard()) {
                runtime.usernamePromptError = "Clipboard is empty.";
            }
        }
    }
    runtime.wasEndpointPasteShortcutPressed = pasteShortcutPressed;

    ImGui::SameLine();
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Paste")) {
        if (!pasteEndpointFromClipboard()) {
            runtime.usernamePromptError = "Clipboard is empty.";
        }
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
            runtime.usernamePromptError = "Server must be host:port (example: 127.0.0.1:27015).";
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
            const size_t endpointCopyLen = std::min(endpoint.size(), runtime.pendingServerEndpointInput.size() - 1);
            std::memcpy(runtime.pendingServerEndpointInput.data(), endpoint.data(), endpointCopyLen);

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

void App::drawKillFeed(Runtime& runtime) {
    if (ImGui::GetCurrentContext() == nullptr || runtime.killFeedEntries.empty()) {
        return;
    }

    const double now = glfwGetTime();
    while (!runtime.killFeedEntries.empty() && runtime.killFeedEntries.back().expiresAt <= now) {
        runtime.killFeedEntries.pop_back();
    }
    if (runtime.killFeedEntries.empty()) {
        return;
    }

    const std::string localName = runtime.clientNet.GetAssignedUsername();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    float y = 24.0f;

    for (const Runtime::KillFeedEntry& entry : runtime.killFeedEntries) {
        const std::string line =
            entry.killer + " [" +
            std::string(GunTypeName(static_cast<GunType>(entry.weaponId))) +
            "] " + entry.victim;
        const ImVec2 textSize = ImGui::CalcTextSize(line.c_str());
        const float x = io.DisplaySize.x - textSize.x - 24.0f;

        ImU32 textColor = IM_COL32(232, 232, 232, 255);
        if (!localName.empty() && entry.killer == localName) {
            textColor = IM_COL32(130, 255, 160, 255);
        }
        else if (!localName.empty() && entry.victim == localName) {
            textColor = IM_COL32(255, 120, 120, 255);
        }

        const ImVec2 bgMin(x - 8.0f, y - 3.0f);
        const ImVec2 bgMax(x + textSize.x + 8.0f, y + textSize.y + 3.0f);
        drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 125), 4.0f);
        drawList->AddText(ImVec2(x, y), textColor, line.c_str());
        y += textSize.y + 8.0f;
    }
}

void App::drawScoreboard(Runtime& runtime) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const bool showScoreboard =
        (glfwGetKey(m_Window, GLFW_KEY_TAB) == GLFW_PRESS) || runtime.matchEnded;
    if (!showScoreboard) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const float panelWidth = 560.0f;
    const float rowHeight = ImGui::GetTextLineHeight() + 8.0f;
    const float headerHeight = 66.0f;
    const float tableHeaderHeight = rowHeight;
    const float panelHeight =
        headerHeight + tableHeaderHeight + rowHeight * static_cast<float>(runtime.scoreboardEntries.size()) + 14.0f;
    const float x = (io.DisplaySize.x - panelWidth) * 0.5f;
    const float y = 72.0f;

    drawList->AddRectFilled(
        ImVec2(x, y),
        ImVec2(x + panelWidth, y + panelHeight),
        IM_COL32(10, 10, 10, 215),
        8.0f
    );

    const int clampedRemaining = std::max(0, runtime.matchRemainingSeconds);
    const int minutes = clampedRemaining / 60;
    const int seconds = clampedRemaining % 60;
    char timerLine[64]{};
    if (!runtime.matchStarted) {
        std::snprintf(timerLine, sizeof(timerLine), "Waiting for players");
    }
    else {
        std::snprintf(timerLine, sizeof(timerLine), "Time Left: %02d:%02d", minutes, seconds);
    }

    std::string title = "Deathmatch";
    if (runtime.matchEnded) {
        title = "Match Ended";
        if (!runtime.matchWinner.empty()) {
            title += " - Winner: ";
            title += runtime.matchWinner;
        }
    }

    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    drawList->AddText(
        ImVec2(x + (panelWidth - titleSize.x) * 0.5f, y + 12.0f),
        IM_COL32(245, 245, 245, 255),
        title.c_str()
    );
    const ImVec2 timerSize = ImGui::CalcTextSize(timerLine);
    drawList->AddText(
        ImVec2(x + (panelWidth - timerSize.x) * 0.5f, y + 34.0f),
        IM_COL32(210, 210, 210, 255),
        timerLine
    );

    const float tableY = y + headerHeight;
    drawList->AddRectFilled(
        ImVec2(x + 8.0f, tableY),
        ImVec2(x + panelWidth - 8.0f, tableY + tableHeaderHeight),
        IM_COL32(32, 32, 32, 220),
        4.0f
    );

    const float nameX = x + 24.0f;
    const float killsX = x + 360.0f;
    const float deathsX = x + 430.0f;
    const float pingX = x + 495.0f;
    drawList->AddText(ImVec2(nameX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "Player");
    drawList->AddText(ImVec2(killsX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "K");
    drawList->AddText(ImVec2(deathsX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "D");
    drawList->AddText(ImVec2(pingX, tableY + 4.0f), IM_COL32(220, 220, 220, 255), "Ping");

    const std::string localName = runtime.clientNet.GetAssignedUsername();
    float rowY = tableY + tableHeaderHeight;
    for (size_t i = 0; i < runtime.scoreboardEntries.size(); ++i) {
        const ClientNetwork::ScoreboardEntry& entry = runtime.scoreboardEntries[i];
        const bool oddRow = ((i % 2) != 0);
        if (oddRow) {
            drawList->AddRectFilled(
                ImVec2(x + 8.0f, rowY),
                ImVec2(x + panelWidth - 8.0f, rowY + rowHeight),
                IM_COL32(20, 20, 20, 145),
                0.0f
            );
        }

        ImU32 nameColor = IM_COL32(230, 230, 230, 255);
        if (!localName.empty() && entry.username == localName) {
            nameColor = IM_COL32(130, 255, 160, 255);
        }
        drawList->AddText(ImVec2(nameX, rowY + 4.0f), nameColor, entry.username.c_str());
        drawList->AddText(
            ImVec2(killsX, rowY + 4.0f),
            IM_COL32(230, 230, 230, 255),
            std::to_string(entry.kills).c_str()
        );
        drawList->AddText(
            ImVec2(deathsX, rowY + 4.0f),
            IM_COL32(230, 230, 230, 255),
            std::to_string(entry.deaths).c_str()
        );
        const std::string pingText = (entry.pingMs >= 0) ? std::to_string(entry.pingMs) : std::string("--");
        drawList->AddText(
            ImVec2(pingX, rowY + 4.0f),
            IM_COL32(230, 230, 230, 255),
            pingText.c_str()
        );

        rowY += rowHeight;
    }
}

void App::drawPingCounter(Runtime& runtime) {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    const int pingMs = runtime.clientNet.GetPingMs();
    const std::string line = (pingMs >= 0)
        ? ("Ping: " + std::to_string(pingMs) + " ms")
        : "Ping: --";

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const float x = 24.0f;
    const float y = 24.0f;
    const ImVec2 textSize = ImGui::CalcTextSize(line.c_str());

    ImU32 textColor = IM_COL32(232, 232, 232, 255);
    if (pingMs >= 150) {
        textColor = IM_COL32(255, 120, 120, 255);
    }
    else if (pingMs >= 80) {
        textColor = IM_COL32(255, 220, 120, 255);
    }

    const ImVec2 bgMin(x - 8.0f, y - 3.0f);
    const ImVec2 bgMax(x + textSize.x + 8.0f, y + textSize.y + 3.0f);
    drawList->AddRectFilled(bgMin, bgMax, IM_COL32(0, 0, 0, 125), 4.0f);
    drawList->AddText(ImVec2(x, y), textColor, line.c_str());
}

void App::drawDeathOverlay(Runtime& runtime) {
    if (ImGui::GetCurrentContext() == nullptr || runtime.localPlayerAlive) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 displaySize = io.DisplaySize;
    const ImVec2 center(displaySize.x * 0.5f, displaySize.y * 0.5f);

    drawList->AddRectFilled(
        ImVec2(0.0f, 0.0f),
        displaySize,
        IM_COL32(0, 0, 0, 120)
    );

    std::string title = "You were killed";
    if (!runtime.localDeathKiller.empty()) {
        title += " by [";
        title += runtime.localDeathKiller;
        title += "]";
    }
    char timerLine[64]{};
    const float secondsRemaining = std::max(0.0f, runtime.localRespawnSeconds);
    if (secondsRemaining > 0.05f) {
        std::snprintf(timerLine, sizeof(timerLine), "Respawning in %.1fs", secondsRemaining);
    }
    else {
        std::snprintf(timerLine, sizeof(timerLine), "Click to respawn");
    }

    const ImVec2 titleSize = ImGui::CalcTextSize(title.c_str());
    const ImVec2 timerSize = ImGui::CalcTextSize(timerLine);
    const float blockWidth = std::max(titleSize.x, timerSize.x);

    const ImVec2 bgMin(center.x - blockWidth * 0.5f - 24.0f, center.y - 42.0f);
    const ImVec2 bgMax(center.x + blockWidth * 0.5f + 24.0f, center.y + 34.0f);
    drawList->AddRectFilled(bgMin, bgMax, IM_COL32(12, 12, 12, 210), 8.0f);

    drawList->AddText(
        ImVec2(center.x - titleSize.x * 0.5f, center.y - 24.0f),
        IM_COL32(255, 210, 210, 255),
        title.c_str()
    );
    drawList->AddText(
        ImVec2(center.x - timerSize.x * 0.5f, center.y + 2.0f),
        IM_COL32(235, 235, 235, 255),
        timerLine
    );
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
    if (!runtime.localPlayerAlive || GameData::cursorEnabled || IsImGuiTextInputActive()) {
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
    if (!runtime.localPlayerAlive) {
        return;
    }

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

    ClientNetwork::KillFeedEvent killEvent{};
    while (runtime.clientNet.PopKillFeedEvent(killEvent)) {
        const std::string localName = runtime.clientNet.GetAssignedUsername();
        if (!localName.empty() && killEvent.victim == localName) {
            runtime.localDeathKiller = killEvent.killer;
        }

        Runtime::KillFeedEntry entry;
        entry.killer = std::move(killEvent.killer);
        entry.victim = std::move(killEvent.victim);
        entry.weaponId = killEvent.weaponId;
        entry.expiresAt = now + Runtime::KillFeedDurationSec;
        runtime.killFeedEntries.push_front(std::move(entry));
        while (runtime.killFeedEntries.size() > Runtime::MaxKillFeedEntries) {
            runtime.killFeedEntries.pop_back();
        }
    }

    ClientNetwork::ScoreboardSnapshot scoreboardSnapshot{};
    while (runtime.clientNet.PopScoreboardSnapshot(scoreboardSnapshot)) {
        runtime.matchRemainingSeconds = std::max(0, scoreboardSnapshot.remainingSeconds);
        runtime.matchStarted = scoreboardSnapshot.matchStarted;
        runtime.matchEnded = scoreboardSnapshot.matchEnded;
        runtime.matchWinner = std::move(scoreboardSnapshot.winner);
        runtime.scoreboardEntries = std::move(scoreboardSnapshot.entries);
    }

    bool hasNewestSelfSnapshot = false;
    uint32_t newestServerTick = 0;
    uint32_t newestAckedInputSeq = 0;
    glm::vec3 newestServerPos(0.0f);
    glm::vec3 newestServerVel(0.0f);
    bool newestServerOnGround = false;
    bool newestServerFlyMode = false;
    bool newestServerAllowFlyMode = false;
    bool newestServerAlive = true;
    float newestRespawnSeconds = 0.0f;
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
        newestServerAlive = (localSnapshot->isAlive != 0);
        newestRespawnSeconds = std::max(0.0f, localSnapshot->respawnSeconds);

        newestRemoteSelfPlayerId = snapshotFrame.selfPlayerId;
        newestRemotePlayerSnapshots = std::move(snapshotFrame.players);
        hasNewestRemotePlayers = true;
    }

    if (hasNewestRemotePlayers) {
        std::unordered_map<PlayerID, PlayerState> newestRemotePlayers;
        newestRemotePlayers.reserve(newestRemotePlayerSnapshots.size());
        for (const PlayerSnapshot& snapshot : newestRemotePlayerSnapshots) {
            if (snapshot.id == newestRemoteSelfPlayerId || snapshot.isAlive == 0) {
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
            remoteState.weaponId = snapshot.weaponId;
            newestRemotePlayers[snapshot.id] = remoteState;
        }
        runtime.player->setConnectedPlayers(newestRemotePlayers);
    }

    if (hasNewestSelfSnapshot &&
        (!runtime.hasAppliedServerTick || IsNewerU32(newestServerTick, runtime.lastAppliedServerTick))) {
        const bool wasAlive = runtime.localPlayerAlive;
        runtime.localPlayerAlive = newestServerAlive;
        runtime.localRespawnSeconds = newestRespawnSeconds;
        if (wasAlive && !runtime.localPlayerAlive) {
            runtime.pendingInputs.clear();
            runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
            runtime.localSimAccumulator = 0.0;
        }
        if (!wasAlive && runtime.localPlayerAlive) {
            runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
            runtime.hasSmoothedPlayerCameraPos = false;
            runtime.localDeathKiller.clear();
        }

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
        const float latencyBlend = LatencyCorrectionBlend(runtime.clientNet);
        const float deadzoneDist = Runtime::BasicAuthReconcileDeadzone + (0.35f * latencyBlend);
        const float softTeleportDist = Runtime::BasicAuthReconcileTeleportDistance + 1.5f + (3.0f * latencyBlend);
        const float hardSnapDist = softTeleportDist + 5.0f + (4.0f * latencyBlend);
        const float maxPendingCorrection = 1.25f + (2.5f * latencyBlend);
        const float deadzoneDistSq = deadzoneDist * deadzoneDist;
        const float softTeleportDistSq = softTeleportDist * softTeleportDist;
        const float hardSnapDistSq = hardSnapDist * hardSnapDist;

        if (correctionLenSq > hardSnapDistSq) {
            runtime.player->restoreSimulationState(reconciledState);
            runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
        }
        else {
            runtime.player->restoreSimulationState(predictedState);
            glm::vec3 queuedCorrection = correction;
            if (correctionLenSq > softTeleportDistSq) {
                // Medium divergence on higher-latency links: bias toward fast catch-up, not teleport.
                queuedCorrection *= 0.65f;
            }
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
            if (queuedLenSq > deadzoneDistSq || correctionLenSq > softTeleportDistSq) {
                runtime.pendingAuthoritativeCorrection += queuedCorrection;
                const float pendingLen = glm::length(runtime.pendingAuthoritativeCorrection);
                if (pendingLen > maxPendingCorrection) {
                    runtime.pendingAuthoritativeCorrection *= maxPendingCorrection / pendingLen;
                }
            }
        }
    }

    if (!runtime.clientNet.IsConnected()) {
        runtime.pendingInputs.clear();
        runtime.killFeedEntries.clear();
        runtime.matchRemainingSeconds = 600;
        runtime.matchStarted = false;
        runtime.matchEnded = false;
        runtime.matchWinner.clear();
        runtime.scoreboardEntries.clear();
        runtime.localPlayerAlive = true;
        runtime.localRespawnSeconds = 0.0f;
        runtime.localDeathKiller.clear();
        runtime.wasRespawnClickDown = false;
        runtime.player->setFlyModeAllowed(false);
        runtime.player->clearConnectedPlayers();
        runtime.hasAppliedServerTick = false;
        runtime.hasReceivedSelfSnapshotTick = false;
        runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
        runtime.hasRenderSimState = false;
        runtime.hasSmoothedPlayerCameraPos = false;
        return;
    }

    const bool respawnClickDown = (glfwGetMouseButton(m_Window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
    if (!runtime.localPlayerAlive && runtime.localRespawnSeconds <= 0.0f) {
        if (respawnClickDown && !runtime.wasRespawnClickDown) {
            (void)runtime.clientNet.SendRespawnRequest();
        }
    }
    runtime.wasRespawnClickDown = respawnClickDown;

    constexpr size_t kMaxInputSendsPerFrame = 4;
    size_t inputSendsThisFrame = 0;
    while (
        now - runtime.lastInputSendTime >= Runtime::InputSendInterval &&
        inputSendsThisFrame < kMaxInputSendsPerFrame
    ) {
        runtime.lastInputSendTime += Runtime::InputSendInterval;

        NetworkInputState input = runtime.player->getNetworkInputState();
        if (!runtime.localPlayerAlive) {
            input.moveX = 0.0f;
            input.moveZ = 0.0f;
            input.flags = 0;
            input.flyMode = false;
        }
        PlayerInput packet;
        packet.sequenceNumber = runtime.netSeq++;
        packet.inputFlags = input.flags;
        packet.flyMode = input.flyMode ? 1 : 0;
        packet.weaponId = runtime.equippedGun ? runtime.equippedGun->getWeaponId() : ToWeaponId(kDefaultGunType);
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
    if (!runtime.hasRenderSimState) {
        const Player::SimulationState initialSimState = runtime.player->captureSimulationState();
        runtime.renderPrevSimState = initialSimState;
        runtime.renderCurrSimState = initialSimState;
        runtime.hasRenderSimState = true;
    }

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
        const float latencyBlend = LatencyCorrectionBlend(runtime.clientNet);
        const float groundCorrectionSpeed =
            Runtime::BasicAuthCorrectionSpeedGround * (1.0f - (0.35f * latencyBlend));
        const float airCorrectionSpeed =
            Runtime::BasicAuthCorrectionSpeedAir * (1.0f - (0.25f * latencyBlend));
        const float correctionSpeed = state.onGround
            ? groundCorrectionSpeed
            : airCorrectionSpeed;
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
    if (runtime.localPlayerAlive) {
        while (
            runtime.localSimAccumulator >= Runtime::LocalPredictionStep &&
            localPredictionSteps < Runtime::MaxLocalPredictionStepsPerFrame
        ) {
            applyBasicAuthoritativeCorrection(Runtime::LocalPredictionStep);
            runtime.player->update(m_Window, Runtime::LocalPredictionStep);
            runtime.localSimAccumulator -= Runtime::LocalPredictionStep;
            runtime.renderPrevSimState = runtime.renderCurrSimState;
            runtime.renderCurrSimState = runtime.player->captureSimulationState();
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
    }
    else {
        runtime.localSimAccumulator = 0.0;
        runtime.pendingAuthoritativeCorrection = glm::vec3(0.0f);
        const Player::SimulationState frozenState = runtime.player->captureSimulationState();
        runtime.renderPrevSimState = frozenState;
        runtime.renderCurrSimState = frozenState;
    }

    processWorldInteraction(runtime);
    processMovementNetworking(runtime);
    runtime.player->updateRemotePlayers(static_cast<float>(GameData::deltaTime));
    processShooting(runtime);

    const Player::SimulationState simStateAfterPrediction = runtime.player->captureSimulationState();
    const glm::vec3 renderStateError = simStateAfterPrediction.position - runtime.renderCurrSimState.position;
    const float renderStateErrorSq = glm::dot(renderStateError, renderStateError);
    const float renderLatencyBlend = LatencyCorrectionBlend(runtime.clientNet);
    const float renderSnapDist = Runtime::BasicAuthReconcileTeleportDistance + 5.5f + (4.0f * renderLatencyBlend);
    const float renderStateSnapDistSq = renderSnapDist * renderSnapDist;
    if (renderStateErrorSq > renderStateSnapDistSq) {
        runtime.renderPrevSimState = simStateAfterPrediction;
        runtime.renderCurrSimState = simStateAfterPrediction;
        runtime.hasSmoothedPlayerCameraPos = false;
    }

    const Camera& latestCamera = runtime.player->getCamera();
    runtime.interpolatedPlayerCamera = latestCamera;

    const float simAlpha = std::clamp(
        static_cast<float>(runtime.localSimAccumulator / Runtime::LocalPredictionStep),
        0.0f,
        1.0f
    );
    const glm::vec3 interpolatedBodyPos = glm::mix(
        runtime.renderPrevSimState.position,
        runtime.renderCurrSimState.position,
        simAlpha
    );
    const glm::vec3 extrapolatedBodyPos =
        runtime.renderCurrSimState.position +
        runtime.renderCurrSimState.velocity * static_cast<float>(runtime.localSimAccumulator);
    glm::vec3 targetBodyPos = glm::mix(
        interpolatedBodyPos,
        extrapolatedBodyPos,
        Runtime::RenderExtrapolationBlend
    );
    const glm::vec3 renderLead = targetBodyPos - runtime.renderCurrSimState.position;
    const float renderLeadLenSq = glm::dot(renderLead, renderLead);
    const float renderLeadMaxSq = Runtime::RenderLeadMaxDistance * Runtime::RenderLeadMaxDistance;
    if (renderLeadLenSq > renderLeadMaxSq && renderLeadLenSq > 1e-8f) {
        const float renderLeadLen = std::sqrt(renderLeadLenSq);
        targetBodyPos = runtime.renderCurrSimState.position +
            renderLead * (Runtime::RenderLeadMaxDistance / renderLeadLen);
    }
    const float interpolatedStepOffset = glm::mix(
        runtime.renderPrevSimState.stepUpVisualOffset,
        runtime.renderCurrSimState.stepUpVisualOffset,
        simAlpha
    );
    const float eyeHeight = Shared::PlayerData::GetMovementSettings().eyeHeight;
    const glm::vec3 targetCameraPos =
        targetBodyPos + glm::vec3(0.0f, eyeHeight - interpolatedStepOffset, 0.0f);
    if (!runtime.hasSmoothedPlayerCameraPos) {
        runtime.smoothedPlayerCameraPos = targetCameraPos;
        runtime.hasSmoothedPlayerCameraPos = true;
    }
    const float smoothingHz = simStateAfterPrediction.onGround
        ? Runtime::RenderCameraSmoothingGroundHz
        : Runtime::RenderCameraSmoothingAirHz;
    const float smoothingAlpha = std::clamp(
        1.0f - std::exp(-smoothingHz * static_cast<float>(GameData::deltaTime)),
        0.0f,
        1.0f
    );
    runtime.smoothedPlayerCameraPos = glm::mix(
        runtime.smoothedPlayerCameraPos,
        targetCameraPos,
        smoothingAlpha
    );
    runtime.interpolatedPlayerCamera.position = runtime.smoothedPlayerCameraPos;

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
    renderRemotePlayerGuns(runtime, activeCamera);
    if (!m_UseDebugCamera && runtime.localPlayerAlive) {
        renderHeldGun(runtime, runtime.interpolatedPlayerCamera);
    }

    if (runtime.debugUi) {
        runtime.debugUi->drawCrosshair(!GameData::cursorEnabled && runtime.localPlayerAlive);
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
        drawScoreboard(runtime);
        drawPingCounter(runtime);
        drawKillFeed(runtime);
        drawDeathOverlay(runtime);
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
