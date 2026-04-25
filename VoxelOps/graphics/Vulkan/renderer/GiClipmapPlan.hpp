#pragma once

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace GiClipmapPlan {

constexpr uint32_t MAX_CASCADES = 4;

struct GiProbeTextureLayout {
    // DDGI-style defaults: 6x6 irradiance, 14x14 visibility, with 1-texel border.
    uint32_t irradianceInteriorTexels = 6;
    uint32_t visibilityInteriorTexels = 14;
    uint32_t borderTexels = 1;
};

struct GiCascadeSchedule {
    // Update cadence in frames (1 = every frame).
    uint32_t updateEveryNFrames = 1;
    // Max probes refreshed when this cascade is scheduled.
    uint32_t maxProbeUpdatesPerTick = 0;
    // Rays traced per refreshed probe.
    uint32_t raysPerProbe = 0;
};

struct GiCascadeConfig {
    // Grid spacing in world blocks.
    uint32_t spacingBlocks = 1;
    // Probe grid dimensions in probe cells.
    glm::uvec3 probeCounts{0u};
    GiCascadeSchedule schedule{};
};

struct GiClipmapConfig {
    uint32_t cascadeCount = 0;
    std::array<GiCascadeConfig, MAX_CASCADES> cascades{};
    GiProbeTextureLayout textureLayout{};
};

struct GiCascadeRuntimeState {
    // World-space origin of probe [0,0,0], snapped to cascade spacing.
    glm::ivec3 snappedOriginBlocks{0};
    // Round-robin probe update cursor in flattened probe index space.
    uint32_t updateCursor = 0;
};

struct GiCascadeFrameBudget {
    uint32_t firstProbe = 0;
    uint32_t probesUpdated = 0;
    uint32_t endProbe = 0;
    uint64_t raysCast = 0;
};

struct GiFrameBudget {
    std::array<GiCascadeFrameBudget, MAX_CASCADES> cascades{};
    uint32_t totalProbesUpdated = 0;
    uint64_t totalRaysCast = 0;
};

struct GiCascadeAtlasExtent {
    // Packed as X x (Y*Z) tiles for each probe volume slice.
    glm::uvec2 irradianceTexels{0u};
    glm::uvec2 visibilityTexels{0u};
};

inline uint32_t probeCount(const GiCascadeConfig &cascade) {
    const uint64_t xy =
        static_cast<uint64_t>(cascade.probeCounts.x) * static_cast<uint64_t>(cascade.probeCounts.y);
    const uint64_t xyz = xy * static_cast<uint64_t>(cascade.probeCounts.z);
    if (xyz > static_cast<uint64_t>(UINT32_MAX)) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(xyz);
}

inline uint32_t probeTileTexels(uint32_t interiorTexels, uint32_t borderTexels) {
    return interiorTexels + (borderTexels * 2u);
}

inline GiCascadeAtlasExtent computeAtlasExtent(const GiCascadeConfig &cascade,
                                               const GiProbeTextureLayout &layout) {
    const uint32_t irradianceTile =
        probeTileTexels(layout.irradianceInteriorTexels, layout.borderTexels);
    const uint32_t visibilityTile =
        probeTileTexels(layout.visibilityInteriorTexels, layout.borderTexels);
    const uint32_t packedRows = cascade.probeCounts.y * cascade.probeCounts.z;

    GiCascadeAtlasExtent out{};
    out.irradianceTexels =
        glm::uvec2(cascade.probeCounts.x * irradianceTile, packedRows * irradianceTile);
    out.visibilityTexels =
        glm::uvec2(cascade.probeCounts.x * visibilityTile, packedRows * visibilityTile);
    return out;
}

inline int32_t floorDiv(int32_t value, int32_t divisor) {
    const int32_t q = value / divisor;
    const int32_t r = value % divisor;
    if (r != 0 && ((r > 0) != (divisor > 0))) {
        return q - 1;
    }
    return q;
}

inline glm::ivec3 snapCascadeOriginBlocks(const glm::vec3 &playerPositionBlocks,
                                          const GiCascadeConfig &cascade) {
    const int32_t spacing = std::max(1, static_cast<int32_t>(cascade.spacingBlocks));
    const glm::ivec3 playerCell(
        floorDiv(static_cast<int32_t>(std::floor(playerPositionBlocks.x)), spacing),
        floorDiv(static_cast<int32_t>(std::floor(playerPositionBlocks.y)), spacing),
        floorDiv(static_cast<int32_t>(std::floor(playerPositionBlocks.z)), spacing));
    const glm::ivec3 halfGrid = glm::ivec3(cascade.probeCounts) / 2;
    return (playerCell - halfGrid) * spacing;
}

inline GiCascadeFrameBudget buildCascadeFrameBudget(const GiCascadeConfig &cascade,
                                                    GiCascadeRuntimeState &runtime,
                                                    uint64_t frameIndex) {
    GiCascadeFrameBudget out{};
    const uint32_t totalProbes = probeCount(cascade);
    if (totalProbes == 0) {
        return out;
    }

    const GiCascadeSchedule &schedule = cascade.schedule;
    if (schedule.updateEveryNFrames == 0 || schedule.maxProbeUpdatesPerTick == 0 ||
        schedule.raysPerProbe == 0) {
        return out;
    }
    if ((frameIndex % schedule.updateEveryNFrames) != 0) {
        return out;
    }

    const uint32_t updates = std::min(schedule.maxProbeUpdatesPerTick, totalProbes);
    const uint32_t firstProbe = runtime.updateCursor % totalProbes;
    const uint32_t endProbe = (firstProbe + updates) % totalProbes;

    runtime.updateCursor = endProbe;

    out.firstProbe = firstProbe;
    out.probesUpdated = updates;
    out.endProbe = endProbe;
    out.raysCast = static_cast<uint64_t>(updates) * static_cast<uint64_t>(schedule.raysPerProbe);
    return out;
}

inline GiFrameBudget buildFrameBudget(const GiClipmapConfig &config,
                                      std::array<GiCascadeRuntimeState, MAX_CASCADES> &runtime,
                                      uint64_t frameIndex) {
    GiFrameBudget frame{};
    const uint32_t cascadeCount = std::min(config.cascadeCount, MAX_CASCADES);
    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        frame.cascades[cascadeIndex] = buildCascadeFrameBudget(config.cascades[cascadeIndex],
                                                               runtime[cascadeIndex], frameIndex);
        frame.totalProbesUpdated += frame.cascades[cascadeIndex].probesUpdated;
        frame.totalRaysCast += frame.cascades[cascadeIndex].raysCast;
    }
    return frame;
}

