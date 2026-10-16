/// @file   SpecializationConstants.cpp
/// @brief  Tests for [SpecializationConstant] reflection and SPIR-V emission.

#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine;
// Helpers

static auto CompileComputeForReflection(const Path& SourcePath, StringView Entry = "computeMain")
    -> std::expected<ShaderComputeProgram, ErrorMessage> {
    return ShaderCompiler::Get().CompileCompute(ComputeCompileDesc{
        .Compute = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = String(Entry), .Backend = ShaderBackend::Slang},
    });
}

class SpecializationConstantTest : public ::testing::Test {
  protected:
    static inline Path m_SlangDir = {};

    static auto SetUpTestSuite() -> void {
        const char* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

        m_SlangDir = Path(TestSourceDir) / "Slang";

        auto Source = ReadFile(ShaderPath("ReflectionSpecializationConstants.slang"));
        ASSERT_TRUE(Source.has_value()) << Source.error().ToString();
    }

    [[nodiscard]] static auto ShaderPath(StringView Name) -> Path {
        return m_SlangDir / Path(String(Name));
    }
};
// SpecializationConstant reflection

TEST_F(SpecializationConstantTest, ReflectionEntries) {
    auto Result = CompileComputeForReflection(ShaderPath("ReflectionSpecializationConstants.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& Constants = Result->Reflection.SpecializationConstants;
    ASSERT_EQ(Constants.size(), 3UL);

    const auto FindByName = [&Constants](StringView Name) {
        return std::ranges::find_if(Constants,
                                    [Name](const ShaderSpecializationConstant& C) { return C.Name == Name; });
    };

    // bool constant: integer-channel default reads back as 0/1.
    const auto BoolIt = FindByName("kIsInputEnvironmentMap");
    ASSERT_NE(BoolIt, Constants.end());
    EXPECT_EQ(BoolIt->ConstantId, 0U);
    EXPECT_EQ(BoolIt->ScalarType, ShaderScalarType::Bool);
    ASSERT_TRUE(BoolIt->DefaultValue.has_value());
    EXPECT_EQ(*BoolIt->DefaultValue, 1);

    // int constant: default available through the Slang integer channel.
    const auto IntIt = FindByName("kSeedValue");
    ASSERT_NE(IntIt, Constants.end());
    EXPECT_EQ(IntIt->ConstantId, 1U);
    EXPECT_EQ(IntIt->ScalarType, ShaderScalarType::Int32);
    ASSERT_TRUE(IntIt->DefaultValue.has_value());
    EXPECT_EQ(*IntIt->DefaultValue, 7);

    // float constant: the Slang API exposes no float default getter, so the reflected default stays unknown.
    const auto FloatIt = FindByName("kScale");
    ASSERT_NE(FloatIt, Constants.end());
    EXPECT_EQ(FloatIt->ConstantId, 2U);
    EXPECT_EQ(FloatIt->ScalarType, ShaderScalarType::Float32);
    EXPECT_FALSE(FloatIt->DefaultValue.has_value());
}
// SPIR-V spec-constant emission

TEST_F(SpecializationConstantTest, SpirvEmitsSpecConstants) {
    auto Result = CompileComputeForReflection(ShaderPath("ReflectionSpecializationConstants.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& Code = Result->Code;
    ASSERT_GE(Code.size(), 5UL);
    EXPECT_EQ(Code[0], 0x07230203UL);

    // Slang may emit OpSpecConstantTrue (48) / OpSpecConstantFalse (49) or OpSpecConstant (50); all carry the
    // SpecId decoration (decoration 1) via OpDecorate (71). The fixture declares three used constants.
    constexpr std::array<Uint32, 3> SpecConstantOpcodes{48U, 49U, 50U};
    Uint32                          SpecConstantCount = 0;
    Uint32                          SpecIdDecorations = 0;
    for (size_t Index = 5; Index < Code.size();) {
        const Uint32 WordCount = Code[Index] >> 16U;
        const Uint32 Opcode    = Code[Index] & 0xFFFFU;
        if (std::ranges::find(SpecConstantOpcodes, Opcode) != SpecConstantOpcodes.end())
            ++SpecConstantCount;
        if (Opcode == 71U && WordCount >= 3U && Index + 2 < Code.size() && Code[Index + 2] == 1U)
            ++SpecIdDecorations;
        if (WordCount == 0U)
            break; // Defensive: malformed stream would loop forever.
        Index += WordCount;
    }
    EXPECT_EQ(SpecConstantCount, 3U);
    EXPECT_EQ(SpecIdDecorations, 3U);
}
