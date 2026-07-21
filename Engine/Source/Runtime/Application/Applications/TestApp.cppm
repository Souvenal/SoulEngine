/// @file   Applications/TestApp.cppm
/// @brief  Demo/test application — self-registers with ApplicationFactory.

module;

// Required while Scene exposes entt::registry in its object layout. TestApplication's
// Application base can instantiate Scene lifetime operations in this module.
#include <entt/entt.hpp>

export module TestApp;

import Core;
import Application;
import Renderer;
import Resource;
import Scene;
import Window;

namespace SoulEngine {

using namespace SoulEngine::Core;

class TestApplication final : public Application::Application {
  public:
    TestApplication() = default;

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        LogInfo("TestApplication: Attaching...");

        auto CreatedRenderer = Renderer::CreateDefault();
        if (!CreatedRenderer)
            return std::unexpected(CreatedRenderer.error().Append("TestApplication renderer creation failed"));
        m_Renderer = std::move(*CreatedRenderer);
        if (auto R = m_Renderer->OnAttach(); !R)
            return std::unexpected(R.error().Append("Configured renderer OnAttach failed"));

        const auto ScenePath = ConfigManager::Get().CurrentApplicationDir() / "Assets" / "TestScene.yaml";
        auto Loaded = m_Scene.LoadFromFile(ScenePath);
        if (!Loaded)
            return std::unexpected(Loaded.error().Append("Test scene load failed"));
        for (const auto& Warning : Loaded->Warnings)
            LogWarning("Test scene warning at '{}': {}", Warning.Path, Warning.Message);

        return {};
    }

    auto OnDetach() -> void override {
        LogInfo("TestApplication: Detaching...");
        if (m_Renderer) {
            m_Renderer->OnDetach();
            m_Renderer.reset();
        }

    }

    auto OnTick(float DeltaTime, WindowDisplay& Window) -> void override {
        const float Forward = (Window.IsKeyPressed(WindowKey::W) ? 1.0f : 0.0f) -
                              (Window.IsKeyPressed(WindowKey::S) ? 1.0f : 0.0f);
        const float Right = (Window.IsKeyPressed(WindowKey::D) ? 1.0f : 0.0f) -
                            (Window.IsKeyPressed(WindowKey::A) ? 1.0f : 0.0f);
        const float Vertical = (Window.IsKeyPressed(WindowKey::E) ? 1.0f : 0.0f) -
                               (Window.IsKeyPressed(WindowKey::Q) ? 1.0f : 0.0f);
        m_Scene.MoveFirstCamera(Forward, Right, Vertical, Window.ConsumeScrollDelta(), DeltaTime);
        const auto CursorDelta = Window.ConsumeCursorDelta();
        m_Scene.RotateFirstCamera(CursorDelta.X, CursorDelta.Y);
    }
};

/// Auto-register with the application factory.
Application::ApplicationFactory::AutoRegistrar<TestApplication> RegTestApp{"Test"};

} // namespace SoulEngine
