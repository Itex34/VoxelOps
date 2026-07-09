#include "AppHelpers.hpp"

#include "../gun/Gun.hpp"
#include "../network/ClientNetwork.hpp"
#include "../data/GameData.hpp"
#include "../../Shared/runtime/Paths.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iostream>

namespace {

    const std::array<AppHelpers::GunDefinition, 2> kGunDefinitions{
        {{.type = GunType::Pistol,
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
          .worldEulerDeg = glm::vec3(0.0f, 180.0f, 0.0f)},
         {.type = GunType::Sniper,
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
          .worldEulerDeg = glm::vec3(-2.0f, 90.0f, 0.0f)}}
    };

} // namespace

namespace AppHelpers {

    const glm::vec3 kRemoteGunRightHandAnchorOffset(0.5f, 1.24f, 0.3f);

    bool IsImGuiTextInputActive() {
        return GameData::uiWantsTextInput || GameData::uiWantsKeyboardCapture;
    }

    float LatencyCorrectionBlend(const ClientNetwork &net) {
        const int pingMs = net.GetPingMs();
        if (pingMs <= 35) {
            return 0.0f;
        }
        // Blend from 0..1 over roughly 35ms -> 155ms.
        return std::clamp((static_cast<float>(pingMs) - 35.0f) / 120.0f, 0.0f, 1.0f);
    }

    double GetTimeSeconds() noexcept {
        return static_cast<double>(SDL_GetTicksNS()) / 1'000'000'000.0;
    }

    std::span<const GunDefinition> GetGunDefinitions() {
        return std::span<const GunDefinition>(kGunDefinitions.data(), kGunDefinitions.size());
    }

    const GunDefinition *FindGunDefinition(GunType gunType) {
        for (const GunDefinition &definition : kGunDefinitions) {
            if (definition.type == gunType) {
                return &definition;
            }
        }
        return nullptr;
    }

    const GunDefinition *FindGunDefinitionByWeaponId(uint16_t weaponId) {
        for (const GunDefinition &definition : kGunDefinitions) {
            if (ToWeaponId(definition.type) == weaponId) {
                return &definition;
            }
        }
        return nullptr;
    }

    std::string ResolveGunModelPath(const GunDefinition &definition) {
        return Shared::RuntimePaths::ResolveModelsPath(std::string(definition.modelPath))
            .generic_string();
    }

    std::unique_ptr<Gun> BuildGunFromDefinition(const GunDefinition &definition) {
        return std::make_unique<Gun>(
            definition.type,
            definition.fireIntervalSeconds,
            definition.reloadTimeSeconds,
            definition.maxAmmo
        );
    }

    float NormalizeYawDegrees(float yawDegrees) {
        if (!std::isfinite(yawDegrees)) {
            return 0.0f;
        }
        float y = std::fmod(yawDegrees, 360.0f);
        if (y >= 180.0f)
            y -= 360.0f;
        if (y < -180.0f)
            y += 360.0f;
        return y;
    }

    float ToModelYawDegrees(float lookYawDegrees, bool invertYaw, float yawOffsetDeg) {
        const float signedYaw = invertYaw ? -lookYawDegrees : lookYawDegrees;
        return NormalizeYawDegrees(signedYaw + yawOffsetDeg);
    }

    void PrintUsage() {
        std::cout
            << "VoxelOps client options:\n"
            << "  --server-ip <host>   (default: 127.0.0.1)\n"
            << "  --server-port <port> (default: 27015)\n"
            << "  --name <username>    (optional, max 32 chars)\n"
            << "  --bot                auto-connect and drive randomized input\n"
            << "  --bot-duration <sec> exit after N seconds in bot mode (0 = run until stopped)\n"
            << "  --bot-seed <n>       deterministic bot behavior seed\n"
            << "  --bot-shoot-rate <n> average shots per second in bot mode\n"
            << "  --bot-render-distance <n> chunk view distance for bot mode (default: 6)\n"
            << "  --bot-visible        do not minimize the bot client window\n"
            << "  --help\n";
    }

