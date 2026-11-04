/// @file   GraphicsCommands.cpp
/// @brief  Public graphics-command recording and lifetime coverage.

#include <gtest/gtest.h>

import RHI;
import std;
import Core;

using namespace SoulEngine;

class MockCommandBuffer final : public RHIBuffer {
  public:
    MockCommandBuffer(StringView     Name,
                      Uint64         Size,
                      RHIBufferUsage Usage,
                      RHIMemoryUsage MemoryUsage  = RHIMemoryUsage::CpuToGpu,
                      Uint32*        Destructions = nullptr)
        : RHIBuffer(String(Name), Size, Usage, MemoryUsage, RHILifetime::Transient), m_Destructions(Destructions) {}
    ~MockCommandBuffer() override {
        if (m_Destructions)
            ++*m_Destructions;
    }

  private:
    Uint32* m_Destructions = nullptr;
};

class MockCommandTexture final : public RHITexture {
  public:
    explicit MockCommandTexture(StringView Name)
        : RHITexture(String(Name),
                     RHITextureDesc{.Width       = 16,
                                    .Height      = 8,
                                    .MipLevels   = 4,
                                    .Format      = RHIFormat::R8G8B8A8_UNORM,
                                    .Usage       = RHITextureUsage::TransferSrc,
                                    .MemoryUsage = RHIMemoryUsage::GpuOnly}) {}
};

class MockCommandReadback final : public RHIReadback {
  public:
    MockCommandReadback() : RHIReadback("Readback", 4096) {}
    [[nodiscard]] auto TryRead() const -> std::vector<std::byte> override {
        return {};
    }
};

struct TestGraphicsPass final : IRHIGraphicsPass {
    using IRHIGraphicsPass::DrawIndexedIndirect;
    using IRHIGraphicsPass::DrawIndirect;

    [[nodiscard]] auto GetName() const noexcept -> StringView override {
        return "TestGraphics";
    }
    [[nodiscard]] auto Record() -> std::expected<void, ErrorMessage> override {
        m_Commands.clear();
        return {};
    }
};

struct TestTransferPass final : IRHITransferPass {
    using IRHITransferPass::BlitToSwapchain;
    using IRHITransferPass::ReadbackTexture;

    [[nodiscard]] auto GetName() const noexcept -> StringView override {
        return "TestTransfer";
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
        auto Result    = RHIRef<RHIBuffer>::Create();
        auto Published = Result.Publish(UPtr<MockCommandBuffer>(std::make_unique<MockCommandBuffer>(
                                            Name, Size, Usage, RHIMemoryUsage::CpuToGpu, Destructions)),
                                        RHIRefState::Ready);
        EXPECT_TRUE(Published.has_value());
        return Result;
    }

    [[nodiscard]] auto MakeTexture(StringView Name) -> RHIRef<RHITexture> {
        auto Result = RHIRef<RHITexture>::Create();
        auto Published =
            Result.Publish(UPtr<MockCommandTexture>(std::make_unique<MockCommandTexture>(Name)), RHIRefState::Ready);
        EXPECT_TRUE(Published.has_value());
        return Result;
    }

    [[nodiscard]] auto MakeReadback() -> RHIRef<RHIReadback> {
        auto Result = RHIRef<RHIReadback>::Create();
        auto Published =
            Result.Publish(UPtr<MockCommandReadback>(std::make_unique<MockCommandReadback>()), RHIRefState::Ready);
        EXPECT_TRUE(Published.has_value());
        return Result;
    }

    RHIDeferredDeletionQueue m_DeletionQueue;
};

TEST_F(GraphicsCommandTest, RecordsIndexedInputsAndRetainsReferencedBuffers) {
    Uint32        VertexDestructions   = 0;
    Uint32        IndexDestructions    = 0;
    Uint32        IndirectDestructions = 0;
    auto          Vertex               = MakeBuffer("Vertex", 128, RHIBufferUsage::Vertex, &VertexDestructions);
    auto          Index                = MakeBuffer("Index", 64, RHIBufferUsage::Index, &IndexDestructions);
    auto          Indirect             = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect, &IndirectDestructions);
    RHIBufferView VertexView{
        .Buffer = Vertex, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Vertex, .Offset = 16, .Range = 96};
    RHIBufferView IndexView{
        .Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Offset = 4, .Range = 60};
    RHIBufferView IndirectView{
        .Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Offset = 20, .Range = 40};
    std::array VertexViews{VertexView};

    TestGraphicsPass Pass;
    Pass.DrawIndexedIndirect(IndirectView, IndexView, VertexViews, 2);
    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 1u);
    const auto* Draw = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands.front());
    ASSERT_NE(Draw, nullptr);
    ASSERT_EQ(Draw->VertexBuffers.size(), 1u);
    EXPECT_EQ(Draw->VertexBuffers[0].Buffer.TryGet(), Vertex.TryGet());
    EXPECT_EQ(Draw->VertexBuffers[0].Offset, 16u);
    EXPECT_EQ(Draw->VertexBuffers[0].Range, 96u);
    EXPECT_EQ(Draw->IndexBuffer.Buffer.TryGet(), Index.TryGet());
    EXPECT_EQ(Draw->IndexBuffer.Offset, 4u);
    EXPECT_EQ(Draw->IndirectBuffer.Buffer.TryGet(), Indirect.TryGet());
    EXPECT_EQ(Draw->IndirectBuffer.Offset, 20u);
    EXPECT_EQ(Draw->DrawCount, 2u);

    VertexView   = {};
    IndexView    = {};
    IndirectView = {};
    VertexViews  = {};
    EXPECT_EQ(Draw->VertexBuffers[0].Offset, 16u);
    EXPECT_EQ(Draw->VertexBuffers[0].Range, 96u);
    Vertex   = nullptr;
    Index    = nullptr;
    Indirect = nullptr;
    m_DeletionQueue.Drain();
    EXPECT_EQ(VertexDestructions, 0u);
    EXPECT_EQ(IndexDestructions, 0u);
    EXPECT_EQ(IndirectDestructions, 0u);
    ASSERT_TRUE(Pass.Record());
    m_DeletionQueue.Drain();
    EXPECT_EQ(VertexDestructions, 1u);
    EXPECT_EQ(IndexDestructions, 1u);
    EXPECT_EQ(IndirectDestructions, 1u);
}

