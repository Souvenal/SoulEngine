module;

#include <stb_image.h>

export module Resource:Manager;

export import Core;
export import RHI;
export import :Types;
import ShaderCompiler;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

struct DecodedTexture {
    std::vector<Uint8> Pixels;
    Uint32             Width  = 0;
    Uint32             Height = 0;
};

[[nodiscard]] auto NormalizeResourcePath(StringView InPath) -> String {
    return Path(String(InPath)).lexically_normal().string();
}

[[nodiscard]] auto MakePipelineKey(const GraphicsPipelineRequest& Req) -> String {
    String Key = Format("vp={}:{}:{}|fp={}:{}:{}|topo={}|cf={}|df={}|rs={}:{}:{}|ds={}:{}",
                        Req.VertEntry.SourcePath.lexically_normal().string(),
                        Req.VertEntry.EntryPoint,
                        static_cast<Uint8>(Req.VertEntry.Backend),
                        Req.FragEntry.SourcePath.lexically_normal().string(),
                        Req.FragEntry.EntryPoint,
                        static_cast<Uint8>(Req.FragEntry.Backend),
                        static_cast<Uint8>(Req.Topology),
                        static_cast<Uint8>(Req.ColorFormat),
                        static_cast<Uint8>(Req.DepthFormat),
                        Req.Rasterizer.FillMode,
                        Req.Rasterizer.CullMode,
                        Req.Rasterizer.LineWidth,
                        Req.DepthStencil.DepthTestEnable,
                        Req.DepthStencil.DepthWriteEnable);

    Key += Format("|vbind={}:{}", Req.VertexInputLayout.Binding, Req.VertexInputLayout.Stride);
    for (const auto& Attribute : Req.VertexInputLayout.Attributes)
        Key += Format("|attr={}:{}:{}", Attribute.Location, static_cast<Uint8>(Attribute.Format), Attribute.Offset);

    for (const auto& Attachment : Req.Blend.Attachments)
        Key += Format("|blend={}", Attachment.BlendEnable);

    return Key;
}

struct PreparedGraphicsPipeline {
    RHI::GraphicsPipelineDesc Desc = {};
};

struct GpuPendingSampledTexture {
    SPtr<ResourceSlot<SampledTextureResource>> Slot             = nullptr;
    ResourceGeneration                         Generation       = 0;
    String                                     Key              = {};
    SampledTextureResource                     Resource         = {};
    Uint32                                     Width            = 0;
    Uint32                                     Height           = 0;
    RHI::GpuCompletionToken                    UploadCompletion = {};
};

struct GpuPendingVertexBuffer {
    SPtr<ResourceSlot<VertexBufferResource>> Slot             = nullptr;
    ResourceGeneration                       Generation       = 0;
    String                                   Key              = {};
    VertexBufferResource                     Resource         = {};
    RHI::GpuCompletionToken                  UploadCompletion = {};
};

struct GpuPendingIndexBuffer {
    SPtr<ResourceSlot<IndexBufferResource>> Slot             = nullptr;
    ResourceGeneration                      Generation       = 0;
    String                                  Key              = {};
    IndexBufferResource                     Resource         = {};
    RHI::GpuCompletionToken                 UploadCompletion = {};
};

/// @brief Central resource manager — singleton.
///
/// Loads assets from disk, uploads to GPU, and caches by path hash.
/// CPU pixel data is released immediately after GPU upload.
class Manager : public Singleton<Manager> {
    friend class Singleton<Manager>;

  public:
    /// @brief Attach engine task graph for asynchronous resource work.
    auto Init(TaskGraph& InTaskGraph) -> void {
        std::lock_guard Lock(m_PublishMutex);
        m_ShutdownRequested.store(false, std::memory_order_release);
        m_TaskGraph = &InTaskGraph;
    }

