/// @file   RHIResourceRef.cpp
/// @brief  Tests for intrusive RHI resource references.

#include <gtest/gtest.h>

import RHI;
import std;
import Core;
import magic_enum;
import WindowSystem;

using namespace SoulEngine;

namespace {

class MockSampler final : public RHISampler {
  public:
    MockSampler(StringView Name, Uint32& DestructionCount)
        : RHISampler(String(Name), RHISamplerDesc{}), m_DestructionCount(DestructionCount) {}
    ~MockSampler() override {
        ++m_DestructionCount;
    }

  private:
    Uint32& m_DestructionCount;
};

class MockVertexBuffer final : public RHIBuffer {
  public:
    MockVertexBuffer(StringView Name, Uint32& DestructionCount)
        : RHIBuffer(String(Name), 0, RHIBufferUsage::Vertex, RHIMemoryUsage::GpuOnly, RHILifetime::Persistent), m_DestructionCount(DestructionCount) {}
    ~MockVertexBuffer() override {
        ++m_DestructionCount;
    }

  private:
    Uint32& m_DestructionCount;
};

class MockTransientShaderStorageBuffer final : public RHIBuffer {
  public:
    MockTransientShaderStorageBuffer(String Name, Uint64 Size, RHIBufferUsage Usage)
        : RHIBuffer(std::move(Name), Size, Usage, RHIMemoryUsage::CpuToGpu, RHILifetime::Transient) {}
};

TEST(RHIRefArrayTest, ReservesNullDescriptorSlotZero) {
    RHIDeferredDeletionQueue DeletionQueue;
    GDeferredDeletionQueue = &DeletionQueue;

    {
        RHIRefArray<int> Resources;
        EXPECT_EQ(Resources.GetSize(), 1U);
        EXPECT_FALSE(Resources.GetElement(RHIRefArray<int>::NullDescriptorSlot));

        auto Resource = RHIRef<int>::Create();
        ASSERT_TRUE(Resource.Publish(std::make_unique<int>(42), RHIRefState::Ready).has_value());
        ASSERT_TRUE(Resources.Append(Resource).has_value());

        const auto Index = Resources.FindIndex(Resource);
        ASSERT_TRUE(Index.has_value());
        EXPECT_EQ(*Index, RHIRefArray<int>::FirstResourceSlot);
        EXPECT_EQ(Resources.GetSize(), 2U);
        EXPECT_TRUE(Resources.GetElement(RHIRefArray<int>::FirstResourceSlot));

        const auto Changed = Resources.GetChangedElements();
        ASSERT_EQ(Changed.size(), 1U);
        EXPECT_EQ(Changed[0].first, RHIRefArray<int>::FirstResourceSlot);
        EXPECT_EQ(Changed[0].second, Resource);

        Resource = nullptr;
    }
    DeletionQueue.Drain();
    GDeferredDeletionQueue = nullptr;
}

class MockRenderDevice final : public RHIRenderDevice {
  public:
    MockRenderDevice() {
        GDeferredDeletionQueue = &m_DeletionQueue;
    }
    ~MockRenderDevice() override {
        GDeferredDeletionQueue = nullptr;
    }

    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override {
        return RHIBackendType::Unknown;
    }

    [[nodiscard]] auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> override {
        if (Desc.Lifetime == RHILifetime::Transient) {
            if (Desc.SizeBytes == 0)
                return std::unexpected(ErrorMessage("mock transient shader-storage size must be greater than zero"));
            if (Desc.Usage == RHIBufferUsage::Unknown)
                return std::unexpected(ErrorMessage("mock transient shader-storage usage must not be unknown"));
            auto Resource = RHIRef<RHIBuffer>::Create();
            auto Payload  = std::make_unique<MockTransientShaderStorageBuffer>(
                String(Name), Desc.SizeBytes, Desc.Usage);
            if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
                return std::unexpected(Publish.error());
            return Resource;
        }
        auto Resource = RHIRef<RHIBuffer>::Create();
        if (m_FailVertexBuffer) {
            Resource.MarkFailed(ErrorMessage("mock buffer creation failure"));
            return std::unexpected(ErrorMessage("mock buffer creation failure"));
        }
        if (auto Publish = Resource.Publish(
                UPtr<RHIBuffer>{std::make_unique<MockVertexBuffer>(Name, m_VertexDestructions)},
                RHIRefState::Ready);
            !Publish) {
            return std::unexpected(Publish.error());
        }
        return Resource;
    }

