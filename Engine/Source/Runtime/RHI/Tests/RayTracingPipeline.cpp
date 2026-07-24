/// @file   RayTracingPipeline.cpp
/// @brief  Unit tests for Vulkan ray-tracing shader-binding-table layout arithmetic.

#include <gtest/gtest.h>

#include <GLFW/glfw3.h>

import RHI;
import Shader;
import ShaderCompiler;
import Vulkan;
import std;

using namespace SoulEngine;

TEST(ShaderBindingTableLayoutTest, AlignsRecordsAndRegions) {
    auto Layout = VulkanCreateShaderBindingTableLayout(
        24, 32, 64, 4096, 1, 2, 1, 0);

    ASSERT_TRUE(Layout.has_value()) << Layout.error().ToString();
    EXPECT_EQ(Layout->HandleSize, 24);
    EXPECT_EQ(Layout->RecordStride, 32);
    EXPECT_EQ(Layout->RayGeneration.Offset, 0);
    EXPECT_EQ(Layout->RayGeneration.Size, 32);
    EXPECT_EQ(Layout->Miss.Offset, 64);
    EXPECT_EQ(Layout->Miss.Size, 64);
    EXPECT_EQ(Layout->Hit.Offset, 128);
    EXPECT_EQ(Layout->Hit.Size, 32);
    EXPECT_EQ(Layout->Callable.RecordCount, 0);
    EXPECT_EQ(Layout->Callable.Size, 0);
    EXPECT_EQ(Layout->TotalSize, 160);
}

TEST(ShaderBindingTableLayoutTest, SupportsEmptyCallableRegion) {
    auto Layout = VulkanCreateShaderBindingTableLayout(
        32, 32, 64, 4096, 1, 1, 1, 0);

    ASSERT_TRUE(Layout.has_value()) << Layout.error().ToString();
    EXPECT_EQ(Layout->Callable.Offset, 0);
    EXPECT_EQ(Layout->Callable.Stride, 0);
    EXPECT_EQ(Layout->Callable.Size, 0);
    EXPECT_EQ(Layout->Callable.RecordCount, 0);
}

TEST(ShaderBindingTableLayoutTest, RejectsInvalidRayGenerationRecordCount) {
    auto Layout = VulkanCreateShaderBindingTableLayout(
        32, 32, 64, 4096, 0, 1, 1, 0);

    ASSERT_FALSE(Layout.has_value());
    EXPECT_NE(Layout.error().ToString().find("exactly one ray-generation"), String::npos);
}

TEST(ShaderBindingTableLayoutTest, RejectsRecordStrideBeyondDeviceLimit) {
    auto Layout = VulkanCreateShaderBindingTableLayout(
        64, 128, 64, 64, 1, 1, 1, 0);

    ASSERT_FALSE(Layout.has_value());
    EXPECT_NE(Layout.error().ToString().find("maxShaderGroupStride"), String::npos);
}

