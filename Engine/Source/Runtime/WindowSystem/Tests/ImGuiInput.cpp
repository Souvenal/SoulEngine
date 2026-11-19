#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <imgui.h>

import std;
import Core;
import EditorCommands;
import WindowSystem;

using namespace SoulEngine;

namespace SoulEngine {

class ImGuiInputFixture {
  public:
    ImGuiInputFixture() {
        ImGui::CreateContext();
        auto& IO                   = ImGui::GetIO();
        IO.IniFilename             = nullptr;
        IO.DisplaySize             = ImVec2(800, 600);
        IO.DisplayFramebufferScale = ImVec2(2, 2);
        IO.DeltaTime               = 1.0F / 60.0F;
        unsigned char* Pixels      = nullptr;
        int            Width = 0, Height = 0;
        IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);
        IO.Fonts->SetTexID(ImTextureID(1));
        IO.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    }

    ~ImGuiInputFixture() {
        ImGuiGlfwAdapter::ResetInput();
        ImGui::DestroyContext();
    }

    auto Begin(InputFrameState& State, Float32 DeltaTime = 1.0F / 60.0F) -> void {
        ImGui::GetIO().DeltaTime = DeltaTime;
        ImGuiGlfwAdapter::ApplyInput(State.AdvanceFrame());
        ImGui::NewFrame();
    }

    auto End() -> void {
        ImGui::Render();
    }
};

} // namespace SoulEngine

TEST(ImGuiInputAdapter, ClickDragReleaseAndWheelAreAppliedOnceAcrossVisibleFrames) {
    ImGuiInputFixture Fixture;
    InputFrameState   State;
    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 10, .Y = 10}}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Left, .Action = InputAction::Press}));
    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 30, .Y = 25}}));
    ASSERT_TRUE(State.Apply(WheelInputEvent{.Y = 1.5F}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Left, .Action = InputAction::Release}));

    Fixture.Begin(State);
    const auto& IO = ImGui::GetIO();
    EXPECT_TRUE(ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    EXPECT_TRUE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    EXPECT_EQ(IO.MousePos.x, 10);
    EXPECT_EQ(IO.DisplayFramebufferScale.x, 2);
    Fixture.End();

    Fixture.Begin(State);
    EXPECT_TRUE(ImGui::IsMouseDragging(ImGuiMouseButton_Left));
    EXPECT_EQ(IO.MousePos.x, 30);
    EXPECT_EQ(IO.MousePos.y, 25);
    EXPECT_EQ(IO.MouseWheel, 0);
    EXPECT_TRUE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    Fixture.End();

    Fixture.Begin(State);
    EXPECT_EQ(IO.MouseWheel, 1.5F);
    EXPECT_TRUE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    EXPECT_FALSE(ImGui::IsMouseReleased(ImGuiMouseButton_Left));
    Fixture.End();

    Fixture.Begin(State);
    EXPECT_TRUE(ImGui::IsMouseReleased(ImGuiMouseButton_Left));
    EXPECT_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
    EXPECT_EQ(IO.MouseWheel, 0);
    Fixture.End();

    Fixture.Begin(State);
    EXPECT_FALSE(ImGui::IsMouseClicked(ImGuiMouseButton_Left));
    EXPECT_FALSE(ImGui::IsMouseReleased(ImGuiMouseButton_Left));
    EXPECT_EQ(IO.MouseDelta.x, 0);
    Fixture.End();
}

