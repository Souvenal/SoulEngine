#include <gtest/gtest.h>

import Renderer;

using namespace SoulEngine;

namespace {

class RendererSelectionTestRenderer final : public IRenderer {
  public:
    ~RendererSelectionTestRenderer() override {
        OnDetach();
    }

    [[nodiscard]] auto OnAttach() -> std::expected<void, ErrorMessage> override {
        ++AttachCount;
        m_IsAttached = true;
        return {};
    }

    auto OnDetach() -> void override {
        if (!m_IsAttached)
            return;
        ++DetachCount;
        m_IsAttached = false;
    }

    [[nodiscard]] auto Render(const SceneSnapshot&) -> std::expected<RenderResult, ErrorMessage> override {
        return RenderResult{};
    }

    static inline Int32 AttachCount = 0;
    static inline Int32 DetachCount = 0;

  private:
    bool m_IsAttached = false;
};

RendererFactory::AutoRegistrar<RendererSelectionTestRenderer> RegRendererSelectionTestRenderer{
    "RendererSelectionTest"};

TEST(RendererSelection, CachesSuccessfulSelectionAndPreservesCurrentRendererOnFailure) {
    CloseRenderers();
    RendererSelectionTestRenderer::AttachCount = 0;
    RendererSelectionTestRenderer::DetachCount = 0;

    const auto FirstSelection = SelectRenderer("RendererSelectionTest");
    ASSERT_TRUE(FirstSelection.has_value()) << FirstSelection.error().ToString();
    const auto FirstRenderer = GetCurrentRenderer();
    ASSERT_NE(FirstRenderer, nullptr);
    EXPECT_EQ(GetCurrentRendererName(), "RendererSelectionTest");
    EXPECT_EQ(RendererSelectionTestRenderer::AttachCount, 1);

    const auto CachedSelection = SelectRenderer("RendererSelectionTest");
    ASSERT_TRUE(CachedSelection.has_value()) << CachedSelection.error().ToString();
    EXPECT_EQ(GetCurrentRenderer().get(), FirstRenderer.get());
    EXPECT_EQ(RendererSelectionTestRenderer::AttachCount, 1);

    const auto FailedSelection = SelectRenderer("UnknownRendererSelectionTest");
    EXPECT_FALSE(FailedSelection.has_value());
    EXPECT_EQ(GetCurrentRenderer().get(), FirstRenderer.get());
    EXPECT_EQ(GetCurrentRendererName(), "RendererSelectionTest");

    CloseRenderers();
    EXPECT_FALSE(GetCurrentRenderer());
    EXPECT_EQ(RendererSelectionTestRenderer::DetachCount, 1);
}

} // namespace
