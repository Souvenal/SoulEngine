/// @file   Applications/VikingApp.cppm
/// @brief  Viking room showcase application — self-registers with ApplicationFactory.

module;

// Required while Scene exposes entt::registry in its object layout. VikingApplication's
// Application base can instantiate Scene lifetime operations in this module.
#include <entt/entt.hpp>

export module VikingApp;

import Application;
import Scene;
import WindowSystem;

namespace SoulEngine {

class VikingApplication final : public Application {
  public:
    VikingApplication() = default;

    auto OnTick(float DeltaTime, IWindowSystem& Window) -> void override {
        const bool CameraInputActive = Window.IsMouseButtonPressed(WindowMouseButton::Right);
        Window.SetCursorCaptured(CameraInputActive);
        if (!CameraInputActive) {
            (void)Window.ConsumeScrollDelta();
            (void)Window.ConsumeCursorDelta();
            return;
        }

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
ApplicationFactory::AutoRegistrar<VikingApplication> RegVikingApp{"Viking"};

} // namespace SoulEngine
