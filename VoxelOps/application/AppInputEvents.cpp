#include <SDL3/SDL.h>

#include "App.hpp"
#include "AppHelpers.hpp"

#include "../data/GameData.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <imgui.h>

namespace {
    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }

    bool IsMouseButtonDown(uint8_t button) {
        return (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_MASK(button)) != 0;
    }
} // namespace

void App::pollEvents(Runtime &runtime) {
    SDL_Event event;
    const SDL_WindowID windowId = (m_Window != nullptr) ? SDL_GetWindowID(m_Window) : 0;
    while (SDL_PollEvent(&event)) {
        if (runtime.ui.debugUi) {
            runtime.ui.debugUi->processEvent(event);
        }

        switch (event.type) {
        case SDL_EVENT_QUIT:
            m_ShouldQuit = true;
            break;
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            if (event.window.windowID == windowId) {
                m_ShouldQuit = true;
            }
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED:
            if (event.window.windowID == windowId && runtime.gameplay.inputCallbacks) {
                runtime.gameplay.inputCallbacks->framebuffer_size_callback(
                    m_Window, event.window.data1, event.window.data2
                );
                runtime.render.renderer->onWindowResized(event.window.data1, event.window.data2);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.windowID == windowId && runtime.gameplay.inputCallbacks) {
                runtime.gameplay.inputCallbacks->mouse_motion_callback(
                    m_Window,
                    event.motion.x,
                    event.motion.y,
                    event.motion.xrel,
                    event.motion.yrel,
                    m_UseDebugCamera
                );
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.windowID == windowId && runtime.gameplay.inputCallbacks) {
                runtime.gameplay.inputCallbacks->mouse_button_callback(
                    m_Window, event.button.button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                );
            }
            break;
        default:
            break;
        }
    }
}

void App::updateDebugCamera(Runtime &runtime) {
    if (m_UseDebugCamera && m_Window && SDL_GetWindowRelativeMouseMode(m_Window)) {
        float mouseDx = 0.0f;
        float mouseDy = 0.0f;
        SDL_GetRelativeMouseState(&mouseDx, &mouseDy);
        runtime.app.inputLook.yaw += (mouseDx * 0.1f);
        runtime.app.inputLook.pitch -= (mouseDy * 0.1f);
        runtime.app.inputLook.pitch = glm::clamp(runtime.app.inputLook.pitch, -89.0f, 89.0f);
    } else {
        float mouseX = 0.0f;
        float mouseY = 0.0f;
        SDL_GetMouseState(&mouseX, &mouseY);
        runtime.app.inputLook.xpos = mouseX;
        runtime.app.inputLook.ypos = mouseY;
    }

    const bool keyboardBlockedByUi = AppHelpers::IsImGuiTextInputActive();
    glm::vec3 moveDir(0.0f);
    if (!keyboardBlockedByUi) {
        if (IsScancodeDown(SDL_SCANCODE_U))
            moveDir += runtime.render.debugCamera.XZfront;
        if (IsScancodeDown(SDL_SCANCODE_J))
            moveDir -= runtime.render.debugCamera.XZfront;
        if (IsScancodeDown(SDL_SCANCODE_H))
            moveDir -= glm::normalize(glm::cross(runtime.render.debugCamera.front, runtime.render.debugCamera.up));
        if (IsScancodeDown(SDL_SCANCODE_K))
            moveDir += glm::normalize(glm::cross(runtime.render.debugCamera.front, runtime.render.debugCamera.up));
        if (IsScancodeDown(SDL_SCANCODE_RALT))
            moveDir += runtime.render.debugCamera.up;
        if (IsScancodeDown(SDL_SCANCODE_V))
            moveDir -= runtime.render.debugCamera.up;
    }

    if (glm::length(moveDir) > 0.0f) {
        moveDir = glm::normalize(moveDir);
    }
    runtime.render.debugCamera.position += moveDir * 10.0f * static_cast<float>(GameData::deltaTime);

    if (m_UseDebugCamera && (!m_Window || !SDL_GetWindowRelativeMouseMode(m_Window))) {
        const double xoffset = runtime.app.inputLook.xpos - runtime.app.inputLook.lastX;
        const double yoffset = runtime.app.inputLook.ypos - runtime.app.inputLook.lastY;
        runtime.app.inputLook.lastX = runtime.app.inputLook.xpos;
        runtime.app.inputLook.lastY = runtime.app.inputLook.ypos;

        runtime.app.inputLook.yaw += static_cast<float>(xoffset * 0.1);
        runtime.app.inputLook.pitch -= static_cast<float>(yoffset * 0.1);
        runtime.app.inputLook.pitch = glm::clamp(runtime.app.inputLook.pitch, -89.0f, 89.0f);
    }

    runtime.render.debugCamera.updateRotation(runtime.app.inputLook.yaw, runtime.app.inputLook.pitch);
}

