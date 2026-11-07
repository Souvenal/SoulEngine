#include <entt/entt.hpp>
#include <gtest/gtest.h>
#include <hlsl++.h>

import Core;
import EditorTypes;
import RHI;
import RenderGraph;
import Renderer.Common;
import Renderer.EditorPasses;
import Renderer.Raster.CullingPass;
import Renderer.Raster.DeferredLightingPass;
import Renderer.Raster.GeometryPass;
import Renderer.Raster.RasterRenderer;
import Scene;
import TaskGraph;
import WindowSystem;
import std;

using namespace SoulEngine;

[[nodiscard]] auto MakeIndirectViews(RGBuffer Buffer, Uint32 Count = 2)
    -> std::vector<RGBufferView<RHIBufferUsage::Indirect, RHIAccess::Read>> {
    std::vector<RGBufferView<RHIBufferUsage::Indirect, RHIAccess::Read>> Views(Count);
    for (Uint32 Slot = 0; Slot < Count; ++Slot) {
        Views[Slot].Buffer = Buffer;
        Views[Slot].Offset = Slot * sizeof(RHIIndexedIndirectCommand);
        Views[Slot].Range  = sizeof(RHIIndexedIndirectCommand);
    }
    return Views;
}

class MockRasterTestBuffer final : public RHIBuffer {
  public:
    MockRasterTestBuffer(StringView Name, Uint64 Size, RHIBufferUsage Usage, Uint64 DeviceAddress = 0)
        : RHIBuffer(String(Name), Size, Usage, RHIMemoryUsage::CpuToGpu, RHILifetime::Persistent),
          m_DeviceAddress(DeviceAddress),
          m_Bytes(Size) {}

    [[nodiscard]] auto GetDeviceAddress() const noexcept -> Uint64 override {
        return m_DeviceAddress;
    }
    [[nodiscard]] auto GetBytes() const -> std::span<const std::byte> {
        return m_Bytes;
    }
    auto Write(std::span<const std::byte> Data) -> void {
        std::ranges::copy(Data, m_Bytes.begin());
    }

  private:
    Uint64                 m_DeviceAddress = 0;
    std::vector<std::byte> m_Bytes         = {};
};

class MockRasterTestTexture final : public RHITexture {
  public:
    MockRasterTestTexture(StringView Name, const RHITextureDesc& Desc) : RHITexture(String(Name), Desc) {}
};

class MockRasterTestSampler final : public RHISampler {
  public:
    explicit MockRasterTestSampler(StringView Name) : RHISampler(String(Name), RHISamplerDesc{}) {}
};

class MockRasterTestBindingSet final : public RHIShaderBindingSet {
  public:
    MockRasterTestBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        : RHIShaderBindingSet(String(Name), Desc) {}

    [[nodiscard]] auto OnResourcesBound(Uint32) -> std::expected<void, ErrorMessage> override {
        return {};
    }
};

class MockRasterTestPipeline final : public RHIGraphicsPipeline {
  public:
    MockRasterTestPipeline(StringView Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIGraphicsPipeline(String(Name), std::move(BindingSet)) {}
};

class MockRasterTestComputePipeline final : public RHIComputePipeline {
  public:
    MockRasterTestComputePipeline(StringView Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIComputePipeline(String(Name), std::move(BindingSet)) {}
};

class MockRasterTestDevice final : public RHIRenderDevice {
  public:
    [[nodiscard]] auto Initialize(IWindowSystem*) -> std::expected<void, ErrorMessage> override {
        return {};
    }
    [[nodiscard]] auto GetBackendType() const -> RHIBackendType override {
        return RHIBackendType::Unknown;
    }
    [[nodiscard]] auto CreateShaderBindingSet(StringView Name, const RHIShaderBindingSetDesc& Desc)
        -> std::expected<RHIRef<RHIShaderBindingSet>, ErrorMessage> override {
        auto Result = RHIRef<RHIShaderBindingSet>::Create();
        if (auto Published = Result.Publish(UPtr<MockRasterTestBindingSet>(new MockRasterTestBindingSet(Name, Desc)),
                                            RHIRefState::Ready);
            !Published)
            return std::unexpected(Published.error());
        return Result;
    }
    [[nodiscard]] auto CreateBuffer(StringView Name, const RHIBufferDesc& Desc)
        -> std::expected<RHIRef<RHIBuffer>, ErrorMessage> override {
        auto Result = RHIRef<RHIBuffer>::Create();
        auto Buffer =
            UPtr<MockRasterTestBuffer>(new MockRasterTestBuffer(Name, Desc.SizeBytes, Desc.Usage, m_NextDeviceAddress));
        m_NextDeviceAddress += 0x1000;
        if (auto Published = Result.Publish(std::move(Buffer), RHIRefState::Ready); !Published) {
            return std::unexpected(Published.error());
        }
        return Result;
    }
    [[nodiscard]] auto UploadBuffer(RHIRef<RHIBuffer> Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> override {
        if (!Buffer) {
            return std::unexpected(ErrorMessage("Mock upload requires a ready buffer"));
        }
        auto& Mock = static_cast<MockRasterTestBuffer&>(*Buffer.TryGet());
        if (Data.size_bytes() != Mock.GetSize()) {
            return std::unexpected(ErrorMessage("Mock upload size does not match the buffer"));
        }
        Mock.Write(Data);
        return {};
    }

    [[nodiscard]] auto CreateTexture(StringView Name, const RHITextureDesc& Desc)
        -> std::expected<RHIRef<RHITexture>, ErrorMessage> override {
        auto Result = RHIRef<RHITexture>::Create();
        if (auto Published =
                Result.Publish(UPtr<MockRasterTestTexture>(new MockRasterTestTexture(Name, Desc)), RHIRefState::Ready);
            !Published)
            return std::unexpected(Published.error());
        return Result;
    }
    [[nodiscard]] auto CreateSampler(StringView Name, const RHISamplerDesc&)
        -> std::expected<RHIRef<RHISampler>, ErrorMessage> override {
        auto Result = RHIRef<RHISampler>::Create();
        if (auto Published =
                Result.Publish(UPtr<MockRasterTestSampler>(new MockRasterTestSampler(Name)), RHIRefState::Ready);
            !Published)
            return std::unexpected(Published.error());
        return Result;
    }
    [[nodiscard]] auto CreateComputePipeline(StringView Name, const RHIComputePipelineDesc& Desc)
        -> std::expected<RHIRef<RHIComputePipeline>, ErrorMessage> override {
        auto Result = RHIRef<RHIComputePipeline>::Create();
        if (auto Published = Result.Publish(
                UPtr<MockRasterTestComputePipeline>(new MockRasterTestComputePipeline(Name, Desc.BindingSet)),
                RHIRefState::Ready);
            !Published)
            return std::unexpected(Published.error());
        return Result;
    }

    [[nodiscard]] auto CreateGraphicsPipeline(StringView Name, const RHIGraphicsPipelineDesc& Desc)
        -> std::expected<RHIRef<RHIGraphicsPipeline>, ErrorMessage> override {
        auto Result = RHIRef<RHIGraphicsPipeline>::Create();
        if (auto Published = Result.Publish(
                UPtr<MockRasterTestPipeline>(new MockRasterTestPipeline(Name, Desc.BindingSet)), RHIRefState::Ready);
            !Published)
            return std::unexpected(Published.error());
        return Result;
    }

  private:
    Uint64 m_NextDeviceAddress = 0x1000;
};

class MockRasterTestWindow final : public IWindowSystem {
  public:
    [[nodiscard]] auto GetType() const -> WindowSystemType override {
        return WindowSystemType::Unknown;
    }
    [[nodiscard]] auto IsValid() const -> bool override {
        return true;
    }
    auto               Shutdown() -> void override {}
    [[nodiscard]] auto Tick() -> bool override {
        return true;
    }
    [[nodiscard]] auto GetEventDispatcher() -> entt::dispatcher& override {
        return m_Dispatcher;
    }
    [[nodiscard]] auto GetFramebufferExtent() const -> FramebufferExtent override {
        return {};
    }
    auto SetCursorMode(CursorMode) -> void override {}