TEST(ImGuiInputAdapter, ModifiersHeldRepeatAndTextUsePublicImGuiInputEvents) {
    ImGuiInputFixture Fixture;
    InputFrameState   State;
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key       = WindowKey::LeftControl,
                                          .NativeKey = GLFW_KEY_LEFT_CONTROL,
                                          .Action    = InputAction::Press,
                                          .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(KeyInputEvent{
        .Key = WindowKey::A, .NativeKey = GLFW_KEY_A, .Action = InputAction::Press, .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(TextInputEvent{.Codepoint = 'x'}));
    Fixture.Begin(State);
    auto& IO = ImGui::GetIO();
    EXPECT_TRUE(IO.KeyCtrl);
    EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_A));
    ASSERT_EQ(IO.InputQueueCharacters.size(), 1);
    EXPECT_EQ(IO.InputQueueCharacters[0], 'x');
    Fixture.End();

    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key       = WindowKey::A,
                                          .NativeKey = GLFW_KEY_A,
                                          .Action    = InputAction::Repeat,
                                          .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(TextInputEvent{.Codepoint = 'x'}));
    Fixture.Begin(State, 1.0F);
    EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_A));
    EXPECT_FALSE(ImGui::IsKeyPressed(ImGuiKey_A, false));
    EXPECT_TRUE(ImGui::IsKeyPressed(ImGuiKey_A, true));
    ASSERT_EQ(IO.InputQueueCharacters.size(), 1);
    EXPECT_EQ(IO.InputQueueCharacters[0], 'x');
    Fixture.End();

    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key       = WindowKey::A,
                                          .NativeKey = GLFW_KEY_A,
                                          .Action    = InputAction::Release,
                                          .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(KeyInputEvent{
        .Key = WindowKey::LeftControl, .NativeKey = GLFW_KEY_LEFT_CONTROL, .Action = InputAction::Release}));
    Fixture.Begin(State);
    EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_A));
    EXPECT_FALSE(IO.KeyCtrl);
    Fixture.End();
}

TEST(ImGuiInputAdapter, SaturationRestoresUnchangedFocusAndCursorBeforeOrdinaryInput) {
    ImGuiInputFixture    Fixture;
    CommandDispatcher    Commands(1);
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = true}));
    Commands.ExecutePhase(CommandPhase::Input, 1);
    Fixture.Begin(State);
    Fixture.End();
    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 12, .Y = 24}}));
    Commands.ExecutePhase(CommandPhase::Input, 2);
    Fixture.Begin(State);
    Fixture.End();
    ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
    Commands.ExecutePhase(CommandPhase::Input, 3);
    Fixture.Begin(State);
    const auto& IO = ImGui::GetIO();
    EXPECT_FALSE(IO.AppFocusLost);
    EXPECT_EQ(IO.MousePos.x, 12);
    EXPECT_EQ(IO.MousePos.y, 24);
    ASSERT_TRUE(State.GetFrame().IsFocused);
    ASSERT_TRUE(State.GetFrame().IsCursorInside);
    Fixture.End();

    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "test.input.occupant", .Phase = CommandPhase::Input},
                                  [](CommandContext&) -> void {}));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
    EXPECT_FALSE(Provider.Submit(PointerInputEvent{.Position = {.X = 99, .Y = 99}}));

    Commands.ExecutePhase(CommandPhase::Input, 4);
    Fixture.Begin(State);
    EXPECT_FALSE(State.GetFrame().IsFocused);
    EXPECT_FALSE(State.GetFrame().IsCursorInside);
    Fixture.End();

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 5);
    Fixture.Begin(State);
    EXPECT_TRUE(State.GetFrame().IsFocused);
    EXPECT_TRUE(State.GetFrame().IsCursorInside);
    EXPECT_FALSE(IO.AppFocusLost);
    EXPECT_EQ(IO.MousePos.x, 12);
    EXPECT_EQ(IO.MousePos.y, 24);
    Fixture.End();

    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 42, .Y = 36}}));
    Commands.ExecutePhase(CommandPhase::Input, 6);
    Fixture.Begin(State);
    EXPECT_EQ(IO.MousePos.x, 42);
    EXPECT_EQ(IO.MousePos.y, 36);
    Fixture.End();
    Provider.Unbind();
}

