/// @file   CommandUsage.cpp
/// @brief  Tests for RHIUsageVisitor — verifies that RHIGpuResource usage tokens
///         are correctly updated when visiting each command variant.

#include <gtest/gtest.h>

import RHI;
import Shader;
import std;

using namespace SoulEngine;

// ── Mocks ────────────────────────────────────────────────────────────────

/// Concrete SampledTexture for testing (RHISampledTexture has pure virtuals).
class MockSampledTexture final : public RHISampledTexture {
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
class MockRenderTarget final : public RHIRenderTarget {
  public:
    [[nodiscard]] auto GetWidth() const -> Uint32 override {
        return 256;
    }
    [[nodiscard]] auto GetHeight() const -> Uint32 override {
        return 256;
    }
    [[nodiscard]] auto GetFormat() const -> RHIFormat override {
        return RHIFormat::R16G16B16A16_SFLOAT;
    }
    [[nodiscard]] auto GetUsage() const -> RHITextureUsage override {
        return RHITextureUsage::RenderTarget | RHITextureUsage::ShaderStorage;
    }
};

/// Minimal RenderDevice implementation used to exercise logical transient-handle allocation.
class MockRenderDevice final : public RHIRenderDevice {
  public:
    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override {
        return RHIBackendType::Unknown;
    }
    [[nodiscard]] auto CreateVertexBuffer(const RHIVertexBufferDesc&)
        -> std::expected<RHIVertexBufferCreateResult, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create vertex buffers"));
    }
    [[nodiscard]] auto CreateIndexBuffer(const RHIIndexBufferDesc&)
        -> std::expected<RHIIndexBufferCreateResult, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create index buffers"));
    }
    [[nodiscard]] auto CreateSampler(const RHISamplerDesc&) -> std::expected<UPtr<RHISampler>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create samplers"));
    }
    [[nodiscard]] auto CreateSampledTexture(const RHISampledTextureDesc&)
        -> std::expected<RHISampledTextureCreateResult, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create sampled textures"));
    }
    [[nodiscard]] auto CreateRenderTarget(const RHIRenderTargetDesc&)
        -> std::expected<RHIRenderTargetCreateResult, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create render targets"));
    }
    [[nodiscard]] auto CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&)
        -> std::expected<UPtr<RHIGraphicsPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create graphics pipelines"));
    }
    [[nodiscard]] auto CreateRayTracingPipeline(const RHIRayTracingPipelineDesc&)
        -> std::expected<UPtr<RHIRayTracingPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create ray-tracing pipelines"));
    }
    [[nodiscard]] auto CreateBottomLevelAccelerationStructure(const RHIBottomLevelAccelerationStructureDesc&)
        -> std::expected<UPtr<RHIBottomLevelAccelerationStructure>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create BLAS resources"));
    }
    [[nodiscard]] auto CreateTopLevelAccelerationStructure(const RHITopLevelAccelerationStructureDesc&)
        -> std::expected<UPtr<RHITopLevelAccelerationStructure>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("MockRenderDevice does not create TLAS resources"));
    }
    [[nodiscard]] auto GetRayTracingGeometryTable() -> RHIRayTracingGeometryTable* override {
        return nullptr;
    }
    [[nodiscard]] auto Execute(const RHICommandList&) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override {
        return 0;
    }
    [[nodiscard]] auto IsGpuComplete(RHIGpuCompletionToken) -> bool override {
        return true;
    }
    auto WaitIdle() -> void override {}
    auto Shutdown() -> void override {}
};

/// Concrete RT pipeline exposing the protected reflection-layout setup for testing.
class MockRayTracingGeometryTable final : public RHIRayTracingGeometryTable {};

class MockRayTracingPipeline final : public RHIRayTracingPipeline {
  public:
    auto ConfigureShaderParameterLayout(RHIShaderParameterLayout Layout) -> void {
        SetShaderParameterLayout(std::move(Layout));
    }
};

// ── Fixtures ─────────────────────────────────────────────────────────────

