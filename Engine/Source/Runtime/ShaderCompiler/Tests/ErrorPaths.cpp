/// @file   ErrorPaths.cpp
/// @brief  Tests for shader compiler error handling.

#include <gtest/gtest.h>

import Core;
import Shader;
import std;
import ShaderCompiler;

using namespace SoulEngine;

// ── Helpers ────────────────────────────────────────────────────────────

static auto ExpectErrorContains(const std::expected<ShaderGraphicsProgram, ErrorMessage>& Result,
                                StringView                                                              ExpectedText)
    -> void {
    ASSERT_FALSE(Result.has_value());

    const String ErrorText = Result.error().ToString();
    EXPECT_NE(ErrorText.find(ExpectedText), String::npos) << ErrorText;
}

class ShaderCompilerErrorPathTest : public ::testing::Test {
  protected:
    static inline Path m_TestShaderPath        = {};
    static inline Path m_InvalidShaderPath     = {};

    static auto SetUpTestSuite() -> void {
        const char* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

        const Path SlangDir = Path(TestSourceDir) / "Slang";
        m_TestShaderPath    = SlangDir / "TestShader.slang";
        m_InvalidShaderPath = SlangDir / "ErrorSyntax.slang";

        auto TestShader = ReadFile(m_TestShaderPath);
        ASSERT_TRUE(TestShader.has_value()) << TestShader.error().ToString();

        auto InvalidShader = ReadFile(m_InvalidShaderPath);
        ASSERT_TRUE(InvalidShader.has_value()) << InvalidShader.error().ToString();
    }
};

TEST_F(ShaderCompilerErrorPathTest, InvalidEntryPointName) {
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "doesNotExist", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "FragmentMain", .Backend = ShaderBackend::Slang},
    });
    ExpectErrorContains(Result, "Entry point 'doesNotExist' not found");
}

TEST_F(ShaderCompilerErrorPathTest, InvalidFragmentEntryPointName) {
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "VertexMain", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "doesNotExist", .Backend = ShaderBackend::Slang},
    });
    ExpectErrorContains(Result, "Entry point 'doesNotExist' not found");
}

TEST_F(ShaderCompilerErrorPathTest, BackendsMustMatch) {
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "VertexMain", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = m_TestShaderPath, .EntryPoint = "FragmentMain", .Backend = ShaderBackend::Unknown},
    });
    ExpectErrorContains(Result, "Graphics shader compile requires matching vertex/fragment backends");
}

TEST(ShaderCompilerTest, NonexistentFile) // NOLINT-RAW-MEM
{
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = "/nonexistent/path/to/file.slang", .EntryPoint = "VertexMain", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = "/nonexistent/path/to/file.slang", .EntryPoint = "FragmentMain", .Backend = ShaderBackend::Slang},
    });
    ExpectErrorContains(Result, "Cannot open file");
}

TEST_F(ShaderCompilerErrorPathTest, InvalidShaderSource) {
    auto Result = ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = m_InvalidShaderPath, .EntryPoint = "VertexMain", .Backend = ShaderBackend::Slang},
        .Fragment = ShaderEntry{.SourcePath = m_InvalidShaderPath, .EntryPoint = "FragmentMain", .Backend = ShaderBackend::Slang},
    });
    ExpectErrorContains(Result, "Failed to load module");
    ExpectErrorContains(Result, "ErrorSyntax.slang");
}