TEST(ImGuiInputAdapter, FailedFocusAndCursorTransitionsResynchronizeRecoveryIntent) {
    for (const bool IsFocusTrigger : {true, false}) {
        SCOPED_TRACE(IsFocusTrigger ? "focus loss trigger" : "cursor leave trigger");
        ImGuiInputFixture    Fixture;
        CommandDispatcher    Commands(1);
        InputFrameState      State;
        InputCommandProvider Provider(State);
        const auto&          IO = ImGui::GetIO();
        ASSERT_TRUE(Provider.Bind(Commands));
        ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = true}));
        Commands.ExecutePhase(CommandPhase::Input, 1);
        Fixture.Begin(State);
        Fixture.End();
        ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 12, .Y = 24}}));
        Commands.ExecutePhase(CommandPhase::Input, 2);
        Fixture.Begin(State);
        Fixture.End();
        ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
        Commands.ExecutePhase(CommandPhase::Input, 3);
        Fixture.Begin(State);
        ASSERT_TRUE(State.GetFrame().IsFocused);
        ASSERT_TRUE(State.GetFrame().IsCursorInside);
        Fixture.End();

        ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "test.input.occupant", .Phase = CommandPhase::Input},
                                      [](CommandContext&) -> void {}));
        ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
        if (IsFocusTrigger) {
            EXPECT_FALSE(Provider.Submit(FocusInputEvent{.IsFocused = false}));
            ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = false}));
        } else {
            EXPECT_FALSE(Provider.Submit(CursorEnterInputEvent{.IsInside = false}));
            ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = false}));
        }

        Commands.ExecutePhase(CommandPhase::Input, 4);
        Fixture.Begin(State);
        EXPECT_FALSE(State.GetFrame().IsFocused);
        Fixture.End();
        bool ObservedFocusLoss = false;
        for (Uint64 Frame = 5; Frame <= 7; ++Frame) {
            Provider.RetryRecovery();
            Commands.ExecutePhase(CommandPhase::Input, Frame);
            Fixture.Begin(State);
            ObservedFocusLoss = ObservedFocusLoss || IO.AppFocusLost;
            Fixture.End();
        }
        EXPECT_TRUE(ObservedFocusLoss);
        EXPECT_FALSE(State.GetFrame().IsFocused);
        EXPECT_FALSE(State.GetFrame().IsCursorInside);
        EXPECT_LT(IO.MousePos.x, -1.0e30F);

        ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 42, .Y = 36}}));
        Commands.ExecutePhase(CommandPhase::Input, 8);
        Fixture.Begin(State);
        EXPECT_EQ(IO.MousePos.x, 42);
        EXPECT_EQ(IO.MousePos.y, 36);
        Fixture.End();
        Provider.Unbind();
    }
}

TEST(ImGuiInputAdapter, AdmittedFocusAndCursorTransitionsResynchronizeRecoveryIntent) {
    for (const bool IsFocusPending : {true, false}) {
        SCOPED_TRACE(IsFocusPending ? "queued focus loss" : "queued cursor leave");
        ImGuiInputFixture    Fixture;
        CommandDispatcher    Commands(1);
        InputFrameState      State;
        InputCommandProvider Provider(State);
        const auto&          IO = ImGui::GetIO();
        ASSERT_TRUE(Provider.Bind(Commands));
        ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = true}));
        Commands.ExecutePhase(CommandPhase::Input, 1);
        Fixture.Begin(State);
        Fixture.End();
        ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 12, .Y = 24}}));
        Commands.ExecutePhase(CommandPhase::Input, 2);
        Fixture.Begin(State);
        Fixture.End();
        ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
        Commands.ExecutePhase(CommandPhase::Input, 3);
        Fixture.Begin(State);
        Fixture.End();

        if (IsFocusPending)
            ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = false}));
        else
            ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = false}));
        EXPECT_FALSE(Provider.Submit(PointerInputEvent{.Position = {.X = 99, .Y = 99}}));

        Commands.ExecutePhase(CommandPhase::Input, 4);
        Fixture.Begin(State);
        EXPECT_FALSE(State.GetFrame().IsFocused);
        Fixture.End();
        bool ObservedFocusLoss = false;
        for (Uint64 Frame = 5; Frame <= 7; ++Frame) {
            Provider.RetryRecovery();
            Commands.ExecutePhase(CommandPhase::Input, Frame);
            Fixture.Begin(State);
            ObservedFocusLoss = ObservedFocusLoss || IO.AppFocusLost;
            Fixture.End();
        }
        EXPECT_EQ(ObservedFocusLoss, IsFocusPending);
        EXPECT_EQ(State.GetFrame().IsFocused, !IsFocusPending);
        EXPECT_EQ(State.GetFrame().IsCursorInside, IsFocusPending);
        if (IsFocusPending) {
            EXPECT_EQ(IO.MousePos.x, 12);
            EXPECT_EQ(IO.MousePos.y, 24);
        } else {
            EXPECT_LT(IO.MousePos.x, -1.0e30F);
        }

        ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 42, .Y = 36}}));
        Commands.ExecutePhase(CommandPhase::Input, 8);
        Fixture.Begin(State);
        EXPECT_EQ(IO.MousePos.x, 42);
        EXPECT_EQ(IO.MousePos.y, 36);
        Fixture.End();
        Provider.Unbind();
    }
}

