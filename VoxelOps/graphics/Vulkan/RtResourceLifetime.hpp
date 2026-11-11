#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

struct RtBuildTicket {
    uint64_t revision = 0;
    bool cancelled = false;

    bool matches(uint64_t currentRevision) const noexcept {
        return !cancelled && revision == currentRevision;
    }
    bool canPublish(std::optional<uint64_t> queuedRevision) const noexcept {
        return !cancelled && (!queuedRevision || *queuedRevision <= revision);
    }
};

// A retired BLAS must remain alive while any live TLAS generation can refer to it.
template <typename Handle, typename References, typename Retired, typename GetReferences>
bool rtBlasIsReferenced(
    Handle blas, const References &active, const References *pending,
    const Retired &retired, GetReferences getReferences
) {
    const auto contains = [blas](const auto &references) {
        return std::find(references.begin(), references.end(), blas) != references.end();
    };
    if (contains(active) || (pending != nullptr && contains(*pending))) {
        return true;
    }
    return std::any_of(retired.begin(), retired.end(), [&](const auto &generation) {
        return contains(getReferences(generation));
    });
}
