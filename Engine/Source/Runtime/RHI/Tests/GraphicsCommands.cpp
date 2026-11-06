/// @file   GraphicsCommands.cpp
/// @brief  Public graphics-command recording, view invariants, and lifetime coverage.

#include <gtest/gtest.h>

import RHI;
import std;
import Core;

using namespace SoulEngine;

static_assert(!std::is_default_constructible_v<RHIBufferView>);
static_assert(!std::is_default_constructible_v<RHITextureView>);

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
    using IRHIGraphicsPass::Draw;
    using IRHIGraphicsPass::DrawIndexed;
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

[[nodiscard]] auto MakeBufferView(RHIRef<RHIBuffer> Buffer,
                                  RHIBufferUsage    Usage,
                                  RHIAccess         Access = RHIAccess::Read,
                                  Uint64            Offset = 0,
                                  Uint64            Range  = kRHIWholeBuffer) -> RHIBufferView {
    auto View = RHIBufferView::Create(std::move(Buffer), Usage, Access, Offset, Range);
    EXPECT_TRUE(View.has_value());
    return std::move(*View);
}

[[nodiscard]] auto MakeTextureView(RHIRef<RHITexture>  Texture,
                                   RHITextureUsage     Usage,
                                   RHIAccess           Access = RHIAccess::Read,
                                   RHISubresourceRange Range  = {}) -> RHITextureView {
    auto View = RHITextureView::Create(std::move(Texture), Usage, Access, Range);
    EXPECT_TRUE(View.has_value());
    return std::move(*View);
}

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
    Uint32 VertexDestructions   = 0;
    Uint32 IndexDestructions    = 0;
    Uint32 IndirectDestructions = 0;
    auto   Vertex               = MakeBuffer("Vertex", 128, RHIBufferUsage::Vertex, &VertexDestructions);
    auto   Index                = MakeBuffer("Index", 64, RHIBufferUsage::Index, &IndexDestructions);
    auto   Indirect             = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect, &IndirectDestructions);
    std::optional<RHIBufferView> VertexView = MakeBufferView(Vertex, RHIBufferUsage::Vertex, RHIAccess::Read, 16, 96);
    std::optional<RHIBufferView> IndexView  = MakeBufferView(Index, RHIBufferUsage::Index, RHIAccess::Read, 4, 60);
    std::optional<RHIBufferView> IndirectView =
        MakeBufferView(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 20, 40);
    std::vector<RHIBufferView> VertexViews{*VertexView};

    TestGraphicsPass Pass;
    Pass.DrawIndexedIndirect(VertexViews, *IndexView, *IndirectView, 2);
    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 1u);
    const auto* Draw = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands.front());
    ASSERT_NE(Draw, nullptr);
    ASSERT_EQ(Draw->VertexBuffers.size(), 1u);
    EXPECT_EQ(Draw->VertexBuffers[0].GetBuffer().TryGet(), Vertex.TryGet());
    EXPECT_EQ(Draw->VertexBuffers[0].GetOffset(), 16u);
    EXPECT_EQ(Draw->VertexBuffers[0].GetRange(), 96u);
    EXPECT_EQ(Draw->IndexBuffer.GetBuffer().TryGet(), Index.TryGet());
    EXPECT_EQ(Draw->IndexBuffer.GetOffset(), 4u);
    EXPECT_EQ(Draw->IndirectBuffer.GetBuffer().TryGet(), Indirect.TryGet());
    EXPECT_EQ(Draw->IndirectBuffer.GetOffset(), 20u);
    EXPECT_EQ(Draw->DrawCount, 2u);

    VertexView.reset();
    IndexView.reset();
    IndirectView.reset();
    VertexViews.clear();
    EXPECT_EQ(Draw->VertexBuffers[0].GetOffset(), 16u);
    EXPECT_EQ(Draw->VertexBuffers[0].GetRange(), 96u);
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
    Uint32                       Destructions = 0;
    auto                         Buffer       = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect, &Destructions);
    std::optional<RHIBufferView> View = MakeBufferView(Buffer, RHIBufferUsage::Indirect, RHIAccess::Read, 20, 40);
    TestGraphicsPass             Pass;
    Pass.DrawIndirect({}, *View, 2);
    ASSERT_EQ(Pass.GetCommands().size(), 1u);
    const auto* Draw = std::get_if<RHIDrawIndirectCmd>(&Pass.GetCommands().front());
    ASSERT_NE(Draw, nullptr);
    EXPECT_EQ(Draw->IndirectBuffer.GetBuffer().TryGet(), Buffer.TryGet());
    EXPECT_EQ(Draw->IndirectBuffer.GetOffset(), 20u);
    EXPECT_EQ(Draw->DrawCount, 2u);
    View.reset();
    Buffer = nullptr;
    m_DeletionQueue.Drain();
    EXPECT_EQ(Destructions, 0u);
    ASSERT_TRUE(Pass.Record());
    m_DeletionQueue.Drain();
    EXPECT_EQ(Destructions, 1u);
}