    [[nodiscard]] auto UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> override {
        const auto* Payload = Buffer.TryGet();
        if (!Payload)
            return std::unexpected(ErrorMessage("mock upload: buffer ref is not Ready"));
        if (Data.size_bytes() != Payload->GetSize())
            return std::unexpected(ErrorMessage("mock upload: data size does not match buffer size"));
        m_LastUpload.assign(Data.begin(), Data.end());
        return {};
    }

    [[nodiscard]] auto CreateReadback(StringView, Uint64)
        -> std::expected<RHIRef<RHIReadback>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock readback creation is not implemented"));
    }

    [[nodiscard]] auto CreateTexture(StringView, const RHITextureDesc&)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock texture creation is not implemented"));
    }

    [[nodiscard]] auto UploadTexture(RHIRef<RHITexture>, std::span<const std::byte>)
        -> std::expected<void, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock texture upload is not implemented"));
    }

    [[nodiscard]] auto CreateSampler(StringView Name, const RHISamplerDesc&)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage> override {
        auto Resource = RHIRef<RHISampler>::Create();
        if (auto Publish = Resource.Publish(
                UPtr<RHISampler>{std::make_unique<MockSampler>(Name, m_SamplerDestructions)}, RHIRefState::Ready);
            !Publish) {
            return std::unexpected(Publish.error());
        }
        return Resource;
    }

    [[nodiscard]] auto CreateShaderBindingSet(StringView, const RHIShaderBindingSetDesc&)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock shader-binding-set creation is not implemented"));
    }

    [[nodiscard]] auto CreateGraphicsPipeline(StringView, const RHIGraphicsPipelineDesc&)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock graphics-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateComputePipeline(StringView, const RHIComputePipelineDesc&)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock compute-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateRayTracingPipeline(StringView, const RHIRayTracingPipelineDesc&)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock ray-tracing-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateBottomLevelAccelerationStructure(StringView,
                                                              const RHIBottomLevelAccelerationStructureDesc&)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock BLAS creation is not implemented"));
    }

    [[nodiscard]] auto CreateTopLevelAccelerationStructure(StringView, const RHITopLevelAccelerationStructureDesc&)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock TLAS creation is not implemented"));
    }

    [[nodiscard]] auto Execute(RenderResult&) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto EndFrame() -> std::expected<RHIFrameCompletion, ErrorMessage> override {
        return RHIFrameCompletion{};
    }
    [[nodiscard]] auto WaitFinish(const RHIFrameCompletion&) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override {
        return 0;
    }
    [[nodiscard]] auto WaitIdle() -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto Shutdown() -> std::expected<void, ErrorMessage> override {
        m_DeletionQueue.Drain();
        return {};
    }

    [[nodiscard]] auto Tick() -> std::expected<void, ErrorMessage> override {
        return {};
    }

    bool   m_FailVertexBuffer    = false;
    Uint32                m_VertexDestructions  = 0;
    Uint32                m_SamplerDestructions = 0;
    std::vector<std::byte> m_LastUpload         = {};

  private:
    RHIDeferredDeletionQueue           m_DeletionQueue       = {};
};

} // namespace

TEST(RHIResourceRefTest, DefaultConstructedRefIsEmpty) {
    RHIRef<RHISampler> Ref = {};

    EXPECT_FALSE(Ref);
    EXPECT_EQ(Ref.GetState(), RHIRefState::Unknown);
    EXPECT_EQ(Ref.TryGet(), nullptr);
    EXPECT_FALSE(Ref.GetError().has_value());
}
TEST(RHIResourceRefTest, CopiesObserveTheSamePublishedState) {
    RHIDeferredDeletionQueue Queue;
    GDeferredDeletionQueue = &Queue;
    {
        auto Ref  = RHIRef<RHISampler>::Create();
        auto Copy = Ref;

        EXPECT_EQ(Ref.GetState(), RHIRefState::RhiCommitting);
        EXPECT_EQ(Copy.GetState(), RHIRefState::RhiCommitting);
        EXPECT_FALSE(Copy);

        Ref.MarkFailed(ErrorMessage("enqueue failure"));
        EXPECT_EQ(Copy.GetState(), RHIRefState::Failed);
        ASSERT_TRUE(Copy.GetError().has_value());
        EXPECT_NE(Copy.GetError()->ToString().find("enqueue failure"), String::npos);
    }
    Queue.Drain();
    GDeferredDeletionQueue = nullptr;
}