    bool ParsePort(std::string_view text, uint16_t &outPort) {
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
        } catch (...) {
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

    bool ParseHost(std::string_view text, std::string &outHost) {
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

    bool ParseServerEndpoint(std::string_view text, std::string &outIp, uint16_t &outPort) {
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
                endpoint.data() + bracketClose + 2, endpoint.size() - bracketClose - 2
            );
        } else {
            const size_t colonPos = endpoint.rfind(':');
            if (colonPos == std::string::npos) {
                return false;
            }
            hostPart = std::string_view(endpoint.data(), colonPos);
            portPart =
                std::string_view(endpoint.data() + colonPos + 1, endpoint.size() - colonPos - 1);
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

    bool ParseNonNegativeDouble(std::string_view text, double &outValue) {
        if (text.empty()) {
            return false;
        }
        try {
            size_t consumed = 0;
            const double value = std::stod(std::string(text), &consumed);
            if (consumed != text.size() || !std::isfinite(value) || value < 0.0) {
                return false;
            }
            outValue = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ParsePositiveFloat(std::string_view text, float &outValue) {
        if (text.empty()) {
            return false;
        }
        try {
            size_t consumed = 0;
            const float value = std::stof(std::string(text), &consumed);
            if (consumed != text.size() || !std::isfinite(value) || value <= 0.0f) {
                return false;
            }
            outValue = value;
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ParseUint32(std::string_view text, uint32_t &outValue) {
        if (text.empty()) {
            return false;
        }
        for (char c : text) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        try {
            const unsigned long value = std::stoul(std::string(text));
            if (value > 0xFFFFFFFFul) {
                return false;
            }
            outValue = static_cast<uint32_t>(value);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ParseLaunchOptions(int argc, char **argv, LaunchOptions &outOptions) {
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg =
                (argv[i] != nullptr) ? std::string_view(argv[i]) : std::string_view();
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

            if (arg == "--bot") {
                outOptions.botMode = true;
                continue;
            }

            if (arg == "--bot-visible") {
                outOptions.botMinimizeWindow = false;
                continue;
            }

            if (arg == "--bot-duration") {
                if (i + 1 >= argc || argv[i + 1] == nullptr) {
                    std::cerr << "Missing value for " << arg << "\n";
                    return false;
                }
                if (!ParseNonNegativeDouble(argv[++i], outOptions.botDurationSeconds)) {
                    std::cerr << "Invalid bot duration: " << argv[i] << "\n";
                    return false;
                }
                continue;
            }

            if (arg == "--bot-seed") {
                if (i + 1 >= argc || argv[i + 1] == nullptr) {
                    std::cerr << "Missing value for " << arg << "\n";
                    return false;
                }
                if (!ParseUint32(argv[++i], outOptions.botSeed)) {
                    std::cerr << "Invalid bot seed: " << argv[i] << "\n";
                    return false;
                }
                continue;
            }

            if (arg == "--bot-shoot-rate") {
                if (i + 1 >= argc || argv[i + 1] == nullptr) {
                    std::cerr << "Missing value for " << arg << "\n";
                    return false;
                }
                if (!ParsePositiveFloat(argv[++i], outOptions.botShootRate)) {
                    std::cerr << "Invalid bot shoot rate: " << argv[i] << "\n";
                    return false;
                }
                continue;
            }

            if (arg == "--bot-render-distance") {
                if (i + 1 >= argc || argv[i + 1] == nullptr) {
                    std::cerr << "Missing value for " << arg << "\n";
                    return false;
                }
                uint16_t parsedDistance = 0;
                if (!ParsePort(argv[++i], parsedDistance)) {
                    std::cerr << "Invalid bot render distance: " << argv[i] << "\n";
                    return false;
                }
                outOptions.botRenderDistance =
                    static_cast<uint16_t>(std::clamp<int>(static_cast<int>(parsedDistance), 2, 24));
                continue;
            }

            std::cerr << "Unknown option: " << arg << "\n";
            return false;
        }

        return true;
    }

} // namespace AppHelpers
