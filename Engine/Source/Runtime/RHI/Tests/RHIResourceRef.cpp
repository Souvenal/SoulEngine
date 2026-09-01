/// @file   RHIResourceRef.cpp
/// @brief  Tests for intrusive RHI resource references.

#include <gtest/gtest.h>

import RHI;
import std;

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

class MockVertexBuffer final : public RHIVertexBuffer {
  public:
    MockVertexBuffer(StringView Name, Uint32& DestructionCount)
        : RHIVertexBuffer(String(Name), RHIVertexBufferDesc{}), m_DestructionCount(DestructionCount) {}
    ~MockVertexBuffer() override {
        ++m_DestructionCount;
    }

  private:
    Uint32& m_DestructionCount;
};

class MockTransientConstantBuffer final : public RHITransientConstantBuffer {
  public:
    MockTransientConstantBuffer(String Name, Uint64 Size) : RHITransientConstantBuffer(std::move(Name), Size) {}
};

class MockTransientShaderStorageBuffer final : public RHITransientShaderStorageBuffer {
  public:
    MockTransientShaderStorageBuffer(String Name, Uint64 Size, RHITransientBufferUsage Usage)
        : RHITransientShaderStorageBuffer(std::move(Name), Size, Usage) {}
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

    [[nodiscard]] auto CreateVertexBuffer(StringView Name, const RHIVertexBufferDesc&)
        -> std::expected<RHIRef<RHIVertexBuffer>, ErrorMessage> override {
        auto Resource = RHIRef<RHIVertexBuffer>::Create();
        if (m_FailVertexBuffer) {
            Resource.MarkFailed(ErrorMessage("mock vertex-buffer creation failure"));
            return std::unexpected(ErrorMessage("mock vertex-buffer creation failure"));
        }
        auto Payload = Resource.m_Payload;
        if (auto Publish = Resource.Publish(
                UPtr<RHIVertexBuffer>{std::make_unique<MockVertexBuffer>(Name, m_VertexDestructions)},
                RHIRefState::GpuPending);
            !Publish) {
            return std::unexpected(Publish.error());
        }
        m_CompletionCallbacks.emplace_back([Payload] { (void)Payload->TryMarkReady(); });
        return Resource;
    }

    [[nodiscard]] auto CreateIndexBuffer(StringView, const RHIIndexBufferDesc&)
        -> std::expected<RHIRef<RHIIndexBuffer>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock index-buffer creation is not implemented"));
    }

    [[nodiscard]] auto CreateTransientConstantBuffer(StringView Name, const RHITransientConstantBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientConstantBuffer>, ErrorMessage> override {
        if (Desc.Data.empty())
            return std::unexpected(ErrorMessage("mock transient constant-buffer data is empty"));
        auto Resource = RHIRef<RHITransientConstantBuffer>::Create();
        auto Payload = std::make_unique<MockTransientConstantBuffer>(String(Name), Desc.Data.size_bytes());
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateTransientShaderStorageBuffer(
        StringView Name,
        const RHITransientShaderStorageBufferDesc& Desc)
        -> std::expected<RHIRef<RHITransientShaderStorageBuffer>, ErrorMessage> override {
        if (Desc.Data.empty())
            return std::unexpected(ErrorMessage("mock transient shader-storage data is empty"));
        auto Resource = RHIRef<RHITransientShaderStorageBuffer>::Create();
        auto Payload = std::make_unique<MockTransientShaderStorageBuffer>(
            String(Name), Desc.Data.size_bytes(), Desc.Usage);
        if (auto Publish = Resource.Publish(std::move(Payload), RHIRefState::Ready); !Publish)
            return std::unexpected(Publish.error());
        return Resource;
    }

    [[nodiscard]] auto CreateSampledTexture(StringView, const RHISampledTextureDesc&)
        -> std::expected<RHIRef<RHISampledTexture>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock sampled-texture creation is not implemented"));
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

    [[nodiscard]] auto CreateRenderTarget(StringView, const RHIRenderTargetDesc&)
        -> std::expected<RHIRef<RHIRenderTarget>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock render-target creation is not implemented"));
    }

    [[nodiscard]] auto CreateShaderBindingSet(StringView, const RHIShaderBindingSetDesc&)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock shader-binding-set creation is not implemented"));
    }

    [[nodiscard]] auto CreateGraphicsPipeline(StringView, const RHIGraphicsPipelineDesc&)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock graphics-pipeline creation is not implemented"));
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

    [[nodiscard]] auto Execute(RenderPassList&&) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto BeginFrame() -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto EndFrame() -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override {
        return 0;
    }
    auto WaitIdle() -> void override {}
    auto Shutdown() -> void override {
        m_DeletionQueue.Drain();
    }

    auto Tick() -> void override {
        if (!m_GpuComplete)
            return;
        auto Callbacks = std::move(m_CompletionCallbacks);
        for (auto& Complete : Callbacks)
            Complete();
    }

    bool   m_FailVertexBuffer    = false;
    bool   m_GpuComplete         = false;
    Uint32 m_VertexDestructions  = 0;
    Uint32 m_SamplerDestructions = 0;

  private:
    RHIDeferredDeletionQueue           m_DeletionQueue       = {};
    std::vector<std::function<void()>> m_CompletionCallbacks = {};
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
    using magic_enum::bitwise_operators::operator|;

    MockRenderDevice Device;
    const std::array<std::byte, 16> Data = {};

    auto Constant = Device.CreateTransientConstantBuffer(
        "Test/TransientConstant", RHITransientConstantBufferDesc{.Data = Data});
    ASSERT_TRUE(Constant.has_value());
    ASSERT_TRUE(*Constant);
    EXPECT_EQ((*Constant)->GetSize(), Data.size());

    auto Storage = Device.CreateTransientShaderStorageBuffer(
        "Test/TransientStorage",
        RHITransientShaderStorageBufferDesc{
            .Data       = Data,
            .Usage = RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead,
        });
    ASSERT_TRUE(Storage.has_value());
    ASSERT_TRUE(*Storage);
    EXPECT_EQ((*Storage)->GetSize(), Data.size());
    EXPECT_EQ((*Storage)->GetUsage(),
              RHITransientBufferUsage::ShaderRead | RHITransientBufferUsage::IndirectCommandRead);

    Constant = nullptr;
    Storage = nullptr;
    DrainRHIDeferredDeletions();
}