  private:
    entt::dispatcher m_Dispatcher = {};
};

struct RasterTestDeviceInjector {
    RasterTestDeviceInjector() {
        RHIBackendFactory::Get().Register<MockRasterTestDevice>("RasterIndexedTest");
        static auto Window = MockRasterTestWindow{};
        const auto  Result = RHIRenderDevice::Create("RasterIndexedTest", &Window);
        (void)Result;
    }
};

const auto G_RasterTestDevice = RasterTestDeviceInjector{};

struct RasterTestResources {
    RHIRef<RHIBuffer>  Indirect;
    RHIRef<RHIBuffer>  Position0;
    RHIRef<RHIBuffer>  Normal0;
    RHIRef<RHIBuffer>  Tangent0;
    RHIRef<RHIBuffer>  TexCoord0;
    RHIRef<RHIBuffer>  Index0;
    RHIRef<RHIBuffer>  Position1;
    RHIRef<RHIBuffer>  Normal1;
    RHIRef<RHIBuffer>  Tangent1;
    RHIRef<RHIBuffer>  TexCoord1;
    RHIRef<RHIBuffer>  Index1;
    RHIRef<RHIBuffer>  Instances;
    RHIRef<RHIBuffer>  Geometry;
    RHIRef<RHIBuffer>  Materials;
    RHIRef<RHIBuffer>  Frame;
    RHIRef<RHIBuffer>  View;
    RHIRef<RHITexture> Albedo;
    RHIRef<RHITexture> Normal;
    RHIRef<RHITexture> MaterialId;
    RHIRef<RHITexture> EntityId;
    RHIRef<RHITexture> Depth;
    RHIRef<RHITexture> Mask;
    RHIRef<RHISampler> Sampler;
};

class RasterIndexedPassTest : public testing::Test {
  protected:
    static auto SetUpTestSuite() -> void {
        const auto* TestSourceDir = std::getenv("SOUL_ENGINE_TEST_SOURCE_DIR");
        ASSERT_NE(TestSourceDir, nullptr) << "Missing SOUL_ENGINE_TEST_SOURCE_DIR";
        const auto EngineDir = Path(TestSourceDir).parent_path().parent_path().parent_path().parent_path();
        EnginePaths::Get().Init(EngineDir);
    }

    auto SetUp() -> void override {
        GDeferredDeletionQueue = &m_DeletionQueue;
        TaskGraph::Get().Shutdown();
        RenderGraph::Get().Init();
        TaskGraph::Get().Init(1);
    }

    auto TearDown() -> void override {
        RenderGraph::Get().Shutdown();
        TaskGraph::Get().Shutdown();
        m_DeletionQueue.Drain();
        GDeferredDeletionQueue = nullptr;
    }

    template <typename TPass>
    [[nodiscard]] auto PreparePipeline() -> bool {
        auto& Registry = PipelineRegistry::Get();
        Registry.Register<TPass>();
        const auto Key = PipelineKeyOf<TPass>();
        Registry.EnsureRequestStarted(Key);
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (Registry.GetState(Key) == RGPipelineState::Pending && std::chrono::steady_clock::now() < Deadline) {
            TaskGraph::Get().DrainTasks(ThreadQueue::RHI);
            std::this_thread::yield();
        }
        TaskGraph::Get().DrainTasks(ThreadQueue::RHI);
        if (const auto Error = Registry.GetError(Key))
            ADD_FAILURE() << Error->ToString();
        return Registry.GetState(Key) == RGPipelineState::Ready;
    }

    [[nodiscard]] auto MakeBuffer(StringView Name, Uint64 Size, RHIBufferUsage Usage) -> RHIRef<RHIBuffer> {
        auto Result = RHIRef<RHIBuffer>::Create();
        if (auto Published = Result.Publish(UPtr<MockRasterTestBuffer>(new MockRasterTestBuffer(Name, Size, Usage)),
                                            RHIRefState::Ready);
            !Published) {
            ADD_FAILURE() << Published.error().ToString();
        }
        return Result;
    }

    [[nodiscard]] auto MakeTexture(StringView Name, RHIFormat Format, RHITextureUsage Usage) -> RHIRef<RHITexture> {
        auto                 Result = RHIRef<RHITexture>::Create();
        const RHITextureDesc Desc{.Width = 8, .Height = 8, .Format = Format, .Usage = Usage};
        if (auto Published =
                Result.Publish(UPtr<MockRasterTestTexture>(new MockRasterTestTexture(Name, Desc)), RHIRefState::Ready);
            !Published) {
            ADD_FAILURE() << Published.error().ToString();
        }
        return Result;
    }

    [[nodiscard]] auto MakeSampler(StringView Name) -> RHIRef<RHISampler> {
        auto Result = RHIRef<RHISampler>::Create();
        if (auto Published =
                Result.Publish(UPtr<MockRasterTestSampler>(new MockRasterTestSampler(Name)), RHIRefState::Ready);
            !Published) {
            ADD_FAILURE() << Published.error().ToString();
        }
        return Result;
    }

