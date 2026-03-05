#include "Paths.hpp"

#include <algorithm>
#include <sstream>
#include <string_view>
#include <vector>

namespace Shared::RuntimePaths {
namespace {

struct RuntimePathState {
    bool initialized = false;
    std::vector<std::filesystem::path> roots;
    std::filesystem::path voxelOpsBase;
    std::filesystem::path modelsBase;
    std::filesystem::path sharedBase;
};

RuntimePathState g_state;

std::filesystem::path NormalizePath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolutePath = std::filesystem::absolute(path, ec);
    if (ec) {
        return path.lexically_normal();
    }
    return absolutePath.lexically_normal();
}

bool PathExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

void AddUniquePath(std::vector<std::filesystem::path>& out, const std::filesystem::path& candidate) {
    const std::filesystem::path normalized = NormalizePath(candidate);
    const auto alreadyThere = std::find(out.begin(), out.end(), normalized);
    if (alreadyThere == out.end()) {
        out.push_back(normalized);
    }
}

void AddPathAndParents(std::vector<std::filesystem::path>& out, const std::filesystem::path& start) {
    if (start.empty()) {
        return;
    }

    std::filesystem::path current = NormalizePath(start);
    while (!current.empty()) {
        AddUniquePath(out, current);

        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
}

std::filesystem::path DetectExecutableDir(const std::filesystem::path& executablePath) {
    if (executablePath.empty()) {
        return NormalizePath(std::filesystem::current_path());
    }

    std::error_code ec;
    if (std::filesystem::is_directory(executablePath, ec) && !ec) {
        return NormalizePath(executablePath);
    }

    const std::filesystem::path parent = executablePath.parent_path();
    if (parent.empty()) {
        return NormalizePath(std::filesystem::current_path());
    }
    return NormalizePath(parent);
}

std::filesystem::path FindVoxelOpsBase(const std::vector<std::filesystem::path>& roots) {
    for (const auto& root : roots) {
        const std::filesystem::path asProject = root / "VoxelOps";
        if (PathExists(asProject / "shaders") && PathExists(asProject / "assets")) {
            return asProject;
        }
    }

    for (const auto& root : roots) {
        if (PathExists(root / "shaders") && PathExists(root / "assets")) {
            return root;
        }
    }

    if (!roots.empty()) {
        return roots.front() / "VoxelOps";
    }
    return NormalizePath(std::filesystem::current_path() / "VoxelOps");
}

std::filesystem::path FindModelsBase(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& voxelOpsBase
) {
    for (const auto& root : roots) {
        const std::filesystem::path modelsDir = root / "Models";
        if (PathExists(modelsDir)) {
            return modelsDir;
        }
    }

    for (const auto& root : roots) {
        const std::filesystem::path modelsDir = root / "models";
        if (PathExists(modelsDir)) {
            return modelsDir;
        }
    }

    const std::filesystem::path nearVoxelOps = voxelOpsBase.parent_path() / "Models";
    if (PathExists(nearVoxelOps)) {
        return nearVoxelOps;
    }

    if (!roots.empty()) {
        return roots.front() / "Models";
    }
    return NormalizePath(std::filesystem::current_path() / "Models");
}

std::filesystem::path FindSharedBase(
    const std::vector<std::filesystem::path>& roots,
    const std::filesystem::path& voxelOpsBase
) {
    for (const auto& root : roots) {
        const std::filesystem::path sharedDir = root / "Shared";
        if (PathExists(sharedDir)) {
            return sharedDir;
        }
    }

    const std::filesystem::path nearVoxelOps = voxelOpsBase.parent_path() / "Shared";
    if (PathExists(nearVoxelOps)) {
        return nearVoxelOps;
    }

    if (!roots.empty()) {
        return roots.front() / "Shared";
    }
    return NormalizePath(std::filesystem::current_path() / "Shared");
}

void EnsureInitialized() {
    if (g_state.initialized) {
        return;
    }
    Initialize({});
}

std::string PathToString(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

} // namespace

void Initialize(const std::filesystem::path& executablePath) {
    RuntimePathState nextState;

    std::filesystem::path executableDir = DetectExecutableDir(executablePath);
    std::filesystem::path cwd = NormalizePath(std::filesystem::current_path());

    AddPathAndParents(nextState.roots, executableDir);
    AddPathAndParents(nextState.roots, cwd);

    if (nextState.roots.empty()) {
        nextState.roots.push_back(cwd);
    }

    nextState.voxelOpsBase = NormalizePath(FindVoxelOpsBase(nextState.roots));
    nextState.modelsBase = NormalizePath(FindModelsBase(nextState.roots, nextState.voxelOpsBase));
    nextState.sharedBase = NormalizePath(FindSharedBase(nextState.roots, nextState.voxelOpsBase));
    nextState.initialized = true;

    g_state = std::move(nextState);
}

std::filesystem::path ResolveVoxelOpsPath(const std::filesystem::path& relativePath) {
    EnsureInitialized();
    return (g_state.voxelOpsBase / relativePath).lexically_normal();
}

std::filesystem::path ResolveModelsPath(const std::filesystem::path& relativePath) {
    EnsureInitialized();
    return (g_state.modelsBase / relativePath).lexically_normal();
}

std::filesystem::path ResolveSharedPath(const std::filesystem::path& relativePath) {
    EnsureInitialized();
    return (g_state.sharedBase / relativePath).lexically_normal();
}

std::string Describe() {
    EnsureInitialized();

    std::ostringstream out;
    out << "voxelops=" << PathToString(g_state.voxelOpsBase)
        << " | models=" << PathToString(g_state.modelsBase)
        << " | shared=" << PathToString(g_state.sharedBase);
    return out.str();
}

} // namespace Shared::RuntimePaths
