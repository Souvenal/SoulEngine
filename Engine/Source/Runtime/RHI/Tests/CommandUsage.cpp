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

TEST_F(UsageVisitorTest, SetPipelineCmdUpdatesToken) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    std::visit(m_Visitor, Command{SetPipelineCmd{m_Pipeline.get()}});
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, BindVertexBufferCmdUpdatesToken) {
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    std::visit(m_Visitor, Command{BindVertexBufferCmd{m_VB.get(), 0}});
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, BindIndexBufferCmdUpdatesToken) {
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);
    std::visit(m_Visitor, Command{BindIndexBufferCmd{m_IB.get(), 0}});
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, SetDrawMaterialDataCmdUpdatesTextureToken) {
    ASSERT_EQ(m_Texture->GetLastUsageToken().Id, 0);
    DrawMaterialData Mat{.TestTexture = m_Texture.get()};
    std::visit(m_Visitor, Command{SetDrawMaterialDataCmd{Mat}});
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, NullPipelineDoesNotCrash) {
    std::visit(m_Visitor, Command{SetPipelineCmd{nullptr}});
    // Should not crash — no assertion needed beyond survival.
}

TEST_F(UsageVisitorTest, NullMaterialTextureDoesNotCrash) {
    std::visit(m_Visitor, Command{SetDrawMaterialDataCmd{DrawMaterialData{}}});
    // Should not crash — no assertion needed beyond survival.
}

TEST_F(UsageVisitorTest, NonResourceCommandsDoNotUpdateAnyToken) {
    // These command types don't reference GPU resources.
    std::visit(m_Visitor, Command{SetViewportCmd{}});
    std::visit(m_Visitor, Command{SetFullViewportCmd{}});
    std::visit(m_Visitor, Command{SetScissorCmd{}});
    std::visit(m_Visitor, Command{SetFullScissorRectCmd{}});
    std::visit(m_Visitor, Command{DrawIndexedCmd{}});
    std::visit(m_Visitor, Command{DrawCmd{}});

    // Tokens on all resources should remain default (0).
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 0);
}

TEST_F(UsageVisitorTest, VisitEntireCommandList) {
    CommandList CmdList;
    auto&       Pass = CmdList.Passes.emplace_back();

    Pass.SetPipeline(m_Pipeline.get());
    Pass.BindVertexBuffer(m_VB.get());
    Pass.BindIndexBuffer(m_IB.get());
    Pass.SetDrawMaterialData(DrawMaterialData{.TestTexture = m_Texture.get()});
    Pass.SetViewport(0, 0, 800, 600);
    Pass.DrawIndexed(6);

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
