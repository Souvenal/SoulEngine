#include <GLFW/glfw3.h>
#include <entt/entt.hpp>
#include <gtest/gtest.h>
#include <imgui.h>

import std;
import Core;
import Scene;
import WindowSystem;
import Editor.EditorCamera;

using namespace SoulEngine;

namespace SoulEngine {

class CameraWindow final : public IWindowSystem {
  public:
    [[nodiscard]] auto GetType() const -> WindowSystemType override {
        return WindowSystemType::Unknown;
    }
    [[nodiscard]] auto IsValid() const -> bool override {
        return true;
    }
    auto               Shutdown() -> void override {}
    [[nodiscard]] auto PollEvents() -> bool override {
        return false;
    }
    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override {
        return m_Dispatcher;
    }
    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return {};
    }
    auto SetCursorMode(CursorMode Mode) -> void override {
        m_CurrentCursorMode = Mode;
    }

    CursorMode m_CurrentCursorMode = CursorMode::Normal;

  private:
    entt::dispatcher m_Dispatcher;
};

struct CameraFixture {
    CameraFixture() : System(Registry, Window) {
        Registry.ctx().emplace<entt::dispatcher>();
        Entity                = Registry.create();
        auto& Camera          = Registry.emplace<EditorCameraComponent>(Entity);
        Camera.Viewport       = {.X = 0, .Y = 0, .Width = 200, .Height = 200};
        Camera.ReadbackEntity = Entity;
        Registry.emplace<TransformComponent>(Entity);
        ImGui::CreateContext();
        auto& IO                   = ImGui::GetIO();
        IO.IniFilename             = nullptr;
        IO.DisplaySize             = ImVec2(400, 400);
        IO.DisplayFramebufferScale = ImVec2(2, 2);
        IO.DeltaTime               = 1.0F / 60.0F;
        unsigned char* Pixels      = nullptr;
        int            Width = 0, Height = 0;
        IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
        IO.Fonts->SetTexID(ImTextureID(1));
        IO.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    }

    ~CameraFixture() {
        ImGuiGlfwAdapter::ResetInput();
        ImGui::DestroyContext();
    }

    auto Advance(InputFrameState& State, Float32 DeltaTime = 0.5F) -> void {
        ImGuiGlfwAdapter::ApplyInput(State.AdvanceFrame());
        ImGui::NewFrame();
        System.OnUpdate(DeltaTime);
        ImGui::Render();
    }

    entt::registry     Registry;
    CameraWindow       Window;
    EditorCameraSystem System;
    entt::entity       Entity = entt::null;
};

} // namespace SoulEngine

TEST(EditorCameraInput, RightDragWAndWheelUseExecutedSharedInput) {
    CameraFixture   Fixture;
    InputFrameState State;
    ASSERT_TRUE(State.Apply(PointerInputEvent{}));
    Fixture.Advance(State);
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Right, .Action = InputAction::Press}));
    Fixture.Advance(State);
    auto& Transform = Fixture.Registry.get<TransformComponent>(Fixture.Entity);
    EXPECT_EQ(Fixture.Window.m_CurrentCursorMode, CursorMode::Disabled);
    EXPECT_EQ(Transform.Translation.z, 0.0F);

    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    Fixture.Advance(State);
    EXPECT_LT(Transform.Translation.z, 0.0F);

    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 20, .Y = 0}}));
    Fixture.Advance(State);
    EXPECT_GT(Transform.Rotation.y, 0.0F);
    ASSERT_TRUE(State.Apply(WheelInputEvent{.Y = 2}));
    Fixture.Advance(State);
    EXPECT_LT(Transform.Translation.z, -1.0F);

    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Right, .Action = InputAction::Release}));
    Fixture.Advance(State);
    EXPECT_EQ(Fixture.Window.m_CurrentCursorMode, CursorMode::Normal);
    ASSERT_TRUE(
        State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Release}));
    Fixture.Advance(State);
}

TEST(EditorCameraInput, LogicalClickUsesExistingSingleDpiConversion) {
    CameraFixture   Fixture;
    InputFrameState State;
    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 10, .Y = 20}}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Left, .Action = InputAction::Press}));
    Fixture.Advance(State);
    const auto& Camera = Fixture.Registry.get<EditorCameraComponent>(Fixture.Entity);
    ASSERT_TRUE(Camera.SelectedEntity.has_value());
    EXPECT_EQ(*Camera.SelectedEntity, Fixture.Entity);
}
