#include <GLFW/glfw3.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

import std;
import Core;
import EditorCommands;
import WindowSystem;

using namespace SoulEngine;

namespace SoulEngine {

[[nodiscard]] auto Key(WindowKey EngineKey, int NativeKey, InputAction Action) -> InputEvent {
    return KeyInputEvent{.Key = EngineKey, .NativeKey = NativeKey, .Action = Action};
}

} // namespace SoulEngine

TEST(InputCommandIngress, SubmitByUserExecutesBeforePublicFrameAndIsNotAgentExposed) {
    CommandDispatcher    Commands;
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 12, .Y = 34}}));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    EXPECT_FALSE(State.GetFrame().Keyboard.IsKeyDown(WindowKey::W));
    EXPECT_FALSE(Commands.SubmitByAgent("window.input.apply", nlohmann::json::object()));

    Commands.ExecutePhase(CommandPhase::Input, 1);
    const auto& Frame = State.AdvanceFrame();
    EXPECT_TRUE(Frame.Keyboard.IsKeyDown(WindowKey::W));
    EXPECT_EQ(Frame.Mouse.CurrentCursorPosition.X, 12);
    ASSERT_EQ(Frame.Events.size(), 2);
    Provider.Unbind();
}

TEST(InputCommandIngress, FrozenBudgetRetainsReleaseAndLaterInputWithoutCollapsingEdges) {
    CommandDispatcher    Commands;
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Release)));
    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 50, .Y = 60}}));

    Commands.ExecutePhase(CommandPhase::Input, 1, 2);
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Pressed);
    Commands.ExecutePhase(CommandPhase::Input, 2, 2);
    const auto& Frame = State.AdvanceFrame();
    EXPECT_EQ(Frame.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    EXPECT_EQ(Frame.Mouse.CurrentCursorPosition.X, 50);
    Commands.ExecutePhase(CommandPhase::Input, 3);
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Up);
    Provider.Unbind();
}

TEST(InputCommandIngress, ReentrantInputWaitsForTheNextFrozenBatch) {
    CommandDispatcher    Commands;
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name     = "test.input.reentrant",
                                                    .Phase    = CommandPhase::Input,
                                                    .Category = CommandCategory::Input},
                                  [&](CommandContext&) -> void {
                                      ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 77, .Y = 88}}));
                                  }));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.reentrant"));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::A, GLFW_KEY_A, InputAction::Press)));

    Commands.ExecutePhase(CommandPhase::Input, 1);
    const auto& First = State.AdvanceFrame();
    ASSERT_EQ(First.Events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<KeyInputEvent>(First.Events[0]));
    EXPECT_EQ(First.Mouse.CurrentCursorPosition.X, 0);

    Commands.ExecutePhase(CommandPhase::Input, 2);
    const auto& Second = State.AdvanceFrame();
    ASSERT_EQ(Second.Events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<PointerInputEvent>(Second.Events[0]));
    EXPECT_EQ(Second.Mouse.CurrentCursorPosition.X, 77);
    Provider.Unbind();
}

TEST(InputCommandIngress, AdmissionSaturationUsesOneReservedRecoveryResetAndResumes) {
    CommandDispatcher    Commands(1);
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    Commands.ExecutePhase(CommandPhase::Input, 1);
    (void)State.AdvanceFrame();
    ASSERT_TRUE(State.GetFrame().Keyboard.IsKeyDown(WindowKey::W));

    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "test.input.occupant", .Phase = CommandPhase::Input},
                                  [](CommandContext&) -> void {}));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
    EXPECT_FALSE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Release)));
    EXPECT_FALSE(Provider.Submit(PointerInputEvent{}));
    Provider.RetryRecovery();

    Commands.ExecutePhase(CommandPhase::Input, 2);
    const auto& Reset = State.AdvanceFrame();
    EXPECT_EQ(Reset.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    ASSERT_FALSE(Reset.Events.empty());
    EXPECT_TRUE(std::holds_alternative<ResetInputEvent>(Reset.Events.back()));

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 3);
    const auto& Replayed = State.AdvanceFrame();
    ASSERT_EQ(Replayed.Events.size(), 2);
    EXPECT_TRUE(Replayed.IsFocused);
    EXPECT_FALSE(Replayed.IsCursorInside);

    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 9, .Y = 10}}));
    Commands.ExecutePhase(CommandPhase::Input, 4);
    EXPECT_EQ(State.AdvanceFrame().Mouse.CurrentCursorPosition.X, 9);
    Provider.Unbind();
}

