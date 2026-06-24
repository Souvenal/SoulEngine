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
using SoulEngine::ShaderCompiler::CompileDesc;
using SoulEngine::ShaderCompiler::ShaderCompiler;
using SoulEngine::ShaderCompiler::ShaderEntry;

// ── Helpers ────────────────────────────────────────────────────────────

/// Verify that a bytecode buffer starts with the SPIR-V magic number.
[[nodiscard]] static auto IsValidSPIRV(std::span<const std::uint32_t> Words) -> bool {
    return Words.size() >= 5 && Words[0] == 0x07230203UL;
}

/// Verify a single ShaderProgram has the expected name and stage.
static auto ExpectProgram(const Program& Program, StringView ExpectedName, Stage ExpectedStage) -> void {
    EXPECT_EQ(Program.EntryPointName, ExpectedName);
    EXPECT_EQ(Program.Stage, ExpectedStage);
    ASSERT_NE(Program.Reflection, nullptr);
}

static auto ExpectVertexReflection(const Program& Program) -> void {
    ASSERT_NE(Program.Reflection, nullptr);

    const auto& Reflection = *Program.Reflection;
    ASSERT_EQ(Reflection.Bindings.size(), 3UL);
    EXPECT_EQ(Reflection.Bindings[0].Set, 0U);
    EXPECT_EQ(Reflection.Bindings[0].Type, ResourceType::UniformBuffer);
    EXPECT_EQ(Reflection.Bindings[0].ArrayCount, 1U);

    EXPECT_EQ(Reflection.Bindings[1].Set, 0U);
    EXPECT_EQ(Reflection.Bindings[1].Type, ResourceType::SampledTexture);
    EXPECT_EQ(Reflection.Bindings[1].ArrayCount, 1U);

    EXPECT_EQ(Reflection.Bindings[2].Set, 0U);
    EXPECT_EQ(Reflection.Bindings[2].Type, ResourceType::Sampler);
    EXPECT_EQ(Reflection.Bindings[2].ArrayCount, 1U);

    ASSERT_EQ(Reflection.VertexInputs.size(), 2UL);
    EXPECT_EQ(Reflection.VertexInputs[0].SemanticName, "POSITION");
    EXPECT_EQ(Reflection.VertexInputs[0].SemanticIndex, 0U);
    EXPECT_EQ(Reflection.VertexInputs[0].ValueType.ScalarType, ScalarType::Float32);
    EXPECT_EQ(Reflection.VertexInputs[0].ValueType.RowCount, 1U);
    EXPECT_EQ(Reflection.VertexInputs[0].ValueType.ColumnCount, 3U);

    EXPECT_EQ(Reflection.VertexInputs[1].SemanticName, "TEXCOORD");
    EXPECT_EQ(Reflection.VertexInputs[1].SemanticIndex, 0U);
    EXPECT_EQ(Reflection.VertexInputs[1].ValueType.ScalarType, ScalarType::Float32);
    EXPECT_EQ(Reflection.VertexInputs[1].ValueType.RowCount, 1U);
    EXPECT_EQ(Reflection.VertexInputs[1].ValueType.ColumnCount, 2U);
}

static auto ExpectFragmentReflection(const Program& Program) -> void {
    ASSERT_NE(Program.Reflection, nullptr);

    const auto& Reflection = *Program.Reflection;
    ASSERT_EQ(Reflection.Bindings.size(), 3UL);
    EXPECT_TRUE(Reflection.PushConstants.empty());
    EXPECT_TRUE(Reflection.VertexInputs.empty());
}

class ShaderCompilerTest : public ::testing::Test {
  protected:
    static inline Path   m_TestShaderPath   = {};
    static inline String m_TestShaderSource = {};

    static auto SetUpTestSuite() -> void {
        const char* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

        m_TestShaderPath = Path(TestSourceDir) / "Slang" / "TestShader.slang";

        auto SourceResult = ReadFile(m_TestShaderPath);
        ASSERT_TRUE(SourceResult.has_value()) << SourceResult.error().ToString();

        m_TestShaderSource = *SourceResult;
    }
};

// ── Success: named entry points ────────────────────────────────────────

TEST_F(ShaderCompilerTest, CompileVertexEntryPointFromPath) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = m_TestShaderPath,
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    auto& Program = Result->front();
    ASSERT_NE(Program.Code, nullptr);
    EXPECT_TRUE(IsValidSPIRV(*Program.Code));
    ExpectProgram(Program, "VertexMain", Stage::Vertex);
    ExpectVertexReflection(Program);
}

