#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "HeadlessEditor.h"

import std;
import Application;
import Core;
import Editor.UIManager;
import EditorTypes;
import Renderer;
import RHI;
import Scene;

namespace SoulEngine {

class AEditorCommandApplication final : public Application {};
class BrokenEditorCommandApplication final : public Application {};
ApplicationFactory::AutoRegistrar<AEditorCommandApplication>      RegAEditorCommandApplication{"AEditorCommand"};
ApplicationFactory::AutoRegistrar<BrokenEditorCommandApplication> RegBrokenEditorCommandApplication{"Broken"};

template <int Kind>
class EditorCommandTestRenderer final : public IRenderer {
  public:
    ~EditorCommandTestRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        ++AttachCount;
        if constexpr (Kind == 2)
            return std::unexpected(ErrorMessage("Intentional Editor command attachment failure"));
        m_IsAttached = true;
        return {};
    }
    auto OnDetach() -> void override {
        if (!m_IsAttached)
            return;
        ++DetachCount;
        m_IsAttached = false;
    }
    [[nodiscard]] auto Render(const GameSnapshot&, const EditorSnapshot&)
        -> std::expected<RenderResult, ErrorMessage> override {
        return RenderResult{};
    }

    static inline Int32 AttachCount = 0;
    static inline Int32 DetachCount = 0;

  private:
    bool m_IsAttached = false;
};

using PrimaryEditorCommandRenderer   = EditorCommandTestRenderer<0>;
using SecondaryEditorCommandRenderer = EditorCommandTestRenderer<1>;
using FailingEditorCommandRenderer   = EditorCommandTestRenderer<2>;
RendererFactory::AutoRegistrar<PrimaryEditorCommandRenderer>   RegPrimaryEditorCommandRenderer{"EditorCommandPrimary"};
RendererFactory::AutoRegistrar<SecondaryEditorCommandRenderer> RegSecondaryEditorCommandRenderer{
    "EditorCommandSecondary"};
RendererFactory::AutoRegistrar<FailingEditorCommandRenderer> RegFailingEditorCommandRenderer{"EditorCommandFailure"};

struct RuntimeReset {
    RuntimeReset() {
        CloseApplication();
        CloseRenderers();
    }
    ~RuntimeReset() {
        CloseApplication();
        CloseRenderers();
    }
};

TEST(ApplicationCommandIntegration, ActionReplacementPreservesFailuresCurrentTargetsAndSceneLifetime) {
    RuntimeReset Reset;
    const auto*  SourceDirectory = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
    ASSERT_NE(SourceDirectory, nullptr);
    EnginePaths::Get().Init((Path(SourceDirectory) / "Fixtures" / "Engine").lexically_normal());

    HeadlessEditor Fixture;
    ASSERT_TRUE(Fixture.Bind());
    ASSERT_TRUE(OpenApplication("Test"));
    auto* InitialApplication = GetCurrentApplication();
    ASSERT_NE(InitialApplication, nullptr);
    auto* InitialScene = std::addressof(InitialApplication->GetScene());
    InitialApplication->GetScene().AddTexturePath("preserve-current-scene");
    Fixture.EditorValue.BindScene(*InitialScene);

    ASSERT_TRUE(UIManager::Get().SubmitPanelToggle(Fixture.Commands, "Debug.ShowMaterials"));
    Fixture.DispatchAction();
    Fixture.Frame();

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.application.open", String("Broken")));
    EXPECT_EQ(GetCurrentApplication(), InitialApplication);
    Fixture.DispatchAction();
    auto Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "application_open_failed");
    EXPECT_EQ(GetCurrentApplication(), InitialApplication);
    EXPECT_EQ(std::addressof(GetCurrentApplication()->GetScene()), InitialScene);
    ASSERT_EQ(InitialScene->GetTexturePaths().size(), 1);
    EXPECT_EQ(InitialScene->GetTexturePaths().front(), "preserve-current-scene");
    Fixture.Frame();

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.application.open", String("MissingApplication")));
    Fixture.DispatchAction();
    Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "stale_target");
    EXPECT_EQ(GetCurrentApplication(), InitialApplication);
    ASSERT_EQ(InitialScene->GetTexturePaths().size(), 1);
    EXPECT_EQ(InitialScene->GetTexturePaths().front(), "preserve-current-scene");

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.application.open", String("Test")));
    Fixture.DispatchAction();
    EXPECT_TRUE(Fixture.Commands.TakeDiagnostics().empty());
    EXPECT_EQ(GetCurrentApplication(), InitialApplication);
    ASSERT_EQ(InitialScene->GetTexturePaths().size(), 1);
    EXPECT_EQ(InitialScene->GetTexturePaths().front(), "preserve-current-scene");

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.application.open", String("Viking")));
    EXPECT_EQ(GetCurrentApplication(), InitialApplication);
    Fixture.DispatchAction();
    EXPECT_TRUE(Fixture.Commands.TakeDiagnostics().empty());
    ASSERT_NE(GetCurrentApplication(), nullptr);
    EXPECT_EQ(GetCurrentApplication()->GetName(), "Viking");
    EXPECT_TRUE(GetCurrentApplication()->GetScene().GetTexturePaths().empty());
    Fixture.Frame();

    GetCurrentApplication()->GetScene().AddTexturePath("preserve-repeated-viking");
    auto* VikingApplication = GetCurrentApplication();
    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.application.open", String("Viking")));
    Fixture.DispatchAction();
    EXPECT_EQ(GetCurrentApplication(), VikingApplication);
    ASSERT_EQ(GetCurrentApplication()->GetScene().GetTexturePaths().size(), 1);
    EXPECT_EQ(GetCurrentApplication()->GetScene().GetTexturePaths().front(), "preserve-repeated-viking");

    std::optional<CommandResult> AgentResult;
    EXPECT_FALSE(Fixture.Commands.SubmitByAgent(
        "editor.application.open", nlohmann::json{{"application", "Test"}}, [&](CommandResult Result) -> void {
            AgentResult = std::move(Result);
        }));
    ASSERT_TRUE(AgentResult);
    ASSERT_TRUE(AgentResult->Failure);
    EXPECT_EQ(AgentResult->Failure->Code, "denied_exposure");
    EXPECT_EQ(GetCurrentApplication(), VikingApplication);
}

