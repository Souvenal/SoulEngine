module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <imgui.h>

export module Editor:EditorCamera;

import std;
import Core;
import RHI;
import Scene;
import WindowSystem;
import EditorTypes;

export namespace SoulEngine {

/// @brief Render targets owned by one editor camera.
struct EditorCameraRenderTargets {
    CameraRenderTargets     Camera        = {};
    RHIRef<RHIRenderTarget> SelectionMask = nullptr;

    /// @brief Return whether every editor-camera render target is ready.
    [[nodiscard]] auto IsValid() const -> bool {
        return Camera.IsValid() && SelectionMask;
    }
};

/// @brief Resource loader for editor-camera render targets.
struct EditorCameraRenderTargetsLoader {
    using result_type = SPtr<EditorCameraRenderTargets>;

    auto operator()(StringView ResourceName, Uint32 Width, Uint32 Height) const -> result_type {
        if (Width == 0 || Height == 0)
            return nullptr;

        auto CameraTargets = CameraRenderTargetsLoader{}(ResourceName, Width, Height);
        if (!CameraTargets)
            return nullptr;

        auto SelectionMask = RHIRenderDevice::Get().CreateRenderTarget(
            Format("{}/SelectionMask", ResourceName),
            RHIRenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHIFormat::R8_UNORM,
                .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource,
            });
        if (!SelectionMask) {
            LogWarning("Failed to queue editor-camera selection-mask creation: {}", SelectionMask.error().ToString());
            return nullptr;
        }

        const auto HasQueuedCreation = [](const auto& Ref) -> bool {
            return Ref.GetState() != RHIRefState::Unknown;
        };
        const auto& GBuffer = CameraTargets->GBuffer;
        if (!HasQueuedCreation(GBuffer.AlbedoRT) || !HasQueuedCreation(GBuffer.NormalRT) ||
            !HasQueuedCreation(GBuffer.MaterialIdRT) || !HasQueuedCreation(GBuffer.EntityIdRT) ||
            !HasQueuedCreation(GBuffer.DepthRT) || !HasQueuedCreation(CameraTargets->SceneColorRT))
            return nullptr;

        return std::make_shared<EditorCameraRenderTargets>(EditorCameraRenderTargets{
            .Camera        = std::move(*CameraTargets),
            .SelectionMask = std::move(*SelectionMask),
        });
    }
};

using EditorCameraRenderTargetsCache  = entt::resource_cache<EditorCameraRenderTargets, EditorCameraRenderTargetsLoader>;
using EditorCameraRenderTargetsHandle = entt::resource<EditorCameraRenderTargets>;

/// @brief Resize request for one editor camera.
struct EditorCameraResizeEvent {
    entt::entity CameraEntity = entt::null;
    Uint32       Width        = 0;
    Uint32       Height       = 0;
};

/// @brief Editor-only camera component with viewport and selection state.
struct EditorCameraComponent {
    Float32                         FOV            = 60.0f;
    Float32                         NearPlane      = 0.1f;
    Float32                         FarPlane       = 100.0f;
    Float32                         ExposureEV100  = 15.0f;
    Uint32                          ViewportWidth  = 0;
    Uint32                          ViewportHeight = 0;
    EditorCameraRenderTargetsHandle Targets        = {};
    std::optional<entt::entity>    ReadbackEntity = std::nullopt;
    std::optional<entt::entity>    SelectedEntity = std::nullopt;
    RHIRef<RHIReadbackBuffer>      Readback       = nullptr;
};

/// @brief Editor-owned camera system and editor viewport resource owner.
class EditorCameraSystem final : public ISystem {
  public:
    /// @brief Create an editor camera system bound to one window system.
    explicit EditorCameraSystem(entt::registry& Registry, IWindowSystem& Window)
        : ISystem(Registry), m_Window(&Window) {}

    auto OnUpdate(Float32 DeltaTime) -> void override {
        m_Registry.ctx().get<entt::dispatcher>().update<EditorCameraResizeEvent>();
        UpdatePerspective(DeltaTime);
        UpdateSelection();
    }

