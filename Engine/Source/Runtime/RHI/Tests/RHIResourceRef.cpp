/// @file   RHIResourceRef.cpp
/// @brief  Tests for intrusive RHI resource references.

#include <gtest/gtest.h>

import RHI;
import std;

using namespace SoulEngine;

namespace {

class MockSampler final : public RHISampler {
  public:
    explicit MockSampler(Uint32& DestructionCount) : RHISampler(RHISamplerDesc{}), m_DestructionCount(DestructionCount) {}
    ~MockSampler() override { ++m_DestructionCount; }

  private:
    Uint32& m_DestructionCount;
};

class MockVertexBuffer final : public RHIVertexBuffer {
  public:
    explicit MockVertexBuffer(Uint32& DestructionCount) : m_DestructionCount(DestructionCount) {}
    ~MockVertexBuffer() override { ++m_DestructionCount; }

  private:
    Uint32& m_DestructionCount;
};

class MockRenderDevice final : public RHIRenderDevice {
  public:
    MockRenderDevice() { GDeferredDeletionQueue = &GetDeletionQueue(); }
    ~MockRenderDevice() override { GDeferredDeletionQueue = nullptr; }

    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override { return {}; }
    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override { return RHIBackendType::Unknown; }

    [[nodiscard]] auto CreateVertexBuffer(const RHIVertexBufferDesc&)
        -> std::expected<RHIRef<RHIVertexBuffer>, ErrorMessage> override {
        auto Resource = RHIRef<RHIVertexBuffer>::Create();
        if (m_FailVertexBuffer) {
            Resource.MarkFailed(ErrorMessage("mock vertex-buffer creation failure"));
            return std::unexpected(ErrorMessage("mock vertex-buffer creation failure"));
        }
        auto Payload = Resource.m_Payload;
        if (auto Publish = PublishPendingPayload(
                Resource, UPtr<RHIVertexBuffer>{std::make_unique<MockVertexBuffer>(m_VertexDestructions)});
            !Publish) {
            return std::unexpected(Publish.error());
        }
        m_CompletionCallbacks.emplace_back([Payload] { (void)Payload->TryMarkReady(); });
        return Resource;
    }

    [[nodiscard]] auto CreateIndexBuffer(const RHIIndexBufferDesc&)
        -> std::expected<RHIRef<RHIIndexBuffer>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock index-buffer creation is not implemented"));
    }

    [[nodiscard]] auto CreateSampledTexture(const RHISampledTextureDesc&)
        -> std::expected<RHIRef<RHISampledTexture>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock sampled-texture creation is not implemented"));
    }

    [[nodiscard]] auto CreateSampler(const RHISamplerDesc&) -> std::expected<RHIRef<RHISampler>, ErrorMessage> override {
        auto Resource = RHIRef<RHISampler>::Create();
        if (auto Publish = PublishReadyPayload(
                Resource, UPtr<RHISampler>{std::make_unique<MockSampler>(m_SamplerDestructions)});
            !Publish) {
            return std::unexpected(Publish.error());
        }
        return Resource;
    }

    [[nodiscard]] auto CreateRenderTarget(const RHIRenderTargetDesc&)
        -> std::expected<RHIRef<RHIRenderTarget>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock render-target creation is not implemented"));
    }

    [[nodiscard]] auto CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock graphics-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateGraphicsPipeline(const RHIGraphicsPipelineDesc&, RHIRef<RHIGraphicsPipeline>)
        -> std::expected<void, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock graphics-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateRayTracingPipeline(const RHIRayTracingPipelineDesc&)
        -> std::expected<RHIRef<RHIRayTracingPipeline>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock ray-tracing-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateRayTracingPipeline(const RHIRayTracingPipelineDesc&, RHIRef<RHIRayTracingPipeline>)
        -> std::expected<void, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock ray-tracing-pipeline creation is not implemented"));
    }

    [[nodiscard]] auto CreateBottomLevelAccelerationStructure(const RHIBottomLevelAccelerationStructureDesc&)
        -> std::expected<RHIRef<RHIBottomLevelAccelerationStructure>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock BLAS creation is not implemented"));
    }

    [[nodiscard]] auto CreateTopLevelAccelerationStructure(const RHITopLevelAccelerationStructureDesc&)
        -> std::expected<RHIRef<RHITopLevelAccelerationStructure>, ErrorMessage> override {
        return std::unexpected(ErrorMessage("mock TLAS creation is not implemented"));
    }

    [[nodiscard]] auto Execute(RHICommandList&&) -> std::expected<void, ErrorMessage> override { return {}; }
    [[nodiscard]] auto GetCurrentFrameIndex() const -> Uint32 override { return 0; }
    auto WaitIdle() -> void override {}
    auto Shutdown() -> void override { GetDeletionQueue().Drain(); }

    bool   m_FailVertexBuffer   = false;
    bool   m_GpuComplete        = false;
    Uint32 m_VertexDestructions = 0;
    Uint32 m_SamplerDestructions = 0;

  protected:
    auto TickBackendCompletions() -> void override {
        if (!m_GpuComplete)
            return;
        auto Callbacks = std::move(m_CompletionCallbacks);
        for (auto& Complete : Callbacks)
            Complete();
    }

  private:
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
        auto Ref = RHIRef<RHISampler>::Create();
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