class UsageVisitorTest : public ::testing::Test {
  protected:
    SPtr<RHIGraphicsPipeline>                      m_Pipeline   = std::make_shared<RHIGraphicsPipeline>();
    SPtr<MockRayTracingPipeline>                 m_RayPipeline = std::make_shared<MockRayTracingPipeline>();
    SPtr<MockRayTracingGeometryTable>             m_GeometryTable = std::make_shared<MockRayTracingGeometryTable>();
    SPtr<RHIBottomLevelAccelerationStructure>       m_BLAS       = std::make_shared<RHIBottomLevelAccelerationStructure>();
    SPtr<RHITopLevelAccelerationStructure>          m_TLAS       = std::make_shared<RHITopLevelAccelerationStructure>();
    SPtr<RHIVertexBuffer>                           m_VB         = std::make_shared<RHIVertexBuffer>();
    SPtr<RHIVertexBuffer>                           m_SecondVB   = std::make_shared<RHIVertexBuffer>();
    SPtr<RHIIndexBuffer>                            m_IB         = std::make_shared<RHIIndexBuffer>();
    SPtr<MockSampledTexture>                     m_Texture    = std::make_shared<MockSampledTexture>();
    SPtr<MockRenderTarget>                       m_Output     = std::make_shared<MockRenderTarget>();

    RHIGpuCompletionToken m_Token{42};
    RHIUsageVisitor       m_Visitor{m_Token};
};

// ── Tests ────────────────────────────────────────────────────────────────

TEST_F(UsageVisitorTest, SetGraphicsPipelineCmdUpdatesToken) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor, RHICommand{RHISetGraphicsPipelineCmd{.PipelinePtr = m_Pipeline.get()}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, SetRayTracingPipelineCmdUpdatesToken) {
    ASSERT_EQ(m_RayPipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor, RHICommand{RHISetRayTracingPipelineCmd{.PipelinePtr = m_RayPipeline.get()}});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, PushConstantsCmdUpdatesPipelineToken) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHIPushConstantsCmd{
                   .PipelinePtr = m_Pipeline.get(),
                   .Data        = {std::byte{0}},
               }});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, PushConstantsCmdSupportsRayTracingPipeline) {
    ASSERT_EQ(m_RayPipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHIPushConstantsCmd{
                   .PipelinePtr = m_RayPipeline.get(),
                   .Data        = {std::byte{0}},
               }});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, BindShaderParametersCmdUpdatesReferencedResources) {
    RHIResourceArray<RHISampledTexture> Textures;
    Textures.Set(3, m_Texture.get());
    EXPECT_EQ(Textures.GetSize(), 4);
    auto Parameters = RHIShaderParameters::Create(RHIShaderParameterLayout::Create(ShaderReflection{
        .Bindings =
            {
                ShaderBinding{
                    .ParameterPath = "g_textures.uTextures",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = ShaderResourceType::SampledTexture,
                    .ArrayCount    = std::numeric_limits<Uint32>::max(),
                },
            },
    }));
    ASSERT_TRUE(Parameters.SetResourceArray("g_textures.uTextures", Textures));

    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Texture->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHIBindShaderParametersCmd{
                   .PipelinePtr = m_Pipeline.get(),
                   .Parameters  = std::move(Parameters),
               }});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 42);
}

