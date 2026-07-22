/// @file   RayTracingPipeline.cpp
/// @brief  Unit tests for Vulkan ray-tracing shader-binding-table layout arithmetic.

#include <gtest/gtest.h>

#include <GLFW/glfw3.h>

import RHI;
import Shader;
import ShaderCompiler;
import Vulkan;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine::RHI;
using namespace SoulEngine::RHI::Vulkan;
using namespace SoulEngine::ShaderCompiler;

TEST(ShaderBindingTableLayoutTest, AlignsRecordsAndRegions) {
    auto Layout = CreateShaderBindingTableLayout(
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
    auto Layout = CreateShaderBindingTableLayout(
        32, 32, 64, 4096, 1, 1, 1, 0);

    ASSERT_TRUE(Layout.has_value()) << Layout.error().ToString();
    EXPECT_EQ(Layout->Callable.Offset, 0);
    EXPECT_EQ(Layout->Callable.Stride, 0);
    EXPECT_EQ(Layout->Callable.Size, 0);
    EXPECT_EQ(Layout->Callable.RecordCount, 0);
}

TEST(ShaderBindingTableLayoutTest, RejectsInvalidRayGenerationRecordCount) {
    auto Layout = CreateShaderBindingTableLayout(
        32, 32, 64, 4096, 0, 1, 1, 0);

    ASSERT_FALSE(Layout.has_value());
    EXPECT_NE(Layout.error().ToString().find("exactly one ray-generation"), String::npos);
}

TEST(ShaderBindingTableLayoutTest, RejectsRecordStrideBeyondDeviceLimit) {
    auto Layout = CreateShaderBindingTableLayout(
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

    auto DeviceResult = RenderDevice::Create(Window);
    if (!DeviceResult) {
        glfwDestroyWindow(Window);
        glfwTerminate();
        GTEST_SKIP() << DeviceResult.error().ToString();
    }

    const auto Cleanup = [Window]() -> void {
        RenderDevice::Destroy();
        glfwDestroyWindow(Window);
        glfwTerminate();
    };

    const std::array<Float32, 9> TriangleVertices{
        -0.5f, -0.5f, 0.0f,
         0.5f, -0.5f, 0.0f,
         0.0f,  0.5f, 0.0f,
    };
    const std::array<Uint32, 3> TriangleIndices{0, 1, 2};
    auto VertexBuffer = RenderDevice::Get().CreateVertexBuffer(VertexBufferDesc{
        .Data        = TriangleVertices.data(),
        .VertexCount = 3,
        .Stride      = sizeof(Float32) * 3,
    });
    auto IndexBuffer = RenderDevice::Get().CreateIndexBuffer(IndexBufferDesc{
        .Data       = TriangleIndices.data(),
        .IndexCount = 3,
    });
    if (!VertexBuffer || !IndexBuffer) {
        const auto Error = !VertexBuffer ? VertexBuffer.error().ToString() : IndexBuffer.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    auto Blas = RenderDevice::Get().CreateBottomLevelAccelerationStructure(BottomLevelAccelerationStructureDesc{
        .Geometries = {
            TriangleAccelerationStructureGeometryDesc{
                .VertexBufferPtr = VertexBuffer->Buffer.get(),
                .VertexCount     = 3,
                .VertexStride    = sizeof(Float32) * 3,
                .VertexFormat    = Format::R32G32B32_SFLOAT,
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
        .RayGeneration = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "rayGenMain", .Backend = Backend::Slang},
        .MissEntries   = {ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "missMain", .Backend = Backend::Slang}},
        .HitGroups = {
            RayTracingHitGroupCompileDesc{
                .Type       = SoulEngine::Shader::RayTracingHitGroupType::Triangles,
                .ClosestHit = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "closestHitMain", .Backend = Backend::Slang},
            },
        },
    });
    if (!Program) {
        const auto Error = Program.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    auto Pipeline = RenderDevice::Get().CreateRayTracingPipeline(RayTracingPipelineDesc{.Program = std::move(*Program)});
    if (!Pipeline) {
        const auto Error = Pipeline.error().ToString();
        Cleanup();
        GTEST_SKIP() << Error;
    }
    EXPECT_NE(Pipeline->get(), nullptr);

    auto Tlas = RenderDevice::Get().CreateTopLevelAccelerationStructure(TopLevelAccelerationStructureDesc{
        .InitialInstanceCapacity = 1,
        .BuildFlags = AccelerationStructureBuildFlags::AllowUpdate,
    });
    if (!Tlas) {
        const auto Error = Tlas.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }
    auto Output = RenderDevice::Get().CreateRenderTarget(RenderTargetDesc{
        .Width  = 1,
        .Height = 1,
        .Format = Format::B8G8R8A8_UNORM,
        .Usage  = TextureUsage::RenderTarget | TextureUsage::ShaderStorage | TextureUsage::FrameOutput,
    });
    if (!Output) {
        const auto Error = Output.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    auto* GeometryTable = RenderDevice::Get().GetRayTracingGeometryTable();
    if (!GeometryTable) {
        Cleanup();
        GTEST_SKIP() << "BDA geometry table is unavailable";
    }

    auto Parameters = ShaderParameters::Create(**Pipeline);
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

    const std::array<AccelerationStructureInstance, 2> Instances{
        AccelerationStructureInstance{
            .BottomLevelPtr = Blas->get(),
            .CustomIndex = 0,
        },
        AccelerationStructureInstance{
            .BottomLevelPtr = Blas->get(),
            .Transform = RowMajorTransform3x4{
                .M03 = 0.1f,
            },
            .CustomIndex = 1,
        },
    };
    const RayTracingGeometryTableUpdate GeometryUpdate{
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
    CommandList Commands;
    auto& ScopeValue = Commands.Scopes.emplace_back(NonRenderingPass{});
    auto& Scope = std::get<NonRenderingPass>(ScopeValue);
    Scope.UpdateRayTracingGeometryTable(GeometryTable, GeometryUpdate);
    Scope.BuildOrUpdateTopLevelAccelerationStructure(Tlas->get(), Instances);
    Scope.SetRayTracingPipeline(Pipeline->get());
    Scope.BindShaderParameters(Pipeline->get(), std::move(Parameters));
    Scope.TraceRays(Pipeline->get(), 1, 1);
    Commands.PresentSource = Output->Texture.get();

    if (auto R = RenderDevice::Get().Execute(Commands); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    auto UpdateParameters = ShaderParameters::Create(**Pipeline);
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

    CommandList UpdateCommands;
    auto& UpdateScopeValue = UpdateCommands.Scopes.emplace_back(NonRenderingPass{});
    auto& UpdateScope = std::get<NonRenderingPass>(UpdateScopeValue);
    UpdateScope.UpdateRayTracingGeometryTable(GeometryTable, GeometryUpdate);
    UpdateScope.BuildOrUpdateTopLevelAccelerationStructure(
        Tlas->get(), Instances, TopLevelAccelerationStructureBuildMode::Update);
    UpdateScope.SetRayTracingPipeline(Pipeline->get());
    UpdateScope.BindShaderParameters(Pipeline->get(), std::move(UpdateParameters));
    UpdateScope.TraceRays(Pipeline->get(), 1, 1);
    UpdateCommands.PresentSource = Output->Texture.get();

    if (auto R = RenderDevice::Get().Execute(UpdateCommands); !R) {
        const auto Error = R.error().ToString();
        Cleanup();
        ADD_FAILURE() << Error;
        return;
    }

    Output->Texture.reset();
    (*Tlas).reset();
    (*Pipeline).reset();
    (*Blas).reset();
    VertexBuffer->Buffer.reset();
    IndexBuffer->Buffer.reset();
    Cleanup();
}
