/// @file   RayTracing.cpp
/// @brief  Tests for Slang ray-tracing program compilation and reflection.

#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine;

namespace {

[[nodiscard]] auto MakeCompileDesc(const Path& SourcePath) -> RayTracingCompileDesc {
    return RayTracingCompileDesc{
        .RayGeneration = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = "rayGenMain", .Backend = ShaderBackend::Slang},
        .MissEntries   = {ShaderEntry{.SourcePath = SourcePath, .EntryPoint = "missMain", .Backend = ShaderBackend::Slang}},
        .HitGroups = {
            RayTracingHitGroupCompileDesc{
                .Type       = ShaderRayTracingHitGroupType::Triangles,
                .ClosestHit = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = "closestHitMain", .Backend = ShaderBackend::Slang},
            },
        },
    };
}

[[nodiscard]] auto FindBinding(const ShaderReflection& ReflectionValue, StringView ParameterPath) -> const ShaderBinding* {
    const auto It = std::ranges::find_if(ReflectionValue.Bindings, [ParameterPath](const ShaderBinding& Candidate) {
        return Candidate.ParameterPath == ParameterPath;
    });
    return It == ReflectionValue.Bindings.end() ? nullptr : &*It;
}

class RayTracingCompilerTest : public ::testing::Test {
  protected:
    static inline Path m_ShaderPath = {};
    static inline Path m_ParameterBlockShaderPath = {};
    static inline Path m_BdaShaderPath = {};
    static inline Path m_RuntimeBdaShaderPath = {};

    static auto SetUpTestSuite() -> void {
        const auto* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";
        m_ShaderPath = Path(TestSourceDir) / "Slang" / "RayTracing.slang";
        m_ParameterBlockShaderPath = Path(TestSourceDir) / "Slang" / "RayTracingParameterBlock.slang";
        m_BdaShaderPath = Path(TestSourceDir) / "Slang" / "RayTracingBda.slang";
        const Path EngineDir = Path(TestSourceDir).parent_path().parent_path().parent_path().parent_path();
        m_RuntimeBdaShaderPath = EngineDir / "Shaders" / "RayTracing.slang";
        auto Source = ReadFile(m_ShaderPath);
        ASSERT_TRUE(Source.has_value()) << Source.error().ToString();
        auto ParameterBlockSource = ReadFile(m_ParameterBlockShaderPath);
        ASSERT_TRUE(ParameterBlockSource.has_value()) << ParameterBlockSource.error().ToString();
        auto BdaSource = ReadFile(m_BdaShaderPath);
        ASSERT_TRUE(BdaSource.has_value()) << BdaSource.error().ToString();
        auto RuntimeBdaSource = ReadFile(m_RuntimeBdaShaderPath);
        ASSERT_TRUE(RuntimeBdaSource.has_value()) << RuntimeBdaSource.error().ToString();
    }
};

TEST_F(RayTracingCompilerTest, CompilesLinkedRayTracingProgram) {
    auto Result = ShaderCompiler::Get().CompileRayTracing(MakeCompileDesc(m_ShaderPath));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_FALSE(Result->Code.empty());
    EXPECT_EQ(Result->RayGenerationEntryPointName, "rayGenMain");
    ASSERT_EQ(Result->MissEntryPointNames.size(), 1);
    EXPECT_EQ(Result->MissEntryPointNames[0], "missMain");
    ASSERT_EQ(Result->HitGroups.size(), 1);
    EXPECT_EQ(Result->HitGroups[0].Type, ShaderRayTracingHitGroupType::Triangles);
    ASSERT_TRUE(Result->HitGroups[0].ClosestHitEntryPointName.has_value());
    EXPECT_EQ(*Result->HitGroups[0].ClosestHitEntryPointName, "closestHitMain");
    EXPECT_FALSE(Result->HitGroups[0].AnyHitEntryPointName.has_value());
    EXPECT_FALSE(Result->HitGroups[0].IntersectionEntryPointName.has_value());
    EXPECT_TRUE(Result->Reflection.VertexInputs.empty());
}

TEST_F(RayTracingCompilerTest, ReflectsTopLevelAccelerationStructureAndStorageOutput) {
    auto Result = ShaderCompiler::Get().CompileRayTracing(MakeCompileDesc(m_ShaderPath));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto* Tlas = FindBinding(Result->Reflection, "g_tlas");
    ASSERT_NE(Tlas, nullptr);
    EXPECT_EQ(Tlas->Set, 0U);
    EXPECT_EQ(Tlas->BindingIndex, 0U);
    EXPECT_EQ(Tlas->Type, ShaderResourceType::AccelerationStructure);

    const auto* Output = FindBinding(Result->Reflection, "g_output");
    ASSERT_NE(Output, nullptr);
    EXPECT_EQ(Output->Set, 0U);
    EXPECT_EQ(Output->BindingIndex, 1U);
    EXPECT_EQ(Output->Type, ShaderResourceType::StorageTexture);
}