TEST(ApplicationCommandIntegration, DisabledCurrentMenuItemAdmitsNoCommand) {
    RuntimeReset Reset;
    const auto*  SourceDirectory = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
    ASSERT_NE(SourceDirectory, nullptr);
    EnginePaths::Get().Init((Path(SourceDirectory) / "Fixtures" / "Engine").lexically_normal());

    HeadlessEditor Fixture(1);
    ASSERT_TRUE(Fixture.Bind());
    ASSERT_TRUE(OpenApplication("AEditorCommand"));
    auto* CurrentApplication = GetCurrentApplication();
    ASSERT_NE(CurrentApplication, nullptr);
    CurrentApplication->GetScene().AddTexturePath("disabled-current-marker");
    Fixture.EditorValue.BindScene(CurrentApplication->GetScene());
    Fixture.Frame();

    const float ProjectX = MainMenuX({"Settings", "Project"}, "Project");
    Fixture.Click(ImVec2(ProjectX, 10.0F));
    Fixture.Click(ImVec2(ProjectX, 32.0F));
    ASSERT_TRUE(ClickImGuiMenuRow(Fixture, "Open Application", 0));

    auto Sentinel = UIManager::Get().SubmitPanelToggle(Fixture.Commands, "Settings");
    ASSERT_TRUE(Sentinel) << Sentinel.error().ToString();
    Fixture.DispatchAction();
    EXPECT_TRUE(UIManager::Get().IsUIShowing("Settings"));
    EXPECT_EQ(GetCurrentApplication(), CurrentApplication);

    Fixture.Click(ImVec2(700.0F, 550.0F));
    Fixture.Click(ImVec2(ProjectX, 10.0F));
    Fixture.Click(ImVec2(ProjectX, 32.0F));
    ASSERT_TRUE(ClickImGuiMenuRow(Fixture, "Open Application", 1));
    EXPECT_FALSE(UIManager::Get().SubmitPanelToggle(Fixture.Commands, "Debug.ShowMaterials"));
    Fixture.DispatchAction();
    const auto Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "application_open_failed");
    EXPECT_EQ(GetCurrentApplication(), CurrentApplication);
    ASSERT_EQ(CurrentApplication->GetScene().GetTexturePaths().size(), 1);
    EXPECT_EQ(CurrentApplication->GetScene().GetTexturePaths().front(), "disabled-current-marker");
}