TEST_F(GraphicsCommandTest, RecordsEmptyAndRuntimeSizedVertexStreams) {
    auto                             Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect);
    auto                             Index    = MakeBuffer("Index", 64, RHIBufferUsage::Index);
    std::vector<RHIBufferView>       Vertices;
    std::array<RHIRef<RHIBuffer>, 5> Buffers;
    Vertices.reserve(Buffers.size());
    for (Uint32 I = 0; I < Buffers.size(); ++I) {
        Buffers[I] = MakeBuffer("Vertex", 32, RHIBufferUsage::Vertex);
        Vertices.push_back(MakeBufferView(Buffers[I], RHIBufferUsage::Vertex, RHIAccess::Read, 0, 32));
    }
    TestGraphicsPass Pass;
    Pass.DrawIndexedIndirect({},
                             MakeBufferView(Index, RHIBufferUsage::Index, RHIAccess::Read, 0, 64),
                             MakeBufferView(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 0, 20));
    Pass.DrawIndexedIndirect(Vertices,
                             MakeBufferView(Index, RHIBufferUsage::Index, RHIAccess::Read, 0, 64),
                             MakeBufferView(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 0, 20));
    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 2u);
    const auto* Empty    = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[0]);
    const auto* Recorded = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[1]);
    ASSERT_NE(Empty, nullptr);
    ASSERT_NE(Recorded, nullptr);
    EXPECT_TRUE(Empty->VertexBuffers.empty());
    ASSERT_EQ(Recorded->VertexBuffers.size(), Buffers.size());
    for (std::size_t I = 0; I < Recorded->VertexBuffers.size(); ++I)
        EXPECT_EQ(Recorded->VertexBuffers[I].GetBuffer().TryGet(), Buffers[I].TryGet());
}

TEST_F(GraphicsCommandTest, RecordsTransferCommands) {
    auto             Texture  = MakeTexture("Texture");
    auto             Readback = MakeReadback();
    const auto       MipView  = MakeTextureView(Texture,
                                                RHITextureUsage::TransferSrc,
                                                RHIAccess::Read,
                                                RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1});
    TestTransferPass Pass;
    Pass.ReadbackTexture(MipView, 1, 0, Readback, 3, 2);
    Pass.BlitToSwapchain(MipView, 2, 3, 8, 4);

    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 2u);
    const auto* Copy = std::get_if<RHIReadbackTextureCmd>(&Commands[0]);
    ASSERT_NE(Copy, nullptr);
    EXPECT_EQ(Copy->Source.GetTexture().TryGet(), Texture.TryGet());
    EXPECT_EQ(Copy->Source.GetRange().BaseMipLevel, 2u);
    EXPECT_EQ(Copy->SrcX, 1u);
    EXPECT_EQ(Copy->SrcY, 0u);
    EXPECT_EQ(Copy->SrcWidth, 3u);
    EXPECT_EQ(Copy->SrcHeight, 2u);
    EXPECT_EQ(Copy->Target.TryGet(), Readback.TryGet());

    const auto* Blit = std::get_if<RHIBlitToSwapchainCmd>(&Commands[1]);
    ASSERT_NE(Blit, nullptr);
    EXPECT_EQ(Blit->Source.GetTexture().TryGet(), Texture.TryGet());
    EXPECT_EQ(Blit->Source.GetRange().BaseMipLevel, 2u);
    EXPECT_EQ(Blit->DstX, 2u);
    EXPECT_EQ(Blit->DstY, 3u);
    EXPECT_EQ(Blit->DstWidth, 8u);
    EXPECT_EQ(Blit->DstHeight, 4u);
}

