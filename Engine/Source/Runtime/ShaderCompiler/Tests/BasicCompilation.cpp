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
