#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

using TextureHandle = uintptr_t;

struct Rect {
    float x, y, w, h;
};

using Color = glm::vec4;

struct InputState {
    glm::vec2 mousePos{};
    bool mouseDown = false;
    bool mousePressed = false;  // true only on the frame it was pressed
    bool mouseReleased = false; // true only on the frame it was released
};

struct RectCommand {
    Rect rect;
    Color color;
};

struct TextCommand {
    std::string text;
    glm::vec2 pos;
    Color color;
};

struct ImageCommand {
    TextureHandle texture;
    Rect rect;
    Color tint;
};

using UICommand = std::variant<RectCommand, TextCommand, ImageCommand>;

class UIContext {
public:
    void beginFrame(glm::vec2 screenSize, const InputState &input, float dt);
    void endFrame();

    bool button(std::string_view text, Rect rect);
    void label(std::string_view text, glm::vec2 pos, Color color = {1, 1, 1, 1});
    void panel(Rect rect, Color color);
    void image(TextureHandle texture, Rect rect, Color tint = {1, 1, 1, 1});

    const std::vector<UICommand> &getCommands() const;

private:
    glm::vec2 m_screenSize{};
    InputState m_input{};
    float m_dt = 0.0f;

    std::vector<UICommand> m_commands;

    uint64_t m_hotId = 0;
    uint64_t m_activeId = 0;

    uint64_t makeId(std::string_view label, Rect rect) const;
    bool contains(Rect rect, glm::vec2 point) const;
};