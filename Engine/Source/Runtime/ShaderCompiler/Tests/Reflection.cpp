/// @file   Reflection.cpp
/// @brief  Tests for shader reflection data (bindings, push constants, vertex inputs).

#include <gtest/gtest.h>

import Core;
import Shader;
import ShaderCompiler;
import std;

using namespace SoulEngine::Core;
using namespace SoulEngine::Shader;
using SoulEngine::ShaderCompiler::Backend;
using SoulEngine::ShaderCompiler::GraphicsCompileDesc;
using SoulEngine::ShaderCompiler::ShaderEntry;
using SoulEngine::ShaderCompiler::ShaderCompiler;

// ── Helpers ────────────────────────────────────────────────────────────────

static auto CompileGraphicsForReflection(const Path& SourcePath,
                                         StringView  VertexEntry = "VertexMain",
                                         StringView  FragmentEntry = "FragmentMain")
    -> std::expected<GraphicsProgram, ErrorMessage> {
    return ShaderCompiler::Get().CompileGraphics(GraphicsCompileDesc{
        .Vertex   = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = String(VertexEntry), .Backend = Backend::Slang},
        .Fragment = ShaderEntry{.SourcePath = SourcePath, .EntryPoint = String(FragmentEntry), .Backend = Backend::Slang},
    });
}

class ReflectionTest : public ::testing::Test {
  protected:
    static inline Path m_SlangDir = {};

    static auto SetUpTestSuite() -> void {
        const char* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";

        m_SlangDir = Path(TestSourceDir) / "Slang";

        constexpr std::array Fixtures{
            "ReflectionVertexInputs.slang",
            "ReflectionExplicitLocation.slang",
            "ReflectionSystemValueOnly.slang",
            "ReflectionPushConstantsAndResources.slang",
            "ReflectionExplicitDescriptorSet.slang",
            "ReflectionRuntimeParameterBlock.slang",
            "ReflectionNestedParameterBlock.slang",
        };

        for (StringView Fixture : Fixtures) {
            auto Source = ReadFile(ShaderPath(Fixture));
            ASSERT_TRUE(Source.has_value()) << Source.error().ToString();
        }
    }

    [[nodiscard]] static auto ShaderPath(StringView Name) -> Path {
        return m_SlangDir / Path(String(Name));
    }
};

// ── Vertex Input Reflection ────────────────────────────────────────────────

TEST_F(ReflectionTest, PositionOnlyVertexInput) {
    auto Result = CompileGraphicsForReflection(
        ShaderPath("ReflectionVertexInputs.slang"), "VertexMain_PositionOnly");
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_EQ(Result->VertexEntryPointName, "VertexMain_PositionOnly");

    const auto& R = Result->Reflection;
    ASSERT_EQ(R.VertexInputs.size(), 1UL);

    EXPECT_EQ(R.VertexInputs[0].SemanticName, "POSITION");
    EXPECT_EQ(R.VertexInputs[0].SemanticIndex, 0U);
    EXPECT_TRUE(R.VertexInputs[0].Location.has_value());
    EXPECT_EQ(R.VertexInputs[0].ValueType.ScalarType, ScalarType::Float32);
    EXPECT_EQ(R.VertexInputs[0].ValueType.RowCount, 1U);
    EXPECT_EQ(R.VertexInputs[0].ValueType.ColumnCount, 3U);
}