inline uint64_t estimateWorstCaseRaysPerFrame(const GiClipmapConfig &config) {
    uint64_t rays = 0;
    const uint32_t cascadeCount = std::min(config.cascadeCount, MAX_CASCADES);
    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        const GiCascadeConfig &cascade = config.cascades[cascadeIndex];
        const uint64_t probes =
            std::min<uint64_t>(probeCount(cascade), cascade.schedule.maxProbeUpdatesPerTick);
        rays += probes * static_cast<uint64_t>(cascade.schedule.raysPerProbe);
    }
    return rays;
}

inline double estimateAverageRaysPerFrame(const GiClipmapConfig &config) {
    double rays = 0.0;
    const uint32_t cascadeCount = std::min(config.cascadeCount, MAX_CASCADES);
    for (uint32_t cascadeIndex = 0; cascadeIndex < cascadeCount; ++cascadeIndex) {
        const GiCascadeConfig &cascade = config.cascades[cascadeIndex];
        const uint32_t n = std::max(1u, cascade.schedule.updateEveryNFrames);
        const double probesPerFrame =
            static_cast<double>(cascade.schedule.maxProbeUpdatesPerTick) / static_cast<double>(n);
        rays += probesPerFrame * static_cast<double>(cascade.schedule.raysPerProbe);
    }
    return rays;
}

inline GiClipmapConfig makeDefaultConfig() {
    GiClipmapConfig config{};
    config.cascadeCount = 3;
    config.textureLayout = GiProbeTextureLayout{};

    // C0: near field, dense sampling.
    config.cascades[0].spacingBlocks = 2;
    config.cascades[0].probeCounts = glm::uvec3(24u, 12u, 24u);
    config.cascades[0].schedule.updateEveryNFrames = 1;
    config.cascades[0].schedule.maxProbeUpdatesPerTick = 256;
    config.cascades[0].schedule.raysPerProbe = 128;

    // C1: mid field.
    config.cascades[1].spacingBlocks = 4;
    config.cascades[1].probeCounts = glm::uvec3(20u, 10u, 20u);
    config.cascades[1].schedule.updateEveryNFrames = 3;
    config.cascades[1].schedule.maxProbeUpdatesPerTick = 160;
    config.cascades[1].schedule.raysPerProbe = 64;

    // C2: far field, coarse and slow.
    config.cascades[2].spacingBlocks = 8;
    config.cascades[2].probeCounts = glm::uvec3(16u, 8u, 16u);
    config.cascades[2].schedule.updateEveryNFrames = 8;
    config.cascades[2].schedule.maxProbeUpdatesPerTick = 96;
    config.cascades[2].schedule.raysPerProbe = 32;

    return config;
}

} // namespace GiClipmapPlan