TEST_F(RayTracingCompilerTest, ReflectsParameterBlockTopLevelAccelerationStructure) {
    auto Result = ShaderCompiler::Get().CompileRayTracing(MakeCompileDesc(m_ParameterBlockShaderPath));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto* Tlas = FindBinding(Result->Reflection, "g_rayTracingResources.tlas");
    ASSERT_NE(Tlas, nullptr);
    EXPECT_EQ(Tlas->Type, ShaderResourceType::AccelerationStructure);

    const auto* Output = FindBinding(Result->Reflection, "g_rayTracingResources.output");
    ASSERT_NE(Output, nullptr);
    EXPECT_EQ(Output->Type, ShaderResourceType::StorageTexture);
}

TEST_F(RayTracingCompilerTest, CompilesPhysicalStorageBdaAndReflectsFixedMetadataBinding) {
    auto Result = ShaderCompiler::Get().CompileRayTracing(MakeCompileDesc(m_BdaShaderPath));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    EXPECT_FALSE(Result->Code.empty());

    const auto* Metadata = FindBinding(Result->Reflection, "g_bdaMetadata.metadata");
    ASSERT_NE(Metadata, nullptr);
    EXPECT_EQ(Metadata->Type, ShaderResourceType::StorageBuffer);
    EXPECT_EQ(Metadata->ArrayCount, 1U);
}

TEST_F(RayTracingCompilerTest, CompilesRuntimePathTracingShaderWithFixedBdaMetadata) {
    auto Result = ShaderCompiler::Get().CompileRayTracing(RayTracingCompileDesc{
        .RayGeneration = ShaderEntry{.SourcePath = m_RuntimeBdaShaderPath, .EntryPoint = "rayGenMain", .Backend = ShaderBackend::Slang},
        .MissEntries = {
            ShaderEntry{.SourcePath = m_RuntimeBdaShaderPath, .EntryPoint = "missMain", .Backend = ShaderBackend::Slang},
            ShaderEntry{.SourcePath = m_RuntimeBdaShaderPath, .EntryPoint = "shadowMissMain", .Backend = ShaderBackend::Slang},
        },
        .HitGroups = {
            RayTracingHitGroupCompileDesc{
                .Type = ShaderRayTracingHitGroupType::Triangles,
                .ClosestHit = ShaderEntry{.SourcePath = m_RuntimeBdaShaderPath, .EntryPoint = "closestHitMain", .Backend = ShaderBackend::Slang},
            },
            RayTracingHitGroupCompileDesc{
                .Type = ShaderRayTracingHitGroupType::Triangles,
                .ClosestHit = ShaderEntry{.SourcePath = m_RuntimeBdaShaderPath, .EntryPoint = "shadowClosestHitMain", .Backend = ShaderBackend::Slang},
            },
        },
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto* Instances = FindBinding(Result->Reflection, "g_rayTracing.instances");
    ASSERT_NE(Instances, nullptr);
    EXPECT_EQ(Instances->Type, ShaderResourceType::StorageBuffer);
    EXPECT_EQ(Instances->ArrayCount, 1U);
    const auto* Geometries = FindBinding(Result->Reflection, "g_rayTracing.geometries");
    ASSERT_NE(Geometries, nullptr);
    EXPECT_EQ(Geometries->Type, ShaderResourceType::StorageBuffer);
    EXPECT_EQ(Geometries->ArrayCount, 1U);
    EXPECT_NE(FindBinding(Result->Reflection, "g_rayTracing.materials"), nullptr);
    EXPECT_NE(FindBinding(Result->Reflection, "g_rayTracing.accumulation"), nullptr);
}
TEST_F(RayTracingCompilerTest, MissingRayGenerationEntryReportsContext) {
    auto Desc = MakeCompileDesc(m_ShaderPath);
    Desc.RayGeneration.EntryPoint = "doesNotExist";

    auto Result = ShaderCompiler::Get().CompileRayTracing(Desc);
    ASSERT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("Ray-generation entry point 'doesNotExist' not found"), String::npos)
        << Result.error().ToString();
}

TEST_F(RayTracingCompilerTest, StageMismatchReportsContext) {
    auto Desc = MakeCompileDesc(m_ShaderPath);
    Desc.RayGeneration.EntryPoint = "missMain";

    auto Result = ShaderCompiler::Get().CompileRayTracing(Desc);
    ASSERT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("not a Ray-generation shader"), String::npos) << Result.error().ToString();
}

TEST_F(RayTracingCompilerTest, TriangleHitGroupRequiresClosestHitEntry) {
    auto Desc = MakeCompileDesc(m_ShaderPath);
    Desc.HitGroups[0].ClosestHit = std::nullopt;

    auto Result = ShaderCompiler::Get().CompileRayTracing(Desc);
    ASSERT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("requires a closest-hit entry point"), String::npos)
        << Result.error().ToString();
}

} // namespace
