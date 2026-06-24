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

    /// @brief Request asynchronous texture load and GPU upload.
    [[nodiscard]] auto RequestTexture(StringView TexturePath) -> TextureHandle {
        const auto Key = NormalizeResourcePath(TexturePath);
        const auto Id  = static_cast<ResourceId>(std::hash<String>{}(Key));

        if (IsShutdownRequested()) {
            LogWarning("Texture request rejected after shutdown '{}'", Key);
            return {};
        }

        SPtr<ResourceSlot<TextureResource>> Slot;
        ResourceGeneration                  Generation = 0;
        {
            std::lock_guard Lock(m_TextureMutex);
            if (auto It = m_TextureRequests.find(Key); It != m_TextureRequests.end()) {
                Slot       = It->second;
                Generation = Slot->GetGeneration();
                LogDebug("Texture request coalesced '{}'", Key);
                return TextureHandle::Create(Slot, Id, Generation);
            }

            Slot       = std::make_shared<ResourceSlot<TextureResource>>();
            Generation = Slot->Reset(Id);
            m_TextureRequests.emplace(Key, Slot);
        }

        LogDebug("Texture requested '{}'", Key);

        auto* Graph = m_TaskGraph;
        if (!Graph) {
            PublishTextureFailed(
                Slot, Generation, Key, ErrorMessage(Format("Resource manager not initialized for '{}'", Key)));
            return TextureHandle::Create(Slot, Id, Generation);
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

                RHI::TextureDesc Desc{
                    .Data     = Decoded.Pixels.data(),
                    .Width    = Decoded.Width,
                    .Height   = Decoded.Height,
                    .Channels = 4,
                    .Format   = RHI::Format::R8G8B8A8_UNORM,
                    .Usage    = RHI::TextureUsage::ShaderResource,
                };

                auto TexResult = RHI::RenderDevice::Get().CreateTexture(Desc);
                if (!TexResult) {
                    PublishTextureFailed(
                        Slot,
                        Generation,
                        Key,
                        TexResult.error().Append(Format("Failed to create GPU texture for '{}'", Key)));
                    return;
                }

                PublishTextureReady(Slot,
                                    Generation,
                                    Key,
                                    TextureResource{.Texture = std::move(*TexResult)},
                                    Decoded.Width,
                                    Decoded.Height);
            });
        });

        return TextureHandle::Create(Slot, Id, Generation);
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

    /// @brief Clear all cached resources. Call during shutdown.
    auto Clear() -> void {
        {
            std::lock_guard Lock(m_TextureMutex);
            m_TextureCache.clear();
            m_TextureRequests.clear();
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

    auto PublishTextureReady(SPtr<ResourceSlot<TextureResource>> Slot,
                             ResourceGeneration                  Generation,
                             StringView                          Key,
                             TextureResource                     Resource,
                             Uint32                              Width,
                             Uint32                              Height) -> void {
        std::lock_guard Lock(m_PublishMutex);
        if (IsShutdownRequested()) {
            LogDebug("Async texture ready discarded after shutdown '{}'", Key);
            return;
        }

        if (!Slot->PublishReady(Generation, std::move(Resource))) {
            LogDebug("Stale async texture ready discarded '{}'", Key);
            return;
        }

        LogInfo("Async texture ready '{}' ({}x{})", Key, Width, Height);
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

    auto PublishTextureFailed(SPtr<ResourceSlot<TextureResource>> Slot,
                              ResourceGeneration                  Generation,
                              StringView                          Key,
                              ErrorMessage                        Error) -> void {
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
    std::unordered_map<Uint64, SPtr<RHI::Texture>>                           m_TextureCache;
    std::unordered_map<String, SPtr<ResourceSlot<TextureResource>>>          m_TextureRequests;
    std::mutex                                                               m_PipelineMutex;
    std::unordered_map<String, SPtr<ResourceSlot<GraphicsPipelineResource>>> m_PipelineRequests;
    TaskGraph*                                                               m_TaskGraph         = nullptr;
    std::atomic<bool>                                                        m_ShutdownRequested = false;
    std::mutex                                                               m_PublishMutex;
};

} // namespace SoulEngine::Resource
