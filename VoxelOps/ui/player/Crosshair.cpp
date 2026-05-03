#include "Crosshair.hpp"

bool Crosshair::initialize(const Settings::Crosshair &crosshairSettings) {
    if (crosshairSettings.style == Settings::Crosshair::Style::CustomImage) {
        if (crosshairSettings.crosshairImagePath.empty()) {
            return loadCrosshairImageFromFile(crosshairSettings.crosshairImagePath);
        }
    }

    return true;
}

bool Crosshair::loadCrosshairImageFromFile(const std::filesystem::path &path) {
    return true;
}