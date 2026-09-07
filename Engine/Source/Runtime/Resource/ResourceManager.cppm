export module Resource:Manager;
import :AccelerationStructure;
import :Context;
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
    /// @brief Request a reusable BLAS for RHI geometry inputs.
    [[nodiscard]] auto RequestBottomLevelAccelerationStructureRef(
        StringView                                                       MeshPath,
        const std::vector<RHITriangleAccelerationStructureGeometryDesc>& Geometries,
        const BottomLevelAccelerationStructureRequest&                   Request = {})
        -> ResourceRef<ResourceBottomLevelAccelerationStructure> {
        if (MeshPath.empty() || Geometries.empty())
            return {};
        return AcquireResourceRef(m_Context,
                                  RequestBottomLevelAccelerationStructure(m_Context, MeshPath, Geometries, Request));
    }
    /// @brief Request a persistent renderer-scoped TLAS allocation and retain its owner ref.
    [[nodiscard]] auto RequestTopLevelAccelerationStructureRef(StringView                                  ScopeKey,
                                                               const RHITopLevelAccelerationStructureDesc& Desc)
        -> ResourceRef<ResourceTopLevelAccelerationStructure> {
        return AcquireResourceRef(m_Context, RequestTopLevelAccelerationStructure(m_Context, ScopeKey, Desc));
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
    /// @brief Poll high-level RHI dependency waiters.
    auto TickRhiDependencies() -> void {
        m_Context.TickRhiDependencies();
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
    ResourceManager()         = default;
    ~ResourceManager()        = default;
    ResourceContext m_Context = {};
};
} // namespace SoulEngine
