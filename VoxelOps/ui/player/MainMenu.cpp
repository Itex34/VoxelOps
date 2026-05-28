#include "MainMenu.hpp"

#include "../../application/AppHelpers.hpp"
#include "../../data/GameData.hpp"
#include "../../../Shared/runtime/Paths.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>

using namespace AppHelpers;

namespace {
    bool IsScancodeDown(SDL_Scancode scancode) {
        int keyCount = 0;
        const bool *keys = SDL_GetKeyboardState(&keyCount);
        return keys != nullptr && scancode < keyCount && keys[scancode];
    }

    template <size_t N>
    void CopyStringToArray(std::string_view text, std::array<char, N> &target) {
        std::memset(target.data(), 0, target.size());
        const size_t copyLen = std::min(text.size(), target.size() - 1);
        std::memcpy(target.data(), text.data(), copyLen);
    }
} // namespace

class MainMenu::RmlMenuListener final : public Rml::EventListener {
public:
    explicit RmlMenuListener(MainMenu *owner)
        : m_owner(owner) {}


    void setFrameContext(Runtime *runtime, const MainMenuContext &ctx) {
        m_runtime = runtime;
        m_ctx = ctx;
    }

    void ProcessEvent(Rml::Event &event) override {
        if (m_owner == nullptr || m_runtime == nullptr) {
            return;
        }
        m_owner->handleRmlEvent(*m_runtime, m_ctx, event);
    }

private:
    MainMenu *m_owner = nullptr;
    Runtime *m_runtime = nullptr;
    MainMenuContext m_ctx{};
};

MainMenu::MainMenu() = default;
MainMenu::~MainMenu() = default;

void MainMenu::draw(Runtime &runtime, const MainMenuContext &ctx) {
    if (runtime.network.clientNet.IsConnected()) {
        hide();
        return;
    }

    GameData::cursorEnabled = true;

    if (runtime.ui.rmlUi && runtime.ui.rmlUi->isUsingOpenGlBackend()) {
        drawRml(runtime, ctx);
        return;
    }

    drawImGui(runtime, ctx);
}

bool MainMenu::bindRmlContext(Runtime &runtime) {
    if (!runtime.ui.rmlUi || !runtime.ui.rmlUi->isUsingOpenGlBackend()) {
        if (m_rmlDocument) {
            resetRmlDocument();
        } else {
            forgetRmlState();
        }
        return false;
    }

    Rml::Context *context = runtime.ui.rmlUi->context();
    if (context == nullptr) {
        forgetRmlState();
        return false;
    }

    if (m_rmlContext == context && m_rmlDocument != nullptr) {
        return true;
    }

    if (m_rmlContext != nullptr && m_rmlContext != context) {
        forgetRmlState();
    } else {
        resetRmlDocument();
    }
    m_rmlContext = context;

    const std::string mainMenuPath =
        Shared::RuntimePaths::ResolveVoxelOpsPath("ui/rml/documents/main_menu.rml").generic_string();
    m_rmlDocument = m_rmlContext->LoadDocument(mainMenuPath);
    if (m_rmlDocument == nullptr) {
        return false;
    }

    m_endpointInput = m_rmlDocument->GetElementById("endpoint_input");
    m_usernameInput = m_rmlDocument->GetElementById("username_input");
    m_connectButton = m_rmlDocument->GetElementById("connect_button");
    m_pasteButton = m_rmlDocument->GetElementById("paste_button");
    m_errorText = m_rmlDocument->GetElementById("error_text");
    m_statusText = m_rmlDocument->GetElementById("status_text");
    Rml::Element *panel = m_rmlDocument->GetElementById("panel");
    Rml::Element *title = m_rmlDocument->GetElementById("title");
    Rml::Element *hint = m_rmlDocument->GetElementById("hint_text");

    if (!m_endpointInput || !m_usernameInput || !m_connectButton || !m_pasteButton || !m_errorText ||
        !m_statusText) {
        resetRmlDocument();
        return false;
    }

    const std::array<Rml::Element *, 9> fontTargets{
        m_rmlDocument,
        panel,
        title,
        hint,
        m_endpointInput,
        m_usernameInput,
        m_connectButton,
        m_pasteButton,
        m_statusText
    };
    for (Rml::Element *element : fontTargets) {
        if (!element) {
            continue;
        }
        element->SetProperty("font-family", "\"SF Pro Text\"");
        element->SetProperty("font-size", "16px");
    }

    m_rmlListener = std::make_unique<RmlMenuListener>(this);
    m_connectButton->AddEventListener("click", m_rmlListener.get());
    m_pasteButton->AddEventListener("click", m_rmlListener.get());
    m_endpointInput->AddEventListener("keydown", m_rmlListener.get());
    m_usernameInput->AddEventListener("keydown", m_rmlListener.get());
    m_rmlDocument->Show();
    m_rmlDocumentVisible = true;
    m_rmlInputsInitialized = false;
    return true;
}