    /// @brief Stop accepting work and make pending callbacks discard results.
    auto BeginShutdown() -> void {
        std::lock_guard Lock(m_PublishMutex);
        m_ShutdownRequested.store(true, std::memory_order_release);
        LogDebug("Resource manager shutdown requested");
    }

    /// @brief Request asynchronous sampled texture load and GPU upload.
    [[nodiscard]] auto RequestSampledTexture(StringView TexturePath) -> SampledTextureHandle {
        const auto Key = NormalizeResourcePath(TexturePath);
        const auto Id  = static_cast<ResourceId>(std::hash<String>{}(Key));

        if (IsShutdownRequested()) {
            LogWarning("Sampled texture request rejected after shutdown '{}'", Key);
            return {};
        }

        SPtr<ResourceSlot<SampledTextureResource>> Slot;
        ResourceGeneration                         Generation = 0;
        {
            std::lock_guard Lock(m_TextureMutex);
            if (auto It = m_TextureRequests.find(Key); It != m_TextureRequests.end()) {
                Slot       = It->second;
                Generation = Slot->GetGeneration();
                LogDebug("Sampled texture request coalesced '{}'", Key);
                return SampledTextureHandle::Create(Slot, Id, Generation);
            }

            Slot       = std::make_shared<ResourceSlot<SampledTextureResource>>();
            Generation = Slot->Reset(Id);
            m_TextureRequests.emplace(Key, Slot);
        }

        LogDebug("Sampled texture requested '{}'", Key);

        auto* Graph = m_TaskGraph;
        if (!Graph) {
            PublishTextureFailed(
                Slot, Generation, Key, ErrorMessage(Format("Resource manager not initialized for '{}'", Key)));
            return SampledTextureHandle::Create(Slot, Id, Generation);
        }

        Graph->EnqueueBackground([this, Graph, Slot, Generation, Key] {
            if (IsShutdownRequested()) {
                LogDebug("Async texture decode discarded after shutdown '{}'", Key);
                return;
            }

            auto DecodeResult = DecodeTexture(Key);
            if (!DecodeResult) {
                PublishTextureFailed(Slot, Generation, Key, DecodeResult.error());
                return;
            }

            if (IsShutdownRequested()) {
                LogDebug("Async texture upload discarded after shutdown '{}'", Key);
                return;
            }

            Graph->Enqueue(ThreadQueue::RHI, [this, Slot, Generation, Key, Decoded = std::move(*DecodeResult)] {
                if (IsShutdownRequested()) {
                    LogDebug("Async texture publish discarded after shutdown '{}'", Key);
                    return;
                }

                if (!Slot->MarkRhiCommitting(Generation)) {
                    LogDebug("Stale async sampled texture RHI commit discarded '{}'", Key);
                    return;
                }

                RHI::SampledTextureDesc Desc{
                    .Data     = Decoded.Pixels.data(),
                    .Width    = Decoded.Width,
                    .Height   = Decoded.Height,
                    .Channels = 4,
                    .Format   = RHI::Format::R8G8B8A8_UNORM,
                    .Usage    = RHI::TextureUsage::ShaderResource,
                };

                auto TexResult = RHI::RenderDevice::Get().CreateSampledTexture(Desc);
                if (!TexResult) {
                    PublishTextureFailed(
                        Slot,
                        Generation,
                        Key,
                        TexResult.error().Append(Format("Failed to create GPU texture for '{}'", Key)));
                    return;
                }

                PublishSampledTextureGpuPending(Slot,
                                                Generation,
                                                Key,
                                                SampledTextureResource{.Texture = std::move(TexResult->Texture)},
                                                Decoded.Width,
                                                Decoded.Height,
                                                TexResult->UploadCompletion);
            });
        });

        return SampledTextureHandle::Create(Slot, Id, Generation);
    }

