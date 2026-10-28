/// @file   GraphicsCommands.cpp
/// @brief  Public graphics-command recording and lifetime coverage.

#include <gtest/gtest.h>

import RHI;
import RHIVulkan.Capability;
import RHIVulkan.Staging;
import RHIVulkan.Types;
import vulkan;
import std;
import Core;

using namespace SoulEngine;

class MockCommandBuffer final : public RHIBuffer {
  public:
    MockCommandBuffer(StringView Name, Uint64 Size, RHIBufferUsage Usage, Uint32* Destructions = nullptr)
        : RHIBuffer(String(Name), Size, Usage, RHIMemoryUsage::CpuToGpu, RHILifetime::Transient),
          m_Destructions(Destructions) {}
    ~MockCommandBuffer() override {
        if (m_Destructions)
            ++*m_Destructions;
    }

  private:
    Uint32* m_Destructions = nullptr;
};

struct TestGraphicsPass final : IRHIGraphicsPass {
    using IRHIGraphicsPass::BindIndexBuffer;
    using IRHIGraphicsPass::BindVertexBuffers;
    using IRHIGraphicsPass::DrawIndexedIndirect;

    [[nodiscard]] auto GetName() const noexcept -> StringView override {
        return "TestGraphics";
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        return {};
    }
};

class GraphicsCommandTest : public testing::Test {
  protected:
    auto SetUp() -> void override {
        GDeferredDeletionQueue = &m_DeletionQueue;
    }
    auto TearDown() -> void override {
        m_DeletionQueue.Drain();
        GDeferredDeletionQueue = nullptr;
    }

    [[nodiscard]] auto MakeBuffer(StringView Name, Uint64 Size, RHIBufferUsage Usage, Uint32* Destructions = nullptr)
        -> RHIRef<RHIBuffer> {
        auto Result = RHIRef<RHIBuffer>::Create();
        auto Published =
            Result.Publish(std::make_unique<MockCommandBuffer>(Name, Size, Usage, Destructions), RHIRefState::Ready);
        EXPECT_TRUE(Published.has_value());
        return Result;
    }

    RHIDeferredDeletionQueue m_DeletionQueue;
};

TEST_F(GraphicsCommandTest, RecordsIndexedInputAndRetainsReferencedBuffers) {
    Uint32 IndexDestructions    = 0;
    Uint32 IndirectDestructions = 0;
    auto   Index                = MakeBuffer("Index", 64, RHIBufferUsage::Index, &IndexDestructions);
    auto   Indirect             = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect, &IndirectDestructions);

    TestGraphicsPass Pass;
    Pass.BindIndexBuffer(Index, 4);
    Pass.DrawIndexedIndirect(Indirect, 20, 2);

    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 2u);
    const auto* Bind = std::get_if<RHIBindIndexBufferCmd>(&Commands[0]);
    const auto* Draw = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[1]);
    ASSERT_NE(Bind, nullptr);
    ASSERT_NE(Draw, nullptr);
    EXPECT_EQ(Bind->IndexBuffer.TryGet(), Index.TryGet());
    EXPECT_EQ(Bind->Offset, 4u);
    EXPECT_EQ(Draw->IndirectBuffer.TryGet(), Indirect.TryGet());
    EXPECT_EQ(Draw->Offset, 20u);
    EXPECT_EQ(Draw->DrawCount, 2u);
    EXPECT_EQ(Draw->Stride, sizeof(Uint32) * 5);

    Index    = nullptr;
    Indirect = nullptr;
    m_DeletionQueue.Drain();
    EXPECT_EQ(IndexDestructions, 0u);
    EXPECT_EQ(IndirectDestructions, 0u);

    ASSERT_TRUE(Pass.Record());
    m_DeletionQueue.Drain();
    EXPECT_EQ(IndexDestructions, 1u);
    EXPECT_EQ(IndirectDestructions, 1u);
}

TEST_F(GraphicsCommandTest, RecordsVertexBindingsAndRetainsReferencedBuffers) {
    Uint32     Destructions = 0;
    std::array Bindings{
        RHIVertexBufferBinding{.Buffer = MakeBuffer("Position", 64, RHIBufferUsage::Vertex, &Destructions)},
        RHIVertexBufferBinding{.Buffer = MakeBuffer("Normal", 64, RHIBufferUsage::Vertex, &Destructions), .Offset = 4},
        RHIVertexBufferBinding{.Buffer = MakeBuffer("Tangent", 64, RHIBufferUsage::Vertex, &Destructions)},
        RHIVertexBufferBinding{.Buffer = MakeBuffer("TexCoord", 64, RHIBufferUsage::Vertex, &Destructions),
                               .Offset = 8},
    };

    TestGraphicsPass Pass;
    Pass.BindVertexBuffers(Bindings);

    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 1u);
    const auto* Bind = std::get_if<RHIBindVertexBuffersCmd>(&Commands[0]);
    ASSERT_NE(Bind, nullptr);
    ASSERT_EQ(Bind->BindingCount, 4u);
    for (std::size_t I = 0; I < Bindings.size(); ++I) {
        EXPECT_EQ(Bind->Bindings[I].Buffer.TryGet(), Bindings[I].Buffer.TryGet());
        EXPECT_EQ(Bind->Bindings[I].Offset, Bindings[I].Offset);
    }

    for (auto& Binding : Bindings) {
        Binding.Buffer = nullptr;
    }
    m_DeletionQueue.Drain();
    EXPECT_EQ(Destructions, 0u);
    ASSERT_TRUE(Pass.Record());
    m_DeletionQueue.Drain();
    EXPECT_EQ(Destructions, 4u);
}

