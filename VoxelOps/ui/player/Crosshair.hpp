#pragma once
#include <SDL3/SDL.h>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <filesystem>
#include "../../settings/Settings.hpp"

class Crosshair {

  public:
    bool initialize(const Settings::Crosshair &crosshairSettings);
    void draw(const Settings::Crosshair &crosshairSettings);

  private:
    bool loadCrosshairImageFromFile(const std::filesystem::path &path);
};