TEST(RHIResourceRefTest, SynchronousCreateReturnsReadySamplerRef) {
    MockRenderDevice Device;
    {
        auto Sampler = static_cast<RHIRenderDevice&>(Device).CreateSampler(RHISamplerDesc{});

        ASSERT_TRUE(Sampler.has_value());
        EXPECT_EQ(Sampler->GetState(), RHIRefState::Ready);
        EXPECT_NE(Sampler->TryGet(), nullptr);
    }
    Device.DrainDeletionQueue();
    EXPECT_EQ(Device.m_SamplerDestructions, 1);
}

TEST(RHIResourceRefTest, BackendCreatePublishesGpuPendingThenReady) {
    MockRenderDevice Device;
    auto Result = Device.CreateVertexBuffer(RHIVertexBufferDesc{});

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

    Buffer = nullptr;
    Copy   = nullptr;
    Device.DrainDeletionQueue();
    EXPECT_EQ(Device.m_VertexDestructions, 1);
}

TEST(RHIResourceRefTest, BackendCreationFailureReturnsError) {
    MockRenderDevice Device;
    Device.m_FailVertexBuffer = true;

    auto Result = Device.CreateVertexBuffer(RHIVertexBufferDesc{});
    EXPECT_FALSE(Result.has_value());
    EXPECT_NE(Result.error().ToString().find("mock vertex-buffer creation failure"), String::npos);
}


TEST(RHIResourceRefTest, CompletionCallbackKeepsPayloadAliveUntilGpuCompletion) {
    MockRenderDevice Device;
    auto Result = Device.CreateVertexBuffer(RHIVertexBufferDesc{});
    ASSERT_TRUE(Result.has_value());
    auto Buffer = std::move(*Result);
    auto Copy = Buffer;

    Buffer = nullptr;
    Copy = nullptr;
    Device.Tick();
    EXPECT_EQ(Device.m_VertexDestructions, 0);

    Device.m_GpuComplete = true;
    Device.Tick();
    EXPECT_EQ(Device.m_VertexDestructions, 1);
}

TEST(RHIResourceRefTest, CompletionCannotOverwriteFailure) {
    MockRenderDevice Device;
    auto Result = Device.CreateVertexBuffer(RHIVertexBufferDesc{});
    ASSERT_TRUE(Result.has_value());
    auto Buffer = std::move(*Result);

    Buffer.MarkFailed(ErrorMessage("mock completion failure"));
    Device.m_GpuComplete = true;
    Device.Tick();

    EXPECT_EQ(Buffer.GetState(), RHIRefState::Failed);
    ASSERT_TRUE(Buffer.GetError().has_value());
    EXPECT_NE(Buffer.GetError()->ToString().find("mock completion failure"), String::npos);
}
