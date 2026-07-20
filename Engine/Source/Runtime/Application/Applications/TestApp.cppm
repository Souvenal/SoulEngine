/// @file   Applications/TestApp.cppm
/// @brief  Demo/test application — self-registers with ApplicationFactory.

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

        m_Renderer = std::make_unique<Renderer::TestRenderer>();
        if (auto R = m_Renderer->OnAttach(); !R)
            return std::unexpected(R.error().Append("TestRenderer OnAttach failed"));

        m_Scene.m_Meshes.emplace_back(Resource::Manager::Get().RequestMeshRef(
            (ConfigManager::Get().CurrentApplicationDir() / "Assets" / "teapot.obj").string()));

        return {};
    }

    auto OnDetach() -> void override {
        LogInfo("TestApplication: Detaching...");
        if (m_Renderer) {
            m_Renderer->OnDetach();
            m_Renderer.reset();
        }
        m_Scene.m_Meshes.clear();
    }

    auto OnTick(float DeltaTime, WindowDisplay& Window) -> void override {
        const float Forward = (Window.IsKeyPressed(WindowKey::W) ? 1.0f : 0.0f) -
                              (Window.IsKeyPressed(WindowKey::S) ? 1.0f : 0.0f);
        const float Right = (Window.IsKeyPressed(WindowKey::D) ? 1.0f : 0.0f) -
                            (Window.IsKeyPressed(WindowKey::A) ? 1.0f : 0.0f);
        const float Vertical = (Window.IsKeyPressed(WindowKey::E) ? 1.0f : 0.0f) -
                               (Window.IsKeyPressed(WindowKey::Q) ? 1.0f : 0.0f);
        m_Scene.m_Camera.Move(Forward, Right, Vertical, Window.ConsumeScrollDelta(), DeltaTime);
        const auto CursorDelta = Window.ConsumeCursorDelta();
        m_Scene.m_Camera.Rotate(CursorDelta.X, CursorDelta.Y);
    }
};

/// Auto-register with the application factory.
Application::ApplicationFactory::AutoRegistrar<TestApplication> RegTestApp{"Test"};

} // namespace SoulEngine
