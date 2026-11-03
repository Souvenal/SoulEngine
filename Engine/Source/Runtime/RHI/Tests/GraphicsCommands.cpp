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

    [[nodiscard]] auto MakeBuffer(StringView Name, Uint64 Size, RHIBufferUsage Usage, RHIMemoryUsage MemoryUsage)
        -> RHIRef<RHIBuffer> {
        auto Result = RHIRef<RHIBuffer>::Create();
        auto Published =
            Result.Publish(UPtr<MockCommandBuffer>(std::make_unique<MockCommandBuffer>(Name, Size, Usage, MemoryUsage)),
                           RHIRefState::Ready);
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
    ASSERT_TRUE(Pass.DrawIndexedIndirect(IndirectView, IndexView, VertexViews, 2));
    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 1u);
    const auto* Draw = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands.front());
    ASSERT_NE(Draw, nullptr);
    EXPECT_EQ(Draw->VertexBufferCount, 1u);
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
    Vertex       = nullptr;
    Index        = nullptr;
    Indirect     = nullptr;
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
    ASSERT_TRUE(Pass.DrawIndirect(View, 2));
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

TEST_F(GraphicsCommandTest, RejectsInvalidIndexedAndIndirectViews) {
    auto                Indirect = MakeBuffer("Indirect", 100, RHIBufferUsage::Indirect);
    auto                Index    = MakeBuffer("Index", 64, RHIBufferUsage::Index);
    auto                Vertex   = MakeBuffer("Vertex", 64, RHIBufferUsage::Vertex);
    auto                Storage  = MakeBuffer("Storage", 128, RHIBufferUsage::Storage);
    const RHIBufferView GoodIndirect{
        .Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Offset = 0, .Range = 100};
    const RHIBufferView GoodIndex{
        .Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Offset = 0, .Range = 64};
    const RHIBufferView GoodVertex{
        .Buffer = Vertex, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Vertex, .Offset = 0, .Range = 64};
    const auto RejectIndexed = [&](RHIBufferView                  IndirectView,
                                   RHIBufferView                  IndexView,
                                   std::span<const RHIBufferView> VertexViews,
                                   Uint32                         DrawCount = 1) -> void {
        TestGraphicsPass Pass;
        EXPECT_FALSE(Pass.DrawIndexedIndirect(IndirectView, IndexView, VertexViews, DrawCount));
        EXPECT_TRUE(Pass.GetCommands().empty());
    };
    const std::array VertexViews{GoodVertex};
    RejectIndexed({}, GoodIndex, VertexViews);
    RejectIndexed(GoodIndirect, {}, VertexViews);
    RejectIndexed(GoodIndirect, GoodIndex, VertexViews, 0);
    RejectIndexed(GoodIndirect, GoodIndex, VertexViews, 6);
    const std::array<RHIBufferView, 5> TooManyVertices{};
    RejectIndexed(GoodIndirect, GoodIndex, TooManyVertices);
    const std::array InvalidVertex{RHIBufferView{}};
    RejectIndexed(GoodIndirect, GoodIndex, InvalidVertex);
    RejectIndexed(
        GoodIndirect,
        RHIBufferView{.Buffer = Index, .Access = RHIAccess::Write, .Usage = RHIBufferUsage::Index, .Range = 64},
        VertexViews);
    RejectIndexed(
        GoodIndirect,
        RHIBufferView{.Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Vertex, .Range = 64},
        VertexViews);
    RejectIndexed(
        GoodIndirect,
        RHIBufferView{
            .Buffer = Index, .Access = RHIAccess::Read | RHIAccess::Write, .Usage = RHIBufferUsage::Index, .Range = 64},
        VertexViews);
    RejectIndexed(
        GoodIndirect,
        RHIBufferView{.Buffer = Storage, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Range = 64},
        VertexViews);
    RejectIndexed(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Storage, .Range = 100},
        GoodIndex,
        VertexViews);
    RejectIndexed(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Write, .Usage = RHIBufferUsage::Indirect, .Range = 100},
        GoodIndex,
        VertexViews);
    RejectIndexed(
        RHIBufferView{
            .Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Offset = 2, .Range = 96},
        GoodIndex,
        VertexViews);
    RejectIndexed(RHIBufferView{.Buffer = Indirect,
                                .Access = RHIAccess::Read,
                                .Usage  = RHIBufferUsage::Indirect,
                                .Offset = 80,
                                .Range  = 40},
                  GoodIndex,
                  VertexViews);
    RejectIndexed(
        GoodIndirect,
        RHIBufferView{
            .Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Offset = 2, .Range = 62},
        VertexViews);
    RejectIndexed(
        GoodIndirect,
        RHIBufferView{
            .Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Offset = 64, .Range = 4},
        VertexViews);
    RejectIndexed(GoodIndirect,
                  GoodIndex,
                  std::array{RHIBufferView{.Buffer = Vertex,
                                           .Access = RHIAccess::Read,
                                           .Usage  = RHIBufferUsage::Vertex,
                                           .Offset = std::numeric_limits<Uint64>::max() - 3,
                                           .Range  = 8}});

    const auto RejectIndirect = [&](RHIBufferView View, Uint32 Count) -> void {
        TestGraphicsPass Pass;
        EXPECT_FALSE(Pass.DrawIndirect(View, Count));
        EXPECT_TRUE(Pass.GetCommands().empty());
    };
    RejectIndirect({}, 1);
    RejectIndirect(GoodIndirect, 0);
    RejectIndirect(GoodIndirect, 7);
    RejectIndirect(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Write, .Usage = RHIBufferUsage::Indirect, .Range = 100},
        1);
    RejectIndirect(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Storage, .Range = 100},
        1);
    RejectIndirect(RHIBufferView{.Buffer = Indirect,
                                 .Access = RHIAccess::Read | RHIAccess::Write,
                                 .Usage  = RHIBufferUsage::Indirect,
                                 .Range  = 100},
                   1);
    RejectIndirect(
        RHIBufferView{.Buffer = Storage, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Range = 100},
        1);
    RejectIndirect(
        RHIBufferView{
            .Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Offset = 2, .Range = 16},
        1);
    RejectIndirect(
        RHIBufferView{
            .Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Offset = 4, .Range = 12},
        1);
    RejectIndirect(RHIBufferView{.Buffer = Indirect,
                                 .Access = RHIAccess::Read,
                                 .Usage  = RHIBufferUsage::Indirect,
                                 .Offset = 80,
                                 .Range  = 40},
                   2);
}

