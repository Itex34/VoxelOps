#include "../graphics/Vulkan/RtResourceLifetime.hpp"

#include <cstdlib>
#include <vector>

int main() {
    using References = std::vector<int>;
    struct Generation { References references; };
    const auto getReferences = [](const Generation &g) -> const References & { return g.references; };
    References active{1};
    References pending{2};
    std::vector<Generation> retired{{{3}}};

    const auto pinned = [&](int handle, const References *inFlight) {
        return rtBlasIsReferenced(handle, active, inFlight, retired, getReferences);
    };
    if (!pinned(1, &pending) || !pinned(2, &pending) || !pinned(3, &pending) ||
        pinned(4, &pending)) {
        return EXIT_FAILURE;
    }

    // A new TLAS may finish while the previous generation is still in flight.
    retired.push_back({active});
    active = pending;
    if (!pinned(1, nullptr)) {
        return EXIT_FAILURE;
    }
    retired.erase(retired.begin() + 1); // Its graphics completion fence signaled.
    if (pinned(1, nullptr) || !pinned(2, nullptr) || !pinned(3, nullptr)) {
        return EXIT_FAILURE;
    }
    RtBuildTicket first{10, false};
    if (!first.matches(10) || !first.canPublish(std::nullopt) || first.canPublish(11)) {
        return EXIT_FAILURE;
    }
    first.cancelled = true; // Unload before the GPU finishes; reloading cannot revive it.
    RtBuildTicket reloaded{12, false};
    if (first.matches(10) || first.canPublish(std::nullopt) || !reloaded.matches(12) ||
        !reloaded.canPublish(std::nullopt) || reloaded.canPublish(13)) {
        return EXIT_FAILURE;
    }
    if (!rtUpgradeCanPublish(reloaded, 12, false, true, false) ||
        rtUpgradeCanPublish(first, 10, false, true, false) ||
        rtUpgradeCanPublish(reloaded, std::nullopt, false, true, false) ||
        rtUpgradeCanPublish(reloaded, 13, false, true, false) ||
        rtUpgradeCanPublish(reloaded, 12, true, true, false) ||
        rtUpgradeCanPublish(reloaded, 12, false, false, false) ||
        rtUpgradeCanPublish(reloaded, 12, false, true, true)) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
