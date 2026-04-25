#pragma once
#include <cstdint>
#include <filesystem>

namespace Settings
{
    struct Video {
        uint16_t renderDistance = 12;
        float renderScale = 1.0f;
    };

    struct Gameplay
    {

    };

    struct Audio
    {
        float masterVolume = 100.0f;
    };

    struct Controls
    {

    };

    struct Data {
        Video video;
		Gameplay gameplay;
        Audio audio;
		Controls controls;
    };

    struct Crosshair {
        uint32_t color;
        uint32_t outlineColor;
        float size;
        float thickness;
        float outlineThickness;
        float gap;

		std::filesystem::path crosshairImagePath; // optional, may be empty

        enum class Style : uint8_t {
            Dot,
            Cross,
            CrossWithDot,
            CustomImage,
            COUNT
        } style;
    };

    extern Data current;

    void loadFromJson(Data& data, const std::filesystem::path& path);
    void saveToJson(const Data& data, const std::filesystem::path& path);
}