TEST_F(GraphicsCommandTest, AllowsEmptyVertexViewsAndRecordsFourStreams) {
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
    ASSERT_TRUE(Pass.DrawIndexedIndirect(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Range = 20},
        RHIBufferView{.Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Range = 64}));
    ASSERT_TRUE(Pass.DrawIndexedIndirect(
        RHIBufferView{.Buffer = Indirect, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Indirect, .Range = 20},
        RHIBufferView{.Buffer = Index, .Access = RHIAccess::Read, .Usage = RHIBufferUsage::Index, .Range = 64},
        Vertices));
    const auto Commands = Pass.GetCommands();
    ASSERT_EQ(Commands.size(), 2u);
    const auto* Empty = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[0]);
    const auto* Four  = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[1]);
    ASSERT_NE(Empty, nullptr);
    ASSERT_NE(Four, nullptr);
    EXPECT_EQ(Empty->VertexBufferCount, 0u);
    EXPECT_EQ(Four->VertexBufferCount, 4u);
    for (Uint32 I = 0; I < Four->VertexBufferCount; ++I)
        EXPECT_EQ(Four->VertexBuffers[I].Buffer.TryGet(), Buffers[I].TryGet());
}

TEST_F(GraphicsCommandTest, RecordsReadbackFromSelectedTextureMipAndRejectsInvalidRanges) {
    auto                 Texture  = MakeTexture("Texture");
    auto                 Readback = MakeReadback();
    const RHITextureView MipView{.Texture = Texture,
                                 .Access  = RHIAccess::Read,
                                 .Usage   = RHITextureUsage::TransferSrc,
                                 .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}};
    TestTransferPass     Pass;
    ASSERT_TRUE(Pass.ReadbackTexture(MipView, 1, 0, Readback, 3, 2));
    ASSERT_EQ(Pass.GetCommands().size(), 1u);
    const auto* Copy = std::get_if<RHIReadbackTextureCmd>(&Pass.GetCommands().front());
    ASSERT_NE(Copy, nullptr);
    EXPECT_EQ(Copy->Source.Texture.TryGet(), Texture.TryGet());
    EXPECT_EQ(Copy->Source.Range.BaseMipLevel, 2u);
    EXPECT_EQ(Copy->SrcX, 1u);
    EXPECT_EQ(Copy->SrcY, 0u);
    EXPECT_EQ(Copy->SrcWidth, 3u);
    EXPECT_EQ(Copy->SrcHeight, 2u);
    EXPECT_EQ(Copy->Target.TryGet(), Readback.TryGet());

    const auto Reject = [&](RHITextureView View, Uint32 X, Uint32 Y, Uint32 Width, Uint32 Height) -> void {
        TestTransferPass Invalid;
        EXPECT_FALSE(Invalid.ReadbackTexture(View, X, Y, Readback, Width, Height));
        EXPECT_TRUE(Invalid.GetCommands().empty());
    };
    Reject({}, 0, 0, 1, 1);
    Reject(MipView, 0, 0, 0, 1);
    Reject(MipView, 0, 0, 1, 0);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Write,
                          .Usage   = RHITextureUsage::TransferSrc,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}},
           0,
           0,
           1,
           1);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Read,
                          .Usage   = RHITextureUsage::Sampled,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}},
           0,
           0,
           1,
           1);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Read,
                          .Usage   = RHITextureUsage::TransferSrc,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 4, .MipLevelCount = 1}},
           0,
           0,
           1,
           1);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Read,
                          .Usage   = RHITextureUsage::TransferSrc,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 2}},
           0,
           0,
           1,
           1);
    Reject(MipView, 3, 0, 2, 1);
    Reject(MipView, 0, 2, 1, 1);
    Reject(MipView, std::numeric_limits<Uint32>::max(), 0, 2, 1);
}