TEST(InputCommandIngress, AdmissionSaturationRestoresUnchangedFocusAndCursorBeforeOrdinaryInput) {
    CommandDispatcher    Commands(1);
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = true}));
    Commands.ExecutePhase(CommandPhase::Input, 1);
    (void)State.AdvanceFrame();
    ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
    Commands.ExecutePhase(CommandPhase::Input, 2);
    (void)State.AdvanceFrame();
    ASSERT_TRUE(State.GetFrame().IsFocused);
    ASSERT_TRUE(State.GetFrame().IsCursorInside);

    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "test.input.occupant", .Phase = CommandPhase::Input},
                                  [](CommandContext&) -> void {}));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
    EXPECT_FALSE(Provider.Submit(PointerInputEvent{.Position = {.X = 99, .Y = 99}}));

    Commands.ExecutePhase(CommandPhase::Input, 3);
    const auto& Reset = State.AdvanceFrame();
    EXPECT_FALSE(Reset.IsFocused);
    EXPECT_FALSE(Reset.IsCursorInside);
    ASSERT_FALSE(Reset.Events.empty());
    EXPECT_TRUE(std::holds_alternative<ResetInputEvent>(Reset.Events.back()));

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 4);
    const auto& Replayed = State.AdvanceFrame();
    ASSERT_EQ(Replayed.Events.size(), 2);
    ASSERT_TRUE(std::holds_alternative<FocusInputEvent>(Replayed.Events[0]));
    EXPECT_TRUE(std::get<FocusInputEvent>(Replayed.Events[0]).IsFocused);
    ASSERT_TRUE(std::holds_alternative<CursorEnterInputEvent>(Replayed.Events[1]));
    EXPECT_TRUE(std::get<CursorEnterInputEvent>(Replayed.Events[1]).IsInside);
    EXPECT_TRUE(Replayed.IsFocused);
    EXPECT_TRUE(Replayed.IsCursorInside);

    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 9, .Y = 10}}));
    Commands.ExecutePhase(CommandPhase::Input, 5);
    EXPECT_EQ(State.AdvanceFrame().Mouse.CurrentCursorPosition.X, 9);
    Provider.Unbind();
}

TEST(InputCommandIngress, FailedFocusAndCursorTransitionsAreIncludedInRecoveryIntent) {
    for (const bool IsFocusTrigger : {true, false}) {
        SCOPED_TRACE(IsFocusTrigger ? "focus loss trigger" : "cursor leave trigger");
        CommandDispatcher    Commands(1);
        InputFrameState      State;
        InputCommandProvider Provider(State);
        ASSERT_TRUE(Provider.Bind(Commands));
        ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = true}));
        Commands.ExecutePhase(CommandPhase::Input, 1);
        (void)State.AdvanceFrame();
        ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
        Commands.ExecutePhase(CommandPhase::Input, 2);
        (void)State.AdvanceFrame();
        ASSERT_TRUE(State.GetFrame().IsFocused);
        ASSERT_TRUE(State.GetFrame().IsCursorInside);

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

        Commands.ExecutePhase(CommandPhase::Input, 3);
        EXPECT_FALSE(State.AdvanceFrame().IsFocused);
        for (Uint64 Frame = 4; Frame <= 6; ++Frame) {
            Provider.RetryRecovery();
            Commands.ExecutePhase(CommandPhase::Input, Frame);
            (void)State.AdvanceFrame();
        }
        EXPECT_FALSE(State.GetFrame().IsFocused);
        EXPECT_FALSE(State.GetFrame().IsCursorInside);

        ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 9, .Y = 10}}));
        Commands.ExecutePhase(CommandPhase::Input, 7);
        EXPECT_EQ(State.AdvanceFrame().Mouse.CurrentCursorPosition.X, 9);
        Provider.Unbind();
    }
}

