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

/// Concrete RenderTarget for testing storage-image shader parameters.
class MockRenderTarget final : public RenderTarget {
  public:
    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return 256;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return 256;
    }
    [[nodiscard]] auto GetFormat() const -> SoulEngine::RHI::Format override {
        return SoulEngine::RHI::Format::R16G16B16A16_SFLOAT;
    }
    [[nodiscard]] auto GetUsage() const -> TextureUsage override {
        return TextureUsage::RenderTarget | TextureUsage::ShaderStorage;
    }
};

/// Concrete RT pipeline exposing the protected reflection-layout setup for testing.
class MockRayTracingPipeline final : public RayTracingPipeline {
  public:
    auto ConfigureShaderParameterLayout(ShaderParameterLayout Layout) -> void {
        SetShaderParameterLayout(std::move(Layout));
    }
};

// ── Fixtures ─────────────────────────────────────────────────────────────

class UsageVisitorTest : public ::testing::Test {
  protected:
    SPtr<GraphicsPipeline>                      m_Pipeline   = std::make_shared<GraphicsPipeline>();
    SPtr<MockRayTracingPipeline>                 m_RayPipeline = std::make_shared<MockRayTracingPipeline>();
    SPtr<BottomLevelAccelerationStructure>       m_BLAS       = std::make_shared<BottomLevelAccelerationStructure>();
    SPtr<TopLevelAccelerationStructure>          m_TLAS       = std::make_shared<TopLevelAccelerationStructure>();
    SPtr<VertexBuffer>                           m_VB         = std::make_shared<VertexBuffer>();
    SPtr<VertexBuffer>                           m_SecondVB   = std::make_shared<VertexBuffer>();
    SPtr<IndexBuffer>                            m_IB         = std::make_shared<IndexBuffer>();
    SPtr<MockSampledTexture>                     m_Texture    = std::make_shared<MockSampledTexture>();
    SPtr<MockRenderTarget>                       m_Output     = std::make_shared<MockRenderTarget>();

