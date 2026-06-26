export module Resource:Manager;

import :BufferRequests;
import :Context;
import :PipelineRequests;
export import :Ref;
import :RenderTargetRequests;
import :TextureRequests;

export import Core;
import TaskGraph;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

/// @brief Central resource manager facade.
///
/// Public request/query entry point over the ResourceContext-owned registry.
/// ResourceContext owns entries, slots, payloads, and lifetime policy state;
/// Manager keeps that ownership model out of normal runtime call sites.
class Manager : public Singleton<Manager> {
    friend class Singleton<Manager>;
    friend struct FrameResourceScope;

  public:
    /// @brief Attach engine task graph for asynchronous resource work.
    auto Init(TaskGraph& InTaskGraph) -> void {
        m_Context.Init(InTaskGraph);
    }

    /// @brief Stop accepting work and make pending callbacks discard results.
    auto BeginShutdown() -> void {
        m_Context.BeginShutdown();
    }

    /// @brief Request sampled texture and retain an owner ref.
    [[nodiscard]] auto RequestSampledTextureRef(StringView TexturePath) -> ResourceRef<RHI::SampledTexture>;

    /// @brief Request graphics pipeline and retain an owner ref.
    [[nodiscard]] auto RequestGraphicsPipelineRef(const GraphicsPipelineRequest& Req) -> ResourceRef<RHI::GraphicsPipeline>;

    /// @brief Request vertex buffer and retain an owner ref.
    [[nodiscard]] auto RequestVertexBufferRef(String Key, const RHI::VertexBufferDesc& Desc) -> ResourceRef<RHI::VertexBuffer>;

    /// @brief Request index buffer and retain an owner ref.
    [[nodiscard]] auto RequestIndexBufferRef(String Key, const RHI::IndexBufferDesc& Desc) -> ResourceRef<RHI::IndexBuffer>;

    /// @brief Request render target and retain an owner ref.
    [[nodiscard]] auto RequestRenderTargetRef(String Key, const RHI::RenderTargetDesc& Desc)
        -> ResourceRef<RHI::RenderTarget>;

    template <ManagedRHIResource T>
    [[nodiscard]] auto GetState(const ResourceHandle<T>& Handle) -> ResourceState {
        return m_Context.GetState(Handle);
    }

    template <ManagedRHIResource T>
    [[nodiscard]] auto GetError(const ResourceHandle<T>& Handle) -> std::optional<ErrorMessage> {
        return m_Context.GetError(Handle);
    }

    /// @brief Publish GPU-pending resources whose upload tokens have completed.
    auto TickGpuPending() -> void {
        m_Context.TickGpuPending();
    }

    /// @brief Clear all cached resources. Call during shutdown.
    auto Clear() -> void {
        m_Context.Clear();
    }

    /// @brief Erase released transient resources whose pins have dropped.
    auto CollectReleasedResources() -> void {
        m_Context.CollectReleasedResources();
    }

  private:
    Manager()  = default;
    ~Manager() = default;

    /// @brief Low-level pin primitive hidden behind FrameResourceScope.
    ///
    /// Keeping this private prevents render code from taking a command-list
    /// observer pointer without also storing the corresponding pin in the frame
    /// packet. `FrameResourceScope::Acquire()` is the public API for that use
    /// case because it performs both operations atomically from the caller's
    /// perspective.
    template <ManagedRHIResource T>
    [[nodiscard]] auto Pin(const ResourceHandle<T>& Handle) -> ResourcePin<T> {
        return m_Context.Pin(Handle);
    }

    ResourceContext m_Context = {};
};

auto Manager::RequestSampledTextureRef(StringView TexturePath) -> ResourceRef<RHI::SampledTexture> {
    return ResourceRef<RHI::SampledTexture>(m_Context, SubmitSampledTextureRequest(m_Context, TexturePath));
}

auto Manager::RequestGraphicsPipelineRef(const GraphicsPipelineRequest& Req) -> ResourceRef<RHI::GraphicsPipeline> {
    return ResourceRef<RHI::GraphicsPipeline>(m_Context, SubmitGraphicsPipelineRequest(m_Context, Req));
}

auto Manager::RequestVertexBufferRef(String Key, const RHI::VertexBufferDesc& Desc) -> ResourceRef<RHI::VertexBuffer> {
    return ResourceRef<RHI::VertexBuffer>(m_Context, SubmitVertexBufferRequest(m_Context, std::move(Key), Desc));
}

auto Manager::RequestIndexBufferRef(String Key, const RHI::IndexBufferDesc& Desc) -> ResourceRef<RHI::IndexBuffer> {
    return ResourceRef<RHI::IndexBuffer>(m_Context, SubmitIndexBufferRequest(m_Context, std::move(Key), Desc));
}

auto Manager::RequestRenderTargetRef(String Key, const RHI::RenderTargetDesc& Desc) -> ResourceRef<RHI::RenderTarget> {
    return ResourceRef<RHI::RenderTarget>(m_Context, SubmitRenderTargetRequest(m_Context, std::move(Key), Desc));
}

} // namespace SoulEngine::Resource
