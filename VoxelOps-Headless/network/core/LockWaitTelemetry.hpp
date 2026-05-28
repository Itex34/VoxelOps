#pragma once

#include "DiagnosticsFlags.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>

namespace LockWaitTelemetry {

namespace detail {

inline void MaybeLogSlowLockWait(
    const char *category,
    const char *context,
    int64_t waitUs,
    int64_t thresholdUs,
    std::atomic<uint64_t> &counter
) {
    if (waitUs < thresholdUs ||
        !DiagnosticsFlags::g_enableServerPerfDiagnostics.load(std::memory_order_acquire)) {
        return;
    }

    const uint64_t count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 40 || (count % 200) == 0) {
        std::cerr << "[perf/" << category << "] slow lock wait context=" << context
                  << " waitUs=" << waitUs << " count=" << count << "\n";
    }
}

} // namespace detail

inline std::unique_lock<std::mutex> AcquireSessionLock(std::mutex &mutex, const char *context) {
    constexpr int64_t kSlowSessionLockWaitUs = 250;
    static std::atomic<uint64_t> s_slowSessionLockCount{0};

    const auto lockStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex);
    const int64_t waitUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               lockStart)
            .count();
    detail::MaybeLogSlowLockWait(
        "session-lock", context, waitUs, kSlowSessionLockWaitUs, s_slowSessionLockCount
    );
    return lock;
}

inline std::unique_lock<std::mutex>
AcquirePlayerManagerLock(std::mutex &mutex, const char *context) {
    constexpr int64_t kSlowPlayerManagerLockWaitUs = 250;
    static std::atomic<uint64_t> s_slowPlayerManagerLockCount{0};

    const auto lockStart = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex);
    const int64_t waitUs =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               lockStart)
            .count();
    detail::MaybeLogSlowLockWait(
        "player-lock",
        context,
        waitUs,
        kSlowPlayerManagerLockWaitUs,
        s_slowPlayerManagerLockCount
    );
    return lock;
}

} // namespace LockWaitTelemetry