void MainMenu::resetRmlDocument() {
    if (m_connectButton && m_rmlListener) {
        m_connectButton->RemoveEventListener("click", m_rmlListener.get());
    }
    if (m_pasteButton && m_rmlListener) {
        m_pasteButton->RemoveEventListener("click", m_rmlListener.get());
    }
    if (m_endpointInput && m_rmlListener) {
        m_endpointInput->RemoveEventListener("keydown", m_rmlListener.get());
    }
    if (m_usernameInput && m_rmlListener) {
        m_usernameInput->RemoveEventListener("keydown", m_rmlListener.get());
    }

    if (m_rmlDocument) {
        m_rmlDocument->Close();
    }

    forgetRmlState();
}

void MainMenu::forgetRmlState() {
    m_rmlListener.reset();
    m_endpointInput = nullptr;
    m_usernameInput = nullptr;
    m_connectButton = nullptr;
    m_pasteButton = nullptr;
    m_errorText = nullptr;
    m_statusText = nullptr;
    m_rmlDocument = nullptr;
    m_rmlContext = nullptr;
    m_rmlDocumentVisible = false;
    m_rmlInputsInitialized = false;
}

void MainMenu::syncRmlState(Runtime &runtime) {
    if (!m_rmlDocument || !m_endpointInput || !m_usernameInput || !m_connectButton || !m_pasteButton) {
        return;
    }

    const ClientNetwork::ConnectionState connState = runtime.network.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);

    auto *endpointInput = static_cast<Rml::ElementFormControlInput *>(m_endpointInput);
    auto *usernameInput = static_cast<Rml::ElementFormControlInput *>(m_usernameInput);

    if (endpointInput && usernameInput) {
        if (!m_rmlInputsInitialized) {
            endpointInput->SetValue(runtime.app.connection.pendingServerEndpointInput.data());
            usernameInput->SetValue(runtime.app.connection.pendingUsernameInput.data());
            m_rmlInputsInitialized = true;
        }

        CopyStringToArray(endpointInput->GetValue(), runtime.app.connection.pendingServerEndpointInput);
        CopyStringToArray(usernameInput->GetValue(), runtime.app.connection.pendingUsernameInput);
    }

    auto setDisabled = [isConnecting](Rml::Element *element) {
        if (!element) {
            return;
        }
        if (isConnecting) {
            element->SetAttribute("disabled", "");
        } else {
            element->RemoveAttribute("disabled");
        }
    };

    setDisabled(m_endpointInput);
    setDisabled(m_usernameInput);
    setDisabled(m_pasteButton);
    setDisabled(m_connectButton);
    m_connectButton->SetInnerRML(isConnecting ? "Connecting..." : "Connect");

    if (m_errorText) {
        m_errorText->SetInnerRML(runtime.app.connection.usernamePromptError);
    }
    if (m_statusText) {
        m_statusText->SetInnerRML("Status: " + runtime.network.clientNet.GetConnectionStatusText());
    }
}

void MainMenu::handlePaste(Runtime &runtime) {
    char *clipboardText = SDL_GetClipboardText();
    if (clipboardText == nullptr || clipboardText[0] == '\0') {
        if (clipboardText != nullptr) {
            SDL_free(clipboardText);
        }
        runtime.app.connection.usernamePromptError = "Clipboard is empty.";
        return;
    }

    const std::string endpoint = TrimAscii(clipboardText);
    SDL_free(clipboardText);
    if (endpoint.empty()) {
        runtime.app.connection.usernamePromptError = "Clipboard is empty.";
        return;
    }

    CopyStringToArray(endpoint, runtime.app.connection.pendingServerEndpointInput);
    if (m_endpointInput != nullptr) {
        auto *endpointInput = static_cast<Rml::ElementFormControlInput *>(m_endpointInput);
        endpointInput->SetValue(endpoint);
    }
}

