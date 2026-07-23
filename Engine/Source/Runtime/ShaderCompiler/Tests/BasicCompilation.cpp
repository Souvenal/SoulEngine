/// @file   BasicCompilation.cpp
/// @brief  Tests for successful shader compilation paths.

#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine::Shader;
using SoulEngine::ShaderCompiler::Backend;
using SoulEngine::ShaderCompiler::GraphicsCompileDesc;
using SoulEngine::ShaderCompiler::ShaderCompiler;
using SoulEngine::ShaderCompiler::ShaderEntry;

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
        .Vertex   = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "VertexMain", .Backend = Backend::Slang},
        .Fragment = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "FragmentMain", .Backend = Backend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_FALSE(Result->Code.empty());
    EXPECT_TRUE(IsValidSPIRV(Result->Code));
    EXPECT_EQ(Result->VertexEntryPointName, "VertexMain");
    EXPECT_EQ(Result->FragmentEntryPointName, "FragmentMain");
    EXPECT_FALSE(Result->Reflection.Bindings.empty());
}

[[nodiscard]] static auto HasBinding(const Reflection& InReflection, StringView BindingPath, ResourceType Type) -> bool {
    return std::ranges::any_of(InReflection.Bindings, [&](const Binding& InBinding) {
        return InBinding.ParameterPath == BindingPath && InBinding.Type == Type;
    });
}

TEST_F(ShaderCompilerTest, CompileForwardPbrProgramWithExpectedBindings) {
    auto ProjectDir = m_TestShaderPath;
    for (Uint32 Index = 0; Index < 7; ++Index)
        ProjectDir = ProjectDir.parent_path();

    const auto ShaderDir  = ProjectDir / "Engine" / "Shaders";
    const auto ShaderPath = ShaderDir / "ForwardPbr.slang";
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "vertMain", .Backend = Backend::Slang},
        .Fragment = ShaderEntry{.SourcePath = ShaderPath, .EntryPoint = "fragMain", .Backend = Backend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_TRUE(HasBinding(Result->Reflection, "g_forwardFrameView.frame", ResourceType::ConstantBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_forwardFrameView.view", ResourceType::ConstantBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_forwardMaterial.material", ResourceType::ConstantBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_forwardObject.object", ResourceType::ConstantBuffer));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_textures.uTextures", ResourceType::SampledTexture));
    EXPECT_TRUE(HasBinding(Result->Reflection, "g_samplers.uSamplerLinear", ResourceType::Sampler));
    EXPECT_EQ(Result->Reflection.Bindings.size(), 7);
}