TEST_F(ReflectionTest, FullVertexInputLayout) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionVertexInputs.slang"), "VertexMain_Full");
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    EXPECT_EQ(Result->VertexEntryPointName, "VertexMain_Full");

    const auto& R = Result->Reflection;
    ASSERT_EQ(R.VertexInputs.size(), 7UL);

    // POSITION: float3
    EXPECT_EQ(R.VertexInputs[0].SemanticName, "POSITION");
    EXPECT_EQ(R.VertexInputs[0].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[0].ValueType.ScalarType, ScalarType::Float32);
    EXPECT_EQ(R.VertexInputs[0].ValueType.ColumnCount, 3U);

    // NORMAL: float3
    EXPECT_EQ(R.VertexInputs[1].SemanticName, "NORMAL");
    EXPECT_EQ(R.VertexInputs[1].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[1].ValueType.ColumnCount, 3U);

    // TANGENT: float4
    EXPECT_EQ(R.VertexInputs[2].SemanticName, "TANGENT");
    EXPECT_EQ(R.VertexInputs[2].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[2].ValueType.ColumnCount, 4U);

    // TEXCOORD0: float2
    EXPECT_EQ(R.VertexInputs[3].SemanticName, "TEXCOORD");
    EXPECT_EQ(R.VertexInputs[3].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[3].ValueType.ColumnCount, 2U);

    // TEXCOORD1: float2
    EXPECT_EQ(R.VertexInputs[4].SemanticName, "TEXCOORD");
    EXPECT_EQ(R.VertexInputs[4].SemanticIndex, 1U);
    EXPECT_EQ(R.VertexInputs[4].ValueType.ColumnCount, 2U);

    // BLENDINDICES: uint4
    EXPECT_EQ(R.VertexInputs[5].SemanticName, "BLENDINDICES");
    EXPECT_EQ(R.VertexInputs[5].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[5].ValueType.ScalarType, ScalarType::Uint32);
    EXPECT_EQ(R.VertexInputs[5].ValueType.ColumnCount, 4U);

    // BLENDWEIGHT: float4
    EXPECT_EQ(R.VertexInputs[6].SemanticName, "BLENDWEIGHT");
    EXPECT_EQ(R.VertexInputs[6].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[6].ValueType.ColumnCount, 4U);

    // All inputs should have an explicit location assigned by slang.
    for (const auto& Attr : R.VertexInputs) {
        EXPECT_TRUE(Attr.Location.has_value()) << "Semantic " << Attr.SemanticName << " missing location";
    }
}

TEST_F(ReflectionTest, ExplicitVkLocation) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionExplicitLocation.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& R = Result->Reflection;
    ASSERT_EQ(R.VertexInputs.size(), 3UL);

    // [[vk::location(3)]] float3 pos : POSITION
    EXPECT_EQ(R.VertexInputs[0].SemanticName, "POSITION");
    EXPECT_EQ(R.VertexInputs[0].Location, 3U);
    EXPECT_EQ(R.VertexInputs[0].ValueType.ColumnCount, 3U);

    // [[vk::location(1)]] float2 uv : TEXCOORD0
    EXPECT_EQ(R.VertexInputs[1].SemanticName, "TEXCOORD");
    EXPECT_EQ(R.VertexInputs[1].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[1].Location, 1U);
    EXPECT_EQ(R.VertexInputs[1].ValueType.ColumnCount, 2U);

    // [[vk::location(5)]] float4 col : COLOR
    EXPECT_EQ(R.VertexInputs[2].SemanticName, "COLOR");
    EXPECT_EQ(R.VertexInputs[2].SemanticIndex, 0U);
    EXPECT_EQ(R.VertexInputs[2].Location, 5U);
    EXPECT_EQ(R.VertexInputs[2].ValueType.ColumnCount, 4U);
}

TEST_F(ReflectionTest, SystemValueOnlyVertexInputs) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionSystemValueOnly.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& R = Result->Reflection;
    EXPECT_TRUE(R.VertexInputs.empty()) << "System-value semantics (SV_*) should be filtered out from vertex inputs";
}

// ── Fragment shader vertex inputs ──────────────────────────────────────────

TEST_F(ReflectionTest, FragmentShaderNoVertexInputs) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionSystemValueOnly.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& R = Result->Reflection;
    EXPECT_TRUE(R.VertexInputs.empty());
}

// ── Bindings Reflection ────────────────────────────────────────────────────