TEST(RayTracingCommandTest, NonRenderingPassCopiesBdaGeometryTableUpdate) {
    RHIRayTracingGeometryTable Table;
    RHIVertexBuffer Position;
    RHIVertexBuffer Normal;
    RHIIndexBuffer Index;
    RHIVertexBuffer SecondPosition;
    RHIVertexBuffer SecondNormal;
    RHIIndexBuffer SecondIndex;
    RHIRayTracingGeometryTableUpdate Update{
        .Instances = {{.FirstGeometry = 0, .GeometryCount = 1, .MaterialIndex = 9},
                      {.FirstGeometry = 1, .GeometryCount = 1, .MaterialIndex = 17}},
        .Geometries = {{.PositionBuffer = &Position,
                        .NormalBuffer = &Normal,
                        .IndexBuffer = &Index,
                        .VertexCount = 3,
                        .IndexCount = 3},
                       {.PositionBuffer = &SecondPosition,
                        .NormalBuffer = &SecondNormal,
                        .IndexBuffer = &SecondIndex,
                        .VertexCount = 4,
                        .IndexCount = 6}},
    };
    RHINonRenderingPass RHIPass;

    RHIPass.UpdateRayTracingGeometryTable(&Table, Update);
    Update.Instances.clear();
    Update.Geometries.clear();

    ASSERT_EQ(RHIPass.Commands.size(), 1);
    const auto* CommandPtr = std::get_if<RHIUpdateRayTracingGeometryTableCmd>(&RHIPass.Commands.front());
    ASSERT_NE(CommandPtr, nullptr);
    ASSERT_EQ(CommandPtr->Update.Instances.size(), 2);
    ASSERT_EQ(CommandPtr->Update.Geometries.size(), 2);
    EXPECT_EQ(CommandPtr->TablePtr, &Table);
    EXPECT_EQ(CommandPtr->Update.Instances[0].MaterialIndex, 9U);
    EXPECT_EQ(CommandPtr->Update.Instances[1].FirstGeometry, 1U);
    EXPECT_EQ(CommandPtr->Update.Instances[1].MaterialIndex, 17U);
    EXPECT_EQ(CommandPtr->Update.Geometries[0].PositionBuffer, &Position);
    EXPECT_EQ(CommandPtr->Update.Geometries[0].NormalBuffer, &Normal);
    EXPECT_EQ(CommandPtr->Update.Geometries[0].IndexBuffer, &Index);
    EXPECT_EQ(CommandPtr->Update.Geometries[1].PositionBuffer, &SecondPosition);
    EXPECT_EQ(CommandPtr->Update.Geometries[1].NormalBuffer, &SecondNormal);
    EXPECT_EQ(CommandPtr->Update.Geometries[1].IndexBuffer, &SecondIndex);
    EXPECT_EQ(CommandPtr->Update.Geometries[1].IndexCount, 6U);
}

TEST_F(UsageVisitorTest, UpdateBdaGeometryTableCmdUpdatesSourceBufferTokens) {
    ASSERT_EQ(m_GeometryTable->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_SecondVB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHIUpdateRayTracingGeometryTableCmd{
                   .TablePtr = m_GeometryTable.get(),
                   .Update = RHIRayTracingGeometryTableUpdate{
                       .Instances = {{.FirstGeometry = 0, .GeometryCount = 1}},
                       .Geometries = {{.PositionBuffer = m_VB.get(),
                                       .NormalBuffer = m_SecondVB.get(),
                                       .IndexBuffer = m_IB.get(),
                                       .VertexCount = 3,
                                       .IndexCount = 3}},
                   },
               }});

    // Vulkan stamps the table only after a graphics submit succeeds. The
    // generic visitor still protects all source buffers immediately.
    EXPECT_EQ(m_GeometryTable->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_SecondVB->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, BuildTopLevelAccelerationStructureCmdUpdatesTargetAndBlasTokens) {
    ASSERT_EQ(m_TLAS->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_BLAS->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHIBuildOrUpdateTopLevelAccelerationStructureCmd{
                   .TargetPtr = m_TLAS.get(),
                   .Instances = {{.BottomLevelPtr = m_BLAS.get()}},
               }});

    EXPECT_EQ(m_TLAS->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_BLAS->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, TraceRaysCmdUpdatesRayTracingPipelineToken) {
    ASSERT_EQ(m_RayPipeline->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHITraceRaysCmd{
                   .PipelinePtr = m_RayPipeline.get(),
                   .Width       = 640,
                   .Height      = 480,
               }});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
}

