module;

#include <entt/entt.hpp>
#include <imgui.h>

export module Editor.EditorCamera;

import std;
import Core;
import RHI;
import Scene;
import WindowSystem;
import EditorTypes;

namespace SoulEngine {

/// @brief Viewport change request for one editor camera.
export struct EditorCameraResizeEvent {
    entt::entity CameraEntity = entt::null;
    Uint32       X            = 0;
    Uint32       Y            = 0;
    Uint32       Width        = 0;
    Uint32       Height       = 0;
};

/// @brief Editor-only camera component with viewport and selection state.
///
/// Holds no view render targets: the editor view's targets are renderer-created pooled RenderGraph transients
/// derived from the Viewport.
export struct EditorCameraComponent {
    Float32                     FOV            = 60.0f;
    Float32                     NearPlane      = 0.1f;
    Float32                     FarPlane       = 100.0f;
    Float32                     ExposureEV100  = 15.0f;
    CameraViewport              Viewport       = {};
    std::optional<entt::entity> ReadbackEntity = std::nullopt;
    std::optional<entt::entity> SelectedEntity = std::nullopt;
    RHIRef<RHIReadback>         Readback       = nullptr;
};

/// @brief Editor-owned camera system and editor viewport state owner.
export class EditorCameraSystem final : public ISystem {
  public:
    /// @brief Create an editor camera system bound to one window system.
    explicit EditorCameraSystem(entt::registry& Registry, IWindowSystem& Window)
        : ISystem(Registry), m_Window(&Window) {}

    auto OnUpdate(Float32 DeltaTime) -> void override;

    /// @brief Subscribe to editor camera resize events.
    auto SetupObservers() -> void override;

    /// @brief Unsubscribe from editor camera resize events.
    auto TeardownObservers() -> void override;

    /// @brief Collect valid editor camera view snapshots.
    [[nodiscard]] auto CollectViews() const -> std::vector<EditorViewRecord>;

  private:
    auto               UpdatePerspective(Float32 DeltaTime) -> void;
    auto               UpdateSelection() -> void;
    auto               OnCameraResize(EditorCameraResizeEvent& Event) -> void;
    [[nodiscard]] auto GetAxis(ImGuiKey Positive, ImGuiKey Negative) const -> Float32;

    IWindowSystem* m_Window = nullptr;
};

} // namespace SoulEngine