void App::updateToggleStates(Runtime &runtime) {
    const bool keyboardBlockedByUi = AppHelpers::IsImGuiTextInputActive();
    const bool textInputBlocked =
        (ImGui::GetCurrentContext() != nullptr) && ImGui::GetIO().WantTextInput;
    const auto refreshCursorState = [&]() {
        GameData::cursorEnabled = m_ForceCursorEnabled || m_ShowDebugUi || m_ShowInventoryUi ||
                                  runtime.ui.wantsCursor;
        applyMouseInputModes();
    };

    const bool isF1Pressed = IsScancodeDown(SDL_SCANCODE_F1);
    if (!keyboardBlockedByUi && isF1Pressed && !m_WasF1Pressed) {
        m_UseDebugCamera = !m_UseDebugCamera;
    }
    m_WasF1Pressed = isF1Pressed;

    const bool isTPressed = IsScancodeDown(SDL_SCANCODE_T);
    if (!keyboardBlockedByUi && isTPressed && !m_WasTPressed) {
        m_ToggleWireframe = !m_ToggleWireframe;
    }
    m_WasTPressed = isTPressed;

    const bool isF2Pressed = IsScancodeDown(SDL_SCANCODE_F2);
    if (!keyboardBlockedByUi && isF2Pressed && !m_WasF2Pressed) {
        m_ToggleChunkBorders = !m_ToggleChunkBorders;
    }
    m_WasF2Pressed = isF2Pressed;

    const bool isF3Pressed = IsScancodeDown(SDL_SCANCODE_F3);
    if (!keyboardBlockedByUi && isF3Pressed && !m_WasF3Pressed) {
        m_ToggleDebugFrustum = !m_ToggleDebugFrustum;
    }
    m_WasF3Pressed = isF3Pressed;

    const bool isF10Pressed = IsScancodeDown(SDL_SCANCODE_F10);
    if (!keyboardBlockedByUi && isF10Pressed && !m_WasF10Pressed) {
        m_ShowDebugUi = !m_ShowDebugUi;
        if (runtime.ui.debugUi) {
            runtime.ui.debugUi->setVisible(m_ShowDebugUi);
        }
        refreshCursorState();
    }
    m_WasF10Pressed = isF10Pressed;

    const bool isXPressed = IsScancodeDown(SDL_SCANCODE_X);
    if (!textInputBlocked && isXPressed && !m_WasXPressed) {
        m_ShowInventoryUi = !m_ShowInventoryUi;
        if (runtime.ui.inventoryUi) {
            runtime.ui.inventoryUi->setVisible(m_ShowInventoryUi);
        }
        refreshCursorState();
    }
    m_WasXPressed = isXPressed;

    const bool isEscapePressed = IsScancodeDown(SDL_SCANCODE_ESCAPE);
    if (!textInputBlocked && isEscapePressed && !m_WasEscapePressed) {
        m_ForceCursorEnabled = !m_ForceCursorEnabled;
        refreshCursorState();
    }
    m_WasEscapePressed = isEscapePressed;

    const bool canRecaptureCursor = runtime.network.clientNet.IsConnected() && !m_ShowDebugUi &&
                                    !m_ShowInventoryUi && m_ForceCursorEnabled;
    const bool primaryMouseDown = IsMouseButtonDown(SDL_BUTTON_LEFT);
    if (canRecaptureCursor && primaryMouseDown && !textInputBlocked) {
        m_ForceCursorEnabled = false;
        refreshCursorState();
    }
}





