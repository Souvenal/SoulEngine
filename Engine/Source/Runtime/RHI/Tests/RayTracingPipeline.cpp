/// @file   RHIRayTracingPipeline.cpp
/// @brief  Unit tests for Vulkan ray-tracing shader-binding-table layout arithmetic.

#include <gtest/gtest.h>

import RHI;
import Shader;
import ShaderCompiler;
import Vulkan;
import WindowSystem;
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
    GTEST_SKIP();
}

