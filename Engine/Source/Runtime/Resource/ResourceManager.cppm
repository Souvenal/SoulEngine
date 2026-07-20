export module Resource:Manager;

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

/// @brief Move-only logical owner for a resource request.
///
/// `ResourceRef` expresses that a runtime system still wants the resource.
/// Ready payload observer pointers are resolved through `Resource::Manager`;
/// this type owns logical demand only.
template <ManagedResource T>
class ResourceRef {
  public:
    ResourceRef() = default;

    ResourceRef(const ResourceRef&)                    = delete;
    auto operator=(const ResourceRef&) -> ResourceRef& = delete;

    ResourceRef(ResourceRef&& Other) noexcept {
        m_Context = std::exchange(Other.m_Context, nullptr);
        m_Handle  = std::exchange(Other.m_Handle, {});
    }

    auto operator=(ResourceRef&& Other) noexcept -> ResourceRef& {
        if (this != &Other) {
            Reset();
            m_Context = std::exchange(Other.m_Context, nullptr);
            m_Handle  = std::exchange(Other.m_Handle, {});
        }
        return *this;
    }

    ~ResourceRef() {
        Reset();
    }

    [[nodiscard]] explicit operator bool() const {
        return m_Handle.IsValid();
    }

    [[nodiscard]] auto GetHandle() const -> const ResourceHandle<T>& {
        return m_Handle;
    }

    auto Reset() -> void {
        if (!m_Context || !m_Handle.IsValid())
            return;

        m_Context->ReleaseRef(m_Handle);
        m_Context = nullptr;
        m_Handle  = {};
    }

  private:
    friend class Manager;

    // Private construction retains logical demand; if the context rejects the
    // handle, the ref stays empty.
    explicit ResourceRef(ResourceContext& Context, ResourceHandle<T> Handle) {
        if (!Context.AddRef(Handle))
            return;

        m_Context = &Context;
        m_Handle  = std::move(Handle);
    }

    // Non-owning ResourceContext observer; ResourceRef owns logical demand only.
    ResourceContext*  m_Context = nullptr;
    ResourceHandle<T> m_Handle  = {};
};

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
        return ResourceRef<RHI::SampledTexture>(m_Context, SubmitSampledTextureRequest(m_Context, TexturePath));
    }

    /// @brief Request graphics pipeline and retain an owner ref.
    [[nodiscard]] auto RequestGraphicsPipelineRef(const GraphicsPipelineRequest& Req) -> ResourceRef<RHI::GraphicsPipeline> {
        return ResourceRef<RHI::GraphicsPipeline>(m_Context, SubmitGraphicsPipelineRequest(m_Context, Req));
    }

    /// @brief Request vertex buffer and retain an owner ref.
    [[nodiscard]] auto RequestVertexBufferRef(String Key, const RHI::VertexBufferDesc& Desc) -> ResourceRef<RHI::VertexBuffer> {
        return ResourceRef<RHI::VertexBuffer>(m_Context, SubmitVertexBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request index buffer and retain an owner ref.
    [[nodiscard]] auto RequestIndexBufferRef(String Key, const RHI::IndexBufferDesc& Desc) -> ResourceRef<RHI::IndexBuffer> {
        return ResourceRef<RHI::IndexBuffer>(m_Context, SubmitIndexBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request render target and retain an owner ref.
    [[nodiscard]] auto RequestRenderTargetRef(String Key, const RHI::RenderTargetDesc& Desc)
        -> ResourceRef<RHI::RenderTarget> {
        return ResourceRef<RHI::RenderTarget>(m_Context, SubmitRenderTargetRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request constant buffer and retain an owner ref.
    [[nodiscard]] auto RequestConstantBufferRef(String Key, const RHI::ConstantBufferDesc& Desc)
        -> ResourceRef<RHI::ConstantBuffer> {
        return ResourceRef<RHI::ConstantBuffer>(m_Context, SubmitConstantBufferRequest(m_Context, std::move(Key), Desc));
    }

    /// @brief Request sampler state and retain an owner ref.
    [[nodiscard]] auto RequestSamplerRef(const RHI::SamplerDesc& Desc) -> ResourceRef<RHI::Sampler> {
        return ResourceRef<RHI::Sampler>(m_Context, SubmitSamplerRequest(m_Context, Desc));
    }

    /// @brief Request a mesh asset and retain its logical owner ref.
    [[nodiscard]] auto RequestMeshRef(StringView MeshPath) -> ResourceRef<Mesh> {
        return ResourceRef<Mesh>(m_Context, SubmitMeshRequest(m_Context, MeshPath));
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