TEST(RayTracingCommandTest, PassCopiesTopLevelAccelerationStructureInstances) {
    RHIBottomLevelAccelerationStructure BLAS;
    RHITopLevelAccelerationStructure    TLAS;
    std::vector<RHIAccelerationStructureInstance> Instances{{.BottomLevelPtr = &BLAS}};
    RHIPass PassValue{};

    PassValue.BuildOrUpdateTopLevelAccelerationStructure(&TLAS, Instances);
    Instances.clear();

    ASSERT_EQ(PassValue.Commands.size(), 1);
    const auto* CommandPtr = std::get_if<RHIBuildOrUpdateTopLevelAccelerationStructureCmd>(&PassValue.Commands.front());
    ASSERT_NE(CommandPtr, nullptr);
    ASSERT_EQ(CommandPtr->Instances.size(), 1);
    EXPECT_EQ(CommandPtr->TargetPtr, &TLAS);
    EXPECT_EQ(CommandPtr->Instances.front().BottomLevelPtr, &BLAS);
}

TEST_F(UsageVisitorTest, RayTracingGeometryTableShaderParameterDefersTokenToVulkanSubmit) {
    const auto Layout = RHIShaderParameterLayout::Create(ShaderReflection{
        .Bindings = {{.ParameterPath = "g_rt.metadata", .Set = 0, .BindingIndex = 0,
                      .Type = ShaderResourceType::StorageBuffer}},
    });
    auto Parameters = RHIShaderParameters::Create(Layout);
    ASSERT_TRUE(Parameters.SetRayTracingGeometryTable("g_rt.metadata", m_GeometryTable.get()));

    std::visit(m_Visitor,
               RHICommand{RHIBindShaderParametersCmd{.PipelinePtr = m_RayPipeline.get(), .Parameters = std::move(Parameters)}});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_GeometryTable->GetLastUsageToken().Id, 0);
}

TEST_F(UsageVisitorTest, RayTracingShaderParametersUpdateTlasAndStorageOutputTokens) {
    const auto Layout = RHIShaderParameterLayout::Create(ShaderReflection{
        .Bindings =
            {
                ShaderBinding{
                    .ParameterPath = "g_scene.tlas",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = ShaderResourceType::AccelerationStructure,
                },
                ShaderBinding{
                    .ParameterPath = "g_frame.output",
                    .Set           = 0,
                    .BindingIndex  = 1,
                    .Type          = ShaderResourceType::StorageTexture,
                },
            },
    });
    m_RayPipeline->ConfigureShaderParameterLayout(Layout);
    auto Parameters = RHIShaderParameters::Create(*m_RayPipeline);

    ASSERT_TRUE(Parameters.SetTopLevelAccelerationStructure("g_scene.tlas", m_TLAS.get()));
    ASSERT_TRUE(Parameters.SetStorageRenderTarget("g_frame.output", m_Output.get()));
    ASSERT_EQ(m_TLAS->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_Output->GetLastUsageToken().Id, 0);

    std::visit(m_Visitor,
               RHICommand{RHIBindShaderParametersCmd{
                   .PipelinePtr = m_RayPipeline.get(),
                   .Parameters  = std::move(Parameters),
               }});

    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_TLAS->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_Output->GetLastUsageToken().Id, 42);
}

TEST(ResourceArrayTest, SupportsMultipleResourceTypes) {
    RHISampler SamplerValue{RHISamplerDesc{}};
    RHIResourceArray<RHISampler> Samplers;

    Samplers.Set(2, &SamplerValue);

    ASSERT_EQ(Samplers.GetSize(), 3);
    EXPECT_EQ(Samplers.GetResources()[2], &SamplerValue);
}