    /// @brief Subscribe to editor camera resize events.
    auto SetupObservers() -> void override {
        m_Registry.ctx().get<entt::dispatcher>()
            .sink<EditorCameraResizeEvent>()
            .connect<&EditorCameraSystem::OnCameraResize>(*this);
    }

    /// @brief Unsubscribe from editor camera resize events.
    auto TeardownObservers() -> void override {
        m_Registry.ctx().get<entt::dispatcher>()
            .sink<EditorCameraResizeEvent>()
            .disconnect<&EditorCameraSystem::OnCameraResize>(*this);
    }

    /// @brief Collect valid editor camera view snapshots.
    [[nodiscard]] auto CollectViews() const -> std::vector<EditorViewRecord> {
        std::vector<EditorViewRecord> Views;
        for (const auto Entity : m_Registry.view<EditorCameraComponent, TransformComponent>()) {
            const auto& Camera    = m_Registry.get<EditorCameraComponent>(Entity);
            const auto& Transform = m_Registry.get<TransformComponent>(Entity);
            if (!Camera.Targets || !Camera.Targets->IsValid() || Camera.ViewportWidth == 0 ||
                Camera.ViewportHeight == 0)
                continue;

            const Float32 AspectRatio =
                static_cast<Float32>(Camera.ViewportWidth) / static_cast<Float32>(Camera.ViewportHeight);
            const Float32 FovRad = Camera.FOV * (std::numbers::pi_v<Float32> / 180.0f);
            const auto Projection = hlslpp::float4x4::perspective(hlslpp::projection(
                hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, Camera.NearPlane, Camera.FarPlane),
                hlslpp::zclip::zero,
                hlslpp::zdirection::forward,
                hlslpp::zplane::finite));

            const auto Position = hlslpp::float3(
                Transform.WorldTransform[3].x, Transform.WorldTransform[3].y, Transform.WorldTransform[3].z);
            const auto LocalForward = hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f);
            const auto WorldForward = hlslpp::mul(LocalForward, Transform.WorldTransform);
            const auto Forward = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
            const auto View =
                hlslpp::float4x4::look_at(Position, Position + Forward, hlslpp::float3(0.0f, 1.0f, 0.0f));