    /// @brief Request asynchronous graphics pipeline compile and GPU creation.
    [[nodiscard]] auto RequestGraphicsPipeline(const GraphicsPipelineRequest& Req) -> GraphicsPipelineHandle {
        const auto Key = MakePipelineKey(Req);
        const auto Id  = static_cast<ResourceId>(std::hash<String>{}(Key));

        if (IsShutdownRequested()) {
            LogWarning("Graphics pipeline request rejected after shutdown '{}'", Key);
            return {};
        }

        SPtr<ResourceSlot<GraphicsPipelineResource>> Slot;
        ResourceGeneration                           Generation = 0;
        {
            std::lock_guard Lock(m_PipelineMutex);
            if (auto It = m_PipelineRequests.find(Key); It != m_PipelineRequests.end()) {
                Slot       = It->second;
                Generation = Slot->GetGeneration();
                LogDebug("Graphics pipeline request coalesced '{}'", Key);
                return GraphicsPipelineHandle::Create(Slot, Id, Generation);
            }

            Slot       = std::make_shared<ResourceSlot<GraphicsPipelineResource>>();
            Generation = Slot->Reset(Id);
            m_PipelineRequests.emplace(Key, Slot);
        }

        LogDebug("Graphics pipeline requested '{}'", Key);

        auto* Graph = m_TaskGraph;
        if (!Graph) {
            PublishPipelineFailed(
                Slot, Generation, Key, ErrorMessage("Resource manager not initialized for graphics pipeline"));
            return GraphicsPipelineHandle::Create(Slot, Id, Generation);
        }

        Graph->EnqueueBackground([this, Graph, Slot, Generation, Key, Req] {
            if (IsShutdownRequested()) {
                LogDebug("Async graphics pipeline compile discarded after shutdown '{}'", Key);
                return;
            }

            auto Prepared = PrepareGraphicsPipeline(Req);
            if (!Prepared) {
                PublishPipelineFailed(Slot, Generation, Key, Prepared.error());
                return;
            }

            if (IsShutdownRequested()) {
                LogDebug("Async graphics pipeline creation discarded after shutdown '{}'", Key);
                return;
            }

            Graph->Enqueue(ThreadQueue::RHI, [this, Slot, Generation, Key, Prepared = std::move(*Prepared)] {
                if (IsShutdownRequested()) {
                    LogDebug("Async graphics pipeline publish discarded after shutdown '{}'", Key);
                    return;
                }

                if (!Slot->MarkRhiCommitting(Generation)) {
                    LogDebug("Stale async graphics pipeline RHI commit discarded '{}'", Key);
                    return;
                }

                auto PipeResult = RHI::RenderDevice::Get().CreateGraphicsPipeline(Prepared.Desc);
                if (!PipeResult) {
                    PublishPipelineFailed(
                        Slot,
                        Generation,
                        Key,
                        PipeResult.error().Append(Format("Failed to create graphics pipeline '{}'", Key)));
                    return;
                }

                PublishPipelineReady(
                    Slot, Generation, Key, GraphicsPipelineResource{.Pipeline = std::move(*PipeResult)});
            });
        });

        return GraphicsPipelineHandle::Create(Slot, Id, Generation);
    }

