/// @file   CommandUsage.cpp
/// @brief  Tests for UsageVisitor — verifies that GpuResource usage tokens
///         are correctly updated when visiting each command variant.

#include <gtest/gtest.h>

import RHI;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine::RHI;

// ── Mocks ────────────────────────────────────────────────────────────────

/// Concrete SampledTexture for testing (RHI::SampledTexture has pure virtuals).
class MockSampledTexture final : public SampledTexture {
  public:
    MockSampledTexture() = default;
    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return 256;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return 256;
    }
};

// ── Fixtures ─────────────────────────────────────────────────────────────

class UsageVisitorTest : public ::testing::Test {
  protected:
    SPtr<GraphicsPipeline>   m_Pipeline = std::make_shared<GraphicsPipeline>();
    SPtr<VertexBuffer>       m_VB       = std::make_shared<VertexBuffer>();
    SPtr<IndexBuffer>        m_IB       = std::make_shared<IndexBuffer>();
    SPtr<MockSampledTexture> m_Texture  = std::make_shared<MockSampledTexture>();

    GpuCompletionToken m_Token{42};
    UsageVisitor       m_Visitor{m_Token};
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST_F(UsageVisitorTest, SetGraphicsPipelineCmdUpdatesToken) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor, Command{SetGraphicsPipelineCmd{.PipelinePtr = m_Pipeline.get()}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, DrawIndexedCmdUpdatesReferencedResources) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Texture->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{DrawIndexedCmd{.PipelinePtr     = m_Pipeline.get(),
                                       .VertexBufferPtr = m_VB.get(),
                                       .IndexBufferPtr  = m_IB.get(),
                                       .Parameters      = DrawParameter{.TestTexture = m_Texture.get()}}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, DrawCmdUpdatesReferencedResources) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Texture->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{DrawCmd{.PipelinePtr     = m_Pipeline.get(),
                               .VertexBufferPtr = m_VB.get(),
                               .Parameters      = DrawParameter{.TestTexture = m_Texture.get()}}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, NullPipelineDoesNotCrash) {
    std::visit(m_Visitor, Command{DrawCmd{.PipelinePtr = nullptr}});
    // Should not crash — no assertion needed beyond survival.
}

TEST_F(UsageVisitorTest, NullDrawResourcesDoNotCrash) {
    std::visit(m_Visitor, Command{DrawIndexedCmd{}});
    std::visit(m_Visitor, Command{DrawCmd{}});
    // Should not crash — no assertion needed beyond survival.
}

TEST_F(UsageVisitorTest, NonResourceCommandsDoNotUpdateAnyToken) {
    // These command types don't reference GPU resources.
    std::visit(m_Visitor, Command{SetViewportCmd{}});
    std::visit(m_Visitor, Command{SetFullViewportCmd{}});
    std::visit(m_Visitor, Command{SetScissorCmd{}});
    std::visit(m_Visitor, Command{SetFullScissorRectCmd{}});

    // Tokens on all resources should remain default (0).
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 0);
}

TEST_F(UsageVisitorTest, VisitEntireCommandList) {
    CommandList CmdList;
    auto&       Pass = CmdList.Passes.emplace_back();

    Pass.SetViewport(0, 0, 800, 600);
    Pass.SetGraphicsPipeline(m_Pipeline.get());
    Pass.DrawIndexed(m_Pipeline.get(),
                     m_VB.get(),
                     m_IB.get(),
                     DrawParameter{.TestTexture = m_Texture.get()});

    // Token starts at 0 for all resources
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Texture->GetLastUsageToken().Id, 0);

    // Visit all commands with a single UsageVisitor
    for (const auto& Cmd : Pass.Commands)
        std::visit(UsageVisitor{GpuCompletionToken{.Id = 99}}, Cmd);

    // Resource commands updated
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 99);
}