            Views.emplace_back(EditorViewRecord{
                .Camera = CameraViewRecord{
                    .ViewProjection = hlslpp::mul(View, Projection),
                    .CameraPosition = Position,
                    .ExposureEV100  = Camera.ExposureEV100,
                    .Targets        = Camera.Targets->Camera,
                },
                .SelectionMask = Camera.Targets->SelectionMask,
            });
        }
        return Views;
    }

  private:
    auto UpdatePerspective(Float32 DeltaTime) -> void {
        if (!ImGui::GetCurrentContext())
            return;
        const auto& IO = ImGui::GetIO();
        const bool CameraInputActive = !IO.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Right);
        m_Window->SetCursorMode(CameraInputActive ? CursorMode::Disabled : CursorMode::Normal);
        if (!CameraInputActive)
            return;

        const auto        CursorDelta  = IO.MouseDelta;
        const auto        ForwardAxis  = GetAxis(ImGuiKey_W, ImGuiKey_S);
        const auto        RightAxis    = GetAxis(ImGuiKey_D, ImGuiKey_A);
        const auto        VerticalAxis = GetAxis(ImGuiKey_E, ImGuiKey_Q);
        constexpr Float32 Sensitivity  = 0.0025f;
        constexpr Float32 MaxPitch     = 1.55334306f;

        for (const auto CameraEntity : m_Registry.view<EditorCameraComponent, TransformComponent>()) {
            const auto& CurrentTransform     = m_Registry.get<TransformComponent>(CameraEntity);
            const auto  EditorWorldTransform = CurrentTransform.GetLocalMatrix();
            const auto  WorldForward = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), EditorWorldTransform);
            const auto  Forward = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
            const auto  HorizontalForward = hlslpp::normalize(hlslpp::float3(Forward.x, 0.0f, Forward.z));
            const auto  Up                = hlslpp::float3(0.0f, 1.0f, 0.0f);
            const auto  Right             = hlslpp::normalize(hlslpp::cross(HorizontalForward, Up));
            const auto  MoveDirection     = HorizontalForward * ForwardAxis + Right * RightAxis + Up * VerticalAxis;

            m_Registry.patch<TransformComponent>(CameraEntity, [&](auto& Transform) -> void {
                if (MoveDirection.x != 0.0f || MoveDirection.y != 0.0f || MoveDirection.z != 0.0f)
                    Transform.Translation += hlslpp::normalize(MoveDirection) * (2.0f * DeltaTime);
                Transform.Translation += Forward * (IO.MouseWheel * 0.75f);
                Transform.Rotation.y  += CursorDelta.x * Sensitivity * (180.0f / std::numbers::pi_v<Float32>);
                Transform.Rotation.x =
                    std::clamp(static_cast<Float32>(Transform.Rotation.x) +
                                   CursorDelta.y * Sensitivity * (180.0f / std::numbers::pi_v<Float32>),
                               -MaxPitch * (180.0f / std::numbers::pi_v<Float32>),
                               MaxPitch * (180.0f / std::numbers::pi_v<Float32>));
            });
        }
    }

    auto UpdateSelection() -> void {
        if (!ImGui::GetCurrentContext())
            return;
        const auto& IO = ImGui::GetIO();
        for (const auto CameraEntity : m_Registry.view<EditorCameraComponent>()) {
            auto& Camera = m_Registry.get<EditorCameraComponent>(CameraEntity);
            if (Camera.Readback) {
                if (const auto EntityId = Camera.Readback->TryRead<Uint32>()) {
                    Camera.ReadbackEntity = *EntityId == GBuffer::BackgroundEntityId
                                                ? std::nullopt
                                                : std::optional<entt::entity>{static_cast<entt::entity>(*EntityId)};
                }
            }
            if (Camera.ViewportWidth == 0 || Camera.ViewportHeight == 0 || IO.DisplaySize.x <= 0.0f ||
                IO.DisplaySize.y <= 0.0f || IO.MousePos.x < 0.0f || IO.MousePos.y < 0.0f ||
                IO.MousePos.x >= IO.DisplaySize.x || IO.MousePos.y >= IO.DisplaySize.y)
                continue;
            if (!IO.WantCaptureMouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                Camera.SelectedEntity = Camera.ReadbackEntity;
            }
        }
    }

    auto OnCameraResize(EditorCameraResizeEvent& Event) -> void {
        if (!m_Registry.valid(Event.CameraEntity) || !m_Registry.all_of<EditorCameraComponent>(Event.CameraEntity))
            return;
        if (!m_Registry.all_of<NameComponent>(Event.CameraEntity)) {
            LogError("Editor camera entity {} has no NameComponent", entt::to_integral(Event.CameraEntity));
            return;
        }

        auto& Component = m_Registry.get<EditorCameraComponent>(Event.CameraEntity);
        if (Event.Width == 0 || Event.Height == 0) {
            Component.Targets        = {};
            Component.ViewportWidth  = 0;
            Component.ViewportHeight = 0;
            return;
        }

        if (Component.ViewportWidth == Event.Width && Component.ViewportHeight == Event.Height && Component.Targets &&
            Component.Targets->IsValid())
            return;

        const auto& Name = m_Registry.get<NameComponent>(Event.CameraEntity).Name;
        const auto  ResourceKey = Format("EditorCamera/{}/{}", Name, entt::to_integral(Event.CameraEntity));
        const auto  ResourceId  = entt::hashed_string{ResourceKey.data(), ResourceKey.size()};
        auto [It, Loaded] = m_Cache.force_load(ResourceId, ResourceKey, Event.Width, Event.Height);
        if (It->second) {
            Component.Targets        = It->second;
            Component.ViewportWidth  = Event.Width;
            Component.ViewportHeight = Event.Height;
        } else {
            LogError("Failed to load editor camera render targets for '{}'", ResourceKey);
        }
    }

    [[nodiscard]] auto GetAxis(ImGuiKey Positive, ImGuiKey Negative) const -> Float32 {
        return (ImGui::IsKeyDown(Positive) ? 1.0f : 0.0f) - (ImGui::IsKeyDown(Negative) ? 1.0f : 0.0f);
    }

    IWindowSystem*                 m_Window = nullptr;
    EditorCameraRenderTargetsCache m_Cache  = {};
};

} // namespace SoulEngine