    /// @brief Request async vertex buffer creation with GPU upload completion tracking.
    [[nodiscard]] auto RequestVertexBuffer(String Key, const RHI::VertexBufferDesc& Desc) -> VertexBufferHandle {
        const auto Id = static_cast<ResourceId>(std::hash<String>{}(Key));

        if (IsShutdownRequested()) {
            LogWarning("Vertex buffer request rejected after shutdown '{}'", Key);
            return {};
        }

        SPtr<ResourceSlot<VertexBufferResource>> Slot;
        ResourceGeneration                       Generation = 0;
        {
            std::lock_guard Lock(m_BufferMutex);
            if (auto It = m_VertexBufferRequests.find(Key); It != m_VertexBufferRequests.end()) {
                Slot       = It->second;
                Generation = Slot->GetGeneration();
                LogDebug("Vertex buffer request coalesced '{}'", Key);
                return VertexBufferHandle::Create(Slot, Id, Generation);
            }

            Slot       = std::make_shared<ResourceSlot<VertexBufferResource>>();
            Generation = Slot->Reset(Id);
            m_VertexBufferRequests.emplace(Key, Slot);
        }

        LogDebug("Vertex buffer requested '{}'", Key);

        auto* Graph = m_TaskGraph;
        if (!Graph) {
            PublishVertexBufferFailed(
                Slot, Generation, Key, ErrorMessage(Format("Resource manager not initialized for '{}'", Key)));
            return VertexBufferHandle::Create(Slot, Id, Generation);
        }

        // Copy caller's data for async RHI task
        Uint64             Size = Desc.VertexCount * Desc.Stride;
        std::vector<Uint8> DataCopy(static_cast<const Uint8*>(Desc.Data), static_cast<const Uint8*>(Desc.Data) + Size);

        Graph->Enqueue(ThreadQueue::RHI,
                       [this, Slot, Generation, Key = String(Key), Desc, DataCopy = std::move(DataCopy)] {
                           if (IsShutdownRequested()) {
                               LogDebug("Async vertex buffer RHI commit discarded after shutdown '{}'", Key);
                               return;
                           }

                           if (!Slot->MarkRhiCommitting(Generation)) {
                               LogDebug("Stale async vertex buffer RHI commit discarded '{}'", Key);
                               return;
                           }

                           RHI::VertexBufferDesc BufDesc = Desc;
                           BufDesc.Data                  = DataCopy.data();

                           auto Result = RHI::RenderDevice::Get().CreateVertexBuffer(BufDesc);
                           if (!Result) {
                               PublishVertexBufferFailed(
                                   Slot,
                                   Generation,
                                   Key,
                                   Result.error().Append(Format("Failed to create vertex buffer '{}'", Key)));
                               return;
                           }

                           PublishVertexBufferGpuPending(Slot,
                                                         Generation,
                                                         Key,
                                                         VertexBufferResource{.Buffer = std::move(Result->Buffer)},
                                                         Result->UploadCompletion);
                       });

        return VertexBufferHandle::Create(Slot, Id, Generation);
    }

    /// @brief Request async index buffer creation with GPU upload completion tracking.
    [[nodiscard]] auto RequestIndexBuffer(String Key, const RHI::IndexBufferDesc& Desc) -> IndexBufferHandle {
        const auto Id = static_cast<ResourceId>(std::hash<String>{}(Key));

        if (IsShutdownRequested()) {
            LogWarning("Index buffer request rejected after shutdown '{}'", Key);
            return {};
        }

        SPtr<ResourceSlot<IndexBufferResource>> Slot;
        ResourceGeneration                      Generation = 0;
        {
            std::lock_guard Lock(m_BufferMutex);
            if (auto It = m_IndexBufferRequests.find(Key); It != m_IndexBufferRequests.end()) {
                Slot       = It->second;
                Generation = Slot->GetGeneration();
                LogDebug("Index buffer request coalesced '{}'", Key);
                return IndexBufferHandle::Create(Slot, Id, Generation);
            }

            Slot       = std::make_shared<ResourceSlot<IndexBufferResource>>();
            Generation = Slot->Reset(Id);
            m_IndexBufferRequests.emplace(Key, Slot);
        }

        LogDebug("Index buffer requested '{}'", Key);

        auto* Graph = m_TaskGraph;
        if (!Graph) {
            PublishIndexBufferFailed(
                Slot, Generation, Key, ErrorMessage(Format("Resource manager not initialized for '{}'", Key)));
            return IndexBufferHandle::Create(Slot, Id, Generation);
        }

        // Copy caller's data for async RHI task
        Uint64             Size = Desc.IndexCount * 4ULL;
        std::vector<Uint8> DataCopy(static_cast<const Uint8*>(Desc.Data), static_cast<const Uint8*>(Desc.Data) + Size);

        Graph->Enqueue(
            ThreadQueue::RHI, [this, Slot, Generation, Key = String(Key), Desc, DataCopy = std::move(DataCopy)] {
                if (IsShutdownRequested()) {
                    LogDebug("Async index buffer RHI commit discarded after shutdown '{}'", Key);
                    return;
                }

                if (!Slot->MarkRhiCommitting(Generation)) {
                    LogDebug("Stale async index buffer RHI commit discarded '{}'", Key);
                    return;
                }

                RHI::IndexBufferDesc BufDesc = Desc;
                BufDesc.Data                 = DataCopy.data();

                auto Result = RHI::RenderDevice::Get().CreateIndexBuffer(BufDesc);
                if (!Result) {
                    PublishIndexBufferFailed(Slot,
                                             Generation,
                                             Key,
                                             Result.error().Append(Format("Failed to create index buffer '{}'", Key)));
                    return;
                }

                PublishIndexBufferGpuPending(Slot,
                                             Generation,
                                             Key,
                                             IndexBufferResource{.Buffer = std::move(Result->Buffer)},
                                             Result->UploadCompletion);
            });

        return IndexBufferHandle::Create(Slot, Id, Generation);
    }

