/// @file   SpecializationConstant.cpp
/// @brief  Tests for Slang [SpecializationConstant] reflection:
/// reflected entries (name / constant id / scalar type / default) plus a
/// SPIR-V byte-level assertion that the constants lower to OpSpecConstant*.

#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine;

namespace {

// SPIR-V opcodes/decoration needed by the byte-level assertions.
inline constexpr Uint32 SpvOpSpecConstantTrue       = 48;
inline constexpr Uint32 SpvOpSpecConstantFalse      = 49;
inline constexpr Uint32 SpvOpSpecConstant           = 50;
inline constexpr Uint32 SpvOpSpecConstantComposite  = 51;
inline constexpr Uint32 SpvOpDecorate               = 71;
inline constexpr Uint32 SpvDecorationSpecId         = 1;

[[nodiscard]] auto ContainsSpecConstantInstruction(std::span<const Uint32> Code) -> bool {
    // Walk the instruction stream: word 0 packs (word count << 16) | opcode.
    if (Code.size() < 5)
        return false;
    for (size_t Offset = 5; Offset < Code.size();) {
        const auto Word  = Code[Offset];
        const auto Op    = Word & 0xFFFFU;
        const auto Count = Word >> 16U;
        if (Count == 0)
            return false;
        if (Op == SpvOpSpecConstantTrue || Op == SpvOpSpecConstantFalse || Op == SpvOpSpecConstant ||
            Op == SpvOpSpecConstantComposite)
            return true;
        Offset += Count;
    }
    return false;
}

[[nodiscard]] auto ContainsSpecIdDecoration(std::span<const Uint32> Code) -> bool {
    if (Code.size() < 5)
        return false;
    for (size_t Offset = 5; Offset < Code.size();) {
        const auto Word  = Code[Offset];
        const auto Op    = Word & 0xFFFFU;
        const auto Count = Word >> 16U;
        if (Count == 0)
            return false;
        // OpDecorate target decoration ... — decoration is the third word.
        if (Op == SpvOpDecorate && Count >= 3 && Code[Offset + 2] == SpvDecorationSpecId)
            return true;
        Offset += Count;
    }
    return false;
}

class SpecializationConstantTest : public ::testing::Test {
  protected:
    static inline Path m_ShaderPath = {};

    static auto SetUpTestSuite() -> void {
        const char* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";
        m_ShaderPath = Path(TestSourceDir) / "Slang" / "SpecializationConstant.slang";
    }
};

} // namespace

TEST_F(SpecializationConstantTest, ReflectsSpecializationConstants) {
    auto Result = ShaderCompiler::Get().CompileCompute(ComputeCompileDesc{
        .Compute = ShaderEntry{.SourcePath = m_ShaderPath, .EntryPoint = "ComputeMain", .Backend = ShaderBackend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& Constants = Result->Reflection.SpecializationConstants;
    ASSERT_EQ(Constants.size(), 3UL);

    const auto FindByName = [&Constants](StringView Name) {
        return std::ranges::find_if(Constants, [&](const auto& C) { return C.Name == Name; });
    };

    auto It = FindByName("IS_INPUT_ENVIRONMENT_MAP");
    ASSERT_NE(It, Constants.end());
    EXPECT_EQ(It->ScalarType, ShaderScalarType::Bool);
    ASSERT_TRUE(It->DefaultValue.has_value());
    EXPECT_EQ(*It->DefaultValue, 1);

    It = FindByName("SAMPLE_COUNT");
    ASSERT_NE(It, Constants.end());
    EXPECT_EQ(It->ConstantId, 7U);
    EXPECT_EQ(It->ScalarType, ShaderScalarType::Uint32);
    ASSERT_TRUE(It->DefaultValue.has_value());
    EXPECT_EQ(*It->DefaultValue, 16);

    It = FindByName("BLEND_FACTOR");
    ASSERT_NE(It, Constants.end());
    EXPECT_EQ(It->ScalarType, ShaderScalarType::Float32);
    // Float has no Slang SDK default getter.
    EXPECT_FALSE(It->DefaultValue.has_value());
}

TEST_F(SpecializationConstantTest, LowersToSpirvSpecConstants) {
    auto Result = ShaderCompiler::Get().CompileCompute(ComputeCompileDesc{
        .Compute = ShaderEntry{.SourcePath = m_ShaderPath, .EntryPoint = "ComputeMain", .Backend = ShaderBackend::Slang},
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_TRUE(ContainsSpecConstantInstruction(Result->Code));
    EXPECT_TRUE(ContainsSpecIdDecoration(Result->Code));
}
