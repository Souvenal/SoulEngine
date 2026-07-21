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

TEST(RayTracingPipelineHardwareTest, DISABLED_CreatesPipelineFromSlangFixture) {
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

    const Path ShaderPath = EngineDir / "Source" / "Runtime" / "ShaderCompiler" / "Tests" / "Slang" / "RayTracing.slang";
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

    (*Pipeline).reset();
    Cleanup();
}