    /// @brief Publish GPU-pending resources whose upload tokens have completed.
    auto TickGpuPending() -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            m_GpuPendingSampledTextures.clear();
            m_GpuPendingVertexBuffers.clear();
            m_GpuPendingIndexBuffers.clear();
            return;
        }

        // Rebuild instead of erasing in-place so incomplete uploads retain ownership
        // while completed or stale entries are dropped deterministically on this thread.
        {
            std::vector<GpuPendingSampledTexture> Next;
            Next.reserve(m_GpuPendingSampledTextures.size());
            for (auto& Pending : m_GpuPendingSampledTextures) {
                auto State = Pending.Slot->GetState(Pending.Generation);
                if (State == ResourceState::Stale) {
                    LogDebug("Stale async sampled texture GPU pending discarded '{}'", Pending.Key);
                    continue;
                }
                if (State != ResourceState::GpuPending) {
                    LogDebug("Async sampled texture GPU pending discarded '{}'", Pending.Key);
                    continue;
                }
                if (!RHI::RenderDevice::Get().IsGpuComplete(Pending.UploadCompletion)) {
                    Next.push_back(std::move(Pending));
                    continue;
                }
                if (!Pending.Slot->PublishReady(Pending.Generation, std::move(Pending.Resource))) {
                    LogDebug("Stale async sampled texture ready discarded '{}'", Pending.Key);
                    continue;
                }
                LogInfo("Async sampled texture ready '{}' ({}x{})", Pending.Key, Pending.Width, Pending.Height);
            }
            m_GpuPendingSampledTextures = std::move(Next);
        }

        {
            std::vector<GpuPendingVertexBuffer> Next;
            Next.reserve(m_GpuPendingVertexBuffers.size());
            for (auto& Pending : m_GpuPendingVertexBuffers) {
                auto State = Pending.Slot->GetState(Pending.Generation);
                if (State == ResourceState::Stale) {
                    LogDebug("Stale async vertex buffer GPU pending discarded '{}'", Pending.Key);
                    continue;
                }
                if (State != ResourceState::GpuPending) {
                    LogDebug("Async vertex buffer GPU pending discarded '{}'", Pending.Key);
                    continue;
                }
                if (!RHI::RenderDevice::Get().IsGpuComplete(Pending.UploadCompletion)) {
                    Next.push_back(std::move(Pending));
                    continue;
                }
                if (!Pending.Slot->PublishReady(Pending.Generation, std::move(Pending.Resource))) {
                    LogDebug("Stale async vertex buffer ready discarded '{}'", Pending.Key);
                    continue;
                }
                LogInfo("Async vertex buffer ready '{}'", Pending.Key);
            }
            m_GpuPendingVertexBuffers = std::move(Next);
        }

        {
            std::vector<GpuPendingIndexBuffer> Next;
            Next.reserve(m_GpuPendingIndexBuffers.size());
            for (auto& Pending : m_GpuPendingIndexBuffers) {
                auto State = Pending.Slot->GetState(Pending.Generation);
                if (State == ResourceState::Stale) {
                    LogDebug("Stale async index buffer GPU pending discarded '{}'", Pending.Key);
                    continue;
                }
                if (State != ResourceState::GpuPending) {
                    LogDebug("Async index buffer GPU pending discarded '{}'", Pending.Key);
                    continue;
                }
                if (!RHI::RenderDevice::Get().IsGpuComplete(Pending.UploadCompletion)) {
                    Next.push_back(std::move(Pending));
                    continue;
                }
                if (!Pending.Slot->PublishReady(Pending.Generation, std::move(Pending.Resource))) {
                    LogDebug("Stale async index buffer ready discarded '{}'", Pending.Key);
                    continue;
                }
                LogInfo("Async index buffer ready '{}'", Pending.Key);
            }
            m_GpuPendingIndexBuffers = std::move(Next);
        }
    }

    /// @brief Clear all cached resources. Call during shutdown.
    auto Clear() -> void {
        {
            std::lock_guard Lock(m_TextureMutex);
            m_TextureCache.clear();
            m_TextureRequests.clear();
        }
        {
            std::lock_guard Lock(m_BufferMutex);
            m_VertexBufferRequests.clear();
            m_IndexBufferRequests.clear();
        }
        {
            std::lock_guard Lock(m_PublishMutex);
            m_GpuPendingSampledTextures.clear();
            m_GpuPendingVertexBuffers.clear();
            m_GpuPendingIndexBuffers.clear();
        }
        {
            std::lock_guard Lock(m_PipelineMutex);
            m_PipelineRequests.clear();
        }
    }

  private:
    Manager()  = default;
    ~Manager() = default;

    [[nodiscard]] auto IsShutdownRequested() const -> bool {
        return m_ShutdownRequested.load(std::memory_order_acquire);
    }

    auto PublishSampledTextureGpuPending(SPtr<ResourceSlot<SampledTextureResource>> Slot,
                                         ResourceGeneration                         Generation,
                                         StringView                                 Key,
                                         SampledTextureResource                     Resource,
                                         Uint32                                     Width,
                                         Uint32                                     Height,
                                         RHI::GpuCompletionToken                    UploadCompletion) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async sampled texture GPU pending discarded after shutdown '{}'", Key);
            return;
        }

        if (!Slot->PublishGpuPending(Generation)) {
            LogDebug("Stale async sampled texture GPU pending discarded '{}'", Key);
            return;
        }

        m_GpuPendingSampledTextures.push_back(GpuPendingSampledTexture{
            .Slot             = std::move(Slot),
            .Generation       = Generation,
            .Key              = String(Key),
            .Resource         = std::move(Resource),
            .Width            = Width,
            .Height           = Height,
            .UploadCompletion = UploadCompletion,
        });

        LogDebug("Async sampled texture GPU pending '{}' ({}x{})", Key, Width, Height);
    }

    auto PublishPipelineReady(SPtr<ResourceSlot<GraphicsPipelineResource>> Slot,
                              ResourceGeneration                           Generation,
                              StringView                                   Key,
                              GraphicsPipelineResource                     Resource) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async graphics pipeline ready discarded after shutdown '{}'", Key);
            return;
        }

        if (!Slot->PublishReady(Generation, std::move(Resource))) {
            LogDebug("Stale async graphics pipeline ready discarded '{}'", Key);
            return;
        }

        LogInfo("Async graphics pipeline ready '{}'", Key);
    }

    auto PublishTextureFailed(SPtr<ResourceSlot<SampledTextureResource>> Slot,
                              ResourceGeneration                         Generation,
                              StringView                                 Key,
                              ErrorMessage                               Error) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async texture failure discarded after shutdown '{}'", Key);
            return;
        }

        auto Message = Error.ToString();
        if (!Slot->PublishFailed(Generation, std::move(Error))) {
            LogDebug("Stale async texture failure discarded '{}': {}", Key, Message);
            return;
        }

        LogWarning("Async texture failed '{}': {}", Key, Message);
    }

    auto PublishPipelineFailed(SPtr<ResourceSlot<GraphicsPipelineResource>> Slot,
                               ResourceGeneration                           Generation,
                               StringView                                   Key,
                               ErrorMessage                                 Error) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async graphics pipeline failure discarded after shutdown '{}'", Key);
            return;
        }

        auto Message = Error.ToString();
        if (!Slot->PublishFailed(Generation, std::move(Error))) {
            LogDebug("Stale async graphics pipeline failure discarded '{}': {}", Key, Message);
            return;
        }

        LogWarning("Async graphics pipeline failed '{}': {}", Key, Message);
    }

    auto PublishVertexBufferGpuPending(SPtr<ResourceSlot<VertexBufferResource>> Slot,
                                       ResourceGeneration                       Generation,
                                       StringView                               Key,
                                       VertexBufferResource                     Resource,
                                       RHI::GpuCompletionToken                  UploadCompletion) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async vertex buffer GPU pending discarded after shutdown '{}'", Key);
            return;
        }
        if (!Slot->PublishGpuPending(Generation)) {
            LogDebug("Stale async vertex buffer GPU pending discarded '{}'", Key);
            return;
        }
        m_GpuPendingVertexBuffers.push_back(GpuPendingVertexBuffer{
            .Slot             = std::move(Slot),
            .Generation       = Generation,
            .Key              = String(Key),
            .Resource         = std::move(Resource),
            .UploadCompletion = UploadCompletion,
        });
        LogDebug("Async vertex buffer GPU pending '{}'", Key);
    }

    auto PublishIndexBufferGpuPending(SPtr<ResourceSlot<IndexBufferResource>> Slot,
                                      ResourceGeneration                      Generation,
                                      StringView                              Key,
                                      IndexBufferResource                     Resource,
                                      RHI::GpuCompletionToken                 UploadCompletion) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async index buffer GPU pending discarded after shutdown '{}'", Key);
            return;
        }
        if (!Slot->PublishGpuPending(Generation)) {
            LogDebug("Stale async index buffer GPU pending discarded '{}'", Key);
            return;
        }
        m_GpuPendingIndexBuffers.push_back(GpuPendingIndexBuffer{
            .Slot             = std::move(Slot),
            .Generation       = Generation,
            .Key              = String(Key),
            .Resource         = std::move(Resource),
            .UploadCompletion = UploadCompletion,
        });
        LogDebug("Async index buffer GPU pending '{}'", Key);
    }

    auto PublishVertexBufferFailed(SPtr<ResourceSlot<VertexBufferResource>> Slot,
                                   ResourceGeneration                       Generation,
                                   StringView                               Key,
                                   ErrorMessage                             Error) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async vertex buffer failure discarded after shutdown '{}'", Key);
            return;
        }
        auto Message = Error.ToString();
        if (!Slot->PublishFailed(Generation, std::move(Error))) {
            LogDebug("Stale async vertex buffer failure discarded '{}': {}", Key, Message);
            return;
        }
        LogWarning("Async vertex buffer failed '{}': {}", Key, Message);
    }

    auto PublishIndexBufferFailed(SPtr<ResourceSlot<IndexBufferResource>> Slot,
                                  ResourceGeneration                      Generation,
                                  StringView                              Key,
                                  ErrorMessage                            Error) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async index buffer failure discarded after shutdown '{}'", Key);
            return;
        }
        auto Message = Error.ToString();
        if (!Slot->PublishFailed(Generation, std::move(Error))) {
            LogDebug("Stale async index buffer failure discarded '{}': {}", Key, Message);
            return;
        }
        LogWarning("Async index buffer failed '{}': {}", Key, Message);
    }

    [[nodiscard]] static auto DecodeTexture(StringView TexturePath) -> std::expected<DecodedTexture, ErrorMessage> {
        int W  = 0;
        int H  = 0;
        int Ch = 0;

        auto* Pixels = stbi_load(String(TexturePath).c_str(), &W, &H, &Ch, 4);
        if (!Pixels) {
            const char* Reason = stbi_failure_reason();
            return std::unexpected(ErrorMessage(Format(
                "stbi_load failed for '{}': {}", TexturePath, Reason ? StringView(Reason) : StringView("unknown"))));
        }

        const std::size_t  PixelByteCount = static_cast<std::size_t>(W) * static_cast<std::size_t>(H) * 4;
        std::vector<Uint8> PixelBuffer(Pixels, Pixels + PixelByteCount);
        stbi_image_free(Pixels);

        return DecodedTexture{
            .Pixels = std::move(PixelBuffer),
            .Width  = static_cast<Uint32>(W),
            .Height = static_cast<Uint32>(H),
        };
    }

    [[nodiscard]] static auto PrepareGraphicsPipeline(const GraphicsPipelineRequest& Req)
        -> std::expected<PreparedGraphicsPipeline, ErrorMessage> {
        namespace SC = SoulEngine::ShaderCompiler;

        auto Vert = SC::ShaderCompiler::Get().GetOrCompile(Req.VertEntry);
        if (!Vert)
            return std::unexpected(Vert.error().Append(Format("Graphics pipeline vertex shader '{}'/'{}'",
                                                              Req.VertEntry.SourcePath.string(),
                                                              Req.VertEntry.EntryPoint)));

        auto Frag = SC::ShaderCompiler::Get().GetOrCompile(Req.FragEntry);
        if (!Frag)
            return std::unexpected(Frag.error().Append(Format("Graphics pipeline fragment shader '{}'/'{}'",
                                                              Req.FragEntry.SourcePath.string(),
                                                              Req.FragEntry.EntryPoint)));

        return PreparedGraphicsPipeline{
            .Desc =
                RHI::GraphicsPipelineDesc{
                    .VertexProgram     = std::move(*Vert),
                    .FragmentProgram   = std::move(*Frag),
                    .VertexInputLayout = Req.VertexInputLayout,
                    .Topology          = Req.Topology,
                    .Rasterizer        = Req.Rasterizer,
                    .Blend             = Req.Blend,
                    .DepthStencil      = Req.DepthStencil,
                    .ColorFormat       = Req.ColorFormat,
                    .DepthFormat       = Req.DepthFormat,
                },
        };
    }

    std::mutex                                                               m_TextureMutex;
    std::unordered_map<Uint64, SPtr<RHI::SampledTexture>>                    m_TextureCache;
    std::unordered_map<String, SPtr<ResourceSlot<SampledTextureResource>>>   m_TextureRequests;
    std::mutex                                                               m_PipelineMutex;
    std::unordered_map<String, SPtr<ResourceSlot<GraphicsPipelineResource>>> m_PipelineRequests;
    std::mutex                                                               m_BufferMutex;
    std::unordered_map<String, SPtr<ResourceSlot<VertexBufferResource>>>     m_VertexBufferRequests;
    std::unordered_map<String, SPtr<ResourceSlot<IndexBufferResource>>>      m_IndexBufferRequests;
    TaskGraph*                                                               m_TaskGraph         = nullptr;
    std::atomic<bool>                                                        m_ShutdownRequested = false;
    std::mutex                                                               m_PublishMutex;
    std::vector<GpuPendingSampledTexture>                                    m_GpuPendingSampledTextures;
    std::vector<GpuPendingVertexBuffer>                                      m_GpuPendingVertexBuffers;
    std::vector<GpuPendingIndexBuffer>                                       m_GpuPendingIndexBuffers;
};

} // namespace SoulEngine::Resource