TEST_F(GraphicsCommandTest, RejectsInvalidVertexBufferBindings) {
    auto Buffer = MakeBuffer("Vertex", 64, RHIBufferUsage::Vertex);

    TestGraphicsPass Empty;
    Empty.BindVertexBuffers(std::span<const RHIVertexBufferBinding>{});
    EXPECT_TRUE(Empty.GetCommands().empty());

    TestGraphicsPass                            TooMany;
    const std::array<RHIVertexBufferBinding, 5> FiveBindings{};
    TooMany.BindVertexBuffers(FiveBindings);
    EXPECT_TRUE(TooMany.GetCommands().empty());

    const auto ExpectRejected = [&](RHIVertexBufferBinding Binding) {
        TestGraphicsPass Pass;
        Pass.BindVertexBuffers(std::span{&Binding, 1});
        EXPECT_TRUE(Pass.GetCommands().empty());
    };
    ExpectRejected({});
    ExpectRejected({.Buffer = Buffer, .Offset = 2});
    ExpectRejected({.Buffer = Buffer, .Offset = 64});
    ExpectRejected({.Buffer = Buffer, .Offset = 68});

    TestGraphicsPass Boundary;
    Boundary.BindVertexBuffers(std::array{RHIVertexBufferBinding{.Buffer = Buffer, .Offset = 60}});
    EXPECT_EQ(Boundary.GetCommands().size(), 1u);
}

TEST_F(GraphicsCommandTest, RejectsInvalidIndexBufferBindings) {
    auto Index = MakeBuffer("Index", 64, RHIBufferUsage::Index);

    TestGraphicsPass Missing;
    Missing.BindIndexBuffer(nullptr);
    EXPECT_TRUE(Missing.GetCommands().empty());

    const auto ExpectRejected = [&](Uint64 Offset) {
        TestGraphicsPass Pass;
        Pass.BindIndexBuffer(Index, Offset);
        EXPECT_TRUE(Pass.GetCommands().empty());
    };
    ExpectRejected(2);
    ExpectRejected(64);
    ExpectRejected(68);

    TestGraphicsPass Boundary;
    Boundary.BindIndexBuffer(Index, 60);
    EXPECT_EQ(Boundary.GetCommands().size(), 1u);
}

TEST_F(GraphicsCommandTest, RejectsInvalidIndexedIndirectArguments) {
    auto Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect);

    TestGraphicsPass Missing;
    Missing.DrawIndexedIndirect(nullptr);
    EXPECT_TRUE(Missing.GetCommands().empty());

    const auto ExpectRejected = [&](Uint64 Offset, Uint32 DrawCount, Uint32 Stride) {
        TestGraphicsPass Pass;
        Pass.DrawIndexedIndirect(Indirect, Offset, DrawCount, Stride);
        EXPECT_TRUE(Pass.GetCommands().empty());
    };
    ExpectRejected(2, 1, 20);
    ExpectRejected(0, 1, 16);
    ExpectRejected(0, 1, 24);
    ExpectRejected(0, 0, 20);
    ExpectRejected(100, 1, 20);
    ExpectRejected(120, 1, 20);
    ExpectRejected(20, 5, 20);
    ExpectRejected(0, 6, 20);

    TestGraphicsPass Boundary;
    Boundary.DrawIndexedIndirect(Indirect, 80, 1, 20);
    EXPECT_EQ(Boundary.GetCommands().size(), 1u);
}

TEST_F(GraphicsCommandTest, ValidatesIndexedIndirectAgainstNativeDrawCountLimit) {
    auto                      Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect);
    RHIDrawIndexedIndirectCmd Command{.IndirectBuffer = Indirect, .Offset = 0, .DrawCount = 2};

    ASSERT_TRUE(Command.Validate(100, 2));
    auto Result = Command.Validate(100, 1);
    ASSERT_FALSE(Result);
    EXPECT_NE(Result.error().ToString().find("maxDrawIndirectCount"), String::npos);
}

TEST(GraphicsCommandSync, VertexInputMapsToNativeStageAndAccess) {
    const auto Stage = ToVkPipelineStages(RHISyncStage::VertexInput);
    EXPECT_EQ(Stage, vk::PipelineStageFlags2{vk::PipelineStageFlagBits2::eVertexInput});

    const auto VertexAccess = ToVkBufferAccess(RHIBufferUsage::Vertex, RHIAccess::Read);
    EXPECT_EQ(VertexAccess, vk::AccessFlags2{vk::AccessFlagBits2::eVertexAttributeRead});
    const auto IndexAccess = ToVkBufferAccess(RHIBufferUsage::Index, RHIAccess::Read);
    EXPECT_EQ(IndexAccess, vk::AccessFlags2{vk::AccessFlagBits2::eIndexRead});
}

TEST(GraphicsCommandSync, UploadAccessKeepsAccelerationStructureReadForGeometry) {
    const bool RtAvailable         = VulkanCapability::Get().IsRayTracingAvailable();
    const auto CheckGeometryUpload = [&](RHIBufferUsage Usage, vk::AccessFlagBits2 DirectAccess) {
        const auto Access = ToVkBufferUploadAccess(Usage);
        EXPECT_TRUE(static_cast<bool>(Access & DirectAccess));
        EXPECT_EQ(static_cast<bool>(Access & vk::AccessFlagBits2::eAccelerationStructureReadKHR), RtAvailable);
    };
    CheckGeometryUpload(RHIBufferUsage::Vertex, vk::AccessFlagBits2::eVertexAttributeRead);
    CheckGeometryUpload(RHIBufferUsage::Index, vk::AccessFlagBits2::eIndexRead);

    const auto StorageAccess = ToVkBufferUploadAccess(RHIBufferUsage::Storage);
    EXPECT_FALSE(static_cast<bool>(StorageAccess & vk::AccessFlagBits2::eAccelerationStructureReadKHR));
}