TEST(ImGuiInputAdapter, SaturationResetAndReplayResynchronizeFocusAndCursor) {
    ImGuiInputFixture    Fixture;
    CommandDispatcher    Commands(1);
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(State.GetFrame().IsFocused);
    ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
    Commands.ExecutePhase(CommandPhase::Input, 1);
    Fixture.Begin(State);
    Fixture.End();
    ASSERT_TRUE(
        Provider.Submit(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    Commands.ExecutePhase(CommandPhase::Input, 2);
    Fixture.Begin(State);
    EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_W));
    EXPECT_TRUE(State.GetFrame().IsCursorInside);
    Fixture.End();

    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "test.input.occupant", .Phase = CommandPhase::Input},
                                  [](CommandContext&) -> void {}));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
    EXPECT_FALSE(
        Provider.Submit(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Release}));
    ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = false}));
    ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = false}));

    Commands.ExecutePhase(CommandPhase::Input, 3);
    Fixture.Begin(State);
    const auto& IO = ImGui::GetIO();
    EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_W));
    EXPECT_FALSE(State.GetFrame().IsFocused);
    EXPECT_FALSE(State.GetFrame().IsCursorInside);
    Fixture.End();

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 4);
    Fixture.Begin(State);
    EXPECT_TRUE(State.GetFrame().IsFocused);
    EXPECT_TRUE(State.GetFrame().IsCursorInside);
    EXPECT_FALSE(IO.AppFocusLost);
    EXPECT_EQ(IO.MousePos.x, 0);
    EXPECT_EQ(IO.MousePos.y, 0);
    Fixture.End();

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 5);
    Fixture.Begin(State);
    EXPECT_FALSE(State.GetFrame().IsFocused);
    EXPECT_TRUE(State.GetFrame().IsCursorInside);
    EXPECT_TRUE(IO.AppFocusLost);
    Fixture.End();

    Commands.ExecutePhase(CommandPhase::Input, 6);
    Fixture.Begin(State);
    EXPECT_FALSE(State.GetFrame().IsFocused);
    EXPECT_FALSE(State.GetFrame().IsCursorInside);
    EXPECT_LT(IO.MousePos.x, -1.0e30F);
    Fixture.End();
    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 42, .Y = 24}}));
    Commands.ExecutePhase(CommandPhase::Input, 7);
    Fixture.Begin(State);
    EXPECT_EQ(IO.MousePos.x, 42);
    EXPECT_EQ(IO.MousePos.y, 24);
    Fixture.End();
    Provider.Unbind();
}

TEST(ImGuiInputAdapter, FocusLossAndRecoveryResetClearNativeHeldState) {
    ImGuiInputFixture Fixture;
    InputFrameState   State;
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Right, .Action = InputAction::Press}));
    Fixture.Begin(State);
    EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_W));
    EXPECT_TRUE(ImGui::IsMouseDown(ImGuiMouseButton_Right));
    Fixture.End();

    ASSERT_TRUE(State.Apply(FocusInputEvent{.IsFocused = false}));
    Fixture.Begin(State);
    EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_W));
    EXPECT_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Right));
    EXPECT_FALSE(State.GetFrame().IsFocused);
    Fixture.End();

    ASSERT_TRUE(State.Apply(FocusInputEvent{.IsFocused = true}));
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    Fixture.Begin(State);
    EXPECT_TRUE(ImGui::IsKeyDown(ImGuiKey_W));
    Fixture.End();

    ASSERT_TRUE(State.Apply(ResetInputEvent{}));
    Fixture.Begin(State);
    EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_W));
    EXPECT_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Right));
    Fixture.End();
}
