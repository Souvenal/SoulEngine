module;
#include <hlsl++.h>
#include <entt/entity/entity.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_threaded_rendering.h>
#include <magic_enum/magic_enum.hpp>

export module Editor;

import :MainMenu;
import :UIManager;
import :UIPanels;

import Core;
import RHI;
import Resource;
import Scene;
import WindowSystem;

export import std;

export namespace SoulEngine {

/// @brief Callback that draws one UI panel with ImGui immediate-mode calls.
/// Invoked on the engine main thread between NewFrame() and Render().
using UIPanelCallback = std::function<void()>;

/// @brief One registered UI panel in display order.
struct UIPanel {
    String          Name;
    UIPanelCallback Callback;
};

/// @brief Editor UI subsystem: owns the ImGui context, the UI panel registry,
/// and all GPU resources needed to render ImGui draw data through the RHI
/// command list.
///
/// Threading: the ImGui context lives on the engine main thread, where the
/// GLFW platform backend feeds it input during event polling. Each game tick
/// BuildFrame() produces a self-owning UIDrawFrame snapshot published through
/// a latest-wins mailbox; the render thread consumes snapshots in OnRender()
/// and emits UI passes. Backend-native GPU objects (font atlas texture,
/// dynamic buffers) are created by a one-shot RHI-thread task enqueued at
/// BindWindowSystem(); the render thread observes readiness through atomics.
class Editor {
  public:
    Editor() = default;
    ~Editor() {
        Shutdown();
    }

    Editor(const Editor&)                    = delete;
    auto operator=(const Editor&) -> Editor& = delete;
    Editor(Editor&&)                         = delete;
    auto operator=(Editor&&) -> Editor&      = delete;

    /// @brief Create the owned Dear ImGui context.
    [[nodiscard]] auto Create() -> std::expected<void, ErrorMessage> {
        if (m_ImGuiContext)
            return std::unexpected(ErrorMessage("Editor ImGui context is already created"));

        IMGUI_CHECKVERSION();
        m_ImGuiContext = ImGui::CreateContext();
        if (!m_ImGuiContext)
            return std::unexpected(ErrorMessage("Editor ImGui::CreateContext failed"));

        ImGui::SetCurrentContext(m_ImGuiContext);
        ImGui::StyleColorsDark();
        
        // 注册所有 UI 面板
        RegisterAllUI();
        return {};
    }

    /// @brief Bind the ImGui platform backend to the main window system.
    /// Must be called after Create() and ResourceManager::Init().
    /// Also selects the complete window-system and RHI backend combination.
    [[nodiscard]] auto BindPresentation(IWindowSystem* WindowSys, RHIRenderDevice* RenderDevice)
        -> std::expected<void, ErrorMessage> {
        if (!m_ImGuiContext)
            return std::unexpected(ErrorMessage("Editor ImGui context is null"));
        if (m_BoundWindowSystem)
            return std::unexpected(ErrorMessage("Editor window system is already bound"));
        if (m_BoundRenderDevice)
            return std::unexpected(ErrorMessage("Editor render device is already bound"));

        if (!WindowSys || !WindowSys->IsValid())
            return std::unexpected(ErrorMessage("Editor requires a valid window system"));
        if (!RenderDevice)
            return std::unexpected(ErrorMessage("Editor requires a render device"));

        const auto RHIType = RenderDevice->GetBackendType();
        switch (RHIType) {
        case RHIBackendType::Vulkan:
            m_TextureQueue.UpdateTexFunc = ImGui_ImplVulkan_UpdateTexture;
            break;
        case RHIBackendType::Unknown:
            return std::unexpected(
                ErrorMessage(Format("Editor cannot use RHI backend type '{}'", magic_enum::enum_name(RHIType))));
        default:
            return std::unexpected(
                ErrorMessage(Format("Editor does not support RHI backend type '{}'", magic_enum::enum_name(RHIType))));
        }

        m_BoundWindowSystem = WindowSys;
        m_BoundRenderDevice = RenderDevice;
        return {};
    }

    /// @brief Release GPU resource refs. Must be called on the engine main
    /// thread after the render/RHI threads have joined, before
    /// ResourceManager::Clear() and RenderDevice::Destroy().
    auto ReleaseRHIResources() -> void {
        if (m_ImGuiContext) {
            std::scoped_lock Lock(m_TextureQueueMutex);
            m_TextureQueue.Shutdown();
            m_TextureQueue.UpdateTexFunc = nullptr;
        }
        m_SceneViewCamera = {};
        m_BoundRenderDevice = nullptr;
    }

