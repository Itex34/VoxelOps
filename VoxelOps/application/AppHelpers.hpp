#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <glm/vec3.hpp>

#include "../../Shared/gun/GunType.hpp"

class ClientNetwork;
class Gun;

namespace AppHelpers {

bool IsImGuiTextInputActive();
float LatencyCorrectionBlend(const ClientNetwork& net);

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

std::span<const GunDefinition> GetGunDefinitions();
const GunDefinition* FindGunDefinition(GunType gunType);
const GunDefinition* FindGunDefinitionByWeaponId(uint16_t weaponId);
std::string ResolveGunModelPath(const GunDefinition& definition);
std::unique_ptr<Gun> BuildGunFromDefinition(const GunDefinition& definition);

float NormalizeYawDegrees(float yawDegrees);
float ToModelYawDegrees(float lookYawDegrees, bool invertYaw, float yawOffsetDeg);

inline bool IsNewerU32(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b) > 0;
}

inline bool IsAckedU32(uint32_t sequence, uint32_t ack) {
    return static_cast<int32_t>(sequence - ack) <= 0;
}

inline constexpr bool kEnableChunkDiagnostics = false;
inline constexpr bool kDefaultPlayerModelYawInvert = true;
inline constexpr float kDefaultPlayerModelYawOffsetDeg = 0.0f;
inline constexpr float kRemoteGunOwnerYawCorrectionDeg = -90.0f;
extern const glm::vec3 kRemoteGunRightHandAnchorOffset;

struct LaunchOptions {
    std::string serverIp = "127.0.0.1";
    uint16_t serverPort = 27015;
    std::string requestedUsername;
    bool showHelp = false;
};

void PrintUsage();
bool ParsePort(std::string_view text, uint16_t& outPort);
std::string TrimAscii(std::string_view text);
bool ParseHost(std::string_view text, std::string& outHost);
bool ParseServerEndpoint(std::string_view text, std::string& outIp, uint16_t& outPort);
bool ParseLaunchOptions(int argc, char** argv, LaunchOptions& outOptions);

} // namespace AppHelpers