void MainMenu::handleSubmit(Runtime &runtime, const MainMenuContext &ctx) {
    std::string desiredEndpoint = runtime.app.connection.pendingServerEndpointInput.data();
    std::string parsedIp;
    uint16_t parsedPort = 0;
    if (!ParseServerEndpoint(desiredEndpoint, parsedIp, parsedPort)) {
        runtime.app.connection.usernamePromptError =
            "Server must be host:port (example: 127.0.0.1:27015).";
        if (ctx.windowHost != nullptr) {
            ctx.windowHost->applyMouseInputModes();
        }
        return;
    }

    std::string desiredUsername = runtime.app.connection.pendingUsernameInput.data();
    size_t begin = 0;
    while (begin < desiredUsername.size() &&
           std::isspace(static_cast<unsigned char>(desiredUsername[begin])) != 0) {
        ++begin;
    }
    size_t end = desiredUsername.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(desiredUsername[end - 1])) != 0) {
        --end;
    }
    desiredUsername = desiredUsername.substr(begin, end - begin);

    if (desiredUsername.empty()) {
        runtime.app.connection.usernamePromptError = "Please enter a username.";
        if (ctx.windowHost != nullptr) {
            ctx.windowHost->applyMouseInputModes();
        }
        return;
    }

    if (desiredUsername.size() > kMaxConnectUsernameChars) {
        desiredUsername.resize(kMaxConnectUsernameChars);
    }

    if (ctx.serverIp) {
        *ctx.serverIp = parsedIp;
    }
    if (ctx.serverPort) {
        *ctx.serverPort = parsedPort;
    }

    const std::string endpoint = parsedIp + ":" + std::to_string(parsedPort);
    CopyStringToArray(endpoint, runtime.app.connection.pendingServerEndpointInput);

    if (ctx.requestedUsername) {
        *ctx.requestedUsername = desiredUsername;
    }
    CopyStringToArray(desiredUsername, runtime.app.connection.pendingUsernameInput);

    runtime.app.connection.usernamePromptError.clear();
    if (ctx.connectionHost == nullptr || !ctx.connectionHost->beginConnectionAttempt(runtime)) {
        runtime.app.connection.usernamePromptError =
            "Failed to start connection. Check server reachability and retry.";
    }
    runtime.app.connection.nextReconnectAttemptTime = GetTimeSeconds() + 1.0;

    if (ctx.windowHost != nullptr) {
        ctx.windowHost->applyMouseInputModes();
    }
}

void MainMenu::handleRmlEvent(Runtime &runtime, const MainMenuContext &ctx, Rml::Event &event) {
    const ClientNetwork::ConnectionState connState = runtime.network.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);

    if (event.GetType() == "click") {
        const Rml::Element *source = event.GetCurrentElement();
        if (source == m_pasteButton && !isConnecting) {
            handlePaste(runtime);
        } else if (source == m_connectButton && !isConnecting) {
            handleSubmit(runtime, ctx);
        }
        return;
    }

    if (event.GetType() == "keydown" && !isConnecting) {
        const int key = event.GetParameter<int>("key_identifier", 0);
        if (static_cast<Rml::Input::KeyIdentifier>(key) == Rml::Input::KI_RETURN) {
            handleSubmit(runtime, ctx);
        }
    }
}

void MainMenu::drawRml(Runtime &runtime, const MainMenuContext &ctx) {
    if (!bindRmlContext(runtime)) {
        drawImGui(runtime, ctx);
        return;
    }

    if (m_rmlListener) {
        m_rmlListener->setFrameContext(&runtime, ctx);
    }

    if (m_rmlDocument && !m_rmlDocumentVisible) {
        m_rmlDocument->Show();
        m_rmlDocumentVisible = true;
    }

    syncRmlState(runtime);
    if (ctx.windowHost != nullptr) {
        ctx.windowHost->applyMouseInputModes();
    }
}

