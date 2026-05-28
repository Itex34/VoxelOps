#pragma once

#include "../../runtime/Runtime.hpp"
#include "../../application/FrameServices.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
    class Event;
    class EventListener;
    namespace Input {
        enum KeyIdentifier : unsigned char;
    }
} // namespace Rml

struct MainMenuContext {
    SDL_Window *window = nullptr;
    std::string *serverIp = nullptr;
    uint16_t *serverPort = nullptr;
    std::string *requestedUsername = nullptr;
    FrameConnectionHost *connectionHost = nullptr;
    FrameWindowHost *windowHost = nullptr;
};

class MainMenu {
public:
    MainMenu();
    ~MainMenu();
    void draw(Runtime &runtime, const MainMenuContext &ctx);
    void hide();

private:
    class RmlMenuListener;

    void drawImGui(Runtime &runtime, const MainMenuContext &ctx);
    void drawRml(Runtime &runtime, const MainMenuContext &ctx);
    bool bindRmlContext(Runtime &runtime);
    void resetRmlDocument();
    void forgetRmlState();
    void syncRmlState(Runtime &runtime);
    void handleSubmit(Runtime &runtime, const MainMenuContext &ctx);
    void handlePaste(Runtime &runtime);
    void handleRmlEvent(Runtime &runtime, const MainMenuContext &ctx, Rml::Event &event);

    Rml::Context *m_rmlContext = nullptr;
    Rml::ElementDocument *m_rmlDocument = nullptr;
    Rml::Element *m_endpointInput = nullptr;
    Rml::Element *m_usernameInput = nullptr;
    Rml::Element *m_connectButton = nullptr;
    Rml::Element *m_pasteButton = nullptr;
    Rml::Element *m_errorText = nullptr;
    Rml::Element *m_statusText = nullptr;
    std::unique_ptr<RmlMenuListener> m_rmlListener;
    bool m_rmlDocumentVisible = false;
    bool m_rmlInputsInitialized = false;
};