TEST_F(ShaderCompilerTest, CompileVertexEntryPointFromInlineSource) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(m_TestShaderSource),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    auto& Program = Result->front();
    ASSERT_NE(Program.Code, nullptr);
    EXPECT_TRUE(IsValidSPIRV(*Program.Code));
    ExpectProgram(Program, "VertexMain", Stage::Vertex);
    ExpectVertexReflection(Program);
}

TEST_F(ShaderCompilerTest, CompileFragmentEntryPointFromPath) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = m_TestShaderPath,
        .Backend        = Backend::Slang,
        .EntryPointName = "FragmentMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    auto& Program = Result->front();
    ASSERT_NE(Program.Code, nullptr);
    EXPECT_TRUE(IsValidSPIRV(*Program.Code));
    ExpectProgram(Program, "FragmentMain", Stage::Fragment);
    ExpectFragmentReflection(Program);
}

TEST_F(ShaderCompilerTest, MultipleCompilesSucceed) {
    auto& Compiler = ShaderCompiler::Get();

    auto VS = Compiler.Compile(CompileDesc{
        .Source         = m_TestShaderPath,
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(VS.has_value());
    ASSERT_EQ(VS->size(), 1UL);

    auto PS = Compiler.Compile(CompileDesc{
        .Source         = m_TestShaderPath,
        .Backend        = Backend::Slang,
        .EntryPointName = "FragmentMain",
    });
    ASSERT_TRUE(PS.has_value());
    ASSERT_EQ(PS->size(), 1UL);

    // Each entry point produces distinct bytecode — they share no code pointer.
    EXPECT_NE(VS->front().Code, PS->front().Code);
    ASSERT_NE(VS->front().Reflection, nullptr);
    ASSERT_NE(PS->front().Reflection, nullptr);
}

/// Compile without an entry point name — backend compiles the entire module
/// and returns one ShaderProgram per reflected entry point.
TEST_F(ShaderCompilerTest, CompileModuleAllEntryPoints) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source  = m_TestShaderPath,
        .Backend = Backend::Slang,
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    // TestShader.slang has VertexMain and FragmentMain — expect at least 2.
    ASSERT_GE(Result->size(), 2UL);

    // All programs from a module-level compile share the same bytecode.
    auto& FirstProgram = Result->front();
    ASSERT_NE(FirstProgram.Code, nullptr);
    EXPECT_TRUE(IsValidSPIRV(*FirstProgram.Code));
    ASSERT_NE(FirstProgram.Reflection, nullptr);

    bool SawVertex   = false;
    bool SawFragment = false;
    for (size_t i = 0; i < Result->size(); ++i) {
        EXPECT_EQ(Result->at(i).Code, FirstProgram.Code)
            << "Module-level compile: all programs should share the same code pointer";
        ASSERT_NE(Result->at(i).Reflection, nullptr);

        if (Result->at(i).Stage == Stage::Vertex) {
            SawVertex = true;
        }
        if (Result->at(i).Stage == Stage::Fragment) {
            SawFragment = true;
        }
    }

    EXPECT_TRUE(SawVertex);
    EXPECT_TRUE(SawFragment);
}

TEST_F(ShaderCompilerTest, GetOrCompileCachesModuleEntryPoints) {
    ShaderCompiler::Get().ClearCache();

    auto Vertex = ShaderCompiler::Get().GetOrCompile(ShaderEntry{
        .SourcePath = m_TestShaderPath,
        .EntryPoint = "VertexMain",
        .Backend    = Backend::Slang,
    });

    ASSERT_TRUE(Vertex.has_value()) << Vertex.error().ToString();
    EXPECT_EQ(Vertex->EntryPointName, "VertexMain");
    EXPECT_EQ(Vertex->Stage, Stage::Vertex);

    auto Fragment = ShaderCompiler::Get().GetOrCompile(ShaderEntry{
        .SourcePath = m_TestShaderPath,
        .EntryPoint = "FragmentMain",
        .Backend    = Backend::Slang,
    });

    ASSERT_TRUE(Fragment.has_value()) << Fragment.error().ToString();
    EXPECT_EQ(Fragment->EntryPointName, "FragmentMain");
    EXPECT_EQ(Fragment->Stage, Stage::Fragment);
}

/// Compile purely from inline source (no file-system reads at all).
TEST_F(ShaderCompilerTest, InlineSourceOnly) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(m_TestShaderSource),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    auto& Program = Result->front();
    ASSERT_NE(Program.Code, nullptr);
    ASSERT_NE(Program.Reflection, nullptr);
    EXPECT_TRUE(IsValidSPIRV(*Program.Code));
    ExpectVertexReflection(Program);
}
