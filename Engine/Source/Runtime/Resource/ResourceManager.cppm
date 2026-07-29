export module Resource:Manager;

import :AccelerationStructure;
import :Buffer;
import :Context;
import :Mesh;
import :Pipeline;
import :RenderTarget;
import :Sampler;
import :Texture;

export import Core;
import TaskGraph;
export import std;

export namespace SoulEngine {

/// @brief Central resource manager facade.
///
/// Public request/query entry point over the ResourceContext-owned registry.
/// ResourceContext owns entries, slots, payloads, and lifetime policy state;
/// ResourceManager keeps that ownership model out of normal runtime call sites.
class ResourceManager : public Singleton<ResourceManager> {
    friend class Singleton<ResourceManager>;

  public:
    /// @brief Start accepting resource requests.
    auto Init() -> void {
        m_Context.Init();
    }

    /// @brief Stop accepting work and make pending callbacks discard results.
    auto BeginShutdown() -> void {
        m_Context.BeginShutdown();
    }

    /// @brief Request sampled texture and retain an owner ref.
    [[nodiscard]] auto RequestSampledTextureRef(StringView TexturePath) -> ResourceRef<RHISampledTexture> {
        return AcquireResourceRef(m_Context, SubmitSampledTextureRequest(m_Context, TexturePath));
    }

    /// @brief Request graphics pipeline and retain an owner ref.
    [[nodiscard]] auto RequestGraphicsPipelineRef(const GraphicsPipelineRequest& Req) -> ResourceRef<RHIGraphicsPipeline> {
        return AcquireResourceRef(m_Context, SubmitGraphicsPipelineRequest(m_Context, Req));
    }

    /// @brief Request ray-tracing pipeline and retain an owner ref.
    [[nodiscard]] auto RequestRayTracingPipelineRef(const RayTracingPipelineRequest& Req)
        -> ResourceRef<RHIRayTracingPipeline> {
        return AcquireResourceRef(m_Context, SubmitRayTracingPipelineRequest(m_Context, Req));
    }

    /// @brief Request vertex buffer and retain an owner ref.
    [[nodiscard]] auto RequestVertexBufferRef(String Key, const RHIVertexBufferDesc& Desc) -> ResourceRef<RHIVertexBuffer> {
        return AcquireResourceRef(m_Context, SubmitVertexBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request index buffer and retain an owner ref.
    [[nodiscard]] auto RequestIndexBufferRef(String Key, const RHIIndexBufferDesc& Desc) -> ResourceRef<RHIIndexBuffer> {
        return AcquireResourceRef(m_Context, SubmitIndexBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request render target and retain an owner ref.
    [[nodiscard]] auto RequestRenderTargetRef(String Key, const RHIRenderTargetDesc& Desc)
        -> ResourceRef<RHIRenderTarget> {
        return AcquireResourceRef(m_Context, SubmitRenderTargetRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request sampler state and retain an owner ref.
    [[nodiscard]] auto RequestSamplerRef(const RHISamplerDesc& Desc) -> ResourceRef<RHISampler> {
        return AcquireResourceRef(m_Context, SubmitSamplerRequest(m_Context, Desc));
    }

    /// @brief Request a mesh asset and retain its logical owner ref.
    [[nodiscard]] auto RequestMeshRef(StringView MeshPath) -> ResourceRef<ResourceMesh> {
        return AcquireResourceRef(m_Context, SubmitMeshRequest(m_Context, MeshPath));
    }

    /// @brief Request a mesh-derived reusable BLAS and retain its logical owner ref.
    [[nodiscard]] auto RequestBottomLevelAccelerationStructureRef(
        const ResourceRef<ResourceMesh>& MeshRef,
        const BottomLevelAccelerationStructureRequest& Request = {}) -> ResourceRef<ResourceBottomLevelAccelerationStructure> {
        if (!MeshRef)
            return {};
        return AcquireResourceRef(
            m_Context,
            SubmitBottomLevelAccelerationStructureRequest(m_Context, MeshRef.GetHandle(), Request));
    }

    /// @brief Request a persistent renderer-scoped TLAS allocation and retain its owner ref.
    [[nodiscard]] auto RequestTopLevelAccelerationStructureRef(StringView ScopeKey,
                                                                const RHITopLevelAccelerationStructureDesc& Desc)
        -> ResourceRef<ResourceTopLevelAccelerationStructure> {
        return AcquireResourceRef(
            m_Context,
            SubmitTopLevelAccelerationStructureRequest(m_Context, ScopeKey, Desc));
    }

    template <ManagedResource T>
    [[nodiscard]] auto GetState(const ResourceHandle<T>& Handle) -> ResourceState {
        return m_Context.GetState(Handle);
    }

    template <ManagedResource T>
    [[nodiscard]] auto GetError(const ResourceHandle<T>& Handle) -> std::optional<ErrorMessage> {
        return m_Context.GetError(Handle);
    }

    template <ManagedResource T>
    [[nodiscard]] auto TryGetReady(const ResourceHandle<T>& Handle) -> T* {
        return m_Context.TryGetReady(Handle);
    }

    template <ManagedResource T>
    [[nodiscard]] auto TryGetReady(const ResourceRef<T>& Ref) -> T* {
        return m_Context.TryGetReady(Ref.GetHandle());
    }

    /// @brief Publish GPU-pending resources whose upload tokens have completed.
    auto TickGpuPending() -> void {
        m_Context.TickGpuPending();
    }

    /// @brief Clear all cached resources. Call during shutdown.
    auto Clear() -> void {
        m_Context.Clear();
    }

    /// @brief Erase released transient resources whose payloads have been released.
    auto CollectReleasedResources() -> void {
        m_Context.CollectReleasedResources();
    }

  private:
    ResourceManager()  = default;
    ~ResourceManager() = default;

    ResourceContext m_Context = {};
};

} // namespace SoulEngine