TEST_F(GraphicsCommandTest, RecordsBlitToSwapchainAndRejectsInvalidViews) {
    auto                 Texture = MakeTexture("Texture");
    const RHITextureView MipView{.Texture = Texture,
                                 .Access  = RHIAccess::Read,
                                 .Usage   = RHITextureUsage::TransferSrc,
                                 .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}};
    TestTransferPass     Pass;
    ASSERT_TRUE(Pass.BlitToSwapchain(MipView, 2, 3, 8, 4));
    ASSERT_EQ(Pass.GetCommands().size(), 1u);
    const auto* Blit = std::get_if<RHIBlitToSwapchainCmd>(&Pass.GetCommands().front());
    ASSERT_NE(Blit, nullptr);
    EXPECT_EQ(Blit->Source.Texture.TryGet(), Texture.TryGet());
    EXPECT_EQ(Blit->Source.Range.BaseMipLevel, 2u);
    EXPECT_EQ(Blit->DstX, 2u);
    EXPECT_EQ(Blit->DstY, 3u);
    EXPECT_EQ(Blit->DstWidth, 8u);
    EXPECT_EQ(Blit->DstHeight, 4u);

    const auto Reject = [&](RHITextureView View, Uint32 Width, Uint32 Height) -> void {
        TestTransferPass Invalid;
        EXPECT_FALSE(Invalid.BlitToSwapchain(View, 0, 0, Width, Height));
        EXPECT_TRUE(Invalid.GetCommands().empty());
    };
    Reject({}, 1, 1);
    Reject(MipView, 0, 1);
    Reject(MipView, 1, 0);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Write,
                          .Usage   = RHITextureUsage::TransferSrc,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}},
           1,
           1);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Read,
                          .Usage   = RHITextureUsage::Sampled,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 1}},
           1,
           1);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Read,
                          .Usage   = RHITextureUsage::TransferSrc,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 4, .MipLevelCount = 1}},
           1,
           1);
    Reject(RHITextureView{.Texture = Texture,
                          .Access  = RHIAccess::Read,
                          .Usage   = RHITextureUsage::TransferSrc,
                          .Range   = RHISubresourceRange{.BaseMipLevel = 2, .MipLevelCount = 2}},
           1,
           1);
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
    const auto CheckGeometryUpload = [&](RHIBufferUsage Usage, vk::AccessFlagBits2 DirectAccess) -> void {
        const auto Access = ToVkBufferUploadAccess(Usage);
        EXPECT_TRUE(static_cast<bool>(Access & DirectAccess));
        EXPECT_EQ(static_cast<bool>(Access & vk::AccessFlagBits2::eAccelerationStructureReadKHR), RtAvailable);
    };
    CheckGeometryUpload(RHIBufferUsage::Vertex, vk::AccessFlagBits2::eVertexAttributeRead);
    CheckGeometryUpload(RHIBufferUsage::Index, vk::AccessFlagBits2::eIndexRead);

    const auto StorageAccess = ToVkBufferUploadAccess(RHIBufferUsage::Storage);
    EXPECT_FALSE(static_cast<bool>(StorageAccess & vk::AccessFlagBits2::eAccelerationStructureReadKHR));
}