    [[nodiscard]] auto MakeResources() -> RasterTestResources {
        return RasterTestResources{
            .Indirect   = MakeBuffer("Indirect", 40, RHIBufferUsage::Storage | RHIBufferUsage::Indirect),
            .Position0  = MakeBuffer("Position0", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .Normal0    = MakeBuffer("Normal0", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .Tangent0   = MakeBuffer("Tangent0", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .TexCoord0  = MakeBuffer("TexCoord0", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .Index0     = MakeBuffer("Index0", 64, RHIBufferUsage::Index | RHIBufferUsage::Storage),
            .Position1  = MakeBuffer("Position1", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .Normal1    = MakeBuffer("Normal1", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .Tangent1   = MakeBuffer("Tangent1", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .TexCoord1  = MakeBuffer("TexCoord1", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage),
            .Index1     = MakeBuffer("Index1", 64, RHIBufferUsage::Index | RHIBufferUsage::Storage),
            .Instances  = MakeBuffer("Instances", 256, RHIBufferUsage::Storage),
            .Geometry   = MakeBuffer("Geometry", 256, RHIBufferUsage::Storage),
            .Materials  = MakeBuffer("Materials", 256, RHIBufferUsage::Storage),
            .Frame      = MakeBuffer("Frame", 256, RHIBufferUsage::Constant),
            .View       = MakeBuffer("View", 256, RHIBufferUsage::Constant),
            .Albedo     = MakeTexture("Albedo", RHIFormat::B8G8R8A8_UNORM, RHITextureUsage::RenderTarget),
            .Normal     = MakeTexture("Normal", RHIFormat::R16G16B16A16_SFLOAT, RHITextureUsage::RenderTarget),
            .MaterialId = MakeTexture("MaterialId", RHIFormat::R32_UINT, RHITextureUsage::RenderTarget),
            .EntityId   = MakeTexture("EntityId", RHIFormat::R32_UINT, RHITextureUsage::RenderTarget),
            .Depth      = MakeTexture("Depth", RHIFormat::D32_SFLOAT, RHITextureUsage::DepthStencil),
            .Mask       = MakeTexture("Mask", RHIFormat::R8_UNORM, RHITextureUsage::RenderTarget),
            .Sampler    = MakeSampler("Sampler"),
        };
    }

    [[nodiscard]] auto FindTransition(const RHIBarrierList& Prologue, const RHIRef<RHIBuffer>& Buffer)
        -> const RHIBufferTransition* {
        for (const auto& Transition : Prologue.Buffers) {
            if (Transition.Buffer.TryGet() == Buffer.TryGet())
                return &Transition;
        }
        return nullptr;
    }

    auto ExpectGeometryDeclarations(const RHIBarrierList& Prologue, const RasterTestResources& Resources) -> void {
        const std::array AttributeBuffers{
            Resources.Position0,
            Resources.Normal0,
            Resources.Tangent0,
            Resources.TexCoord0,
            Resources.Position1,
            Resources.Normal1,
        };
        for (const auto& Buffer : AttributeBuffers) {
            const auto* Transition = FindTransition(Prologue, Buffer);
            ASSERT_NE(Transition, nullptr) << Buffer->GetName();
            EXPECT_EQ(Transition->Usage, RHIBufferUsage::Storage);
            EXPECT_EQ(Transition->Access, RHIAccess::Read);
            EXPECT_EQ(Transition->Stage, RHISyncStage::AllGraphics);
        }
        const std::array IndexBuffers{Resources.Index0, Resources.Index1};
        for (const auto& Buffer : IndexBuffers) {
            const auto* Transition = FindTransition(Prologue, Buffer);
            ASSERT_NE(Transition, nullptr) << Buffer->GetName();
            EXPECT_EQ(Transition->Usage, RHIBufferUsage::Index);
            EXPECT_EQ(Transition->Access, RHIAccess::Read);
            EXPECT_EQ(Transition->Stage, RHISyncStage::VertexInput);
        }
        const auto* Indirect = FindTransition(Prologue, Resources.Indirect);
        ASSERT_NE(Indirect, nullptr);
        EXPECT_EQ(Indirect->Stage, RHISyncStage::DrawIndirect);
    }

    auto ExpectIndexedPacket(IRHIPass& Pass, const RasterTestResources& Resources) -> void {
        const auto Recorded = Pass.Record();
        ASSERT_TRUE(Recorded.has_value()) << Recorded.error().ToString();
        const auto Commands = Pass.GetCommands();
        ASSERT_EQ(Commands.size(), 4u);
        const auto* Draw0 = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[2]);
        const auto* Draw1 = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[3]);
        ASSERT_NE(Draw0, nullptr);
        ASSERT_NE(Draw1, nullptr);
        EXPECT_EQ(Draw0->IndexBuffer.GetBuffer().TryGet(), Resources.Index0.TryGet());
        EXPECT_EQ(Draw1->IndexBuffer.GetBuffer().TryGet(), Resources.Index1.TryGet());
        EXPECT_EQ(Draw0->IndirectBuffer.GetBuffer().TryGet(), Resources.Indirect.TryGet());
        EXPECT_EQ(Draw1->IndirectBuffer.GetBuffer().TryGet(), Resources.Indirect.TryGet());
        EXPECT_EQ(Draw0->IndirectBuffer.GetOffset(), 0u);
        EXPECT_EQ(Draw1->IndirectBuffer.GetOffset(), 20u);
        EXPECT_EQ(Draw0->IndirectBuffer.GetRange(), 20u);
        EXPECT_EQ(Draw1->IndirectBuffer.GetRange(), 20u);
        EXPECT_EQ(Draw0->DrawCount, 1u);
        EXPECT_EQ(Draw1->DrawCount, 1u);
        EXPECT_TRUE(Draw0->VertexBuffers.empty());
        EXPECT_TRUE(Draw1->VertexBuffers.empty());
        for (const auto& Command : Commands)
            EXPECT_FALSE(std::holds_alternative<RHIDrawIndirectCmd>(Command));
    }

    RHIDeferredDeletionQueue m_DeletionQueue;
};

TEST_F(RasterIndexedPassTest, GeometryRecordsIndexedDrawsAndDeclaresGeometryPointees) {
    const auto Resources = MakeResources();
    ASSERT_TRUE(PreparePipeline<GeometryPass>());
    RenderGraphBuilder      Graph;
    GeometryPass::Parameter Parameter{
        .Albedo             = Graph.Import(Resources.Albedo),
        .Normal             = Graph.Import(Resources.Normal),
        .MaterialId         = Graph.Import(Resources.MaterialId),
        .EntityId           = Graph.Import(Resources.EntityId),
        .Depth              = Graph.Import(Resources.Depth),
        .InstanceBuffer     = Graph.Import(Resources.Instances),
        .IndirectBuffers    = MakeIndirectViews(Graph.Import(Resources.Indirect)),
        .GeometryBuffer     = Graph.Import(Resources.Geometry),
        .MaterialBuffer     = Graph.Import(Resources.Materials),
        .FrameBuffer        = Graph.Import(Resources.Frame),
        .ViewBuffer         = Graph.Import(Resources.View),
        .AttributeBuffers   = {Graph.Import(Resources.Position0),
                               Graph.Import(Resources.Normal0),
                               Graph.Import(Resources.Tangent0),
                               Graph.Import(Resources.TexCoord0),
                               Graph.Import(Resources.Position1),
                               Graph.Import(Resources.Normal1)},
        .IndexBuffers       = {Graph.Import(Resources.Index0), Graph.Import(Resources.Index1)},
        .LinearSampler      = Resources.Sampler,
        .AnisotropicSampler = Resources.Sampler,
        .DrawCount          = 2,
    };
    Graph.AddPass<GeometryPass>(std::move(Parameter));
    auto List = Graph.Compile();
    ASSERT_TRUE(List.has_value()) << List.error().ToString();
    ASSERT_EQ(List->Entries.size(), 1u);
    auto& Entry = List->Entries.front();
    ExpectGeometryDeclarations(Entry.Prologue, Resources);
    ExpectIndexedPacket(*Entry.Pass, Resources);
}

TEST_F(RasterIndexedPassTest, SelectionMaskRecordsIndexedDrawsAndDeclaresGeometryPointees) {
    const auto Resources = MakeResources();
    ASSERT_TRUE(PreparePipeline<SelectionMaskPass>());
    RenderGraphBuilder           Graph;
    SelectionMaskPass::Parameter Parameter{
        .Mask                    = Graph.Import(Resources.Mask),
        .SelectedInstances       = Graph.Import(Resources.Instances),
        .SelectedIndirectBuffers = MakeIndirectViews(Graph.Import(Resources.Indirect)),
        .GeometryTable           = Graph.Import(Resources.Geometry),
        .MaterialTable           = Graph.Import(Resources.Materials),
        .FrameBuffer             = Graph.Import(Resources.Frame),
        .ViewBuffer              = Graph.Import(Resources.View),
        .AttributeBuffers        = {Graph.Import(Resources.Position0),
                                    Graph.Import(Resources.Normal0),
                                    Graph.Import(Resources.Tangent0),
                                    Graph.Import(Resources.TexCoord0),
                                    Graph.Import(Resources.Position1),
                                    Graph.Import(Resources.Normal1)},
        .IndexBuffers            = {Graph.Import(Resources.Index0), Graph.Import(Resources.Index1)},
        .LinearSampler           = Resources.Sampler,
        .DrawCount               = 2,
    };
    Graph.AddPass<SelectionMaskPass>(std::move(Parameter));
    auto List = Graph.Compile();
    ASSERT_TRUE(List.has_value()) << List.error().ToString();
    ASSERT_EQ(List->Entries.size(), 1u);
    auto& Entry = List->Entries.front();
    ExpectGeometryDeclarations(Entry.Prologue, Resources);
    ExpectIndexedPacket(*Entry.Pass, Resources);
}

TEST_F(RasterIndexedPassTest, VertexInputGeometryAndVertexPullingSelectionShareGeometryResources) {
    const auto Resources = MakeResources();
    ASSERT_TRUE(PreparePipeline<VertexInputGeometryPass>());
    ASSERT_TRUE(PreparePipeline<SelectionMaskPass>());
    RenderGraphBuilder Graph;

    const auto Albedo     = Graph.Import(Resources.Albedo);
    const auto Normal     = Graph.Import(Resources.Normal);
    const auto MaterialId = Graph.Import(Resources.MaterialId);
    const auto EntityId   = Graph.Import(Resources.EntityId);
    const auto Depth      = Graph.Import(Resources.Depth);
    const auto Instances  = Graph.Import(Resources.Instances);
    const auto Indirect   = Graph.Import(Resources.Indirect);
    const auto Geometry   = Graph.Import(Resources.Geometry);
    const auto Materials  = Graph.Import(Resources.Materials);
    const auto Frame      = Graph.Import(Resources.Frame);
    const auto View       = Graph.Import(Resources.View);
    const auto Mask       = Graph.Import(Resources.Mask);

    std::vector<RGBufferView<RHIBufferUsage::Vertex, RHIAccess::Read>>  VertexViews;
    std::vector<RGBufferView<RHIBufferUsage::Storage, RHIAccess::Read>> StorageViews;
    const std::array                                                    AttributeBuffers{
        Resources.Position0,
        Resources.Normal0,
        Resources.Tangent0,
        Resources.TexCoord0,
        Resources.Position1,
        Resources.Normal1,
        Resources.Tangent1,
        Resources.TexCoord1,
    };
    for (const auto& Buffer : AttributeBuffers) {
        const auto Handle = Graph.Import(Buffer);
        VertexViews.emplace_back(Handle);
        StorageViews.emplace_back(Handle);
    }
    std::vector<RGBufferView<RHIBufferUsage::Index, RHIAccess::Read>> IndexViews{Graph.Import(Resources.Index0),
                                                                                 Graph.Import(Resources.Index1)};

    Graph.AddPass<VertexInputGeometryPass>({
        .Albedo             = Albedo,
        .Normal             = Normal,
        .MaterialId         = MaterialId,
        .EntityId           = EntityId,
        .Depth              = Depth,
        .InstanceBuffer     = Instances,
        .IndirectBuffers    = MakeIndirectViews(Indirect),
        .MaterialBuffer     = Materials,
        .FrameBuffer        = Frame,
        .ViewBuffer         = View,
        .VertexBuffers      = VertexViews,
        .IndexBuffers       = IndexViews,
        .LinearSampler      = Resources.Sampler,
        .AnisotropicSampler = Resources.Sampler,
        .DrawCount          = 2,
    });
    Graph.AddPass<SelectionMaskPass>({
        .Mask                    = Mask,
        .SelectedInstances       = Instances,
        .SelectedIndirectBuffers = MakeIndirectViews(Indirect),
        .GeometryTable           = Geometry,
        .MaterialTable           = Materials,
        .FrameBuffer             = Frame,
        .ViewBuffer              = View,
        .AttributeBuffers        = StorageViews,
        .IndexBuffers            = IndexViews,
        .LinearSampler           = Resources.Sampler,
        .DrawCount               = 2,
    });

    auto List = Graph.Compile();
    ASSERT_TRUE(List.has_value()) << List.error().ToString();
    ASSERT_EQ(List->Entries.size(), 2u);
    auto& GeometryEntry  = List->Entries[0];
    auto& SelectionEntry = List->Entries[1];
    EXPECT_EQ(GeometryEntry.Pass->GetName(), VertexInputGeometryPass::Name);
    EXPECT_EQ(SelectionEntry.Pass->GetName(), SelectionMaskPass::Name);

    const auto* VertexTransition  = FindTransition(GeometryEntry.Prologue, Resources.Position0);
    const auto* StorageTransition = FindTransition(SelectionEntry.Prologue, Resources.Position0);
    ASSERT_NE(VertexTransition, nullptr);
    ASSERT_NE(StorageTransition, nullptr);
    EXPECT_EQ(VertexTransition->Usage, RHIBufferUsage::Vertex);
    EXPECT_EQ(StorageTransition->Usage, RHIBufferUsage::Storage);
    EXPECT_NE(FindTransition(SelectionEntry.Prologue, Resources.Geometry), nullptr);

    ASSERT_TRUE(GeometryEntry.Pass->Record().has_value());
    ASSERT_TRUE(SelectionEntry.Pass->Record().has_value());
    EXPECT_EQ(GeometryEntry.Pass->GetCommands().size(), 4u);
    EXPECT_EQ(SelectionEntry.Pass->GetCommands().size(), 4u);
}

TEST_F(RasterIndexedPassTest, VertexInputGeometryRecordsFourStreamsWithoutBindingGeometryTable) {
    const auto Resources = MakeResources();
    ASSERT_TRUE(PreparePipeline<VertexInputGeometryPass>());
    RenderGraphBuilder                 Graph;
    VertexInputGeometryPass::Parameter Parameter{
        .Albedo             = Graph.Import(Resources.Albedo),
        .Normal             = Graph.Import(Resources.Normal),
        .MaterialId         = Graph.Import(Resources.MaterialId),
        .EntityId           = Graph.Import(Resources.EntityId),
        .Depth              = Graph.Import(Resources.Depth),
        .InstanceBuffer     = Graph.Import(Resources.Instances),
        .IndirectBuffers    = MakeIndirectViews(Graph.Import(Resources.Indirect)),
        .MaterialBuffer     = Graph.Import(Resources.Materials),
        .FrameBuffer        = Graph.Import(Resources.Frame),
        .ViewBuffer         = Graph.Import(Resources.View),
        .VertexBuffers      = {Graph.Import(Resources.Position0),
                               Graph.Import(Resources.Normal0),
                               Graph.Import(Resources.Tangent0),
                               Graph.Import(Resources.TexCoord0),
                               Graph.Import(Resources.Position1),
                               Graph.Import(Resources.Normal1),
                               Graph.Import(Resources.Tangent1),
                               Graph.Import(Resources.TexCoord1)},
        .IndexBuffers       = {Graph.Import(Resources.Index0), Graph.Import(Resources.Index1)},
        .LinearSampler      = Resources.Sampler,
        .AnisotropicSampler = Resources.Sampler,
        .DrawCount          = 2,
    };
    Graph.AddPass<VertexInputGeometryPass>(std::move(Parameter));
    auto List = Graph.Compile();
    ASSERT_TRUE(List.has_value()) << List.error().ToString();
    ASSERT_EQ(List->Entries.size(), 1u);
    auto& Entry = List->Entries.front();

    const std::array VertexBuffers{
        Resources.Position0,
        Resources.Normal0,
        Resources.Tangent0,
        Resources.TexCoord0,
        Resources.Position1,
        Resources.Normal1,
        Resources.Tangent1,
        Resources.TexCoord1,
    };
    for (const auto& Buffer : VertexBuffers) {
        const auto* Transition = FindTransition(Entry.Prologue, Buffer);
        ASSERT_NE(Transition, nullptr) << Buffer->GetName();
        EXPECT_EQ(Transition->Usage, RHIBufferUsage::Vertex);
        EXPECT_EQ(Transition->Access, RHIAccess::Read);
        EXPECT_EQ(Transition->Stage, RHISyncStage::VertexInput);
    }

    const auto Recorded = Entry.Pass->Record();
    ASSERT_TRUE(Recorded.has_value()) << Recorded.error().ToString();

    const auto BindingSet = Entry.Pass->GetPipeline()->GetShaderBindingSet();
    ASSERT_TRUE(BindingSet);
    for (const auto& Set : BindingSet->GetBindingsBySet()) {
        for (const auto& Slot : Set) {
            if (Slot.Info.ParameterPath.find("geometryTable") != StringView::npos) {
                EXPECT_TRUE(std::holds_alternative<std::monostate>(Slot.Bound)) << Slot.Info.ParameterPath;
            }
        }
    }
    const auto Commands = Entry.Pass->GetCommands();
    ASSERT_EQ(Commands.size(), 4u);
    for (std::size_t Slot = 0; Slot < 2; ++Slot) {
        const auto* Draw = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[Slot + 2]);
        ASSERT_NE(Draw, nullptr);
        ASSERT_EQ(Draw->VertexBuffers.size(), 4u);
        for (std::size_t Attribute = 0; Attribute < 4; ++Attribute) {
            EXPECT_EQ(Draw->VertexBuffers[Attribute].GetBuffer().TryGet(),
                      VertexBuffers[Slot * 4 + Attribute].TryGet());
            EXPECT_EQ(Draw->VertexBuffers[Attribute].GetOffset(), 0u);
            EXPECT_EQ(Draw->VertexBuffers[Attribute].GetUsage(), RHIBufferUsage::Vertex);
        }
        EXPECT_EQ(Draw->IndexBuffer.GetBuffer().TryGet(),
                  Slot == 0 ? Resources.Index0.TryGet() : Resources.Index1.TryGet());
        EXPECT_EQ(Draw->IndirectBuffer.GetOffset(), Slot * 20);
        EXPECT_EQ(Draw->IndirectBuffer.GetRange(), 20u);
        EXPECT_EQ(Draw->DrawCount, 1u);
    }
}

TEST_F(RasterIndexedPassTest, VertexInputSelectionAlignsWithGeometryAndRecordsPositionUv) {
    const auto Request = VertexInputSelectionMaskPass::BuildPipelineRequest();
    EXPECT_EQ(Request.VertEntry.EntryPoint, "vertVertexInputMain");
    ASSERT_EQ(Request.VertexInputLayout.Bindings.size(), 2u);
    ASSERT_EQ(Request.VertexInputLayout.Attributes.size(), 2u);
    const std::array<Uint32, 2>    ExpectedStrides{12, 8};
    const std::array<RHIFormat, 2> ExpectedFormats{RHIFormat::R32G32B32_SFLOAT, RHIFormat::R32G32_SFLOAT};
    for (std::size_t Index = 0; Index < ExpectedStrides.size(); ++Index) {
        const auto& Binding = Request.VertexInputLayout.Bindings[Index];
        EXPECT_EQ(Binding.Binding, Index);
        EXPECT_EQ(Binding.Stride, ExpectedStrides[Index]);
        const auto& Attribute = Request.VertexInputLayout.Attributes[Index];
        EXPECT_EQ(Attribute.Location, Index);
        EXPECT_EQ(Attribute.Binding, Index);
        EXPECT_EQ(Attribute.Format, ExpectedFormats[Index]);
        EXPECT_EQ(Attribute.Offset, 0u);
    }

    const auto Resources = MakeResources();
    ASSERT_TRUE(PreparePipeline<VertexInputGeometryPass>());
    ASSERT_TRUE(PreparePipeline<VertexInputSelectionMaskPass>());
    RenderGraphBuilder Graph;

    const auto Albedo     = Graph.Import(Resources.Albedo);
    const auto Normal     = Graph.Import(Resources.Normal);
    const auto MaterialId = Graph.Import(Resources.MaterialId);
    const auto EntityId   = Graph.Import(Resources.EntityId);
    const auto Depth      = Graph.Import(Resources.Depth);
    const auto Instances  = Graph.Import(Resources.Instances);
    const auto Indirect   = Graph.Import(Resources.Indirect);
    const auto Materials  = Graph.Import(Resources.Materials);
    const auto Frame      = Graph.Import(Resources.Frame);
    const auto View       = Graph.Import(Resources.View);
    const auto Mask       = Graph.Import(Resources.Mask);

    const std::array AttributeBuffers{
        Resources.Position0,
        Resources.Normal0,
        Resources.Tangent0,
        Resources.TexCoord0,
        Resources.Position1,
        Resources.Normal1,
        Resources.Tangent1,
        Resources.TexCoord1,
    };
    std::vector<RGBufferView<RHIBufferUsage::Vertex, RHIAccess::Read>> MainVertexViews;
    for (const auto& Buffer : AttributeBuffers) {
        MainVertexViews.emplace_back(Graph.Import(Buffer));
    }
    std::vector<RGBufferView<RHIBufferUsage::Vertex, RHIAccess::Read>> SelectionVertexViews{
        MainVertexViews[0],
        MainVertexViews[3],
        MainVertexViews[4],
        MainVertexViews[7],
    };
    std::vector<RGBufferView<RHIBufferUsage::Index, RHIAccess::Read>> IndexViews{
        Graph.Import(Resources.Index0),
        Graph.Import(Resources.Index1),
    };

    Graph.AddPass<VertexInputGeometryPass>({
        .Albedo             = Albedo,
        .Normal             = Normal,
        .MaterialId         = MaterialId,
        .EntityId           = EntityId,
        .Depth              = Depth,
        .InstanceBuffer     = Instances,
        .IndirectBuffers    = MakeIndirectViews(Indirect),
        .MaterialBuffer     = Materials,
        .FrameBuffer        = Frame,
        .ViewBuffer         = View,
        .VertexBuffers      = MainVertexViews,
        .IndexBuffers       = IndexViews,
        .LinearSampler      = Resources.Sampler,
        .AnisotropicSampler = Resources.Sampler,
        .DrawCount          = 2,
    });
    Graph.AddPass<VertexInputSelectionMaskPass>({
        .Mask                    = Mask,
        .SelectedInstances       = Instances,
        .SelectedIndirectBuffers = MakeIndirectViews(Indirect),
        .MaterialTable           = Materials,
        .FrameBuffer             = Frame,
        .ViewBuffer              = View,
        .VertexBuffers           = SelectionVertexViews,
        .IndexBuffers            = IndexViews,
        .LinearSampler           = Resources.Sampler,
        .DrawCount               = 2,
    });

    auto List = Graph.Compile();
    ASSERT_TRUE(List.has_value()) << List.error().ToString();
    ASSERT_EQ(List->Entries.size(), 2u);
    auto& GeometryEntry  = List->Entries[0];
    auto& SelectionEntry = List->Entries[1];
    EXPECT_EQ(GeometryEntry.Pass->GetName(), VertexInputGeometryPass::Name);
    EXPECT_EQ(SelectionEntry.Pass->GetName(), VertexInputSelectionMaskPass::Name);

    const std::array SelectionBuffers{
        Resources.Position0, Resources.TexCoord0, Resources.Position1, Resources.TexCoord1};
    for (const auto& Buffer : SelectionBuffers) {
        const auto* Transition = FindTransition(SelectionEntry.Prologue, Buffer);
        ASSERT_NE(Transition, nullptr) << Buffer->GetName();
        EXPECT_EQ(Transition->Usage, RHIBufferUsage::Vertex);
        EXPECT_EQ(Transition->Access, RHIAccess::Read);
        EXPECT_EQ(Transition->Stage, RHISyncStage::VertexInput);
    }
    EXPECT_EQ(FindTransition(SelectionEntry.Prologue, Resources.Normal0), nullptr);
    EXPECT_EQ(FindTransition(SelectionEntry.Prologue, Resources.Tangent0), nullptr);

    ASSERT_TRUE(GeometryEntry.Pass->Record().has_value());
    const auto SelectionRecorded = SelectionEntry.Pass->Record();
    ASSERT_TRUE(SelectionRecorded.has_value()) << SelectionRecorded.error().ToString();

    const auto BindingSet = SelectionEntry.Pass->GetPipeline()->GetShaderBindingSet();
    ASSERT_TRUE(BindingSet);
    bool HasInstances     = false;
    bool HasMaterials     = false;
    bool HasLinearSampler = false;
    for (const auto& Set : BindingSet->GetBindingsBySet()) {
        for (const auto& Slot : Set) {
            if (Slot.Info.ParameterPath.find("geometryTable") != StringView::npos) {
                EXPECT_TRUE(std::holds_alternative<std::monostate>(Slot.Bound)) << Slot.Info.ParameterPath;
            }
            if (Slot.Info.ParameterPath == "g_selectionMaskDraw.instances") {
                const auto* Bound = std::get_if<RHIBufferView>(&Slot.Bound);
                ASSERT_NE(Bound, nullptr);
                EXPECT_EQ(Bound->GetBuffer().TryGet(), Resources.Instances.TryGet());
                HasInstances = true;
            }
            if (Slot.Info.ParameterPath == "g_selectionMaskDraw.materials") {
                const auto* Bound = std::get_if<RHIBufferView>(&Slot.Bound);
                ASSERT_NE(Bound, nullptr);
                EXPECT_EQ(Bound->GetBuffer().TryGet(), Resources.Materials.TryGet());
                HasMaterials = true;
            }
            HasLinearSampler |= Slot.Info.ParameterPath == "g_selectionMaskFrameView.linearSampler" &&
                                std::holds_alternative<RHISamplerView>(Slot.Bound);
        }
    }
    EXPECT_TRUE(HasInstances);
    EXPECT_TRUE(HasMaterials);
    EXPECT_TRUE(HasLinearSampler);

    const auto Commands = SelectionEntry.Pass->GetCommands();
    ASSERT_EQ(Commands.size(), 4u);
    for (std::size_t Slot = 0; Slot < 2; ++Slot) {
        const auto* Draw = std::get_if<RHIDrawIndexedIndirectCmd>(&Commands[Slot + 2]);
        ASSERT_NE(Draw, nullptr);
        ASSERT_EQ(Draw->VertexBuffers.size(), 2u);
        EXPECT_EQ(Draw->VertexBuffers[0].GetBuffer().TryGet(), SelectionBuffers[Slot * 2].TryGet());
        EXPECT_EQ(Draw->VertexBuffers[1].GetBuffer().TryGet(), SelectionBuffers[Slot * 2 + 1].TryGet());
        for (std::size_t Stream = 0; Stream < 2; ++Stream) {
            EXPECT_EQ(Draw->VertexBuffers[Stream].GetOffset(), 0u);
            EXPECT_EQ(Draw->VertexBuffers[Stream].GetUsage(), RHIBufferUsage::Vertex);
            EXPECT_EQ(Draw->VertexBuffers[Stream].GetAccess(), RHIAccess::Read);
        }
        EXPECT_EQ(Draw->IndexBuffer.GetBuffer().TryGet(),
                  Slot == 0 ? Resources.Index0.TryGet() : Resources.Index1.TryGet());
        EXPECT_EQ(Draw->IndirectBuffer.GetOffset(), Slot * 20);
        EXPECT_EQ(Draw->IndirectBuffer.GetRange(), 20u);
        EXPECT_EQ(Draw->DrawCount, 1u);
    }
}

TEST_F(RasterIndexedPassTest, VertexInputPipelineUsesFourStreamLayout) {
    const auto Request = VertexInputGeometryPass::BuildPipelineRequest();
    EXPECT_EQ(Request.VertEntry.EntryPoint, "vertVertexInputMain");
    ASSERT_EQ(Request.VertexInputLayout.Bindings.size(), 4u);
    ASSERT_EQ(Request.VertexInputLayout.Attributes.size(), 4u);

    const std::array<Uint32, 4>    ExpectedStrides{12, 12, 16, 8};
    const std::array<RHIFormat, 4> ExpectedFormats{
        RHIFormat::R32G32B32_SFLOAT,
        RHIFormat::R32G32B32_SFLOAT,
        RHIFormat::R32G32B32A32_SFLOAT,
        RHIFormat::R32G32_SFLOAT,
    };
    for (std::size_t Index = 0; Index < ExpectedStrides.size(); ++Index) {
        const auto& Binding = Request.VertexInputLayout.Bindings[Index];
        EXPECT_EQ(Binding.Binding, Index);
        EXPECT_EQ(Binding.Stride, ExpectedStrides[Index]);
        const auto& Attribute = Request.VertexInputLayout.Attributes[Index];
        EXPECT_EQ(Attribute.Location, Index);
        EXPECT_EQ(Attribute.Binding, Index);
        EXPECT_EQ(Attribute.Format, ExpectedFormats[Index]);
        EXPECT_EQ(Attribute.Offset, 0u);
    }
}

TEST_F(RasterIndexedPassTest, RenderSelectsMatchingGeometryAndSelectionPaths) {
    auto Geometry                   = SPtr<GeometryRecord>(new GeometryRecord);
    Geometry->Name                  = "RenderPathGeometry";
    Geometry->BufferUploadRequested = true;
    Geometry->VertexCount           = 3;
    Geometry->PositionStride        = sizeof(Float32) * 3;
    Geometry->IndexCount            = 3;
    Geometry->PositionBuffer = MakeBuffer("RenderPathPosition", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
    Geometry->NormalBuffer   = MakeBuffer("RenderPathNormal", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
    Geometry->TangentBuffer  = MakeBuffer("RenderPathTangent", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
    Geometry->TexCoordBuffer = MakeBuffer("RenderPathTexCoord", 64, RHIBufferUsage::Vertex | RHIBufferUsage::Storage);
    Geometry->IndexBuffer    = MakeBuffer("RenderPathIndex", 64, RHIBufferUsage::Index | RHIBufferUsage::Storage);
    ASSERT_TRUE(Geometry->IsReadyOnGpu(false, true));

    GameSnapshot Scene;
    Scene.Instances   = {InstanceRecord{.EntityId = 42, .Geometry = GeometryHandle{Geometry}}};
    const auto Camera = CameraViewRecord{.Viewport = CameraViewport{.Width = 8, .Height = 8}};
    Scene.Views       = {Camera};
    EditorSnapshot Editor;
    Editor.Views          = {EditorViewRecord{.Camera = Camera}};
    Editor.SelectedEntity = static_cast<entt::entity>(42);

    ASSERT_TRUE(PreparePipeline<CullingPass>());
    ASSERT_TRUE(PreparePipeline<SelectionFilterPass>());
    ASSERT_TRUE(PreparePipeline<GeometryPass>());
    ASSERT_TRUE(PreparePipeline<VertexInputGeometryPass>());
    ASSERT_TRUE(PreparePipeline<DeferredLightingPass>());
    ASSERT_TRUE(PreparePipeline<SelectionMaskPass>());
    ASSERT_TRUE(PreparePipeline<VertexInputSelectionMaskPass>());
    ASSERT_TRUE(PreparePipeline<SelectionOutlinePass>());

    RasterRenderer Renderer;
    ASSERT_TRUE(Renderer.OnAttach().has_value());
    constexpr StringView CVarName{"Render.Raster.GeometrySubmissionPath"};
    const auto           Original = CVarManager::Get().Find<String>(CVarName);
    ASSERT_TRUE(Original.has_value());
    struct RestoreSubmissionPath {
        String Value;
        ~RestoreSubmissionPath() {
            CVarManager::Get().SetByName<String>("Render.Raster.GeometrySubmissionPath", Value);
        }
    } Restore{*Original};

    const auto CountPass = [](const RenderResult& Result, StringView Name) -> Uint32 {
        Uint32 Count = 0;
        for (const auto& Entry : Result.CmdList.Entries) {
            Count += Entry.Pass->GetName() == Name;
        }
        return Count;
    };

    ASSERT_TRUE(CVarManager::Get().SetByName<String>(CVarName, "VertexInput"));
    auto VertexInputResult = Renderer.Render(Scene, Editor);
    ASSERT_TRUE(VertexInputResult.has_value()) << VertexInputResult.error().ToString();
    EXPECT_EQ(CountPass(*VertexInputResult, VertexInputGeometryPass::Name), 2u);
    EXPECT_EQ(CountPass(*VertexInputResult, VertexInputSelectionMaskPass::Name), 1u);
    EXPECT_EQ(CountPass(*VertexInputResult, GeometryPass::Name), 0u);
    EXPECT_EQ(CountPass(*VertexInputResult, SelectionMaskPass::Name), 0u);

    ASSERT_TRUE(CVarManager::Get().SetByName<String>(CVarName, "VertexPulling"));
    auto VertexPullingResult = Renderer.Render(Scene, Editor);
    ASSERT_TRUE(VertexPullingResult.has_value()) << VertexPullingResult.error().ToString();
    EXPECT_EQ(CountPass(*VertexPullingResult, GeometryPass::Name), 2u);
    EXPECT_EQ(CountPass(*VertexPullingResult, SelectionMaskPass::Name), 1u);
    EXPECT_EQ(CountPass(*VertexPullingResult, VertexInputGeometryPass::Name), 0u);
    EXPECT_EQ(CountPass(*VertexPullingResult, VertexInputSelectionMaskPass::Name), 0u);
}

TEST_F(RasterIndexedPassTest, GeometryUploaderPreservesExistingAttributeStreams) {
    GeometryRecord Record;
    Record.Name        = "CompleteAttributes";
    Record.HasTangents = true;
    Record.HasUV0      = true;
    Record.Positions.resize(3);
    Record.Normals.resize(3);
    Record.Tangents.resize(3);
    Record.UVs.resize(3);
    Record.Indices = {0, 1, 2};

    GeometryUploader Uploader;
    ASSERT_TRUE(Uploader.Upload(Record, false).has_value());
    const auto* OriginalTangent  = Record.TangentBuffer.TryGet();
    const auto* OriginalTexCoord = Record.TexCoordBuffer.TryGet();
    ASSERT_TRUE(Uploader.Upload(Record, false, true).has_value());
    EXPECT_EQ(Record.TangentBuffer.TryGet(), OriginalTangent);
    EXPECT_EQ(Record.TexCoordBuffer.TryGet(), OriginalTexCoord);

    const auto GpuData = Record.BuildGpuData();
    EXPECT_EQ(GpuData.TangentAddress, OriginalTangent->GetDeviceAddress());
    EXPECT_EQ(GpuData.TexCoordAddress, OriginalTexCoord->GetDeviceAddress());
}

TEST_F(RasterIndexedPassTest, GeometryUploaderCreatesVertexInputDefaultsWithoutChangingVertexPullingAbi) {
    GeometryRecord Record;
    Record.Name = "MissingAttributes";
    Record.Positions.resize(3);
    Record.Normals.resize(3);
    Record.Indices = {0, 1, 2};

    GeometryUploader Uploader;
    ASSERT_TRUE(Uploader.Upload(Record, false).has_value());
    EXPECT_FALSE(Record.TangentBuffer);
    EXPECT_FALSE(Record.TexCoordBuffer);
    EXPECT_TRUE(Record.IsReadyOnGpu(false));
    EXPECT_FALSE(Record.IsReadyOnGpu(false, true));
    EXPECT_TRUE(Record.Positions.empty());
    EXPECT_TRUE(Record.Normals.empty());
    EXPECT_TRUE(Record.Indices.empty());

    const auto* OriginalPosition = Record.PositionBuffer.TryGet();
    ASSERT_TRUE(Uploader.Upload(Record, false, true).has_value());
    ASSERT_TRUE(Record.TangentBuffer);
    ASSERT_TRUE(Record.TexCoordBuffer);
    EXPECT_TRUE(Record.IsReadyOnGpu(false, true));
    EXPECT_EQ(Record.PositionBuffer.TryGet(), OriginalPosition);

    const auto* Tangent  = static_cast<const MockRasterTestBuffer*>(Record.TangentBuffer.TryGet());
    const auto* TexCoord = static_cast<const MockRasterTestBuffer*>(Record.TexCoordBuffer.TryGet());
    ASSERT_NE(Tangent->GetDeviceAddress(), 0u);
    ASSERT_NE(TexCoord->GetDeviceAddress(), 0u);
    ASSERT_EQ(Tangent->GetSize(), 3 * sizeof(hlslpp::interop::float4));
    ASSERT_EQ(TexCoord->GetSize(), 3 * sizeof(hlslpp::interop::float2));
    for (std::size_t Vertex = 0; Vertex < 3; ++Vertex) {
        std::array<Float32, 4> TangentValue{};
        std::array<Float32, 2> UvValue{};
        std::memcpy(
            TangentValue.data(), Tangent->GetBytes().data() + Vertex * sizeof(TangentValue), sizeof(TangentValue));
        std::memcpy(UvValue.data(), TexCoord->GetBytes().data() + Vertex * sizeof(UvValue), sizeof(UvValue));
        EXPECT_EQ(TangentValue, (std::array<Float32, 4>{1.0f, 0.0f, 0.0f, 1.0f}));
        EXPECT_EQ(UvValue, (std::array<Float32, 2>{0.0f, 0.0f}));
    }

    const auto GpuData = Record.BuildGpuData();
    EXPECT_EQ(GpuData.TangentAddress, 0u);
    EXPECT_EQ(GpuData.TexCoordAddress, 0u);

    ASSERT_TRUE(Uploader.Upload(Record, false, true).has_value());
    EXPECT_EQ(Record.TangentBuffer.TryGet(), Tangent);
    EXPECT_EQ(Record.TexCoordBuffer.TryGet(), TexCoord);
}
