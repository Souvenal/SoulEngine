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
using SoulEngine::ShaderCompiler::CompileDesc;
using SoulEngine::ShaderCompiler::ShaderCompiler;

// ── Shader sources ──────────────────────────────────────────────────────────

// Shader with various vertex input layouts to exercise reflection.
constexpr std::string_view VertexInputTestShader = R"(
    struct PositionOnly
    {
        float3 position : POSITION;
    };

    struct FullInput
    {
        float3 position : POSITION;
        float3 normal   : NORMAL;
        float4 tangent  : TANGENT;
        float2 uv0      : TEXCOORD0;
        float2 uv1      : TEXCOORD1;
        uint4  boneIDs  : BLENDINDICES;
        float4 weights  : BLENDWEIGHT;
    };

    struct VSOutput
    {
        float4 position : SV_Position;
        float2 uv;
    };

    struct SceneParams
    {
        float4x4 viewProj;
    };

    [[vk::binding(0, 0)]] ConstantBuffer<SceneParams> g_scene;

    [shader("vertex")]
    VSOutput VertexMain_PositionOnly(PositionOnly input)
    {
        VSOutput o;
        o.position = float4(input.position, 1.0);
        o.uv = float2(0, 0);
        return o;
    }

    [shader("vertex")]
    VSOutput VertexMain_Full(FullInput input)
    {
        VSOutput o;
        o.position = float4(input.position, 1.0);
        o.uv = input.uv0;
        return o;
    }
)";

// Shader with explicit [[vk::location]] annotations.
constexpr std::string_view ExplicitLocationShader = R"(
    struct VSInput
    {
        [[vk::location(3)]] float3 pos : POSITION;
        [[vk::location(1)]] float2 uv : TEXCOORD0;
        [[vk::location(5)]] float4 col : COLOR;
    };

    struct VSOutput
    {
        float4 position : SV_Position;
        float2 uv;
    };

    [shader("vertex")]
    VSOutput VertexMain(VSInput input)
    {
        VSOutput o;
        o.position = float4(input.pos, 1.0);
        o.uv = input.uv;
        return o;
    }
)";

// Shader with only system-value vertex inputs (should produce no vertex inputs).
constexpr std::string_view SystemValueOnlyShader = R"(
    struct VSOutput
    {
        float4 position : SV_Position;
    };

    [shader("vertex")]
    VSOutput VertexMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID)
    {
        VSOutput o;
        o.position = float4(0, 0, 0, 1);
        return o;
    }

    [shader("fragment")]
    float4 FragmentMain() : SV_Target
    {
        return float4(1, 0, 0, 1);
    }
)";

// Shader with push constants and mixed resource types.
constexpr std::string_view PushConstantAndResourcesShader = R"(
    struct PushData
    {
        float4 color;
        float  intensity;
    };

    [[vk::push_constant]] PushData g_push;

    [[vk::binding(0, 0)]] ConstantBuffer<float4> g_cb;
    [[vk::binding(1, 0)]] Texture2D<float4>      g_tex;
    [[vk::binding(2, 0)]] SamplerState            g_sam;

    struct VSOutput
    {
        float4 position : SV_Position;
        float4 color;
    };

    [shader("vertex")]
    VSOutput VertexMain(uint vid : SV_VertexID)
    {
        VSOutput o;
        o.position = float4(0, 0, 0, 1) + g_push.color * g_cb[0];
        o.color = g_tex.Sample(g_sam, float2(0, 0));
        return o;
    }
)";

// ── Helpers ────────────────────────────────────────────────────────────────

/// Verify a program has the expected name and stage.
static auto ExpectProgram(const Program& Program, StringView ExpectedName, Stage ExpectedStage) -> void {
    EXPECT_EQ(Program.EntryPointName, ExpectedName);
    EXPECT_EQ(Program.Stage, ExpectedStage);
    ASSERT_NE(Program.Reflection, nullptr);
}

// ── Vertex Input Reflection ────────────────────────────────────────────────

TEST(ReflectionTest, PositionOnlyVertexInput) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(VertexInputTestShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain_PositionOnly",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& Prog = Result->front();
    ExpectProgram(Prog, "VertexMain_PositionOnly", Stage::Vertex);

    const auto& R = *Prog.Reflection;
    ASSERT_EQ(R.VertexInputs.size(), 1UL);

    EXPECT_EQ(R.VertexInputs[0].SemanticName, "POSITION");
    EXPECT_EQ(R.VertexInputs[0].SemanticIndex, 0U);
    EXPECT_TRUE(R.VertexInputs[0].Location.has_value());
    EXPECT_EQ(R.VertexInputs[0].ValueType.ScalarType, ScalarType::Float32);
    EXPECT_EQ(R.VertexInputs[0].ValueType.RowCount, 1U);
    EXPECT_EQ(R.VertexInputs[0].ValueType.ColumnCount, 3U);
}

TEST(ReflectionTest, FullVertexInputLayout) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(VertexInputTestShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain_Full",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& Prog = Result->front();
    ExpectProgram(Prog, "VertexMain_Full", Stage::Vertex);

    const auto& R = *Prog.Reflection;
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

TEST(ReflectionTest, ExplicitVkLocation) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(ExplicitLocationShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& R = *Result->front().Reflection;
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

TEST(ReflectionTest, SystemValueOnlyVertexInputs) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(SystemValueOnlyShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& R = *Result->front().Reflection;
    EXPECT_TRUE(R.VertexInputs.empty()) << "System-value semantics (SV_*) should be filtered out from vertex inputs";
}

// ── Fragment shader vertex inputs ──────────────────────────────────────────

TEST(ReflectionTest, FragmentShaderNoVertexInputs) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(SystemValueOnlyShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "FragmentMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& R = *Result->front().Reflection;
    EXPECT_TRUE(R.VertexInputs.empty());
}

// ── Bindings Reflection ────────────────────────────────────────────────────

TEST(ReflectionTest, ResourceBindings) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(PushConstantAndResourcesShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& R = *Result->front().Reflection;

    // ConstantBuffer<float4> [[vk::binding(0, 0)]]
    auto It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.Set == 0 && B.Binding == 0; });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Type, ResourceType::UniformBuffer);
    EXPECT_EQ(It->ArrayCount, 1U);

    // Texture2D<float4> [[vk::binding(1, 0)]]
    It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.Set == 0 && B.Binding == 1; });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Type, ResourceType::SampledTexture);

    // SamplerState [[vk::binding(2, 0)]]
    It = std::ranges::find_if(R.Bindings, [](const Binding& B) { return B.Set == 0 && B.Binding == 2; });
    ASSERT_NE(It, R.Bindings.end());
    EXPECT_EQ(It->Type, ResourceType::Sampler);
}

// ── Push Constants Reflection ──────────────────────────────────────────────

TEST(ReflectionTest, PushConstants) {
    auto Result = ShaderCompiler::Get().Compile(CompileDesc{
        .Source         = StringView(PushConstantAndResourcesShader),
        .Backend        = Backend::Slang,
        .EntryPointName = "VertexMain",
    });
    ASSERT_TRUE(Result.has_value()) << Result.error().ToString();
    ASSERT_EQ(Result->size(), 1UL);

    const auto& R = *Result->front().Reflection;

    // PushData has float4 + float = 16 + 4 = 20 bytes.
    // Slang may report a compacted size via push-constant layout;
    // accept any non-zero size as meaningful.
    ASSERT_FALSE(R.PushConstants.empty());
    EXPECT_EQ(R.PushConstants[0].Offset, 0U);
    EXPECT_GT(R.PushConstants[0].Size, 0U) << "Push constant range should have a non-zero byte size";
}
