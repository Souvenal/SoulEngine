/// @file   Applications/TestApp.cppm
/// @brief  Demo/test application — self-registers with ApplicationFactory.

export module TestApp;

import Core;
import Application;
import Renderer;
import Scene;

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

        return {};
    }

    auto OnDetach() -> void override {
        LogInfo("TestApplication: Detaching...");
        if (m_Renderer) {
            m_Renderer->OnDetach();
            m_Renderer.reset();
        }
    }

    auto OnTick(float /*DeltaTime*/) -> void override {}
};

/// Auto-register with the application factory.
Application::ApplicationFactory::AutoRegistrar<TestApplication> RegTestApp{"Test"};

} // namespace SoulEngine
