#include "UIContext.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace {
    void HashCombine(std::uint64_t &seed, std::uint64_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    }

    void CopyToBuffer(std::string_view value, char *buffer, std::size_t capacity) {
        if (buffer == nullptr || capacity == 0) {
            return;
        }
        const std::size_t copyLen = std::min(value.size(), capacity - 1);
        std::memcpy(buffer, value.data(), copyLen);
        buffer[copyLen] = '\0';
    }
}

void UIContext::beginFrame(glm::vec2 screenSize, const InputState &input, float dt) {
    m_screenSize = screenSize;
    m_input = input;
    m_dt = dt;
    m_timeSeconds += std::max(0.0f, dt);
    m_commands.clear();
    m_hotId = 0;
    m_wantsMouseCapture = false;
    m_wantsKeyboardCapture = false;

    if (!m_input.mouseDown && !m_input.mouseReleased) {
        m_activeId = 0;
    }
}

void UIContext::endFrame() {}

bool UIContext::button(std::string_view text, Rect rect, bool enabled) {
    const std::uint64_t id = makeId(text, rect);
    const bool hovered = enabled && contains(rect, m_input.mousePos);
    if (hovered || m_activeId == id) {
        m_wantsMouseCapture = true;
    }
    if (hovered) {
        m_hotId = id;
        if (m_input.mousePressed) {
            m_activeId = id;
        }
    }

    const bool active = enabled && m_activeId == id;
    const bool clicked = enabled && hovered && active && m_input.mouseReleased;

    Color color = enabled ? Color{0.10f, 0.11f, 0.13f, 0.92f} : Color{0.07f, 0.07f, 0.08f, 0.65f};
    if (active) {
        color = Color{0.22f, 0.24f, 0.28f, 0.96f};
    } else if (hovered) {
        color = Color{0.16f, 0.18f, 0.22f, 0.95f};
    }

    panel(rect, color);
    panel(
        Rect{rect.x, rect.y, rect.w, 1.0f},
        Color{1.0f, 1.0f, 1.0f, enabled ? (hovered ? 0.22f : 0.10f) : 0.04f}
    );
    labelInRect(
        text,
        Rect{rect.x + 12.0f, rect.y, rect.w - 24.0f, rect.h},
        enabled ? Color{1.0f, 1.0f, 1.0f, 1.0f} : Color{0.55f, 0.55f, 0.58f, 1.0f},
        TextAlign::Center,
        TextVerticalAlign::Center
    );

    return clicked;
}

bool UIContext::iconButton(std::string_view idText, TextureHandle icon, Rect rect, bool enabled, Color tint) {
    const std::uint64_t id = makeId(idText, rect);
    const bool hovered = enabled && contains(rect, m_input.mousePos);
    if (hovered || m_activeId == id) {
        m_wantsMouseCapture = true;
    }
    if (hovered) {
        m_hotId = id;
        if (m_input.mousePressed) {
            m_activeId = id;
        }
    }

    const bool active = enabled && m_activeId == id;
    const bool clicked = enabled && hovered && active && m_input.mouseReleased;

    Color color = enabled ? Color{0.10f, 0.11f, 0.13f, 0.92f} : Color{0.07f, 0.07f, 0.08f, 0.65f};
    if (active) {
        color = Color{0.22f, 0.24f, 0.28f, 0.96f};
    } else if (hovered) {
        color = Color{0.16f, 0.18f, 0.22f, 0.95f};
    }

    panel(rect, color);
    panel(
        Rect{rect.x, rect.y, rect.w, 1.0f},
        Color{1.0f, 1.0f, 1.0f, enabled ? (hovered ? 0.22f : 0.10f) : 0.04f}
    );

    if (icon != 0) {
        const float iconSize = std::max(0.0f, std::min(rect.w, rect.h) - 16.0f);
        const Rect iconRect{
            rect.x + (rect.w - iconSize) * 0.5f,
            rect.y + (rect.h - iconSize) * 0.5f,
            iconSize,
            iconSize
        };
        image(icon, iconRect, enabled ? tint : Color{tint.r, tint.g, tint.b, tint.a * 0.42f});
    }

    return clicked;
}