TEST_F(GraphicsCommandTest, RecordsIndirectDrawUsingViewExtentAndRetainsReference) {
    Uint32        Destructions = 0;
    auto          Buffer       = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect, &Destructions);
    RHIBufferView View{
        .Buffer = Buffer, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Offset = 20, .Range = 40};
    TestGraphicsPass Pass;
    Pass.DrawIndirect(View, 2);
    ASSERT_EQ(Pass.GetCommands().size(), 1u);
    const auto* Draw = std::get_if<RHIDrawIndirectCmd>(&Pass.GetCommands().front());
    ASSERT_NE(Draw, nullptr);
    EXPECT_EQ(Draw->IndirectBuffer.Buffer.TryGet(), Buffer.TryGet());
    EXPECT_EQ(Draw->IndirectBuffer.Offset, 20u);
    EXPECT_EQ(Draw->DrawCount, 2u);
    View   = {};
    Buffer = nullptr;
    m_DeletionQueue.Drain();
    EXPECT_EQ(Destructions, 0u);
    ASSERT_TRUE(Pass.Record());
    m_DeletionQueue.Drain();
    EXPECT_EQ(Destructions, 1u);
}

TEST_F(GraphicsCommandTest, RecordsEmptyAndFourOwnedVertexStreams) {
    auto                             Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect);
    auto                             Index    = MakeBuffer("Index", 64, RHIBufferUsage::Index);
    std::array<RHIBufferView, 4>     Vertices{};
    std::array<RHIRef<RHIBuffer>, 4> Buffers;
    for (Uint32 I = 0; I < Vertices.size(); ++I) {
        Buffers[I]  = MakeBuffer("Vertex", 32, RHIBufferUsage::Vertex);
        Vertices[I] = RHIBufferView{
            .Buffer = Buffers[I], .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Vertex, .Range = 32};
    }
    TestGraphicsPass Pass;
    Pass.DrawIndexedIndirect(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Range = 20},
        RHIBufferView{.Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Range = 64});
    Pass.DrawIndexedIndirect(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Range = 20},
        RHIBufferView{.Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Range = 64},
        Vertices);
    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 2u);
    const auto* Empty = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[0]);
    const auto* Four  = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[1]);
    ASSERT_NE(Empty, nullptr);
    ASSERT_NE(Four, nullptr);
    EXPECT_TRUE(Empty->VertexBuffers.empty());
    ASSERT_EQ(Four->VertexBuffers.size(), 4u);
    for (std::size_t I = 0; I < Four->VertexBuffers.size(); ++I)
        EXPECT_EQ(Four->VertexBuffers[I].Buffer.TryGet(), Buffers[I].TryGet());
}

TEST_F(GraphicsCommandTest, RecordsTransferCommands) {
    auto                 Texture  = MakeTexture("Texture");
    auto                 Readback = MakeReadback();
    const RHITextureView MipView{.Texture = Texture,
                                 .Access  = RHIAccess::Read,
                                 .Usage   = RHITextureUsage::TransferSrc,
                                 .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}};
    TestTransferPass     Pass;
    Pass.ReadbackTexture(MipView, 1, 0, Readback, 3, 2);
    Pass.BlitToSwapchain(MipView, 2, 3, 8, 4);

    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 2u);
    const auto* Copy = std::get_if<RHIReadbackTextureCmd>(&Commands[0]);
    ASSERT_NE(Copy, nullptr);
    EXPECT_EQ(Copy->Source.Texture.TryGet(), Texture.TryGet());
    EXPECT_EQ(Copy->Source.Range.BaseMipLevel, 2u);
    EXPECT_EQ(Copy->SrcX, 1u);
    EXPECT_EQ(Copy->SrcY, 0u);
    EXPECT_EQ(Copy->SrcWidth, 3u);
    EXPECT_EQ(Copy->SrcHeight, 2u);
    EXPECT_EQ(Copy->Target.TryGet(), Readback.TryGet());

    const auto* Blit = std::get_if<RHIBlitToSwapchainCmd>(&Commands[1]);
    ASSERT_NE(Blit, nullptr);
    EXPECT_EQ(Blit->Source.Texture.TryGet(), Texture.TryGet());
    EXPECT_EQ(Blit->Source.Range.BaseMipLevel, 2u);
    EXPECT_EQ(Blit->DstX, 2u);
    EXPECT_EQ(Blit->DstY, 3u);
    EXPECT_EQ(Blit->DstWidth, 8u);
    EXPECT_EQ(Blit->DstHeight, 4u);
}
