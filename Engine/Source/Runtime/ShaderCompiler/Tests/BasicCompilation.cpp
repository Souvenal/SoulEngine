/// @file   BasicCompilation.cpp
/// @brief  Tests for successful shader compilation paths.

#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine;

// ── Helpers ────────────────────────────────────────────────────────────

/// Verify that a bytecode buffer starts with the SPIR-V magic number.
[[nodiscard]] static auto IsValidSPIRV(std::span<const std::uint32_t> Words) -> bool {
    return Words.size() >= 5 && Words[0] == 0x07230203UL;
}

class ShaderCompilerTest : public ::testing::Test {
  protected:
    static inline Path m_TestShaderPath = {};

    static auto SetUpTestSuite() -> void {
        const char* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

        m_TestShaderPath = Path(TestSourceDir) / "Slang" / "TestShader.slang";

        auto SourceResult = ReadFile(m_TestShaderPath);
        ASSERT_TRUE(SourceResult.has_value()) << SourceResult.error().ToString();
    }
};

// ── Success: graphics pipeline compilation ─────────────────────────────

TEST_F(ShaderCompilerTest, CompileGraphicsProgramFromPath) {
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex =
            ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "VertexMain", .Backend = ShaderBackend::Slang},
        .Fragment =
            ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "FragmentMain", .Backend = ShaderBackend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_FALSE(Result->Code.empty());
    EXPECT_TRUE(IsValidSPIRV(Result->Code));
    EXPECT_EQ(Result->VertexEntryPointName, "VertexMain");
    EXPECT_EQ(Result->FragmentEntryPointName, "FragmentMain");
    EXPECT_FALSE(Result->Reflection.Bindings.empty());
}

[[nodiscard]] static auto
HasBinding(const ShaderReflection& InReflection, StringView BindingPath, ShaderResourceType Type) -> bool {
    return std::ranges::any_of(InReflection.Bindings, [&](const ShaderBinding& InBinding) {
        return InBinding.ParameterPath == BindingPath && InBinding.Type == Type;
    });
}

TEST_F(ShaderCompilerTest, CompileRasterGeometryProgramWithExpectedBindings) {
    auto ProjectDir = m_TestShaderPath;
    for (Uint32 Index = 0; Index < 7; ++Index)
        ProjectDir = ProjectDir.parent_path();

    const auto ShaderDir  = ProjectDir / "Engine" / "Shaders";
    const auto ShaderPath = ShaderDir / "RasterGeometry.slang";
    auto       Result     = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "vertMain", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "fragMain", .Backend = ShaderBackend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_TRUE(HasBinding(Result->Reflection, "g_rasterFrameView.frame", ShaderResourceType::ConstantBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_rasterFrameView.view", ShaderResourceType::ConstantBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_rasterDraw.instances", ShaderResourceType::StorageBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_rasterDraw.geometries", ShaderResourceType::StorageBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_rasterDraw.materials", ShaderResourceType::StorageBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_textures.uTextures", ShaderResourceType::SampledTexture));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_samplers.uSamplerLinear", ShaderResourceType::Sampler));
    EXPECT_EQ(Result->Reflection.Bindings.size(), 9);
    ASSERT_FALSE(Result->Reflection.PushConstants.empty());
    EXPECT_EQ(Result->Reflection.PushConstants[0].Offset, 0U);
    EXPECT_GE(Result->Reflection.PushConstants[0].Size, sizeof(Uint32));
    EXPECT_TRUE(Result->Reflection.VertexInputs.empty());
}

TEST_F(ShaderCompilerTest, CompileEditorSelectionOutlineProgram) {
    auto ProjectDir = m_TestShaderPath;
    for (Uint32 Index = 0; Index < 7; ++Index)
        ProjectDir = ProjectDir.parent_path();

    const auto ShaderPath = ProjectDir / "Engine" / "Shaders" / "EditorSelectionOutline.slang";
    auto       Result     = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "vertMain", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "fragMain", .Backend = ShaderBackend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_editorSelection.entityId", ShaderResourceType::SampledTexture));
    ASSERT_FALSE(Result->Reflection.PushConstants.empty());
    EXPECT_EQ(Result->Reflection.PushConstants[0].Offset, 0U);
    EXPECT_GE(Result->Reflection.PushConstants[0].Size, sizeof(Uint32) * 3);
}