TEST(ShaderParametersTest, ReflectionAutomaticallyPartitionsParameterSets) {
    auto Layout = RHIShaderParameterLayout::Create(ShaderReflection{
        .Bindings =
            {
                ShaderBinding{
                    .ParameterPath = "g_frame.cb",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = ShaderResourceType::ConstantBuffer,
                },
                ShaderBinding{
                    .ParameterPath = "g_samplers.uLinear",
                    .Set           = 1,
                    .BindingIndex  = 0,
                    .Type          = ShaderResourceType::Sampler,
                },
                ShaderBinding{
                    .ParameterPath = "g_textures.uTextures",
                    .Set           = 1,
                    .BindingIndex  = 1,
                    .Type          = ShaderResourceType::SampledTexture,
                    .ArrayCount    = std::numeric_limits<Uint32>::max(),
                },
            },
    });

    auto Parameters = RHIShaderParameters::Create(Layout);

    EXPECT_NE(Layout.GetId(), 0);
    EXPECT_EQ(Parameters.GetLayoutId(), Layout.GetId());
    ASSERT_EQ(Parameters.GetSets().size(), 2);
    EXPECT_EQ(Parameters.GetSets()[0].GetLayout().GetSetIndex(), 0);
    EXPECT_EQ(Parameters.GetSets()[1].GetLayout().GetSetIndex(), 1);

    RHIResourceArray<RHISampledTexture> Textures;
    EXPECT_FALSE(Parameters.SetSampledTexture("g_textures.uTextures", nullptr));
    EXPECT_TRUE(Parameters.SetResourceArray("g_textures.uTextures", Textures));
    const auto TextureSetRevision = Parameters.GetSets()[1].GetRevision();
    EXPECT_TRUE(Parameters.SetResourceArray("g_textures.uTextures", Textures));
    EXPECT_EQ(Parameters.GetSets()[1].GetRevision(), TextureSetRevision);
    EXPECT_FALSE(Parameters.SetSampler("g_textures.uTextures", nullptr));

    RHISampler LinearSampler{RHISamplerDesc{}};
    EXPECT_TRUE(Parameters.SetSampler("g_samplers.uLinear", &LinearSampler));
    const auto SamplerSetRevision = Parameters.GetSets()[1].GetRevision();
    EXPECT_TRUE(Parameters.SetSampler("g_samplers.uLinear", &LinearSampler));
    EXPECT_EQ(Parameters.GetSets()[1].GetRevision(), SamplerSetRevision);
}

TEST(ShaderParametersTest, TransientConstantBufferUsesRenderDeviceHandle) {
    auto Layout = RHIShaderParameterLayout::Create(ShaderReflection{
        .Bindings =
            {
                ShaderBinding{
                    .ParameterPath = "g_object.data",
                    .Set           = 0,
                    .BindingIndex  = 0,
                    .Type          = ShaderResourceType::ConstantBuffer,
                },
            },
    });
    auto Parameters = RHIShaderParameters::Create(Layout);
    MockRenderDevice Device;
    auto Buffer = Device.AllocateTransientConstantBuffer(64);
    ASSERT_TRUE(Buffer);
    std::array<std::byte, 64> Data = {};

    ASSERT_TRUE(Parameters.SetTransientConstantBuffer("g_object.data", *Buffer));
    const auto* Constant = std::get_if<RHITransientConstantBuffer>(&Parameters.GetSets()[0].GetValues()[0]);

    ASSERT_NE(Constant, nullptr);
    EXPECT_EQ(*Constant, *Buffer);
    EXPECT_EQ(Constant->GetSize(), Data.size());

    RHIPass Pass = {};
    ASSERT_TRUE(Pass.WriteTransientConstantBuffer(*Buffer, Data));
    ASSERT_EQ(Pass.Commands.size(), 1);
    const auto* Write = std::get_if<RHIWriteTransientConstantBufferCmd>(&Pass.Commands.front());
    ASSERT_NE(Write, nullptr);
    EXPECT_EQ(Write->Buffer, *Buffer);
    EXPECT_EQ(Write->Data.size(), Data.size());

    EXPECT_FALSE(Device.AllocateTransientConstantBuffer(0));
    EXPECT_FALSE(Parameters.SetTransientConstantBuffer("g_object.data", {}));
}

TEST_F(UsageVisitorTest, DrawIndexedCmdUpdatesReferencedResources) {
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_SecondVB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);
    std::visit(m_Visitor,
               RHICommand{RHIDrawIndexedCmd{.PipelinePtr     = m_Pipeline.get(),
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
               RHICommand{RHIDrawCmd{.PipelinePtr = m_Pipeline.get(), .VertexBuffers = {m_VB.get()}}});

    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 42);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, NullPipelineDoesNotCrash) {
    std::visit(m_Visitor, RHICommand{RHIDrawCmd{.PipelinePtr = nullptr}});
    // Should not crash — no assertion needed beyond survival.
}

