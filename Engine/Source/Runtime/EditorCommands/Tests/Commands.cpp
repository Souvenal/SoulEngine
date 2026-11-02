#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
import std;
import Core;
import EditorCommands;
using namespace SoulEngine;
using Json = nlohmann::json;
TEST(CommandRegistrationAndSubmission, UserCommandWaitsForActionDispatch) {
    CommandDispatcher Commands;
    int               Calls = 0;
    Commands.Register("settings.toggle", [&]() -> void { ++Calls; });
    ASSERT_TRUE(Commands.SubmitByUser("settings.toggle"));
    EXPECT_EQ(Calls, 0);
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(Calls, 1);
}
TEST(CommandRegistrationAndSubmission, PositionalAndNamedArgumentsReachOneHandler) {
    CommandDispatcher Commands;
    String            Seen;
    auto              Registration = Commands.Register(
        CommandDescriptor{
            .Name = "sample", .Parameters = {{.Name = "text"}, {.Name = "count"}}, .IsAgentExposed = true},
        [&](CommandContext&, String Text, Int32 Count) -> void { Seen = Text + std::to_string(Count); });
    ASSERT_TRUE(Registration);
    ASSERT_TRUE(Commands.SubmitByUser("sample", String("user"), Int32(7)));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(Seen, "user7");
    ASSERT_TRUE(Commands.SubmitByAgent("sample", Json{{"count", 9}, {"text", "agent"}}));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_EQ(Seen, "agent9");
}
TEST(CommandRegistrationAndSubmission, IntegerOverflowIsRejectedBeforeAdmission) {
    CommandDispatcher Commands;
    ASSERT_TRUE(
        Commands.Register(CommandDescriptor{.Name = "small", .Parameters = {{.Name = "value"}}, .IsAgentExposed = true},
                          [](CommandContext&, Int32) -> void {}));
    EXPECT_FALSE(Commands.SubmitByAgent("small", Json{{"value", 4294967296LL}}));
}
TEST(CommandRegistrationAndSubmission, DefaultsBoundsAndDescriptorCompatibilityAreShared) {
    CommandDispatcher Commands;
    Int32             Seen = 0;
    CommandDescriptor Desc{.Name           = "bounded",
                           .Parameters     = {{.Name = "value", .Minimum = 1, .Maximum = 5, .Default = 3}},
                           .IsAgentExposed = true};
    ASSERT_TRUE(Commands.Register(Desc, [&](CommandContext&, Int32 Value) -> void { Seen = Value; }));
    ASSERT_TRUE(Commands.SubmitByAgent("bounded", Json::object()));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(Seen, 3);
    EXPECT_FALSE(Commands.SubmitByUser("bounded", Int32(6)));
    EXPECT_FALSE(Commands.SubmitByAgent("bounded", Json{{"other", 2}}));
    EXPECT_FALSE(Commands.SubmitByAgent("bounded", Json{{"value", "wrong"}}));
    EXPECT_FALSE(Commands.Register(Desc, [](CommandContext&, Int32) -> void {}));
    Desc.Name               = "mismatch";
    Desc.Parameters[0].Type = CommandValueType::Boolean;
    EXPECT_FALSE(Commands.Register(Desc, [](CommandContext&, Int32) -> void {}));
}
TEST(MainThreadPhaseDispatch, CapacityAndFrozenBudgetRetainWorkInAdmissionOrder) {
    CommandDispatcher  Commands(2);
    std::vector<Int32> Seen;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "append", .Parameters = {{.Name = "value"}}},
                                  [&](CommandContext&, Int32 Value) -> void { Seen.push_back(Value); }));
    ASSERT_TRUE(Commands.SubmitByUser("append", Int32(1)));
    ASSERT_TRUE(Commands.SubmitByUser("append", Int32(2)));
    EXPECT_FALSE(Commands.SubmitByUser("append", Int32(3)));
    Commands.ExecutePhase(CommandPhase::Action, 1, 1);
    EXPECT_EQ(Seen, (std::vector<Int32>{1}));
    Commands.ExecutePhase(CommandPhase::Action, 2, 1);
    EXPECT_EQ(Seen, (std::vector<Int32>{1, 2}));
}
TEST(MainThreadPhaseDispatch, ReentrantSamePhaseWaitsUntilNextFrame) {
    CommandDispatcher Commands;
    int               Calls = 0;
    Commands.Register("again", [&]() -> void {
        ++Calls;
        ASSERT_TRUE(Commands.SubmitByUser("again"));
    });
    ASSERT_TRUE(Commands.SubmitByUser("again"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(Calls, 1);
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_EQ(Calls, 2);
}
TEST(InvocationCompletionAndProviderLifetime, VoidSuccessReportsIdentityFrameAndOwnedOutput) {
    CommandDispatcher          Commands;
    std::vector<CommandResult> Results;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "query", .IsAgentExposed = true},
                                  [](CommandContext& Context) -> void { Context.SetOutput(Json{{"answer", 42}}); }));
    auto Id = Commands.SubmitByAgent(
        "query", Json::object(), [&](CommandResult Result) -> void { Results.push_back(std::move(Result)); });
    ASSERT_TRUE(Id);
    EXPECT_TRUE(Results.empty());
    Commands.ExecutePhase(CommandPhase::Action, 8);
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Invocation, *Id);
    EXPECT_EQ(Results[0].Frame, 8);
    EXPECT_EQ(Results[0].Status, CommandStatus::Succeeded);
    EXPECT_EQ(Results[0].Output, Json({{"answer", 42}}));
}
TEST(InvocationCompletionAndProviderLifetime, RetirementInvalidatesFrozenWorkAndReregisterDoesNotInheritIt) {
    CommandDispatcher          Commands;
    int                        OldCalls = 0, NewCalls = 0;
    std::vector<CommandResult> Results;
    Uint64                     Token = 0;
    Commands.Register("retire", [&]() -> void { Commands.Retire(Token); });
    auto Registered = Commands.Register(CommandDescriptor{.Name = "target", .IsAgentExposed = true},
                                        [&](CommandContext&) -> void { ++OldCalls; });
    ASSERT_TRUE(Registered);
    Token = *Registered;
    ASSERT_TRUE(Commands.SubmitByUser("retire"));
    ASSERT_TRUE(Commands.SubmitByAgent(
        "target", Json::object(), [&](CommandResult R) -> void { Results.push_back(std::move(R)); }));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_EQ(OldCalls, 0);
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Status, CommandStatus::Cancelled);
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "target"}, [&](CommandContext&) -> void { ++NewCalls; }));
    Commands.ExecutePhase(CommandPhase::Action, 2);
    EXPECT_EQ(NewCalls, 0);
}
TEST(InvocationCompletionAndProviderLifetime, CancelDeadlineAndShutdownSettleOnceWithoutExecuting) {
    CommandDispatcher          Commands;
    int                        Calls = 0;
    std::vector<CommandResult> Results;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "work", .IsAgentExposed = true},
                                  [&](CommandContext&) -> void { ++Calls; }));
    auto Sink = [&](CommandResult R) -> void { Results.push_back(std::move(R)); };
    auto A    = Commands.SubmitByAgent("work", Json::object(), Sink);
    ASSERT_TRUE(A);
    Commands.Cancel(*A);
    Commands.Cancel(*A);
    ASSERT_TRUE(Commands.SubmitByAgent("work", Json::object(), Sink, CommandOptions{.Deadline = 10}));
    Commands.ExecutePhase(CommandPhase::Action, 1, 64, 10);
    ASSERT_TRUE(Commands.SubmitByAgent("work", Json::object(), Sink));
    Commands.Shutdown();
    Commands.Shutdown();
    EXPECT_EQ(Calls, 0);
    ASSERT_EQ(Results.size(), 3);
    EXPECT_EQ(Results[0].Failure->Code, "cancelled");
    EXPECT_EQ(Results[1].Failure->Code, "deadline");
    EXPECT_EQ(Results[2].Failure->Code, "shutdown");
    EXPECT_FALSE(Commands.SubmitByUser("work"));
}
TEST(InvocationCompletionAndProviderLifetime, CancellationOfFrozenCallFromWorkerIsObservedBeforeStart) {
    CommandDispatcher          Commands(2);
    std::barrier               Gate(2);
    Uint64                     Target = 0;
    int                        Calls  = 0;
    std::vector<CommandResult> Results;
    Commands.Register("gate", [&]() -> void {
        Gate.arrive_and_wait();
        Gate.arrive_and_wait();
    });
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "later", .IsAgentExposed = true},
                                  [&](CommandContext&) -> void { ++Calls; }));
    ASSERT_TRUE(Commands.SubmitByUser("gate"));
    auto Accepted = Commands.SubmitByAgent(
        "later", Json::object(), [&](CommandResult R) -> void { Results.push_back(std::move(R)); });
    ASSERT_TRUE(Accepted);
    Target = *Accepted;
    std::jthread Worker([&]() -> void {
        Gate.arrive_and_wait();
        Commands.Cancel(Target);
        Gate.arrive_and_wait();
    });
    Commands.ExecutePhase(CommandPhase::Action, 1);
    Worker.join();
    EXPECT_EQ(Calls, 0);
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Failure->Code, "cancelled");
}
TEST(MainThreadPhaseDispatch, FrozenUnstartedCallsStillConsumeCapacity) {
    CommandDispatcher Commands(2);
    bool              Rejected = false;
    Commands.Register("first", [&]() -> void { Rejected = !Commands.SubmitByUser("second"); });
    Commands.Register("second", []() -> void {});
    ASSERT_TRUE(Commands.SubmitByUser("first"));
    ASSERT_TRUE(Commands.SubmitByUser("second"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(Rejected);
}
TEST(CommandRegistrationAndSubmission, ExposedSnapshotOwnsSchemaAndArgumentResultBudgets) {
    CommandDispatcher Commands;
    ASSERT_TRUE(
        Commands.Register(CommandDescriptor{.Name             = "boundedJson",
                                            .Parameters       = {{.Name = "text"}},
                                            .IsAgentExposed   = true,
                                            .MaxArgumentBytes = 8,
                                            .MaxResultBytes   = 8},
                          [](CommandContext& C, String) -> void { C.SetOutput(Json{{"large", "long output"}}); }));
    const auto Snapshot = Commands.DescribeExposed();
    ASSERT_EQ(Snapshot.size(), 1);
    EXPECT_TRUE(Snapshot[0].Schema["properties"].contains("text"));
    EXPECT_FALSE(Commands.SubmitByAgent("boundedJson", Json{{"text", "too long"}}));
    std::vector<CommandResult> Results;
    ASSERT_TRUE(Commands.SubmitByAgent(
        "boundedJson", Json{{"text", "a"}}, [&](CommandResult R) -> void { Results.push_back(std::move(R)); }));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Failure->Code, "result_budget");
}
TEST(InvocationCompletionAndProviderLifetime, UserExecutionFailuresRemainDiagnosable) {
    CommandDispatcher Commands;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "fail"}, [](CommandContext& C) -> void {
        C.Fail("stale_target", "gone", Json{{"target", 7}});
    }));
    ASSERT_TRUE(Commands.SubmitByUser("fail"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    const auto Diagnostics = Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    EXPECT_EQ(Diagnostics[0].Failure->Code, "stale_target");
    EXPECT_TRUE(Commands.TakeDiagnostics().empty());
}
TEST(CommandRegistrationAndSubmission, InvalidDefaultsAreRejectedAtRegistration) {
    CommandDispatcher Commands;
    EXPECT_FALSE(
        Commands.Register(CommandDescriptor{.Name = "bad", .Parameters = {{.Name = "value", .Default = "not integer"}}},
                          [](CommandContext&, Int32) -> void {}));
}
TEST(CommandRegistrationAndSubmission, StructuredAdmissionFailureAndExposurePolicyAreObservable) {
    CommandDispatcher          Commands;
    std::vector<CommandResult> Results;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "private"}, [](CommandContext&) -> void {}));
    EXPECT_FALSE(Commands.SubmitByAgent(
        "private", Json::object(), [&](CommandResult R) -> void { Results.push_back(std::move(R)); }));
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Failure->Code, "denied_exposure");
    EXPECT_EQ(Results[0].Status, CommandStatus::Failed);
}
TEST(MainThreadPhaseDispatch, OwningArgumentsSurviveCallerMutationAndLaterPhaseRunsThisFrame) {
    CommandDispatcher Commands;
    String            Seen;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name           = "observe",
                                                    .Parameters     = {{.Name = "text"}},
                                                    .IsAgentExposed = true,
                                                    .Phase          = CommandPhase::PostUpdate},
                                  [&](CommandContext&, String Text) -> void { Seen = Text; }));
    Commands.Register("schedule", [&]() -> void {
        Json Arguments{{"text", "owned"}};
        ASSERT_TRUE(Commands.SubmitByAgent("observe", Arguments));
        Arguments["text"] = "changed";
    });
    ASSERT_TRUE(Commands.SubmitByUser("schedule"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    EXPECT_TRUE(Seen.empty());
    Commands.ExecutePhase(CommandPhase::PostUpdate, 1);
    EXPECT_EQ(Seen, "owned");
}
TEST(CommandRegistrationAndSubmission, ExposureCategoryMustBeExplicitlyPermitted) {
    CommandDispatcher Commands;
    ASSERT_TRUE(Commands.Register(
        CommandDescriptor{.Name = "danger", .IsAgentExposed = true, .Category = CommandCategory::Destructive},
        [](CommandContext&) -> void {}));
    EXPECT_TRUE(Commands.DescribeExposed().empty());
    EXPECT_FALSE(Commands.SubmitByAgent("danger", Json::object()));
}
TEST(InvocationCompletionAndProviderLifetime, StartedHandlerFinishesDespiteWorkerCancellation) {
    CommandDispatcher          Commands;
    std::barrier               Gate(2);
    std::vector<CommandResult> Results;
    int                        Calls = 0;
    ASSERT_TRUE(
        Commands.Register(CommandDescriptor{.Name = "started", .IsAgentExposed = true}, [&](CommandContext&) -> void {
            Gate.arrive_and_wait();
            Gate.arrive_and_wait();
            ++Calls;
        }));
    auto Id = Commands.SubmitByAgent(
        "started", Json::object(), [&](CommandResult R) -> void { Results.push_back(std::move(R)); });
    ASSERT_TRUE(Id);
    std::jthread Worker([&]() -> void {
        Gate.arrive_and_wait();
        Commands.Cancel(*Id);
        Gate.arrive_and_wait();
    });
    Commands.ExecutePhase(CommandPhase::Action, 1);
    Worker.join();
    Commands.Cancel(*Id);
    ASSERT_EQ(Results.size(), 1);
    EXPECT_EQ(Results[0].Status, CommandStatus::Succeeded);
    EXPECT_EQ(Calls, 1);
}
TEST(MainThreadPhaseDispatch, RecursiveLaterPhaseDispatchDoesNotRunInsideHandler) {
    CommandDispatcher Commands;
    bool              IsInside = false, ObservedInside = false;
    ASSERT_TRUE(Commands.Register(CommandDescriptor{.Name = "later", .Phase = CommandPhase::PostUpdate},
                                  [&](CommandContext&) -> void { ObservedInside = IsInside; }));
    Commands.Register("outer", [&]() -> void {
        IsInside = true;
        ASSERT_TRUE(Commands.SubmitByUser("later"));
        Commands.ExecutePhase(CommandPhase::PostUpdate, 1);
        IsInside = false;
    });
    ASSERT_TRUE(Commands.SubmitByUser("outer"));
    Commands.ExecutePhase(CommandPhase::Action, 1);
    Commands.ExecutePhase(CommandPhase::PostUpdate, 1);
    EXPECT_FALSE(ObservedInside);
}