TEST(RHIResourceRefTest, BackendCreatePublishesGpuPendingThenReady) {
    MockRenderDevice Device;
    auto             Result = Device.CreateVertexBuffer("Test/VertexBuffer", RHIVertexBufferDesc{});

    ASSERT_TRUE(Result.has_value());
    auto Buffer = std::move(*Result);
    auto Copy   = Buffer;
    EXPECT_EQ(Copy.GetState(), RHIRefState::GpuPending);
    EXPECT_EQ(Copy.TryGet(), nullptr);

    Device.m_GpuComplete = true;
    Device.Tick();
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

    auto Result = Device.CreateVertexBuffer("Test/VertexBuffer", RHIVertexBufferDesc{});
    EXPECT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("mock vertex-buffer creation failure"), String::npos);
}

TEST(RHIResourceRefTest, CompletionCallbackKeepsPayloadAliveUntilGpuCompletion) {
    MockRenderDevice Device;
    auto             Result = Device.CreateVertexBuffer("Test/VertexBuffer", RHIVertexBufferDesc{});
    ASSERT_TRUE(Result.has_value());
    auto Buffer = std::move(*Result);
    auto Copy   = Buffer;

    Buffer = nullptr;
    Copy   = nullptr;
    Device.Tick();
    EXPECT_EQ(Device.m_VertexDestructions, 0);

    Device.m_GpuComplete = true;
    Device.Tick();
    EXPECT_EQ(Device.m_VertexDestructions, 0);
    DrainRHIDeferredDeletions();
    EXPECT_EQ(Device.m_VertexDestructions, 1);
}

TEST(RHIResourceRefTest, CompletionCannotOverwriteFailure) {
    MockRenderDevice Device;
    auto             Result = Device.CreateVertexBuffer("Test/VertexBuffer", RHIVertexBufferDesc{});
    ASSERT_TRUE(Result.has_value());
    auto Buffer = std::move(*Result);

    Buffer.MarkFailed(ErrorMessage("mock completion failure"));
    Device.m_GpuComplete = true;
    Device.Tick();

    EXPECT_EQ(Buffer.GetState(), RHIRefState::Failed);
    ASSERT_TRUE(Buffer.GetError().has_value());
    EXPECT_NE(Buffer.GetError()->ToString().find("mock completion failure"), String::npos);
}
