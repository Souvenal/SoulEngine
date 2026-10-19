module;

module Application;

import Core;
import Scene;
import std;

namespace SoulEngine {

// Application::Create — static factory

namespace {

UPtr<Application> g_CurrentApplication = nullptr;

} // namespace

[[nodiscard]] auto Application::Create(StringView Name) -> std::expected<UPtr<Application>, ErrorMessage> {
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
    App->m_Name          = String(Name);
    App->m_RootDirectory = (EnginePaths::Get().ApplicationsRootDirPath() / Name).lexically_normal();
    const auto ScenePath = App->m_RootDirectory / "Scene.yaml";
    auto       Loaded    = Scene::LoadFromFile(ScenePath);
    if (!Loaded)
        return std::unexpected(Loaded.error().Append("Default Scene document load failed"));
    App->m_Scene = std::move(Loaded->first);
    for (const auto& Warning : Loaded->second.Warnings)
        LogWarning("Default Scene warning at '{}': {}", Warning.Location, Warning.Message);
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