bool UIContext::inputText(std::string_view idText, char *buffer, std::size_t capacity, Rect rect, bool enabled) {
    const std::uint64_t id = makeId(idText, rect);
    const bool hovered = enabled && contains(rect, m_input.mousePos);
    if (hovered || m_focusedId == id) {
        m_wantsMouseCapture = true;
    }

    if (!enabled && m_focusedId == id) {
        m_focusedId = 0;
    }
    if (enabled && hovered && m_input.mousePressed) {
        m_focusedId = id;
        m_textCursor = buffer != nullptr ? std::strlen(buffer) : 0;
    } else if (m_input.mousePressed && m_focusedId == id && !hovered) {
        m_focusedId = 0;
    }

    std::string value = buffer != nullptr ? std::string(buffer) : std::string();
    const bool focused = enabled && m_focusedId == id;
    bool submitted = false;
    if (focused) {
        m_wantsKeyboardCapture = true;
        m_textCursor = std::min(m_textCursor, value.size());

        const auto insertText = [&](std::string_view text) {
            if (capacity <= 1) {
                return;
            }
            for (const char c : text) {
                const unsigned char byte = static_cast<unsigned char>(c);
                if (c == '\r' || c == '\n' || c == '\t' || byte == 0x7f || byte < 0x20) {
                    continue;
                }
                if (value.size() >= capacity - 1) {
                    break;
                }
                value.insert(value.begin() + static_cast<std::ptrdiff_t>(m_textCursor), c);
                ++m_textCursor;
            }
        };

        if (m_input.homePressed) {
            m_textCursor = 0;
        }
        if (m_input.endPressed) {
            m_textCursor = value.size();
        }
        if (m_input.leftPressed && m_textCursor > 0) {
            --m_textCursor;
        }
        if (m_input.rightPressed && m_textCursor < value.size()) {
            ++m_textCursor;
        }
        if (m_input.backspacePressed && m_textCursor > 0) {
            value.erase(value.begin() + static_cast<std::ptrdiff_t>(m_textCursor - 1));
            --m_textCursor;
        }
        if (m_input.deletePressed && m_textCursor < value.size()) {
            value.erase(value.begin() + static_cast<std::ptrdiff_t>(m_textCursor));
        }
        insertText(m_input.pasteText);
        insertText(m_input.textInput);

        if (m_input.enterPressed) {
            submitted = true;
        }
        CopyToBuffer(value, buffer, capacity);
    }

    const Color fill = !enabled
        ? Color{0.06f, 0.06f, 0.07f, 0.70f}
        : focused
            ? Color{0.13f, 0.14f, 0.16f, 0.95f}
            : hovered
                ? Color{0.11f, 0.12f, 0.14f, 0.92f}
                : Color{0.08f, 0.09f, 0.10f, 0.90f};
    panel(rect, fill);
    panel(
        Rect{rect.x, rect.y, rect.w, 1.0f},
        focused ? Color{1.0f, 0.72f, 0.40f, 1.0f} : Color{1.0f, 1.0f, 1.0f, enabled ? 0.12f : 0.05f}
    );

    std::string visibleText = value.empty() ? std::string() : value;
    if (focused && std::fmod(m_timeSeconds, 1.0f) < 0.5f) {
        const std::size_t cursor = std::min(m_textCursor, visibleText.size());
        visibleText.insert(cursor, "|");
    }
    if (visibleText.size() > 56) {
        visibleText = "..." + visibleText.substr(visibleText.size() - 53);
    }

    pushClip(Rect{rect.x + 6.0f, rect.y, rect.w - 12.0f, rect.h});
    labelInRect(
        visibleText,
        Rect{rect.x + 10.0f, rect.y, rect.w - 20.0f, rect.h},
        enabled ? Color{0.94f, 0.94f, 0.94f, 1.0f} : Color{0.50f, 0.50f, 0.52f, 1.0f},
        TextAlign::Start,
        TextVerticalAlign::Center
    );
    popClip();

    return submitted;
}

void UIContext::label(std::string_view text, glm::vec2 pos, Color color) {
    m_commands.push_back(TextCommand{std::string(text), pos, color});
}

void UIContext::labelInRect(
    std::string_view text,
    Rect rect,
    Color color,
    TextAlign align,
    TextVerticalAlign verticalAlign
) {
    if (text.empty() || rect.w <= 0.0f || rect.h <= 0.0f) {
        return;
    }
    TextCommand command;
    command.text = std::string(text);
    command.pos = glm::vec2(rect.x, rect.y);
    command.color = color;
    command.bounded = true;
    command.rect = rect;
    command.align = align;
    command.verticalAlign = verticalAlign;
    pushClip(rect);
    m_commands.push_back(std::move(command));
    popClip();
}

void UIContext::panel(Rect rect, Color color) {
    m_commands.push_back(RectCommand{rect, color});
}

void UIContext::image(TextureHandle texture, Rect rect, Color tint) {
    m_commands.push_back(ImageCommand{texture, rect, tint});
}

void UIContext::pushClip(Rect rect) {
    m_commands.push_back(ClipPushCommand{rect});
}

void UIContext::popClip() {
    m_commands.push_back(ClipPopCommand{});
}

const std::vector<UICommand> &UIContext::getCommands() const {
    return m_commands;
}

glm::vec2 UIContext::screenSize() const noexcept {
    return m_screenSize;
}

bool UIContext::wantsMouseCapture() const noexcept {
    return m_wantsMouseCapture;
}

bool UIContext::wantsKeyboardCapture() const noexcept {
    return m_wantsKeyboardCapture;
}

uint64_t UIContext::makeId(std::string_view label, Rect rect) const {
    std::uint64_t seed = std::hash<std::string_view>{}(label);
    HashCombine(seed, std::hash<float>{}(rect.x));
    HashCombine(seed, std::hash<float>{}(rect.y));
    HashCombine(seed, std::hash<float>{}(rect.w));
    HashCombine(seed, std::hash<float>{}(rect.h));
    return seed;
}

bool UIContext::contains(Rect rect, glm::vec2 point) const {
    return point.x >= rect.x && point.y >= rect.y &&
           point.x < (rect.x + rect.w) && point.y < (rect.y + rect.h);
}

