/// @file   ErrorPaths.cpp
/// @brief  Tests for shader compiler error handling.

#include <gtest/gtest.h>

import Core;
import Shader;
import std;
import ShaderCompiler;

using namespace SoulEngine::Core;
using namespace SoulEngine::ShaderCompiler;

// ── Helpers ────────────────────────────────────────────────────────────

// Minimal test shader with vertex and fragment entry points.
constexpr std::string_view TestShader = R"(
    struct VSOutput
    {
        float4 position : SV_Position;
        float2 uv;
    };

    [shader("vertex")]
    VSOutput VertexMain(uint vertexID : SV_VertexID)
    {
        VSOutput o;
        o.position = float4(0, 0, 0, 1);
        return o;
    }

    [shader("fragment")]
    float4 FragmentMain(VSOutput input) : SV_Target
    {
        return float4(1, 0, 0, 1);
    }
)";

TEST(ShaderCompilerTest, InvalidEntryPointName) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(TestShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "doesNotExist",
    });
    EXPECT_FALSE(Result.has_value());
}

TEST(ShaderCompilerTest, UnsupportedExtension) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = std::filesystem::path("shader.cg"),
        .Backend        = Backend::Slang,
        .EntryPointName = "main",
    });
    EXPECT_FALSE(Result.has_value());
}

TEST(ShaderCompilerTest, NoFileExtension) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = std::filesystem::path("shader"),
        .Backend        = Backend::Slang,
        .EntryPointName = "main",
    });
    EXPECT_FALSE(Result.has_value());
}

TEST(ShaderCompilerTest, NonexistentFile) // NOLINT-RAW-MEM
{
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = std::filesystem::path("/nonexistent/path/to/file.slang"),
        .Backend        = Backend::Slang,
        .EntryPointName = "main",
    });
    EXPECT_FALSE(Result.has_value());
}

TEST(ShaderCompilerTest, InvalidShaderSource) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView("this is not valid shader source"),
        .Backend        = Backend::Slang,
        .EntryPointName = "main",
    });
    EXPECT_FALSE(Result.has_value());
}