TEST(RayTracingPipelineHardwareTest, DISABLED_CreatesBdaGeometryPipelineFromSlangFixture) {
    const auto* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
    ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

    const Path EngineDir = Path(TestSourceDir).parent_path().parent_path().parent_path().parent_path();
    ConfigManager::Get().Init(EngineDir);
    ASSERT_TRUE(ConfigManager::Get().LoadConfig().has_value());

    ASSERT_TRUE(glfwInit()) << "glfwInit failed";
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* Window = glfwCreateWindow(1, 1, "SoulEngine RT pipeline test", nullptr, nullptr);
    ASSERT_NE(Window, nullptr) << "glfwCreateWindow failed";

    auto DeviceResult = RHIRenderDevice::Create(Window);
    if (!DeviceResult) {
        glfwDestroyWindow(Window);
        glfwTerminate();
        GTEST_SKIP() << DeviceResult.error().ToString();
    }

    const auto Cleanup = [Window]() -> void {
        RHIRenderDevice::Destroy();
        glfwDestroyWindow(Window);
        glfwTerminate();
    };

    const std::array<Float32, 9> TriangleVertices{
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    };
    const std::array<Uint32, 3> TriangleIndices{0, 1, 2};
    auto VertexBuffer = RHIRenderDevice::Get().CreateVertexBuffer(RHIVertexBufferDesc{
        .Data        = TriangleVertices.data(),
        .VertexCount = 3,
        .Stride      = sizeof(Float32) * 3,
    });
    auto IndexBuffer = RHIRenderDevice::Get().CreateIndexBuffer(RHIIndexBufferDesc{
        .Data       = TriangleIndices.data(),
        .IndexCount = 3,
    });
    if (!VertexBuffer || !IndexBuffer) {
        const auto Error = !VertexBuffer ? VertexBuffer.error().ToString() : IndexBuffer.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    auto Blas = RHIRenderDevice::Get().CreateBottomLevelAccelerationStructure(RHIBottomLevelAccelerationStructureDesc{
        .Geometries = {
            RHITriangleAccelerationStructureGeometryDesc{
                .VertexBufferPtr = VertexBuffer->Buffer.get(),
                .VertexCount     = 3,
                .VertexStride    = sizeof(Float32) * 3,
                .VertexFormat    = RHIFormat::R32G32B32_SFLOAT,
                .IndexBufferPtr  = IndexBuffer->Buffer.get(),
                .IndexCount      = 3,
            },
        },
    });
    if (!Blas) {
        const auto Error = Blas.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    const Path ShaderPath = EngineDir / "Source" / "Runtime" / "ShaderCompiler" / "Tests" / "Slang" / "RayTracingBda.slang";
    auto Program = ShaderCompiler::Get().CompileRayTracing(RayTracingCompileDesc{
        .RayGeneration = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "rayGenMain", .Backend = ShaderBackend::Slang},
        .MissEntries   = {ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "missMain", .Backend = ShaderBackend::Slang}},
        .HitGroups = {
            RayTracingHitGroupCompileDesc{
                .Type       = ShaderRayTracingHitGroupType::Triangles,
                .ClosestHit = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "closestHitMain", .Backend = ShaderBackend::Slang},
            },
        },
    });
    if (!Program) {
        const auto Error = Program.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    auto RHIPipeline = RHIRenderDevice::Get().CreateRayTracingPipeline(RHIRayTracingPipelineDesc{.Program = std::move(*Program)});
    if (!RHIPipeline) {
        const auto Error = RHIPipeline.error().ToString();
        Cleanup();
        GTEST_SKIP() << Error;
    }
    EXPECT_NE(RHIPipeline->get(), nullptr);

    auto Tlas = RHIRenderDevice::Get().CreateTopLevelAccelerationStructure(RHITopLevelAccelerationStructureDesc{
        .InitialInstanceCapacity = 1,
        .BuildFlags = RHIAccelerationStructureBuildFlags::AllowUpdate,
    });
    if (!Tlas) {
        const auto Error = Tlas.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    auto Output = RHIRenderDevice::Get().CreateRenderTarget(RHIRenderTargetDesc{
        .Width  = 1,
        .Height = 1,
        .Format = RHIFormat::B8G8R8A8_UNORM,
        .Usage  = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderStorage | RHITextureUsage::FrameOutput,
    });
    if (!Output) {
        const auto Error = Output.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    auto* GeometryTable = RHIRenderDevice::Get().GetRayTracingGeometryTable();
    if (!GeometryTable) {
        Cleanup();
        GTEST_SKIP() << "BDA geometry table is unavailable";
    }

    auto Parameters = RHIShaderParameters::Create(**RHIPipeline);
    if (auto R = Parameters.SetTopLevelAccelerationStructure("g_resources.tlas", Tlas->get()); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    if (auto R = Parameters.SetStorageRenderTarget("g_resources.output", Output->Texture.get()); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    if (auto R = Parameters.SetRayTracingGeometryTable("g_bdaMetadata.metadata", GeometryTable); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    const std::array<RHIAccelerationStructureInstance, 2> Instances{
        RHIAccelerationStructureInstance{
            .BottomLevelPtr = Blas->get(),
            .CustomIndex = 0,
        },
        RHIAccelerationStructureInstance{
            .BottomLevelPtr = Blas->get(),
            .Transform = RHIRowMajorTransform3x4{
                .M03 = 0.1f,
            },
            .CustomIndex = 1,
        },
    };
    const RHIRayTracingGeometryTableUpdate GeometryUpdate{
        .Instances = {{.FirstGeometry = 0, .GeometryCount = 1}, {.FirstGeometry = 1, .GeometryCount = 1}},
        .Geometries = {{.PositionBuffer = VertexBuffer->Buffer.get(),
                        .NormalBuffer = VertexBuffer->Buffer.get(),
                        .IndexBuffer = IndexBuffer->Buffer.get(),
                        .PositionStride = sizeof(Float32) * 3,
                        .NormalStride = sizeof(Float32) * 3,
                        .IndexStride = sizeof(Uint32),
                        .VertexCount = 3,
                        .IndexCount = 3},
                       {.PositionBuffer = VertexBuffer->Buffer.get(),
                        .NormalBuffer = VertexBuffer->Buffer.get(),
                        .IndexBuffer = IndexBuffer->Buffer.get(),
                        .PositionStride = sizeof(Float32) * 3,
                        .NormalStride = sizeof(Float32) * 3,
                        .IndexStride = sizeof(Uint32),
                        .VertexCount = 3,
                        .IndexCount = 3}},
    };
    RHICommandList Commands;
    auto& ScopeValue = Commands.Scopes.emplace_back(RHINonRenderingPass{});
    auto& Scope = std::get<RHINonRenderingPass>(ScopeValue);
    Scope.UpdateRayTracingGeometryTable(GeometryTable, GeometryUpdate);
    Scope.BuildOrUpdateTopLevelAccelerationStructure(Tlas->get(), Instances);
    Scope.SetRayTracingPipeline(RHIPipeline->get());
    Scope.BindShaderParameters(RHIPipeline->get(), std::move(Parameters));
    Scope.TraceRays(RHIPipeline->get(), 1, 1);
    Commands.PresentSource = Output->Texture.get();

    if (auto R = RHIRenderDevice::Get().Execute(Commands); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    auto UpdateParameters = RHIShaderParameters::Create(**RHIPipeline);
    if (auto R = UpdateParameters.SetTopLevelAccelerationStructure("g_resources.tlas", Tlas->get()); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    if (auto R = UpdateParameters.SetStorageRenderTarget("g_resources.output", Output->Texture.get()); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    if (auto R = UpdateParameters.SetRayTracingGeometryTable("g_bdaMetadata.metadata", GeometryTable); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    RHICommandList UpdateCommands;
    auto& UpdateScopeValue = UpdateCommands.Scopes.emplace_back(RHINonRenderingPass{});
    auto& UpdateScope = std::get<RHINonRenderingPass>(UpdateScopeValue);
    UpdateScope.UpdateRayTracingGeometryTable(GeometryTable, GeometryUpdate);
    UpdateScope.BuildOrUpdateTopLevelAccelerationStructure(
        Tlas->get(), Instances, RHITopLevelAccelerationStructureBuildMode::Update);
    UpdateScope.SetRayTracingPipeline(RHIPipeline->get());
    UpdateScope.BindShaderParameters(RHIPipeline->get(), std::move(UpdateParameters));
    UpdateScope.TraceRays(RHIPipeline->get(), 1, 1);
    UpdateCommands.PresentSource = Output->Texture.get();

    if (auto R = RHIRenderDevice::Get().Execute(UpdateCommands); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    Output->Texture.reset();
    (*Tlas).reset();
    (*RHIPipeline).reset();
    (*Blas).reset();
    VertexBuffer->Buffer.reset();
    IndexBuffer->Buffer.reset();
    Cleanup();
}