    /// @brief Destroy the ImGui context after the RHI renderer backend shuts down.
    /// @brief Shut down the platform backend and destroy the owned ImGui context.
    auto Shutdown() -> void {
        if (m_BoundRenderDevice)
            ReleaseRHIResources();
        if (!m_ImGuiContext)
            return;
        ImGui::SetCurrentContext(m_ImGuiContext);
        m_BoundWindowSystem = nullptr;
        ImGui::DestroyContext(m_ImGuiContext);
        m_ImGuiContext = nullptr;
    }

    /// @brief Register a UI panel. Panels run in registration order inside
    /// BuildFrame(). Main-thread only; not synchronized.
    auto RegisterPanel(String Name, UIPanelCallback Callback) -> void {
        m_Panels.push_back({.Name = std::move(Name), .Callback = std::move(Callback)});
    }

    /// @brief Resize the editor-owned Scene View output resources.
    auto ResizeSceneViewport(Uint32 Width, Uint32 Height) -> void {
        m_SceneViewCamera.ResizeViewport("editor_scene_view", Width, Height);
    }

    /// @brief Apply focused right-mouse navigation to the editor-local camera.
    auto UpdateSceneCamera(float DeltaTime, IWindowSystem& Window) -> void {
        if (!m_ImGuiContext)
            return;

        ImGui::SetCurrentContext(m_ImGuiContext);
        const bool CameraInputActive =
            !ImGui::GetIO().WantCaptureMouse && Window.IsMouseButtonPressed(WindowMouseButton::Right);
        Window.SetCursorCaptured(CameraInputActive);
        if (!CameraInputActive) {
            static_cast<void>(Window.ConsumeScrollDelta());
            static_cast<void>(Window.ConsumeCursorDelta());
            return;
        }

        const float ForwardInput = (Window.IsKeyPressed(WindowKey::W) ? 1.0f : 0.0f) -
                                   (Window.IsKeyPressed(WindowKey::S) ? 1.0f : 0.0f);
        const float RightInput = (Window.IsKeyPressed(WindowKey::D) ? 1.0f : 0.0f) -
                                 (Window.IsKeyPressed(WindowKey::A) ? 1.0f : 0.0f);
        const float VerticalInput = (Window.IsKeyPressed(WindowKey::E) ? 1.0f : 0.0f) -
                                    (Window.IsKeyPressed(WindowKey::Q) ? 1.0f : 0.0f);
        const auto EditorWorldTransform = GetSceneViewWorldMatrix();
        const auto Forward = m_SceneViewCamera.GetForward(EditorWorldTransform);
        const auto HorizontalForward = hlslpp::normalize(hlslpp::float3(Forward.x, 0.0f, Forward.z));
        const auto Up = hlslpp::float3(0.0f, 1.0f, 0.0f);
        const auto Right = hlslpp::normalize(hlslpp::cross(HorizontalForward, Up));
        const auto MoveDirection = HorizontalForward * ForwardInput + Right * RightInput + Up * VerticalInput;
        if (MoveDirection.x != 0.0f || MoveDirection.y != 0.0f || MoveDirection.z != 0.0f)
            m_SceneViewTransform.Translation += hlslpp::normalize(MoveDirection) * (2.0f * DeltaTime);
        m_SceneViewTransform.Translation += Forward * (Window.ConsumeScrollDelta() * 0.75f);

        constexpr float Sensitivity = 0.0025f;
        constexpr float MaxPitch    = 1.55334306f;
        const auto CursorDelta = Window.ConsumeCursorDelta();
        m_SceneViewTransform.Rotation.y += CursorDelta.X * Sensitivity * (180.0f / std::numbers::pi_v<float>);
        m_SceneViewTransform.Rotation.x = std::clamp(
            static_cast<float>(m_SceneViewTransform.Rotation.x) +
                CursorDelta.Y * Sensitivity * (180.0f / std::numbers::pi_v<float>),
            -MaxPitch * (180.0f / std::numbers::pi_v<float>),
            MaxPitch * (180.0f / std::numbers::pi_v<float>));
    }

    /// @brief Build the editor-owned Scene View render request for this frame.
    [[nodiscard]] auto BuildSceneView() const -> std::optional<RenderViewSnapshot> {
        return m_SceneViewCamera.BuildRenderView(GetSceneViewWorldMatrix());
    }

    auto UpdateSceneSelection(const SceneSnapshot& Snapshot) -> void {
        if (!m_ImGuiContext || Snapshot.Views.empty())
            return;
        ImGui::SetCurrentContext(m_ImGuiContext);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ClearSelection();
            return;
        }
        const auto& IO = ImGui::GetIO();
        if (!IO.MouseClicked[ImGuiMouseButton_Left] || IO.WantCaptureMouse)
            return;
        if (m_SceneViewCamera.ViewportWidth == 0 || m_SceneViewCamera.ViewportHeight == 0 ||
            IO.DisplaySize.x <= 0.0f || IO.DisplaySize.y <= 0.0f)
            return;