TEST(InputCommandIngress, AdmittedFocusAndCursorTransitionsAreIncludedInRecoveryIntent) {
    for (const bool IsFocusPending : {true, false}) {
        SCOPED_TRACE(IsFocusPending ? "queued focus loss" : "queued cursor leave");
        CommandDispatcher    Commands(1);
        InputFrameState      State;
        InputCommandProvider Provider(State);
        ASSERT_TRUE(Provider.Bind(Commands));
        ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = true}));
        Commands.ExecutePhase(CommandPhase::Input, 1);
        (void)State.AdvanceFrame();
        ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
        Commands.ExecutePhase(CommandPhase::Input, 2);
        (void)State.AdvanceFrame();

        if (IsFocusPending)
            ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = false}));
        else
            ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = false}));
        EXPECT_FALSE(Provider.Submit(PointerInputEvent{.Position = {.X = 99, .Y = 99}}));

        Commands.ExecutePhase(CommandPhase::Input, 3);
        EXPECT_FALSE(State.AdvanceFrame().IsFocused);
        for (Uint64 Frame = 4; Frame <= 6; ++Frame) {
            Provider.RetryRecovery();
            Commands.ExecutePhase(CommandPhase::Input, Frame);
            (void)State.AdvanceFrame();
        }
        EXPECT_EQ(State.GetFrame().IsFocused, !IsFocusPending);
        EXPECT_EQ(State.GetFrame().IsCursorInside, IsFocusPending);

        ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 9, .Y = 10}}));
        Commands.ExecutePhase(CommandPhase::Input, 7);
        EXPECT_EQ(State.AdvanceFrame().Mouse.CurrentCursorPosition.X, 9);
        Provider.Unbind();
    }
}

TEST(InputCommandIngress, RecoveryReplaysFocusAndCursorBeforeFurtherInput) {
    CommandDispatcher    Commands(1);
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = true}));
    Commands.ExecutePhase(CommandPhase::Input, 1);
    (void)State.AdvanceFrame();
    ASSERT_TRUE(State.GetFrame().IsCursorInside);
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    Commands.ExecutePhase(CommandPhase::Input, 2);
    (void)State.AdvanceFrame();

    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "test.input.occupant", .Phase = CommandPhase::Input},
                                  [](CommandContext&) -> void {}));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
    EXPECT_FALSE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Release)));
    ASSERT_TRUE(Provider.Submit(FocusInputEvent{.IsFocused = false}));
    ASSERT_TRUE(Provider.Submit(CursorEnterInputEvent{.IsInside = false}));
    EXPECT_FALSE(Provider.Submit(PointerInputEvent{.Position = {.X = 99, .Y = 99}}));

    Commands.ExecutePhase(CommandPhase::Input, 3);
    const auto& Reset = State.AdvanceFrame();
    EXPECT_EQ(Reset.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    EXPECT_FALSE(Reset.IsFocused);
    EXPECT_FALSE(Reset.IsCursorInside);
    EXPECT_TRUE(std::holds_alternative<ResetInputEvent>(Reset.Events.back()));

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 4);
    const auto& Snapshot = State.AdvanceFrame();
    ASSERT_EQ(Snapshot.Events.size(), 2);
    EXPECT_TRUE(std::holds_alternative<FocusInputEvent>(Snapshot.Events[0]));
    EXPECT_TRUE(std::holds_alternative<CursorEnterInputEvent>(Snapshot.Events[1]));
    EXPECT_TRUE(Snapshot.IsFocused);
    EXPECT_TRUE(Snapshot.IsCursorInside);

    Provider.RetryRecovery();
    Commands.ExecutePhase(CommandPhase::Input, 5);
    const auto& Replayed = State.AdvanceFrame();
    ASSERT_EQ(Replayed.Events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<FocusInputEvent>(Replayed.Events[0]));
    EXPECT_FALSE(Replayed.IsFocused);
    EXPECT_TRUE(Replayed.IsCursorInside);

    Commands.ExecutePhase(CommandPhase::Input, 6);
    const auto& Cursor = State.AdvanceFrame();
    ASSERT_EQ(Cursor.Events.size(), 1);
    EXPECT_TRUE(std::holds_alternative<CursorEnterInputEvent>(Cursor.Events[0]));
    EXPECT_FALSE(Cursor.IsFocused);
    EXPECT_FALSE(Cursor.IsCursorInside);
    ASSERT_TRUE(Provider.Submit(PointerInputEvent{.Position = {.X = 9, .Y = 10}}));
    Commands.ExecutePhase(CommandPhase::Input, 7);
    EXPECT_EQ(State.AdvanceFrame().Mouse.CurrentCursorPosition.X, 9);
    Provider.Unbind();
}

