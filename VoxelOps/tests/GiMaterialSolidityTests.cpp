#include "../graphics/Vulkan/GiMaterialSolidity.hpp"

#include <cstdint>
#include <cstdlib>

int main() {
    const uint32_t mask = giTraceSolidMask();
    for (uint32_t raw = 0; raw < 256u; ++raw) {
        const BlockID id = static_cast<BlockID>(raw);
        const auto it = blockTypes.find(id);
        const bool expected = id != BlockID::Air &&
                              (raw >= static_cast<uint32_t>(BlockID::COUNT) ||
                               it == blockTypes.end() || it->second.isSolid);
        if (giTraceMaterialIsSolid(raw, mask) != expected) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
