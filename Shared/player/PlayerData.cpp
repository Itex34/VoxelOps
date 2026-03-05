#include "PlayerData.hpp"

namespace Shared::PlayerData {

const MovementSettings& GetMovementSettings() {
    static const MovementSettings kSettings{};
    return kSettings;
}

} // namespace Shared::PlayerData