TEST(RendererCommandIntegration, UnavailableTargetsWithoutActiveRendererFail) {
    RuntimeReset   Reset;
    HeadlessEditor Fixture;
    ASSERT_TRUE(Fixture.Bind());
    ASSERT_FALSE(GetCurrentRenderer());
    ASSERT_TRUE(GetCurrentRendererName().empty());

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.renderer.select", String{}));
    Fixture.DispatchAction();
    auto Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "stale_target");
    EXPECT_FALSE(GetCurrentRenderer());

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.renderer.select", String("MissingRenderer")));
    Fixture.DispatchAction();
    Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "stale_target");
    EXPECT_FALSE(GetCurrentRenderer());
}

TEST(RendererCommandIntegration, ActionSelectionPreservesFailuresAndAvoidsRepeatedAttachment) {
    RuntimeReset Reset;
    PrimaryEditorCommandRenderer::AttachCount   = 0;
    PrimaryEditorCommandRenderer::DetachCount   = 0;
    SecondaryEditorCommandRenderer::AttachCount = 0;
    SecondaryEditorCommandRenderer::DetachCount = 0;
    FailingEditorCommandRenderer::AttachCount   = 0;
    FailingEditorCommandRenderer::DetachCount   = 0;

    HeadlessEditor Fixture;
    ASSERT_TRUE(Fixture.Bind());
    ASSERT_TRUE(SelectRenderer("EditorCommandPrimary"));
    const auto Primary = GetCurrentRenderer();
    ASSERT_NE(Primary, nullptr);
    EXPECT_EQ(PrimaryEditorCommandRenderer::AttachCount, 1);

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.renderer.select", String("EditorCommandPrimary")));
    EXPECT_EQ(GetCurrentRendererName(), "EditorCommandPrimary");
    Fixture.DispatchAction();
    EXPECT_TRUE(Fixture.Commands.TakeDiagnostics().empty());
    EXPECT_EQ(GetCurrentRenderer().get(), Primary.get());
    EXPECT_EQ(PrimaryEditorCommandRenderer::AttachCount, 1);

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.renderer.select", String("EditorCommandSecondary")));
    EXPECT_EQ(GetCurrentRendererName(), "EditorCommandPrimary");
    Fixture.DispatchAction();
    EXPECT_TRUE(Fixture.Commands.TakeDiagnostics().empty());
    const auto Secondary = GetCurrentRenderer();
    ASSERT_NE(Secondary, nullptr);
    EXPECT_EQ(GetCurrentRendererName(), "EditorCommandSecondary");
    EXPECT_EQ(SecondaryEditorCommandRenderer::AttachCount, 1);

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.renderer.select", String("EditorCommandFailure")));
    Fixture.DispatchAction();
    auto Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "renderer_selection_failed");
    EXPECT_EQ(GetCurrentRenderer().get(), Secondary.get());
    EXPECT_EQ(GetCurrentRendererName(), "EditorCommandSecondary");
    EXPECT_EQ(FailingEditorCommandRenderer::AttachCount, 1);

    ASSERT_TRUE(Fixture.Commands.SubmitByUser("editor.renderer.select", String("MissingRenderer")));
    Fixture.DispatchAction();
    Diagnostics = Fixture.Commands.TakeDiagnostics();
    ASSERT_EQ(Diagnostics.size(), 1);
    ASSERT_TRUE(Diagnostics.front().Failure);
    EXPECT_EQ(Diagnostics.front().Failure->Code, "stale_target");
    EXPECT_EQ(GetCurrentRenderer().get(), Secondary.get());

    std::optional<CommandResult> AgentResult;
    EXPECT_FALSE(
        Fixture.Commands.SubmitByAgent("editor.renderer.select",
                                       nlohmann::json{{"renderer", "EditorCommandPrimary"}},
                                       [&](CommandResult Result) -> void { AgentResult = std::move(Result); }));
    ASSERT_TRUE(AgentResult);
    ASSERT_TRUE(AgentResult->Failure);
    EXPECT_EQ(AgentResult->Failure->Code, "denied_exposure");
    EXPECT_EQ(GetCurrentRenderer().get(), Secondary.get());

    CloseRenderers();
    EXPECT_EQ(PrimaryEditorCommandRenderer::DetachCount, 1);
    EXPECT_EQ(SecondaryEditorCommandRenderer::DetachCount, 1);
    EXPECT_EQ(FailingEditorCommandRenderer::DetachCount, 0);
    EXPECT_FALSE(GetCurrentRenderer());
}

} // namespace SoulEngine
