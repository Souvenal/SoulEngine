/// @file   Application/Application.cppm
/// @brief  Application abstraction layer — lifecycle, game tick, scene ownership.
///
/// Applications self-register with ApplicationFactory via AutoRegistrar in
/// standalone modules (under Applications/).  The facade never imports or
/// constructs concrete application types directly.
///
/// Adding a new application requires:
///   1. Create Applications/MyApp.cppm with `export module MyApp;`
///   2. `ApplicationFactory::AutoRegistrar<MyApp> Reg{"MyApp"};`
///   3. Done — zero changes to Application.cppm

export module Application;

import Core;
import Renderer;
import Scene;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Application {

// ═════════════════════════════════════════════════════════════════════════════
// Application — abstract base class
// ═════════════════════════════════════════════════════════════════════════════

class Application {
  public:
    Application()          = default;
    virtual ~Application() = default;

    Application(const Application&)                    = delete;
    auto operator=(const Application&) -> Application& = delete;
    Application(Application&&)                         = delete;
    auto operator=(Application&&) -> Application&      = delete;

    /// @brief Static factory — creates an application by name.
    ///
    /// Looks up @p Name in the self-registering ApplicationFactory, constructs
    /// the application.  Does NOT call OnAttach() — EngineLoop controls the
    /// attach/detach lifecycle.
    ///
    /// @param Name  Application name matching an AutoRegistrar key (e.g. "Test").
    /// @return An owning pointer to the constructed application,
    ///         or an error description on failure.
    [[nodiscard]] static auto Create(StringView Name) -> std::expected<UPtr<Application>, ErrorMessage>;

    /// @brief Called when this application is attached to the engine loop.
    /// Derived classes construct the scene and renderer here.
    [[nodiscard]] virtual auto OnAttach() -> std::expected<void, ErrorMessage> = 0;

    /// @brief Called when this application is detached from the engine loop.
    /// Derived classes destroy the renderer and release owned resources here.
    virtual auto OnDetach() -> void = 0;

    /// @brief Per-frame application update (game logic, simulation).
    virtual auto OnTick(float DeltaTime) -> void = 0;

    /// @brief Render the current scene.  Non-virtual — fixed pipeline.
    /// Calls m_Renderer->Render(m_Scene) and logs on failure.
    auto OnRender() -> void;

  protected:
    Scene::Scene              m_Scene;
    UPtr<Renderer::IRenderer> m_Renderer = nullptr;
};

// ═════════════════════════════════════════════════════════════════════════════
// ApplicationFactory — factory-backed application creation
// ═════════════════════════════════════════════════════════════════════════════

using ApplicationFactory = Core::Factory<Application>;

// ═════════════════════════════════════════════════════════════════════════════
// Application::Create — static factory
// ═════════════════════════════════════════════════════════════════════════════

[[nodiscard]] inline auto Application::Create(StringView Name) -> std::expected<UPtr<Application>, ErrorMessage> {
    auto App = ApplicationFactory::Get().Create(Name);
    if (!App) {
        String Supported;
        auto   Names = ApplicationFactory::Get().Keys();
        for (std::size_t i = 0; i < Names.size(); ++i) {
            if (i > 0)
                Supported += ", ";
            Supported += Names[i];
        }
        return std::unexpected(ErrorMessage(Format("Unknown application: '{}'. Available: {}", Name, Supported)));
    }
    return App;
}

// ═════════════════════════════════════════════════════════════════════════════
// Application::OnRender — non-virtual render pipeline
// ═════════════════════════════════════════════════════════════════════════════

inline auto Application::OnRender() -> void {
    if (!m_Renderer)
        return;

    if (auto R = m_Renderer->Render(m_Scene); !R)
        LogError("Render failed:\n{}", R.error().ToString());
}

} // namespace SoulEngine::Application