void MainMenu::drawImGui(Runtime &runtime, const MainMenuContext &ctx) {
    if (!runtime.ui.debugUi) {
        return;
    }

    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 windowSize(460.0f, 0.0f);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Always,
        ImVec2(0.5f, 0.5f)
    );

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;

    if (!ImGui::Begin("Connect", nullptr, flags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Server (host:port)");
    const float pasteButtonWidth =
        ImGui::CalcTextSize("Paste").x + (ImGui::GetStyle().FramePadding.x * 2.0f);
    const float endpointFieldWidth =
        ImGui::GetContentRegionAvail().x - pasteButtonWidth - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(endpointFieldWidth > 60.0f ? endpointFieldWidth : -1.0f);
    const ClientNetwork::ConnectionState connState = runtime.network.clientNet.GetConnectionState();
    const bool isConnecting = (connState == ClientNetwork::ConnectionState::Connecting);
    const auto pasteEndpointFromClipboard = [&]() -> bool {
        if (ctx.window == nullptr) {
            return false;
        }
        char *clipboardText = SDL_GetClipboardText();
        if (clipboardText == nullptr || clipboardText[0] == '\0') {
            if (clipboardText != nullptr) {
                SDL_free(clipboardText);
            }
            return false;
        }
        const std::string endpoint = TrimAscii(clipboardText);
        SDL_free(clipboardText);
        if (endpoint.empty()) {
            return false;
        }
        CopyStringToArray(endpoint, runtime.app.connection.pendingServerEndpointInput);
        return true;
    };

    bool submit = false;
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText(
            "##server_endpoint_input",
            runtime.app.connection.pendingServerEndpointInput.data(),
            runtime.app.connection.pendingServerEndpointInput.size(),
            ImGuiInputTextFlags_EnterReturnsTrue
        )) {
        submit = true;
    }
    const bool endpointFieldActive = ImGui::IsItemActive();
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    bool pasteShortcutPressed = false;
    if (endpointFieldActive && !isConnecting) {
        const bool ctrlDown =
            IsScancodeDown(SDL_SCANCODE_LCTRL) || IsScancodeDown(SDL_SCANCODE_RCTRL) ||
            IsScancodeDown(SDL_SCANCODE_LGUI) || IsScancodeDown(SDL_SCANCODE_RGUI);
        const bool shiftDown =
            IsScancodeDown(SDL_SCANCODE_LSHIFT) || IsScancodeDown(SDL_SCANCODE_RSHIFT);
        const bool pasteCtrlV = ctrlDown && IsScancodeDown(SDL_SCANCODE_V);
        const bool pasteShiftInsert = shiftDown && IsScancodeDown(SDL_SCANCODE_INSERT);
        pasteShortcutPressed = pasteCtrlV || pasteShiftInsert;
        if (pasteShortcutPressed && !runtime.app.connection.wasEndpointPasteShortcutPressed) {
            if (!pasteEndpointFromClipboard()) {
                runtime.app.connection.usernamePromptError = "Clipboard is empty.";
            }
        }
    }
    runtime.app.connection.wasEndpointPasteShortcutPressed = pasteShortcutPressed;

    ImGui::SameLine();
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Paste")) {
        if (!pasteEndpointFromClipboard()) {
            runtime.app.connection.usernamePromptError = "Clipboard is empty.";
        }
    }
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Enter username");
    ImGui::SetNextItemWidth(-1.0f);
    if (isConnecting) {
        ImGui::BeginDisabled();
    }
    if (ImGui::InputText(
            "##username_input",
            runtime.app.connection.pendingUsernameInput.data(),
            runtime.app.connection.pendingUsernameInput.size(),
            ImGuiInputTextFlags_EnterReturnsTrue
        )) {
        submit = true;
    }
    if (isConnecting) {
        ImGui::EndDisabled();
    }

    if (!isConnecting) {
        if (ImGui::Button("Connect")) {
            submit = true;
        }
    } else {
        ImGui::BeginDisabled();
        ImGui::Button("Connecting...");
        ImGui::EndDisabled();
    }

    if (submit) {
        handleSubmit(runtime, ctx);
    }

    if (!runtime.app.connection.usernamePromptError.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(
            ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", runtime.app.connection.usernamePromptError.c_str()
        );
    }

    ImGui::Spacing();
    ImGui::TextWrapped("If that username is already taken, enter a different username and retry.");
    ImGui::Text("Status: %s", runtime.network.clientNet.GetConnectionStatusText().c_str());

    ImGui::End();
    if (ctx.windowHost != nullptr) {
        ctx.windowHost->applyMouseInputModes();
    }
}

void MainMenu::hide() {
    if (m_rmlDocument && m_rmlDocumentVisible) {
        m_rmlDocument->Hide();
        m_rmlDocumentVisible = false;
    }
}
