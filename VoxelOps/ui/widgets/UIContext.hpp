#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

#include "../../graphics/TextureHandle.hpp"

struct Rect {
    float x, y, w, h;
};

using Color = glm::vec4;

enum class TextAlign : std::uint8_t {
    Start = 0,
    Center,
    End
};

enum class TextVerticalAlign : std::uint8_t {
    Top = 0,
    Center,
    Bottom
};

struct InputState {
    glm::vec2 mousePos{};
    bool mouseDown = false;
    bool mousePressed = false;  // true only on the frame it was pressed
    bool mouseReleased = false; // true only on the frame it was released
    bool backspacePressed = false;
    bool deletePressed = false;
    bool leftPressed = false;
    bool rightPressed = false;
    bool homePressed = false;
    bool endPressed = false;
    bool enterPressed = false;
    bool tabPressed = false;
    bool ctrlDown = false;
    std::string textInput;
    std::string pasteText;
};

struct RectCommand {
    Rect rect;
    Color color;
};

struct TextCommand {
    std::string text;
    glm::vec2 pos;
    Color color;
    bool bounded = false;
    Rect rect{};
    TextAlign align = TextAlign::Start;
    TextVerticalAlign verticalAlign = TextVerticalAlign::Top;
};

struct ImageCommand {
    TextureHandle texture;
    Rect rect;
    Color tint;
};

struct ClipPushCommand {
    Rect rect;
};

struct ClipPopCommand {};

using UICommand = std::variant<RectCommand, TextCommand, ImageCommand, ClipPushCommand, ClipPopCommand>;

class UIContext {
public:
    void beginFrame(glm::vec2 screenSize, const InputState &input, float dt);
    void endFrame();

    bool button(std::string_view text, Rect rect, bool enabled = true);
    bool iconButton(
        std::string_view id,
        TextureHandle icon,
        Rect rect,
        bool enabled = true,
        Color tint = {1, 1, 1, 1}
    );
    bool inputText(std::string_view id, char *buffer, std::size_t capacity, Rect rect, bool enabled = true);
    void label(std::string_view text, glm::vec2 pos, Color color = {1, 1, 1, 1});
    void labelInRect(
        std::string_view text,
        Rect rect,
        Color color = {1, 1, 1, 1},
        TextAlign align = TextAlign::Start,
        TextVerticalAlign verticalAlign = TextVerticalAlign::Center
    );
    void panel(Rect rect, Color color);
    void image(TextureHandle texture, Rect rect, Color tint = {1, 1, 1, 1});
    void pushClip(Rect rect);
    void popClip();

    const std::vector<UICommand> &getCommands() const;
    glm::vec2 screenSize() const noexcept;
    bool wantsMouseCapture() const noexcept;
    bool wantsKeyboardCapture() const noexcept;

private:
    glm::vec2 m_screenSize{};
    InputState m_input{};
    float m_dt = 0.0f;

    std::vector<UICommand> m_commands;

    uint64_t m_hotId = 0;
    uint64_t m_activeId = 0;
    uint64_t m_focusedId = 0;
    std::size_t m_textCursor = 0;
    float m_timeSeconds = 0.0f;
    bool m_wantsMouseCapture = false;
    bool m_wantsKeyboardCapture = false;

    uint64_t makeId(std::string_view label, Rect rect) const;
    bool contains(Rect rect, glm::vec2 point) const;
};
