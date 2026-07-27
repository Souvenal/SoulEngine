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

module;

// Required while Scene exposes entt::registry in its object layout. Application owns Scene
// and can instantiate its lifetime operations, so EnTT must be directly reachable here.
#include <entt/entt.hpp>

export module Application;

import Core;
import RHI;
import Renderer;
import Resource;
import Scene;
import WindowSystem;

export import std;

export namespace SoulEngine {

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
    /// The base class owns the default scene and renderer lifecycle.
    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage>;

    /// @brief Called when this application is detached from the engine loop.
    /// Derived classes destroy the renderer and release owned resources here.
    /// The base class detaches and releases the default renderer.
    auto OnDetach() -> void;

    /// @brief Per-frame application update (game logic, simulation).
    virtual auto OnTick(float DeltaTime, IWindowSystem& Window) -> void = 0;

    /// @brief The factory key this application was registered under.
    [[nodiscard]] auto GetName() const -> StringView {
        return m_Name;
    }

    /// @brief Mutable access to the active scene.
    /// Used by the GameLoop to update mutable scene data before building
    /// the next frame's SceneSnapshot.
    [[nodiscard]] auto GetScene() -> Scene& {
        return m_Scene;
    }

    /// @brief Read-only access to the active scene.
    [[nodiscard]] auto GetScene() const -> const Scene& {
        return m_Scene;
    }

    /// @brief Access the renderer.
    /// Valid after OnAttach() succeeds and before OnDetach() returns.
    [[nodiscard]] auto GetRenderer() -> IRenderer& {
        return *m_Renderer;
    }

    /// @brief Render the current scene.  Non-virtual — fixed pipeline.
    /// Calls m_Renderer->Render(m_Scene.BuildSnapshot()) and logs on failure.
    auto OnRender() -> void;

  protected:
    String                    m_Name;
    Scene              m_Scene;
    UPtr<IRenderer> m_Renderer = nullptr;
};

// ═════════════════════════════════════════════════════════════════════════════
// ApplicationFactory — factory-backed application creation
// ═════════════════════════════════════════════════════════════════════════════

using ApplicationFactory = Factory<Application>;

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
    App->m_Name = String(Name);
    ConfigManager::Get().SetCurrentApplicationDir(ConfigManager::Get().ApplicationsRootDirPath() / Name);
    return App;
}

// ════════════════════════════════════════════════════════════════
// Application::OnAttach / OnDetach — default lifecycle
// ═════════════════════════════════════════════════════════════════

inline auto Application::OnAttach() -> std::expected<void, ErrorMessage> {
    const auto ScenePath = ConfigManager::Get().CurrentApplicationDir() / "Scene.yaml";
    auto Loaded = m_Scene.LoadFromFile(ScenePath);
    if (!Loaded)
        return std::unexpected(Loaded.error().Append("Default Scene document load failed"));
    for (const auto& Warning : Loaded->Warnings)
        LogWarning("Default Scene warning at '{}': {}", Warning.Path, Warning.Message);

    auto CreatedRenderer = CreateDefault();
    if (!CreatedRenderer)
        return std::unexpected(CreatedRenderer.error().Append("Default renderer creation failed"));
    m_Renderer = std::move(*CreatedRenderer);
    if (auto R = m_Renderer->OnAttach(); !R) {
        OnDetach();
        return std::unexpected(R.error().Append("Default renderer OnAttach failed"));
    }

    return {};
}

inline auto Application::OnDetach() -> void {
    if (!m_Renderer)
        return;
    m_Renderer->OnDetach();
    m_Renderer.reset();
}

// Application::OnRender — non-virtual render pipeline
// ═════════════════════════════════════════════════════════════════════════════

inline auto Application::OnRender() -> void {
    if (!m_Renderer)
        return;

    if (auto R = m_Renderer->Render(m_Scene.BuildSnapshot()); !R)
        LogError("Render failed:\n{}", R.error().ToString());
}

} // namespace SoulEngine