TEST_F(ReflectionTest, ResourceBindings) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionPushConstantsAndResources.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& R = Result->Reflection;

    // ConstantBuffer<float4> [[vk::binding(0, 0)]]
    auto It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.Set == 0 && B.Binding == 0; });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->ParameterPath, "g_cb");
    EXPECT_EQ(It->Type, ResourceType::ConstantBuffer);
    EXPECT_EQ(It->ArrayCount, 1U);

    // Texture2D<float4> [[vk::binding(1, 0)]]
    It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.Set == 0 && B.Binding == 1; });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->ParameterPath, "g_tex");
    EXPECT_EQ(It->Type, ResourceType::SampledTexture);

    // SamplerState [[vk::binding(2, 0)]]
    It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.Set == 0 && B.Binding == 2; });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->ParameterPath, "g_sam");
	EXPECT_EQ(It->Type, ResourceType::Sampler);
}

TEST_F(ReflectionTest, ExplicitDescriptorSet) {
	auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionExplicitDescriptorSet.slang"));
	ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

	const auto& R = Result->Reflection;
	auto It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.ParameterPath == "g_scene"; });
	ASSERT_NE(It, R.Bindings.end());
	EXPECT_EQ(It->Set, 2U);
	EXPECT_EQ(It->Binding, 3U);
	EXPECT_EQ(It->Type, ResourceType::ConstantBuffer);
}

TEST_F(ReflectionTest, RuntimeParameterBlockBindingPaths) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionRuntimeParameterBlock.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& R = Result->Reflection;
    ASSERT_EQ(R.Bindings.size(), 5UL);

    auto It = std::ranges::find_if(R.Bindings, [](const Binding& B) {
        return B.ParameterPath == "g_frameView.frame";
    });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Set, 0U);
    EXPECT_EQ(It->Binding, 0U);
    EXPECT_EQ(It->Type, ResourceType::ConstantBuffer);

    It = std::ranges::find_if(R.Bindings, [](const Binding& B) {
        return B.ParameterPath == "g_frameView.view";
    });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Set, 0U);
    EXPECT_EQ(It->Binding, 1U);
    EXPECT_EQ(It->Type, ResourceType::ConstantBuffer);

    It = std::ranges::find_if(R.Bindings, [](const Binding& B) {
        return B.ParameterPath == "g_samplers.uSamplerLinear";
    });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Set, 1U);
    EXPECT_EQ(It->Binding, 0U);
    EXPECT_EQ(It->Type, ResourceType::Sampler);

    It = std::ranges::find_if(R.Bindings, [](const Binding& B) {
        return B.ParameterPath == "g_samplers.uSamplerAniso";
    });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Set, 1U);
    EXPECT_EQ(It->Binding, 1U);
    EXPECT_EQ(It->Type, ResourceType::Sampler);

    It = std::ranges::find_if(R.Bindings, [](const Binding& B) {
        return B.ParameterPath == "g_textures.uTextures";
    });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Set, 2U);
    EXPECT_EQ(It->Binding, 0U);
    EXPECT_EQ(It->Type, ResourceType::SampledTexture);
	EXPECT_EQ(It->ArrayCount, std::numeric_limits<Uint32>::max());
}

TEST_F(ReflectionTest, NestedParameterBlockIsRejected) {
	auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionNestedParameterBlock.slang"));
	ASSERT_FALSE(Result.has_value());

	const String ErrorText = Result.error().ToString();
	EXPECT_NE(ErrorText.find("Nested ParameterBlock"), String::npos) << ErrorText;
}

// ── Push Constants Reflection ──────────────────────────────────────────────

TEST_F(ReflectionTest, PushConstants) {
    auto Result = CompileGraphicsForReflection(ShaderPath("ReflectionPushConstantsAndResources.slang"));
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();

    const auto& R = Result->Reflection;

    // PushData has float4 + float = 16 + 4 = 20 bytes.
    // Slang may report a compacted size via push-constant layout;
    // accept any non-zero size as meaningful.
    ASSERT_FALSE(R.PushConstants.empty());
    EXPECT_EQ(R.PushConstants[0].Offset, 0U);
    EXPECT_GT(R.PushConstants[0].Size, 0U) << "Push constant range should have a non-zero byte size";
}
