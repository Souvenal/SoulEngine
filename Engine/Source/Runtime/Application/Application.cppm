/// @file   Application/Application.cppm
/// @brief  Application abstraction layer — project identity and scene ownership.
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

export module Application;

import Core;
import Scene;

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
    /// the application, and loads its default scene document.
    ///
    /// @param Name  Application name matching an AutoRegistrar key (e.g. "Test").
    /// @return An owning pointer to the constructed application,
    ///         or an error description on failure.
    [[nodiscard]] static auto Create(StringView Name) -> std::expected<UPtr<Application>, ErrorMessage>;

    /// @brief The factory key this application was registered under.
    [[nodiscard]] auto GetName() const -> StringView {
        return m_Name;
    }

    /// @brief Mutable access to the active scene.
    /// Used by the GameLoop to update mutable scene data before building
    /// the next frame's SceneSnapshot.
    [[nodiscard]] auto GetScene() -> Scene& {
        return *m_Scene;
    }

    /// @brief Read-only access to the active scene.
    [[nodiscard]] auto GetScene() const -> const Scene& {
        return *m_Scene;
    }

  protected:
    String m_Name;
    Path   m_RootDirectory = {};
    UPtr<Scene> m_Scene = {};
};

// ═════════════════════════════════════════════════════════════════════════════
// ApplicationFactory — factory-backed application creation
// ═════════════════════════════════════════════════════════════════════════════

using ApplicationFactory = Factory<Application>;

} // namespace SoulEngine

namespace SoulEngine {

namespace {

UPtr<Application>     g_CurrentApplication = nullptr;

} // namespace

} // namespace SoulEngine

export namespace SoulEngine {

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
    App->m_RootDirectory = (ConfigManager::Get().ApplicationsRootDirPath() / Name).lexically_normal();
    const auto ScenePath = App->m_RootDirectory / "Scene.yaml";
    auto Loaded = Scene::LoadFromFile(ScenePath);
    if (!Loaded)
        return std::unexpected(Loaded.error().Append("Default Scene document load failed"));
    App->m_Scene = std::move(Loaded->first);
    for (const auto& Warning : Loaded->second.Warnings)
        LogWarning("Default Scene warning at '{}': {}", Warning.Path, Warning.Message);
    return App;
}

/// @brief Open an application immediately, preserving the current application
/// when the requested application cannot be created or loaded.
[[nodiscard]] auto OpenApplication(StringView Name) -> std::expected<void, ErrorMessage> {
    auto NewApplication = Application::Create(Name);
    if (!NewApplication)
        return std::unexpected(NewApplication.error().Append("Application creation failed"));

    g_CurrentApplication = std::move(*NewApplication);
    return {};
}

/// @brief Return the currently active application, or null before startup and
/// after shutdown.
[[nodiscard]] auto GetCurrentApplication() -> Application* {
    return g_CurrentApplication.get();
}

/// @brief Destroy the current application.
auto CloseApplication() -> void {
    g_CurrentApplication.reset();
}
 
} // namespace SoulEngine
