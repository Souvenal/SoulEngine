/// @file   Application/Application.cppm
/// @brief  Application abstraction layer — project identity and scene ownership.
///
/// Applications self-register with ApplicationFactory via AutoRegistrar in standalone modules (under
/// Applications/). The facade never imports or constructs concrete application types directly.
///
/// Adding a new application requires:
///   1. Create Applications/MyApp.cppm with `export module MyApp;`
///   2. `ApplicationFactory::AutoRegistrar<MyApp> Reg{"MyApp"};`
///   3. Done — zero changes to Application.cppm

module;

export module Application;

import Core;
import Scene;

import std;

export namespace SoulEngine {

// Application — abstract base class

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
    /// Looks up @p Name in the self-registering ApplicationFactory, constructs the application, and loads its
    /// default scene document.
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
    /// Used by the GameLoop to update mutable scene data before building the next frame's SceneSnapshot.
    [[nodiscard]] auto GetScene() -> Scene& {
        return *m_Scene;
    }

    /// @brief Read-only access to the active scene.
    [[nodiscard]] auto GetScene() const -> const Scene& {
        return *m_Scene;
    }

  protected:
    String      m_Name;
    Path        m_RootDirectory = {};
    UPtr<Scene> m_Scene         = {};
};

// ApplicationFactory — factory-backed application creation

using ApplicationFactory = Factory<Application>;

} // namespace SoulEngine

export namespace SoulEngine {

[[nodiscard]] auto OpenApplication(StringView Name) -> std::expected<void, ErrorMessage>;

[[nodiscard]] auto GetCurrentApplication() -> Application*;

auto CloseApplication() -> void;

} // namespace SoulEngine
