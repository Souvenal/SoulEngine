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

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

/// @brief Central resource manager facade.
///
/// Public request/query entry point over the ResourceContext-owned registry.
/// ResourceContext owns entries, slots, payloads, and lifetime policy state;
/// Manager keeps that ownership model out of normal runtime call sites.
class Manager : public Singleton<Manager> {
    friend class Singleton<Manager>;

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
    [[nodiscard]] auto RequestSampledTextureRef(StringView TexturePath) -> ResourceRef<RHI::SampledTexture> {
        return AcquireResourceRef(m_Context, SubmitSampledTextureRequest(m_Context, TexturePath));
    }

    /// @brief Request graphics pipeline and retain an owner ref.
    [[nodiscard]] auto RequestGraphicsPipelineRef(const GraphicsPipelineRequest& Req) -> ResourceRef<RHI::GraphicsPipeline> {
        return AcquireResourceRef(m_Context, SubmitGraphicsPipelineRequest(m_Context, Req));
    }

    /// @brief Request ray-tracing pipeline and retain an owner ref.
    [[nodiscard]] auto RequestRayTracingPipelineRef(const RayTracingPipelineRequest& Req)
        -> ResourceRef<RHI::RayTracingPipeline> {
        return AcquireResourceRef(m_Context, SubmitRayTracingPipelineRequest(m_Context, Req));
    }

    /// @brief Request vertex buffer and retain an owner ref.
    [[nodiscard]] auto RequestVertexBufferRef(String Key, const RHI::VertexBufferDesc& Desc) -> ResourceRef<RHI::VertexBuffer> {
        return AcquireResourceRef(m_Context, SubmitVertexBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request index buffer and retain an owner ref.
    [[nodiscard]] auto RequestIndexBufferRef(String Key, const RHI::IndexBufferDesc& Desc) -> ResourceRef<RHI::IndexBuffer> {
        return AcquireResourceRef(m_Context, SubmitIndexBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request render target and retain an owner ref.
    [[nodiscard]] auto RequestRenderTargetRef(String Key, const RHI::RenderTargetDesc& Desc)
        -> ResourceRef<RHI::RenderTarget> {
        return AcquireResourceRef(m_Context, SubmitRenderTargetRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request constant buffer and retain an owner ref.
    [[nodiscard]] auto RequestConstantBufferRef(String Key, const RHI::ConstantBufferDesc& Desc)
        -> ResourceRef<RHI::ConstantBuffer> {
        return AcquireResourceRef(m_Context, SubmitConstantBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request sampler state and retain an owner ref.
    [[nodiscard]] auto RequestSamplerRef(const RHI::SamplerDesc& Desc) -> ResourceRef<RHI::Sampler> {
        return AcquireResourceRef(m_Context, SubmitSamplerRequest(m_Context, Desc));
    }

    /// @brief Request a mesh asset and retain its logical owner ref.
    [[nodiscard]] auto RequestMeshRef(StringView MeshPath) -> ResourceRef<Mesh> {
        return AcquireResourceRef(m_Context, SubmitMeshRequest(m_Context, MeshPath));
    }

    /// @brief Request a mesh-derived reusable BLAS and retain its logical owner ref.
    [[nodiscard]] auto RequestBottomLevelAccelerationStructureRef(
        const ResourceRef<Mesh>& MeshRef,
        const BottomLevelAccelerationStructureRequest& Request = {}) -> ResourceRef<BottomLevelAccelerationStructure> {
        if (!MeshRef)
            return {};
        return AcquireResourceRef(
            m_Context,
            SubmitBottomLevelAccelerationStructureRequest(m_Context, MeshRef.GetHandle(), Request));
    }

    /// @brief Request a persistent renderer-scoped TLAS allocation and retain its owner ref.
    [[nodiscard]] auto RequestTopLevelAccelerationStructureRef(StringView ScopeKey,
                                                                const RHI::TopLevelAccelerationStructureDesc& Desc)
        -> ResourceRef<TopLevelAccelerationStructure> {
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
    Manager()  = default;
    ~Manager() = default;

    ResourceContext m_Context = {};
};

} // namespace SoulEngine::Resource