        const float PixelX = IO.MousePos.x;
        const float PixelY = IO.MousePos.y;
        const float Width = IO.DisplaySize.x;
        const float Height = IO.DisplaySize.y;
        if (PixelX < 0.0f || PixelY < 0.0f || PixelX >= Width || PixelY >= Height) {
            ClearSelection();
            return;
        }

        const auto PixelWidth  = static_cast<float>(m_SceneViewCamera.ViewportWidth);
        const auto PixelHeight = static_cast<float>(m_SceneViewCamera.ViewportHeight);
        m_SelectedPixel = RenderPixelCoordinate{
            .X = (std::min)(static_cast<Uint32>((PixelX / Width) * PixelWidth), m_SceneViewCamera.ViewportWidth - 1),
            .Y = (std::min)(static_cast<Uint32>((PixelY / Height) * PixelHeight), m_SceneViewCamera.ViewportHeight - 1),
        };

        const auto InverseViewProjection = hlslpp::inverse(Snapshot.Views.front().ViewProjection);
        const float NdcX = 2.0f * PixelX / Width - 1.0f;
        const float NdcY = 1.0f - 2.0f * PixelY / Height;
        const auto NearPoint = hlslpp::mul(hlslpp::float4{NdcX, NdcY, 0.0f, 1.0f}, InverseViewProjection);
        const auto FarPoint = hlslpp::mul(hlslpp::float4{NdcX, NdcY, 1.0f, 1.0f}, InverseViewProjection);
        const auto Origin = Snapshot.Views.front().CameraPosition;
        const auto NearWorld = hlslpp::float3(NearPoint.x / NearPoint.w, NearPoint.y / NearPoint.w, NearPoint.z / NearPoint.w);
        const auto FarWorld = hlslpp::float3(FarPoint.x / FarPoint.w, FarPoint.y / FarPoint.w, FarPoint.z / FarPoint.w);
        const auto Direction = hlslpp::normalize(FarWorld - NearWorld);

        std::optional<entt::entity> HitEntity = std::nullopt;
        float ClosestDistance = std::numeric_limits<float>::max();
        for (const auto& Renderable : Snapshot.Meshes) {
            auto MeshRef = ResourceManager::Get().RequestMeshRef(Renderable.MeshAsset);
            const auto* Mesh = ResourceManager::Get().TryGetReady(MeshRef);
            if (!Mesh)
                continue;
            for (const auto& Group : Mesh->GetMeshGroups()) {
                for (const auto& SubMesh : Group.SubMeshes) {
                    if (SubMesh.Positions.empty())
                        continue;
                    hlslpp::float3 Min{SubMesh.Positions.front().x, SubMesh.Positions.front().y, SubMesh.Positions.front().z};
                    hlslpp::float3 Max = Min;
                    for (const auto& Position : SubMesh.Positions) {
                        Min.x = std::min(static_cast<float>(Min.x), static_cast<float>(Position.x));
                        Min.y = std::min(static_cast<float>(Min.y), static_cast<float>(Position.y));
                        Min.z = std::min(static_cast<float>(Min.z), static_cast<float>(Position.z));
                        Max.x = std::max(static_cast<float>(Max.x), static_cast<float>(Position.x));
                        Max.y = std::max(static_cast<float>(Max.y), static_cast<float>(Position.y));
                        Max.z = std::max(static_cast<float>(Max.z), static_cast<float>(Position.z));
                    }
                    const auto LocalCenter = (Min + Max) * 0.5f;
                    float RadiusSquared = 0.0f;
                    for (const auto& Position : SubMesh.Positions) {
                        const auto Offset = hlslpp::float3{Position.x, Position.y, Position.z} - LocalCenter;
                        RadiusSquared = std::max(RadiusSquared, static_cast<float>(hlslpp::dot(Offset, Offset)));
                    }
                    const auto WorldCenter4 = hlslpp::mul(hlslpp::float4{LocalCenter.x, LocalCenter.y, LocalCenter.z, 1.0f}, Renderable.WorldTransform);
                    float TransformSquared = 0.0f;
                    for (Uint32 Row = 0; Row < 3; ++Row) {
                        TransformSquared += Renderable.WorldTransform[Row].x * Renderable.WorldTransform[Row].x;
                        TransformSquared += Renderable.WorldTransform[Row].y * Renderable.WorldTransform[Row].y;
                        TransformSquared += Renderable.WorldTransform[Row].z * Renderable.WorldTransform[Row].z;
                    }
                    const auto WorldCenter = hlslpp::float3{WorldCenter4.x, WorldCenter4.y, WorldCenter4.z};
                    const float WorldRadius = std::sqrt(RadiusSquared * TransformSquared);
                    const auto ToCenter = Origin - WorldCenter;
                    const float B = hlslpp::dot(ToCenter, Direction);
                    const float C = hlslpp::dot(ToCenter, ToCenter) - WorldRadius * WorldRadius;
                    const float Discriminant = B * B - C;
                    if (Discriminant < 0.0f)
                        continue;
                    float Distance = -B - std::sqrt(Discriminant);
                    if (Distance < 0.0f)
                        Distance = -B + std::sqrt(Discriminant);
                    if (Distance >= 0.0f && Distance < ClosestDistance) {
                        ClosestDistance = Distance;
                        HitEntity = static_cast<entt::entity>(Renderable.EntityId);
                    }
                }
            }
        }
        if (HitEntity)
            SelectEntity(*HitEntity);
        else
            ClearSelection();
    }