TEST_F(GraphicsCommandTest, RecordsCompleteDirectAndIndirectDrawInputs) {
    auto             Vertex   = MakeBuffer("Vertex", 128, RHIBufferUsage::Vertex);
    auto             Index    = MakeBuffer("Index", 64, RHIBufferUsage::Index);
    auto             Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect);
    const std::array VertexViews{MakeBufferView(Vertex, RHIBufferUsage::Vertex, RHIAccess::Read, 16, 96)};

    TestGraphicsPass Pass;
    Pass.Draw(VertexViews, 3, 2, 4, 1);
    Pass.DrawIndexed(VertexViews, MakeBufferView(Index, RHIBufferUsage::Index, RHIAccess::Read, 4, 60), 6, 2, 1, -4, 2);
    Pass.DrawIndirect(VertexViews, MakeBufferView(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 20, 40), 2);

    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 3u);
    const auto* Draw = std::get_if<RHIDrawCmd>(&Commands[0]);
    ASSERT_NE(Draw, nullptr);
    ASSERT_EQ(Draw->VertexBuffers.size(), 1u);
    EXPECT_EQ(Draw->VertexBuffers[0].GetBuffer().TryGet(), Vertex.TryGet());
    EXPECT_EQ(Draw->VertexCount, 3u);
    EXPECT_EQ(Draw->InstanceCount, 2u);
    EXPECT_EQ(Draw->FirstVertex, 4u);
    EXPECT_EQ(Draw->FirstInstance, 1u);

    const auto* Indexed = std::get_if<RHIDrawIndexedCmd>(&Commands[1]);
    ASSERT_NE(Indexed, nullptr);
    ASSERT_EQ(Indexed->VertexBuffers.size(), 1u);
    EXPECT_EQ(Indexed->IndexBuffer.GetBuffer().TryGet(), Index.TryGet());
    EXPECT_EQ(Indexed->IndexCount, 6u);
    EXPECT_EQ(Indexed->InstanceCount, 2u);
    EXPECT_EQ(Indexed->FirstIndex, 1u);
    EXPECT_EQ(Indexed->VertexOffset, -4);
    EXPECT_EQ(Indexed->FirstInstance, 2u);

    const auto* IndirectDraw = std::get_if<RHIDrawIndirectCmd>(&Commands[2]);
    ASSERT_NE(IndirectDraw, nullptr);
    ASSERT_EQ(IndirectDraw->VertexBuffers.size(), 1u);
    EXPECT_EQ(IndirectDraw->VertexBuffers[0].GetBuffer().TryGet(), Vertex.TryGet());
    EXPECT_EQ(IndirectDraw->IndirectBuffer.GetBuffer().TryGet(), Indirect.TryGet());
    EXPECT_EQ(IndirectDraw->DrawCount, 2u);
}

TEST_F(GraphicsCommandTest, ViewCreationValidatesResourceContracts) {
    auto Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Storage | RHIBufferUsage::Indirect);
    EXPECT_TRUE(RHIBufferView::Create(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read));
    EXPECT_FALSE(RHIBufferView::Create(Indirect, RHIBufferUsage::Index, RHIAccess::Read));
    EXPECT_FALSE(RHIBufferView::Create(Indirect, RHIBufferUsage::Storage | RHIBufferUsage::Indirect, RHIAccess::Read));
    EXPECT_FALSE(RHIBufferView::Create(Indirect, RHIBufferUsage::Indirect, RHIAccess::None));
    EXPECT_FALSE(RHIBufferView::Create(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 101));
    EXPECT_FALSE(RHIBufferView::Create(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 80, 40));
    EXPECT_FALSE(RHIBufferView::Create(Indirect, RHIBufferUsage::Indirect, RHIAccess::Read, 100));
    EXPECT_FALSE(RHIBufferView::Create({}, RHIBufferUsage::Indirect, RHIAccess::Read));

    auto Texture = MakeTexture("Texture");
    EXPECT_FALSE(RHITextureView::Create(Texture, RHITextureUsage::Sampled, RHIAccess::Read));
    EXPECT_FALSE(
        RHITextureView::Create(Texture, RHITextureUsage::Sampled | RHITextureUsage::TransferSrc, RHIAccess::Read));
    EXPECT_FALSE(RHITextureView::Create(Texture, RHITextureUsage::TransferSrc, RHIAccess::None));
    EXPECT_FALSE(RHITextureView::Create(Texture,
                                        RHITextureUsage::TransferSrc,
                                        RHIAccess::Read,
                                        RHISubresourceRange{.BaseMipLevel = 4, .MipLevelCount = 1}));
    EXPECT_FALSE(RHITextureView::Create(Texture,
                                        RHITextureUsage::TransferSrc,
                                        RHIAccess::Read,
                                        RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 3}));
}
