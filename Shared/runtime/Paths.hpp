#pragma once

#include <filesystem>
#include <string>

namespace Shared::RuntimePaths {

void Initialize(const std::filesystem::path& executablePath);

std::filesystem::path ResolveVoxelOpsPath(const std::filesystem::path& relativePath);
std::filesystem::path ResolveModelsPath(const std::filesystem::path& relativePath);
std::filesystem::path ResolveSharedPath(const std::filesystem::path& relativePath);

std::string Describe();

} // namespace Shared::RuntimePaths