    [[nodiscard]] auto GetSelectedEntity() const -> std::optional<entt::entity> {
        return m_SelectedEntity;
    }

    [[nodiscard]] auto GetSelectedPixel() const -> std::optional<RenderPixelCoordinate> {
        return m_SelectedPixel;
    }

    auto SelectEntity(entt::entity Entity) -> void {
        m_SelectedEntity = Entity;
    }

    auto ClearSelection() -> void {
        m_SelectedEntity.reset();
        m_SelectedPixel.reset();
    }

    /// @brief Main-thread entry point: build the ImGui frame for this game
    /// tick and publish a draw-data snapshot for the render thread.
    auto BeginFrame(ImDrawDataSnapshot& Snapshot) -> void {
        if (!m_ImGuiContext)
            return;
        ImGui::SetCurrentContext(m_ImGuiContext);
        {
            std::scoped_lock Lock(m_TextureQueueMutex);
            m_TextureQueue.PreNewFrame();
        }
        if (m_BoundWindowSystem) {
            switch (m_BoundWindowSystem->GetType()) {
            case WindowSystemType::Glfw:
                ImGui_ImplGlfw_NewFrame();
                break;
            default:
                break;
            }
        }
        if (m_BoundRenderDevice) {
            switch (m_BoundRenderDevice->GetBackendType()) {
            case RHIBackendType::Vulkan:
                ImGui_ImplVulkan_NewFrame();
                break;
            default:
                break;
            }
        }
        ImGui::NewFrame();
        DrawMainMenu();
        
        // 绘制所有显示的 UI
        for (auto& [name, entry] : UIManager::Get().GetAll()) {
            if (entry.Show) {
                entry.Callback();
            }
        }
        for (auto& Panel : m_Panels)
            Panel.Callback();
        ImGui::Render();
        ImDrawData* DrawData = ImGui::GetDrawData();
        if (!DrawData || !DrawData->Valid)
            return;

        {
            std::scoped_lock Lock(m_TextureQueueMutex);
            m_TextureQueue.QueueRequests(DrawData);
        }
        Snapshot.SnapUsingSwap(DrawData, ImGui::GetTime());
    }

    /// @brief Render-thread entry point: consume the latest UI frame snapshot
    /// and append its draw pass to the command list. Skips silently until a
    /// frame has been published and the pipeline, font texture, and dynamic
    /// buffers are all ready.
    auto AttachPresentationOverlay(RHICommandList& CmdList, ImDrawDataSnapshot& Snapshot) -> void {
        if (!CmdList.PresentSourceRef.TryGet())
            return;

        if (!Snapshot.DrawData.Valid)
            return;

        CmdList.ImGuiPresentationOverlay = RHIImGuiPresentationOverlayCmd{
            .Snapshot     = &Snapshot,
            .TextureQueue = &m_TextureQueue,
            .TextureMutex = &m_TextureQueueMutex,
        };
    }

  private:
    [[nodiscard]] auto GetSceneViewWorldMatrix() const -> hlslpp::float4x4 {
        return m_SceneViewTransform.GetLocalMatrix();
    }

    ImGuiContext*          m_ImGuiContext = nullptr;
    std::vector<UIPanel>   m_Panels;

    ImTextureQueue m_TextureQueue;
    std::mutex      m_TextureQueueMutex;

    Camera m_SceneViewCamera = {};
    std::optional<entt::entity>          m_SelectedEntity = std::nullopt;
    std::optional<RenderPixelCoordinate> m_SelectedPixel  = std::nullopt;
    Transform m_SceneViewTransform{
        .Translation = hlslpp::float3(1.25f, 1.25f, 2.0f),
        .Rotation = hlslpp::float3(28.0f, -32.0f, 0.0f),
    };
    // Non-owning; EngineLoop keeps the window system alive until Editor::Shutdown().
    IWindowSystem* m_BoundWindowSystem = nullptr;
    RHIRenderDevice* m_BoundRenderDevice = nullptr;
};

} // namespace SoulEngine