#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

import std;
import WindowSystem;

using namespace SoulEngine;

TEST(InputFrameState, PressRepeatReleaseAndUpRemainFrameVisible) {
    InputFrameState State;
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    const auto& Pressed = State.AdvanceFrame();
    EXPECT_EQ(Pressed.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Pressed);
    EXPECT_TRUE(Pressed.Keyboard.IsKeyDown(WindowKey::W));

    ASSERT_TRUE(
        State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Repeat}));
    const auto& Repeated = State.AdvanceFrame();
    EXPECT_EQ(Repeated.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Held);
    ASSERT_EQ(Repeated.Events.size(), 1);
    EXPECT_EQ(std::get<KeyInputEvent>(Repeated.Events[0]).Action, InputAction::Repeat);

    ASSERT_TRUE(
        State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Release}));
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Up);
}

TEST(InputFrameState, MotionAndWheelCoalesceOnlyBetweenOrderedEdges) {
    InputFrameState State;
    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 5, .Y = 6}}));
    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 8, .Y = 9}}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Left, .Action = InputAction::Press}));
    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 20, .Y = 30}}));
    ASSERT_TRUE(State.Apply(WheelInputEvent{.X = 1, .Y = 2}));
    ASSERT_TRUE(State.Apply(WheelInputEvent{.X = 0.5F, .Y = 1}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Left, .Action = InputAction::Release}));

    const auto& Pressed = State.AdvanceFrame();
    ASSERT_EQ(Pressed.Events.size(), 4);
    EXPECT_EQ(std::get<PointerInputEvent>(Pressed.Events[0]).Position.X, 8);
    EXPECT_EQ(std::get<PointerInputEvent>(Pressed.Events[2]).Position.X, 20);
    EXPECT_EQ(Pressed.Mouse.CurrentCursorPosition.X, 20);
    EXPECT_EQ(Pressed.Mouse.CurrentCursorPosition.Y, 30);
    EXPECT_EQ(Pressed.Mouse.ScrollX, 1.5F);
    EXPECT_EQ(Pressed.Mouse.ScrollY, 3);
    EXPECT_EQ(Pressed.Mouse.ButtonStates[std::to_underlying(WindowMouseButton::Left)], InputState::Pressed);

    const auto& Released = State.AdvanceFrame();
    ASSERT_EQ(Released.Events.size(), 1);
    EXPECT_EQ(Released.Mouse.ButtonStates[std::to_underlying(WindowMouseButton::Left)], InputState::Released);
    EXPECT_EQ(Released.Mouse.ScrollY, 0);
    EXPECT_EQ(Released.Mouse.GetCursorDelta().X, 0);
}

TEST(InputFrameState, ModifiersTextAndNativeKeysSurviveSharedDispatch) {
    InputFrameState State;
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key       = WindowKey::LeftControl,
                                          .NativeKey = GLFW_KEY_LEFT_CONTROL,
                                          .Action    = InputAction::Press,
                                          .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(KeyInputEvent{
        .Key = WindowKey::A, .NativeKey = GLFW_KEY_A, .Action = InputAction::Press, .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(TextInputEvent{.Codepoint = 0x1F642}));
    ASSERT_TRUE(State.Apply(
        KeyInputEvent{.NativeKey = GLFW_KEY_F13, .Action = InputAction::Press, .Modifiers = {.IsControl = true}}));
    ASSERT_TRUE(State.Apply(
        KeyInputEvent{.NativeKey = GLFW_KEY_F13, .Action = InputAction::Release, .Modifiers = {.IsControl = true}}));

    const auto& Frame = State.AdvanceFrame();
    EXPECT_TRUE(Frame.Keyboard.Modifiers.IsControl);
    EXPECT_TRUE(Frame.Keyboard.IsKeyDown(WindowKey::A));
    ASSERT_EQ(Frame.Events.size(), 4);
    EXPECT_EQ(std::get<TextInputEvent>(Frame.Events[2]).Codepoint, 0x1F642U);
    EXPECT_EQ(std::get<KeyInputEvent>(Frame.Events[3]).Action, InputAction::Press);
    const auto& Released = State.AdvanceFrame();
    ASSERT_EQ(Released.Events.size(), 1);
    EXPECT_EQ(std::get<KeyInputEvent>(Released.Events[0]).NativeKey, GLFW_KEY_F13);
    EXPECT_EQ(std::get<KeyInputEvent>(Released.Events[0]).Action, InputAction::Release);
}

TEST(InputFrameState, FocusLossDropsStalePendingInputAndReleasesHeldState) {
    InputFrameState State;
    ASSERT_TRUE(State.Apply(KeyInputEvent{
        .Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press, .Modifiers = {.IsShift = true}}));
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Right, .Action = InputAction::Press}));
    (void)State.AdvanceFrame();

    ASSERT_TRUE(State.Apply(PointerInputEvent{.Position = {.X = 999, .Y = 999}}));
    ASSERT_TRUE(State.Apply(FocusInputEvent{.IsFocused = false}));
    const auto& Frame = State.AdvanceFrame();
    EXPECT_FALSE(Frame.IsFocused);
    EXPECT_FALSE(Frame.Keyboard.Modifiers.IsShift);
    EXPECT_EQ(Frame.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    EXPECT_EQ(Frame.Mouse.ButtonStates[std::to_underlying(WindowMouseButton::Right)], InputState::Released);
    EXPECT_EQ(Frame.Mouse.CurrentCursorPosition.X, 0);
    ASSERT_FALSE(Frame.Events.empty());
    EXPECT_TRUE(std::holds_alternative<FocusInputEvent>(Frame.Events.back()));
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Up);
}

TEST(InputFrameState, PendingOverflowProducesExplicitReleaseAndReset) {
    InputFrameState State(2);
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    (void)State.AdvanceFrame();
    ASSERT_TRUE(State.Apply(TextInputEvent{.Codepoint = 'a'}));
    ASSERT_TRUE(State.Apply(TextInputEvent{.Codepoint = 'b'}));
    EXPECT_FALSE(State.Apply(TextInputEvent{.Codepoint = 'c'}));
    const auto& Frame = State.AdvanceFrame();
    EXPECT_EQ(Frame.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    ASSERT_EQ(Frame.Events.size(), 2);
    EXPECT_TRUE(std::holds_alternative<KeyInputEvent>(Frame.Events[0]));
    EXPECT_TRUE(std::holds_alternative<ResetInputEvent>(Frame.Events[1]));
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Up);
}

TEST(InputFrameState, ShutdownClearsPendingAndPublishedHeldInput) {
    InputFrameState State;
    ASSERT_TRUE(State.Apply(MouseButtonInputEvent{.Button = WindowMouseButton::Left, .Action = InputAction::Press}));
    (void)State.AdvanceFrame();
    ASSERT_TRUE(State.Apply(KeyInputEvent{.Key = WindowKey::W, .NativeKey = GLFW_KEY_W, .Action = InputAction::Press}));
    State.Shutdown();
    const auto& Frame = State.GetFrame();
    EXPECT_FALSE(Frame.IsFocused);
    EXPECT_TRUE(Frame.Events.empty());
    EXPECT_FALSE(Frame.Keyboard.IsKeyDown(WindowKey::W));
    EXPECT_FALSE(Frame.Mouse.IsButtonDown(WindowMouseButton::Left));
}
