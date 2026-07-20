/// @file   CommandUsage.cpp
/// @brief  Tests for UsageVisitor — verifies that GpuResource usage tokens
///         are correctly updated when visiting each command variant.

#include <gtest/gtest.h>

import RHI;
import Shader;
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
    SPtr<VertexBuffer>       m_SecondVB = std::make_shared<VertexBuffer>();
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

TEST_F(UsageVisitorTest, PushConstantsCmdUpdatesPipelineToken) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{PushConstantsCmd{
                   .PipelinePtr = m_Pipeline.get(),
                   .Data        = {std::byte{0}},
               }});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, BindShaderParametersCmdUpdatesReferencedResources) {
    ResourceArray<SampledTexture> Textures;
    Textures.Set(3, m_Texture.get());
    EXPECT_EQ(Textures.GetSize(), 4);
    auto Parameters = ShaderParameters::Create(ShaderParameterLayout::Create(SoulEngine::Shader::Reflection{
        .Bindings =
            {
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_textures.uTextures",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = SoulEngine::Shader::ResourceType::SampledTexture,
                    .ArrayCount    = std::numeric_limits<Uint32>::max(),
                },
            },
    }));
    ASSERT_TRUE(Parameters.SetResourceArray("g_textures.uTextures", Textures));

    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Texture->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{BindShaderParametersCmd{
                   .PipelinePtr = m_Pipeline.get(),
                   .Parameters  = std::move(Parameters),
               }});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 42);
}

TEST(ResourceArrayTest, SupportsMultipleResourceTypes) {
    Sampler SamplerValue{SamplerDesc{}};
    ResourceArray<Sampler> Samplers;

    Samplers.Set(2, &SamplerValue);

    ASSERT_EQ(Samplers.GetSize(), 3);
    EXPECT_EQ(Samplers.GetResources()[2], &SamplerValue);
}

TEST(ShaderParametersTest, ReflectionAutomaticallyPartitionsParameterSets) {
    auto Layout = ShaderParameterLayout::Create(SoulEngine::Shader::Reflection{
        .Bindings =
            {
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_frame.cb",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = SoulEngine::Shader::ResourceType::ConstantBuffer,
                },
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_samplers.uLinear",
                    .Set           = 1,
                    .BindingIndex  = 0,
                    .Type          = SoulEngine::Shader::ResourceType::Sampler,
                },
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_textures.uTextures",
                    .Set           = 1,
                    .BindingIndex  = 1,
                    .Type          = SoulEngine::Shader::ResourceType::SampledTexture,
                    .ArrayCount    = std::numeric_limits<Uint32>::max(),
                },
            },
    });

    auto Parameters = ShaderParameters::Create(Layout);

    EXPECT_NE(Layout.GetId(), 0);
    EXPECT_EQ(Parameters.GetLayoutId(), Layout.GetId());
    ASSERT_EQ(Parameters.GetSets().size(), 2);
    EXPECT_EQ(Parameters.GetSets()[0].GetLayout().GetSetIndex(), 0);
    EXPECT_EQ(Parameters.GetSets()[1].GetLayout().GetSetIndex(), 1);

    ResourceArray<SampledTexture> Textures;
    EXPECT_FALSE(Parameters.SetSampledTexture("g_textures.uTextures", nullptr));
    EXPECT_TRUE(Parameters.SetResourceArray("g_textures.uTextures", Textures));
    const auto TextureSetRevision = Parameters.GetSets()[1].GetRevision();
    EXPECT_TRUE(Parameters.SetResourceArray("g_textures.uTextures", Textures));
    EXPECT_EQ(Parameters.GetSets()[1].GetRevision(), TextureSetRevision);
    EXPECT_FALSE(Parameters.SetSampler("g_textures.uTextures", nullptr));

    Sampler LinearSampler{SamplerDesc{}};
    EXPECT_TRUE(Parameters.SetSampler("g_samplers.uLinear", &LinearSampler));
    const auto SamplerSetRevision = Parameters.GetSets()[1].GetRevision();
    EXPECT_TRUE(Parameters.SetSampler("g_samplers.uLinear", &LinearSampler));
    EXPECT_EQ(Parameters.GetSets()[1].GetRevision(), SamplerSetRevision);
}

TEST(ShaderParametersTest, PerDrawConstantUsesTransientAllocation) {
    auto Layout = ShaderParameterLayout::Create(SoulEngine::Shader::Reflection{
        .Bindings =
            {
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_object.data",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = SoulEngine::Shader::ResourceType::ConstantBuffer,
                },
            },
    });
    auto Parameters = ShaderParameters::Create(Layout);
    ConstantBuffer Buffer{ConstantBufferDesc{.Size = 64}};
    std::array<std::byte, 64> Data = {};

    ASSERT_TRUE(Parameters.SetConstantBuffer("g_object.data", &Buffer, Data.data(), Data.size(), true));
    const auto* Constant = std::get_if<ShaderParameterConstant>(&Parameters.GetSets()[0].GetValues()[0]);

    ASSERT_NE(Constant, nullptr);
    EXPECT_EQ(Constant->Buffer, &Buffer);
    EXPECT_TRUE(Constant->bPerDraw);
}

TEST_F(UsageVisitorTest, DrawIndexedCmdUpdatesReferencedResources) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_SecondVB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);
    std::visit(m_Visitor,
               Command{DrawIndexedCmd{.PipelinePtr     = m_Pipeline.get(),
                                       .VertexBuffers   = {m_VB.get(), m_SecondVB.get()},
                                       .IndexBufferPtr  = m_IB.get()}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_SecondVB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, DrawCmdUpdatesReferencedResources) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    std::visit(m_Visitor,
               Command{DrawCmd{.PipelinePtr = m_Pipeline.get(), .VertexBuffers = {m_VB.get()}}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
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
    Pass.DrawIndexed(m_Pipeline.get(), m_VB.get(), m_IB.get());

    // Token starts at 0 for all resources
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);

    // Visit all commands with a single UsageVisitor
    for (const auto& Cmd : Pass.Commands)
        std::visit(UsageVisitor{GpuCompletionToken{.Id = 99}}, Cmd);

    // Resource commands updated
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 99);
}