    GpuCompletionToken m_Token{42};
    UsageVisitor       m_Visitor{m_Token};
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST_F(UsageVisitorTest, SetGraphicsPipelineCmdUpdatesToken) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor, Command{SetGraphicsPipelineCmd{.PipelinePtr = m_Pipeline.get()}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, SetRayTracingPipelineCmdUpdatesToken) {
    ASSERT_EQ(m_RayPipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor, Command{SetRayTracingPipelineCmd{.PipelinePtr = m_RayPipeline.get()}});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
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

TEST_F(UsageVisitorTest, PushConstantsCmdSupportsRayTracingPipeline) {
    ASSERT_EQ(m_RayPipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{PushConstantsCmd{
                   .PipelinePtr = m_RayPipeline.get(),
                   .Data        = {std::byte{0}},
               }});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
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

TEST_F(UsageVisitorTest, BuildTopLevelAccelerationStructureCmdUpdatesTargetAndBlasTokens) {
    ASSERT_EQ(m_TLAS->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_BLAS->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{BuildOrUpdateTopLevelAccelerationStructureCmd{
                   .TargetPtr = m_TLAS.get(),
                   .Instances = {{.BottomLevelPtr = m_BLAS.get()}},
               }});

    EXPECT_EQ(m_TLAS->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_BLAS->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, TraceRaysCmdUpdatesRayTracingPipelineToken) {
    ASSERT_EQ(m_RayPipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{TraceRaysCmd{
                   .PipelinePtr = m_RayPipeline.get(),
                   .Width       = 640,
                   .Height      = 480,
               }});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
}

TEST(RayTracingCommandTest, PassCopiesTopLevelAccelerationStructureInstances) {
    BottomLevelAccelerationStructure BLAS;
    TopLevelAccelerationStructure    TLAS;
    std::vector<AccelerationStructureInstance> Instances{{.BottomLevelPtr = &BLAS}};
    Pass PassValue{};

    PassValue.BuildOrUpdateTopLevelAccelerationStructure(&TLAS, Instances);
    Instances.clear();

    ASSERT_EQ(PassValue.Commands.size(), 1);
    const auto* CommandPtr = std::get_if<BuildOrUpdateTopLevelAccelerationStructureCmd>(&PassValue.Commands.front());
    ASSERT_NE(CommandPtr, nullptr);
    ASSERT_EQ(CommandPtr->Instances.size(), 1);
    EXPECT_EQ(CommandPtr->TargetPtr, &TLAS);
    EXPECT_EQ(CommandPtr->Instances.front().BottomLevelPtr, &BLAS);
}

TEST_F(UsageVisitorTest, RayTracingShaderParametersUpdateTlasAndStorageOutputTokens) {
    const auto Layout = ShaderParameterLayout::Create(SoulEngine::Shader::Reflection{
        .Bindings =
            {
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_scene.tlas",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = SoulEngine::Shader::ResourceType::AccelerationStructure,
                },
                SoulEngine::Shader::Binding{
                    .ParameterPath = "g_frame.output",
                    .Set           = 0,
                    .BindingIndex  = 1,
                    .Type          = SoulEngine::Shader::ResourceType::StorageTexture,
                },
            },
    });
    m_RayPipeline->ConfigureShaderParameterLayout(Layout);
    auto Parameters = ShaderParameters::Create(*m_RayPipeline);

    ASSERT_TRUE(Parameters.SetTopLevelAccelerationStructure("g_scene.tlas", m_TLAS.get()));
    ASSERT_TRUE(Parameters.SetStorageRenderTarget("g_frame.output", m_Output.get()));
    ASSERT_EQ(m_TLAS->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Output->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               Command{BindShaderParametersCmd{
                   .PipelinePtr = m_RayPipeline.get(),
                   .Parameters  = std::move(Parameters),
               }});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_TLAS->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_Output->GetLastUsageToken().Id, 42);
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
    auto& Scope     = CmdList.Scopes.emplace_back(Pass{});
    auto& PassValue = std::get<SoulEngine::RHI::Pass>(Scope);

    PassValue.SetViewport(0, 0, 800, 600);
    PassValue.SetGraphicsPipeline(m_Pipeline.get());
    PassValue.DrawIndexed(m_Pipeline.get(), m_VB.get(), m_IB.get());

    // Token starts at 0 for all resources
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);

    // Visit all commands with a single UsageVisitor
    for (const auto& Cmd : PassValue.Commands)
        std::visit(UsageVisitor{GpuCompletionToken{.Id = 99}}, Cmd);

    // Resource commands updated
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 99);
}


TEST_F(UsageVisitorTest, NonRenderingScopeRecordsRayTracingCommands) {
    NonRenderingPass Scope;
    Scope.SetRayTracingPipeline(m_RayPipeline.get());
    Scope.TraceRays(m_RayPipeline.get(), 1280, 720);

    ASSERT_EQ(Scope.Commands.size(), 2);
    EXPECT_TRUE(std::holds_alternative<SetRayTracingPipelineCmd>(Scope.Commands[0]));
    EXPECT_TRUE(std::holds_alternative<TraceRaysCmd>(Scope.Commands[1]));

    for (const auto& Cmd : Scope.Commands)
        std::visit(m_Visitor, Cmd);
    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, CommandListPreservesRenderingAndNonRenderingScopeOrder) {
    CommandList CmdList;
    auto& RayScope = CmdList.Scopes.emplace_back(NonRenderingPass{});
    std::get<NonRenderingPass>(RayScope).SetRayTracingPipeline(m_RayPipeline.get());
    auto& RenderingScope = CmdList.Scopes.emplace_back(Pass{});
    std::get<Pass>(RenderingScope).SetGraphicsPipeline(m_Pipeline.get());

    ASSERT_EQ(CmdList.Scopes.size(), 2);
    EXPECT_TRUE(std::holds_alternative<NonRenderingPass>(CmdList.Scopes[0]));
    EXPECT_TRUE(std::holds_alternative<Pass>(CmdList.Scopes[1]));
}
