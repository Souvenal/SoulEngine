#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine;

class RasterShaderCompilationTest : public testing::Test {
  protected:
    static auto SetUpTestSuite() -> void {
        const auto* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";
        m_TestSourceDir      = Path(TestSourceDir);
        const auto EngineDir = m_TestSourceDir.parent_path().parent_path().parent_path().parent_path();
        EnginePaths::Get().Init(EngineDir);
    }

    [[nodiscard]] static auto CompileCompute(const Path& SourcePath, StringView EntryPoint)
        -> std::expected<ShaderComputeProgram, ErrorMessage> {
        const std::array IncludeDirs{EnginePaths::Get().EngineShadersDirPath()};
        return ShaderCompiler::Get().CompileCompute(ComputeCompileDesc{
            .Compute     = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = String(EntryPoint)},
            .IncludeDirs = IncludeDirs,
        });
    }

    [[nodiscard]] static auto CompileGraphics(const Path& SourcePath, StringView VertexEntry, StringView FragmentEntry)
        -> std::expected<ShaderGraphicsProgram, ErrorMessage> {
        const std::array IncludeDirs{EnginePaths::Get().EngineShadersDirPath()};
        return ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
            .Vertex      = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = String(VertexEntry)},
            .Fragment    = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = String(FragmentEntry)},
            .IncludeDirs = IncludeDirs,
        });
    }

    inline static Path m_TestSourceDir = {};
};

TEST_F(RasterShaderCompilationTest, CullingAndSelectionFilterUseTheIndexedCommandAbi) {
    const std::array Consumers{
        m_TestSourceDir / "Slang" / "CullingIndexedCommandConsumer.slang",
        m_TestSourceDir / "Slang" / "SelectionIndexedCommandConsumer.slang",
    };
    for (const auto& Consumer : Consumers) {
        auto Result = CompileCompute(Consumer, "computeMain");
        ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
        EXPECT_FALSE(Result->Code.empty());
    }
}

TEST_F(RasterShaderCompilationTest, CullingAndSelectionFilterEntriesCompileAndReflectResources) {
    const auto&                                      ShaderDir = EnginePaths::Get().EngineShadersDirPath();
    const std::array<std::pair<Path, StringView>, 2> Entries{
        std::pair{ShaderDir / "Raster" / "CullingCompute.slang", StringView{"cullMain"}},
        std::pair{ShaderDir / "Editor" / "SelectionFilter.slang", StringView{"filterMain"}},
    };
    for (const auto& [SourcePath, EntryPoint] : Entries) {
        auto Result = CompileCompute(SourcePath, EntryPoint);
        ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
        EXPECT_FALSE(Result->Code.empty());
        EXPECT_FALSE(Result->Reflection.Bindings.empty());
    }
}

TEST_F(RasterShaderCompilationTest, VertexPullingUsesFixedFunctionIndexAndPullsOnlyAttributes) {
    auto Consumer =
        CompileGraphics(m_TestSourceDir / "Slang" / "VertexPullingLocalIndexConsumer.slang", "vertMain", "fragMain");
    ASSERT_TRUE(Consumer.has_value()) << Consumer.error().ToString();
    EXPECT_FALSE(Consumer->Code.empty());
    EXPECT_TRUE(Consumer->Reflection.VertexInputs.empty());

    const auto&      ShaderDir = EnginePaths::Get().EngineShadersDirPath();
    const std::array Paths{
        ShaderDir / "Raster" / "RasterGeometry.slang",
        ShaderDir / "Editor" / "SelectionMask.slang",
    };
    for (const auto& SourcePath : Paths) {
        auto Result = CompileGraphics(SourcePath, "vertMain", "fragMain");
        ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
        EXPECT_FALSE(Result->Code.empty());
        EXPECT_TRUE(Result->Reflection.VertexInputs.empty());
    }
}