TEST(RHIResourceRefTest, ReadyPayloadCanBePublishedToPendingCopies) {
    RHIDeferredDeletionQueue Queue;
    GDeferredDeletionQueue = &Queue;
    {
        auto Pending = RHIRef<RHISampler>::Create();
        auto Ready   = RHIRef<RHISampler>::Create();
        Uint32 Destructions = 0;
        auto   Payload      = std::make_unique<MockSampler>("Test/Sampler", Destructions);
        ASSERT_TRUE(Ready.m_Payload->Publish(std::move(Payload), RHIRefState::Ready));

        ASSERT_TRUE(Pending.Publish(std::move(Ready), RHIRefState::Ready));
        EXPECT_EQ(Pending.GetState(), RHIRefState::Ready);
        EXPECT_NE(Pending.TryGet(), nullptr);
    }
    Queue.Drain();
    GDeferredDeletionQueue = nullptr;
}

TEST(RHIResourceRefTest, SynchronousCreateReturnsReadySamplerRef) {
    MockRenderDevice Device;
    {
        auto Sampler = static_cast<RHIRenderDevice&>(Device).CreateSampler("Test/Sampler", RHISamplerDesc{});

        ASSERT_TRUE(Sampler.has_value());
        EXPECT_EQ(Sampler->GetState(), RHIRefState::Ready);
        EXPECT_NE(Sampler->TryGet(), nullptr);
    }
    DrainRHIDeferredDeletions();
    EXPECT_EQ(Device.m_SamplerDestructions, 1);
}

TEST(RHIResourceRefTest, TransientCreateReturnsReadyRefAndPreservesMetadata) {
    MockRenderDevice Device;
    const std::array<std::byte, 16> Data = {};

    auto Constant = Device.CreateBuffer(
        "Test/TransientConstant",
        RHIBufferDesc{
            .SizeBytes = Data.size(),
            .Usage     = RHIBufferUsage::Constant,
            .MemoryUsage = RHIMemoryUsage::CpuToGpu,
            .Lifetime    = RHILifetime::Transient,
        });
    ASSERT_TRUE(Constant.has_value());
    ASSERT_TRUE(*Constant);
    EXPECT_EQ((*Constant)->GetSize(), Data.size());

    // Bytes flow through UploadBuffer, never through the Desc.
    auto Upload = Device.UploadBuffer(*Constant, std::span<const std::byte>{Data});
    ASSERT_TRUE(Upload.has_value());
    EXPECT_EQ(Device.m_LastUpload.size(), Data.size());

    auto Storage = Device.CreateBuffer(
        "Test/TransientStorage",
        RHIBufferDesc{
            .SizeBytes = Data.size(),
            .Usage     = RHIBufferUsage::Storage | RHIBufferUsage::Indirect,
            .MemoryUsage = RHIMemoryUsage::CpuToGpu,
            .Lifetime    = RHILifetime::Transient,
        });
    ASSERT_TRUE(Storage.has_value());
    ASSERT_TRUE(*Storage);
    EXPECT_EQ((*Storage)->GetSize(), Data.size());
    EXPECT_EQ((*Storage)->GetUsage(),
              RHIBufferUsage::Storage | RHIBufferUsage::Indirect);

    auto Scratch = Device.CreateBuffer(
        "Test/TransientScratch",
        RHIBufferDesc{
            .SizeBytes = 64,
            .Usage     = RHIBufferUsage::Storage,
            .MemoryUsage = RHIMemoryUsage::CpuToGpu,
            .Lifetime    = RHILifetime::Transient,
        });
    ASSERT_TRUE(Scratch.has_value());
    ASSERT_TRUE(*Scratch);
    EXPECT_EQ((*Scratch)->GetSize(), 64u);

    Constant = nullptr;
    Storage = nullptr;
    Scratch = nullptr;
    DrainRHIDeferredDeletions();
}

TEST(RHIResourceRefTest, BackendCreatePublishesReady) {
    MockRenderDevice Device;
    auto             Result = Device.CreateBuffer("Test/VertexBuffer", RHIBufferDesc{});

    ASSERT_TRUE(Result.has_value());
    auto Buffer = std::move(*Result);
    auto Copy   = Buffer;
    EXPECT_EQ(Copy.GetState(), RHIRefState::Ready);
    EXPECT_TRUE(Copy);
    EXPECT_NE(Copy.TryGet(), nullptr);
    EXPECT_EQ(Copy->GetName(), "Test/VertexBuffer");

    Buffer = nullptr;
    Copy   = nullptr;
    DrainRHIDeferredDeletions();
    EXPECT_EQ(Device.m_VertexDestructions, 1);
}

TEST(RHIResourceRefTest, BackendCreationFailureReturnsError) {
    MockRenderDevice Device;
    Device.m_FailVertexBuffer = true;

    auto Result = Device.CreateBuffer("Test/VertexBuffer", RHIBufferDesc{});
    EXPECT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("mock buffer creation failure"), String::npos);
}