TEST(InputCommandIngress, AgentOptionsCannotConsumeReservedPhysicalRecoveryCapacity) {
    CommandDispatcher Commands(1);
    int               AgentCalls = 0;
    ASSERT_TRUE(Commands.Register("test.input.occupant", []() -> void {}));
    ASSERT_TRUE(Commands.Register(
        CommandDescriptor{.Name = "test.input.agent", .IsAgentExposed = true, .Category = CommandCategory::Input},
        [&](CommandContext&) -> void { ++AgentCalls; }));
    ASSERT_TRUE(Commands.SubmitByUser("test.input.occupant"));
    std::vector<CommandResult> Results;
    EXPECT_FALSE(Commands.SubmitByAgent(
        "test.input.agent",
        nlohmann::json::object(),
        [&](CommandResult Result) -> void { Results.push_back(std::move(Result)); },
        CommandOptions{.IsRecovery = true}));
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Failure->Code, "queue_capacity");
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(AgentCalls, 0);
}

TEST(InputCommandIngress, PendingOverflowFailsVisiblyAndReleasesThroughTheSameFrame) {
    CommandDispatcher    Commands;
    InputFrameState      State(1);
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    Commands.ExecutePhase(CommandPhase::Input, 1);
    (void)State.AdvanceFrame();
    ASSERT_TRUE(Provider.Submit(TextInputEvent{.Codepoint = 'a'}));
    ASSERT_TRUE(Provider.Submit(TextInputEvent{.Codepoint = 'b'}));
    Commands.ExecutePhase(CommandPhase::Input, 2);

    const auto Diagnostics = Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    EXPECT_EQ(Diagnostics[0].Failure->Code, "input_capacity");
    const auto& Frame = State.AdvanceFrame();
    EXPECT_EQ(Frame.Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Released);
    EXPECT_TRUE(std::holds_alternative<ResetInputEvent>(Frame.Events.back()));
    Provider.Unbind();
}

TEST(InputCommandIngress, RetirementCancelsQueuedInputAndShutdownClearsHeldState) {
    CommandDispatcher    Commands;
    InputFrameState      State;
    InputCommandProvider Provider(State);
    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    Provider.Unbind();
    Commands.ExecutePhase(CommandPhase::Input, 1);
    EXPECT_EQ(State.AdvanceFrame().Keyboard.States[std::to_underlying(WindowKey::W)], InputState::Up);

    ASSERT_TRUE(Provider.Bind(Commands));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::W, GLFW_KEY_W, InputAction::Press)));
    Commands.ExecutePhase(CommandPhase::Input, 2);
    (void)State.AdvanceFrame();
    ASSERT_TRUE(State.GetFrame().Keyboard.IsKeyDown(WindowKey::W));
    ASSERT_TRUE(Provider.Submit(Key(WindowKey::A, GLFW_KEY_A, InputAction::Press)));
    Commands.Shutdown();
    State.Shutdown();
    EXPECT_FALSE(State.GetFrame().Keyboard.IsKeyDown(WindowKey::W));
    EXPECT_FALSE(State.GetFrame().Keyboard.IsKeyDown(WindowKey::A));
    EXPECT_TRUE(State.GetFrame().Events.empty());
}