TEST_F(UsageVisitorTest, NullDrawResourcesDoNotCrash) {
    std::visit(m_Visitor, RHICommand{RHIDrawIndexedCmd{}});
    std::visit(m_Visitor, RHICommand{RHIDrawCmd{}});
    // Should not crash — no assertion needed beyond survival.
}

TEST_F(UsageVisitorTest, NonResourceCommandsDoNotUpdateAnyToken) {
    // These command types don't reference GPU resources.
    std::visit(m_Visitor, RHICommand{RHISetViewportCmd{}});
    std::visit(m_Visitor, RHICommand{RHISetFullViewportCmd{}});
    std::visit(m_Visitor, RHICommand{RHISetScissorCmd{}});
    std::visit(m_Visitor, RHICommand{RHISetFullScissorRectCmd{}});

    // Tokens on all resources should remain default (0).
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 0);
    EXPECT_EQ(m_Texture->GetLastUsageToken().Id, 0);
}

TEST_F(UsageVisitorTest, VisitEntireCommandList) {
    RHICommandList CmdList;
    auto& Scope     = CmdList.Scopes.emplace_back(RHIPass{});
    auto& PassValue = std::get<RHIPass>(Scope);

    PassValue.SetViewport(0, 0, 800, 600);
    PassValue.SetGraphicsPipeline(m_Pipeline.get());
    PassValue.DrawIndexed(m_Pipeline.get(), m_VB.get(), m_IB.get());

    // Token starts at 0 for all resources
    ASSERT_EQ(m_Pipeline->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_VB->GetLastUsageToken().Id, 0);
    ASSERT_EQ(m_IB->GetLastUsageToken().Id, 0);

    // Visit all commands with a single RHIUsageVisitor
    for (const auto& Cmd : PassValue.Commands)
        std::visit(RHIUsageVisitor{RHIGpuCompletionToken{.Id = 99}}, Cmd);

    // Resource commands updated
    EXPECT_EQ(m_Pipeline->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_VB->GetLastUsageToken().Id, 99);
    EXPECT_EQ(m_IB->GetLastUsageToken().Id, 99);
}


TEST_F(UsageVisitorTest, NonRenderingScopeRecordsRayTracingCommands) {
    RHINonRenderingPass Scope;
    Scope.SetRayTracingPipeline(m_RayPipeline.get());
    Scope.TraceRays(m_RayPipeline.get(), 1280, 720);

    ASSERT_EQ(Scope.Commands.size(), 2);
    EXPECT_TRUE(std::holds_alternative<RHISetRayTracingPipelineCmd>(Scope.Commands[0]));
    EXPECT_TRUE(std::holds_alternative<RHITraceRaysCmd>(Scope.Commands[1]));

    for (const auto& Cmd : Scope.Commands)
        std::visit(m_Visitor, Cmd);
    EXPECT_EQ(m_RayPipeline->GetLastUsageToken().Id, 42);
}

TEST_F(UsageVisitorTest, CommandListPreservesRenderingAndNonRenderingScopeOrder) {
    RHICommandList CmdList;
    auto& RayScope = CmdList.Scopes.emplace_back(RHINonRenderingPass{});
    std::get<RHINonRenderingPass>(RayScope).SetRayTracingPipeline(m_RayPipeline.get());
    auto& RenderingScope = CmdList.Scopes.emplace_back(RHIPass{});
    std::get<RHIPass>(RenderingScope).SetGraphicsPipeline(m_Pipeline.get());

    ASSERT_EQ(CmdList.Scopes.size(), 2);
    EXPECT_TRUE(std::holds_alternative<RHINonRenderingPass>(CmdList.Scopes[0]));
    EXPECT_TRUE(std::holds_alternative<RHIPass>(CmdList.Scopes[1]));
}
