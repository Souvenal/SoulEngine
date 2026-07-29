module;

#include <hlsl++.h>

export module Resource:Types;

export import Core;
export import Material;
export import RHI;
export import ShaderCompiler;
export import std;

export namespace SoulEngine {

using ResourceGeneration = Uint64;

/// @brief Runtime state for asynchronous resources.
enum class ResourceState : Uint8 {
    Unknown = 0,
    CpuPreparing,
    RhiCommitting,
    GpuPending,
    Ready,
    Failed,
    Stale,
};

/// @brief Caller policy when an asynchronous resource is not ready.
enum class ResourceWaitPolicy : Uint8 {
    Unknown = 0,
    Skip,
    Block,
};

/// @brief Cache/eviction policy stored on ResourceContext entries.
enum class ResourceLifetimePolicy : Uint8 {
    Unknown = 0,
    CachedAsset,
    Transient,
};

/// @brief Whether a resource family has a GPU completion wait phase before Ready.
enum class ResourceGpuPendingPolicy : Uint8 {
    Unknown = 0,
    None,
    WaitForCompletion,
};

/// @brief Compile-time metadata for a manager-owned resource type.
struct ResourceTraitInfo {
    ResourceGpuPendingPolicy GpuPendingPolicy = ResourceGpuPendingPolicy::Unknown;
    StringView               Label            = {};
    ResourceLifetimePolicy   DefaultPolicy    = ResourceLifetimePolicy::Unknown;

    consteval ResourceTraitInfo(ResourceGpuPendingPolicy InGpuPendingPolicy,
                                StringView InLabel,
                                ResourceLifetimePolicy InDefaultPolicy) {
        GpuPendingPolicy = InGpuPendingPolicy;
        Label            = InLabel;
        DefaultPolicy    = InDefaultPolicy;
    }

    [[nodiscard]] constexpr auto HasGpuPending() const -> bool {
        return GpuPendingPolicy == ResourceGpuPendingPolicy::WaitForCompletion;
    }
};

template <typename T>
struct ResourceTraits;

class ResourceMesh;
class ResourceBottomLevelAccelerationStructure;
class ResourceTopLevelAccelerationStructure;

template <>
struct ResourceTraits<RHISampledTexture> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::WaitForCompletion,
        "sampled texture",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHIRenderTarget> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "render target",
        ResourceLifetimePolicy::Transient,
    };
};

template <>
struct ResourceTraits<RHIGraphicsPipeline> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "graphics pipeline",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHIRayTracingPipeline> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "ray tracing pipeline",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHIVertexBuffer> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::WaitForCompletion,
        "vertex buffer",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHIIndexBuffer> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::WaitForCompletion,
        "index buffer",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHISampler> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "sampler",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<ResourceMesh> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "mesh",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<ResourceBottomLevelAccelerationStructure> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "bottom-level acceleration structure",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<ResourceTopLevelAccelerationStructure> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "top-level acceleration structure",
        ResourceLifetimePolicy::Transient,
    };
};

/// @brief Central list of RHI payload families managed by Resource.
///
/// A payload type must appear here and define `ResourceTraits<T>::Info` before
/// it satisfies `ManagedRHIResource`. Keeping both requirements in the concept
/// prevents a traits-only type from compiling without Context/FrameScope
/// storage.
using ManagedRHIResourceTypes = std::tuple<RHISampledTexture,
                                           RHIRenderTarget,
                                           RHIGraphicsPipeline,
                                           RHIRayTracingPipeline,
                                           RHIVertexBuffer,
                                           RHIIndexBuffer,
                                           RHISampler>;

/// @brief Central list of high-level asset families managed by Resource.
using ManagedAssetResourceTypes = std::tuple<ResourceMesh, ResourceBottomLevelAccelerationStructure, ResourceTopLevelAccelerationStructure>;

using ManagedResourceTypes =
    decltype(std::tuple_cat(std::declval<ManagedRHIResourceTypes>(), std::declval<ManagedAssetResourceTypes>()));

template <typename T, typename Tuple>
struct TupleContains;

template <typename T, typename... Candidate>
struct TupleContains<T, std::tuple<Candidate...>> {
    static constexpr bool Value = (std::same_as<T, Candidate> || ...);
};

template <typename T>
concept DefinedResourceTraits = requires {
    { ResourceTraits<T>::Info } -> std::same_as<const ResourceTraitInfo&>;
};

template <typename T>
concept ListedManagedRHIResource = TupleContains<T, ManagedRHIResourceTypes>::Value;

template <typename T>
concept ListedManagedAssetResource = TupleContains<T, ManagedAssetResourceTypes>::Value;

template <typename T>
concept ListedManagedResource = TupleContains<T, ManagedResourceTypes>::Value;

template <typename T>
concept ManagedRHIResource = DefinedResourceTraits<T> && ListedManagedRHIResource<T>;

template <typename T>
concept ManagedAssetResource = DefinedResourceTraits<T> && ListedManagedAssetResource<T>;

template <typename T>
concept ManagedResource = DefinedResourceTraits<T> && ListedManagedResource<T>;

template <typename T>
concept GpuPendingManagedRHIResource = ManagedRHIResource<T> && ResourceTraits<T>::Info.HasGpuPending();

static_assert(ManagedRHIResource<RHISampledTexture>);
static_assert(ManagedRHIResource<RHIRenderTarget>);
static_assert(ManagedRHIResource<RHIGraphicsPipeline>);
static_assert(ManagedRHIResource<RHIRayTracingPipeline>);
static_assert(ManagedRHIResource<RHIVertexBuffer>);
static_assert(ManagedRHIResource<RHIIndexBuffer>);
static_assert(ManagedRHIResource<RHISampler>);
static_assert(ManagedAssetResource<ResourceMesh>);
static_assert(ManagedResource<ResourceMesh>);

/// @brief ResourceContext-owned RHI payload for supported resource types.
///
/// The primary template is constrained through `ManagedResource`, so trying
/// to instantiate Resource/Handle/Slot for an unsupported type fails at compile
/// time instead of silently creating an unmanaged resource family.
/// ResourceMesh extends this storage to imported non-RHI asset payloads.
template <ManagedResource T>
struct Resource {
    UPtr<T> Object = nullptr;
};

/// @brief Async graphics pipeline request descriptor.
struct GraphicsPipelineRequest {
    ShaderEntry VertEntry         = {};
    ShaderEntry FragEntry         = {};
    RHIVertexInputLayoutDesc  VertexInputLayout = {};
    RHIPrimitiveTopology      Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState        Rasterizer        = {};
    RHIBlendState             Blend             = {};
    RHIDepthStencilState      DepthStencil      = {};
    RHIFormat                 ColorFormat       = RHIFormat::B8G8R8A8_UNORM;
    RHIFormat                 DepthFormat       = RHIFormat::Unknown;
};

/// @brief Async hardware ray-tracing pipeline request descriptor.
struct RayTracingPipelineRequest {
    ShaderEntry RayGeneration = {};
    std::vector<ShaderEntry> MissEntries = {};
    std::vector<RayTracingHitGroupCompileDesc> HitGroups = {};
    Uint32 MaxRecursionDepth = 1;
};

/// @brief Payload state machine for one ResourceContext entry generation.
///
/// `ResourceSlot` owns only the state needed to publish and release a ready
/// RHI payload safely. It tracks the current generation, public resource
/// state, publish error, and current ready payload.
///
/// It deliberately does not own logical request lifetime. Ref counts, lifetime
/// policy, cache eviction, request coalescing, and key lookup belong to
/// `ResourceContext` entries. Keep new behavior on this type limited to the
/// question "is this generation's payload ready, stale, or failed?"
template <ManagedResource T>
class ResourceSlot {
  public:
    ResourceSlot() = default;

    ResourceSlot(const ResourceSlot&)                    = delete;
    auto operator=(const ResourceSlot&) -> ResourceSlot& = delete;
    ResourceSlot(ResourceSlot&&)                         = delete;
    auto operator=(ResourceSlot&&) -> ResourceSlot&      = delete;
    ~ResourceSlot()                                      = default;

    [[nodiscard]] auto Reset() -> ResourceGeneration {
        std::lock_guard Lock(m_Mutex);
        ++m_Generation;
        m_State = ResourceState::CpuPreparing;
        m_Value.Object.reset();
        m_Error.reset();
        return m_Generation;
    }

    [[nodiscard]] auto MarkCpuPreparing(ResourceGeneration Generation) -> bool {
        return SetState(Generation, ResourceState::CpuPreparing);
    }

    [[nodiscard]] auto MarkRhiCommitting(ResourceGeneration Generation) -> bool {
        return SetState(Generation, ResourceState::RhiCommitting);
    }

    [[nodiscard]] auto PublishGpuPending(ResourceGeneration Generation) -> bool {
        return SetState(Generation, ResourceState::GpuPending);
    }

    [[nodiscard]] auto PublishReady(ResourceGeneration Generation, Resource<T> Value) -> bool {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return false;

        m_Value = std::move(Value);
        m_Error.reset();
        m_State = ResourceState::Ready;
        return true;
    }

    [[nodiscard]] auto PublishFailed(ResourceGeneration Generation, ErrorMessage Error) -> bool {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return false;

        m_Value.Object.reset();
        m_Error = std::move(Error);
        m_State = ResourceState::Failed;
        return true;
    }

    [[nodiscard]] auto GetState(ResourceGeneration Generation) const -> ResourceState {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return ResourceState::Stale;
        return m_State;
    }

    [[nodiscard]] auto GetError(ResourceGeneration Generation) const -> std::optional<ErrorMessage> {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation || m_State != ResourceState::Failed || !m_Error)
            return std::nullopt;
        return *m_Error;
    }

    [[nodiscard]] auto GetGeneration() const -> ResourceGeneration {
        std::lock_guard Lock(m_Mutex);
        return m_Generation;
    }

    [[nodiscard]] auto TryGetReady(ResourceGeneration Generation) const -> T* {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation || m_State != ResourceState::Ready || !m_Value.Object)
            return nullptr;

        return m_Value.Object.get();
    }

    /// @brief Request payload release.
    auto RequestRelease() -> void {
        std::lock_guard Lock(m_Mutex);
        RequestReleaseLocked();
    }

    [[nodiscard]] auto IsReleased() const -> bool {
        std::lock_guard Lock(m_Mutex);
        return m_State == ResourceState::Stale && !m_Value.Object && !m_Error;
    }

  private:
    auto ReleasePayloadLocked() -> void {
        m_Value.Object.reset();
        m_Error.reset();
    }

    auto RequestReleaseLocked() -> void {
        m_State = ResourceState::Stale;
        ++m_Generation;
        ReleasePayloadLocked();
    }

    [[nodiscard]] auto SetState(ResourceGeneration Generation, ResourceState State) -> bool {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation)
            return false;

        m_State = State;
        return true;
    }

    mutable std::mutex          m_Mutex;
    ResourceGeneration          m_Generation = 0;
    ResourceState               m_State      = ResourceState::Unknown;
    Resource<T>                 m_Value      = {};
    std::optional<ErrorMessage> m_Error      = std::nullopt;
};

class ResourceContext;

/// @brief Typed handle for an asynchronously published resource.
///
/// A handle is a stable ticket only: it carries key + generation and never keeps
/// the resource slot or GPU payload alive.
template <ManagedResource T>
class ResourceHandle {
  public:
    ResourceHandle() = default;

    [[nodiscard]] auto IsValid() const -> bool {
        return !m_Key.empty() && m_Generation != 0;
    }

    [[nodiscard]] auto GetKey() const -> const String& {
        return m_Key;
    }

    [[nodiscard]] auto GetGeneration() const -> ResourceGeneration {
        return m_Generation;
    }

  private:
    friend class ResourceContext;

    [[nodiscard]] static auto Create(String Key, ResourceGeneration Generation) -> ResourceHandle {
        ResourceHandle Handle;
        Handle.m_Key        = std::move(Key);
        Handle.m_Generation = Generation;
        return Handle;
    }

    String             m_Key        = {};
    ResourceGeneration m_Generation = 0;
};

/// @brief Move-only logical owner for a resource request.
///
/// `ResourceRef` expresses that a runtime system still wants the resource.
/// Ready payload observer pointers are resolved through `ResourceManager`;
/// this type owns logical demand only.
/// The type stores an opaque release callback so resource payload types can own
/// dependency refs without importing ResourceContext and creating a module cycle.
template <ManagedResource T>
class ResourceRef {
  public:
    ResourceRef() = default;

    ResourceRef(const ResourceRef&)                    = delete;
    auto operator=(const ResourceRef&) -> ResourceRef& = delete;

    ResourceRef(ResourceRef&& Other) noexcept {
        m_Context = std::exchange(Other.m_Context, nullptr);
        m_Handle = std::exchange(Other.m_Handle, {});
        m_Release = std::exchange(Other.m_Release, nullptr);
    }

    auto operator=(ResourceRef&& Other) noexcept -> ResourceRef& {
        if (this != &Other) {
            Reset();
            m_Context = std::exchange(Other.m_Context, nullptr);
            m_Handle = std::exchange(Other.m_Handle, {});
            m_Release = std::exchange(Other.m_Release, nullptr);
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
        if (!m_Context || !m_Handle.IsValid() || !m_Release)
            return;

        m_Release(m_Context, m_Handle);
        m_Context = nullptr;
        m_Handle = {};
        m_Release = nullptr;
    }

  private:
    using ReleaseFn = void (*)(void*, const ResourceHandle<T>&);

    template <ManagedResource U>
    friend auto AcquireResourceRef(ResourceContext&, const ResourceHandle<U>&) -> ResourceRef<U>;

    // Private construction retains logical demand; if the context rejects the
    // handle, the ref stays empty.
    [[nodiscard]] static auto Create(void* Context, ResourceHandle<T> Handle, ReleaseFn Release) -> ResourceRef {
        ResourceRef Ref;
        Ref.m_Context = Context;
        Ref.m_Handle = std::move(Handle);
        Ref.m_Release = Release;
        return Ref;
    }

    // Non-owning ResourceContext observer; ResourceRef owns logical demand only.
    void*             m_Context = nullptr;
    ResourceHandle<T> m_Handle = {};
    ReleaseFn         m_Release = nullptr;
};

template <ManagedResource T>
[[nodiscard]] auto AcquireResourceRef(ResourceContext& Context, const ResourceHandle<T>& Handle) -> ResourceRef<T>;

/// @brief Single drawable submesh using structure-of-arrays vertex buffers.
struct SubMesh {
    std::vector<hlslpp::interop::float3> Positions = {};
    std::vector<hlslpp::interop::float3> Normals   = {};
    std::vector<hlslpp::interop::float4> Tangents  = {};
    std::vector<hlslpp::interop::float2> UVs       = {};
    std::vector<Uint32>                  Indices   = {};

    ResourceHandle<RHIVertexBuffer> PositionVB = {};
    ResourceHandle<RHIVertexBuffer> NormalVB   = {};
    ResourceHandle<RHIVertexBuffer> TangentVB  = {};
    ResourceHandle<RHIVertexBuffer> UVVB       = {};
    ResourceHandle<RHIIndexBuffer>  IB         = {};

    Uint32 VertexCount  = 0;
    Uint32 MaterialSlot = 0;
    bool   HasUV0       = false;
    bool   HasTangents  = false;
};

/// @brief PBR material imported with a mesh asset and addressed by Assimp material slot.
struct ImportedPbrMaterial {
    String                       Name     = {};
    PbrMetallicRoughnessMaterial Material = {};
};

struct MeshGroup {
    String               Name      = {};
    std::vector<SubMesh> SubMeshes = {};
};

/// @brief High-level imported mesh asset.
class ResourceMesh {
  private:
    friend auto ParseAssimpMeshes(StringView, ResourceMesh&) -> std::expected<void, ErrorMessage>;

    std::vector<MeshGroup>           m_MeshGroups        = {};
    std::vector<ImportedPbrMaterial> m_ImportedMaterials = {};
    String                           m_Name              = {};

  public:
    ResourceMesh() = default;

    [[nodiscard]] auto GetName() const -> const String& {
        return m_Name;
    }

    [[nodiscard]] auto GetMeshGroups() -> std::vector<MeshGroup>&;
    [[nodiscard]] auto GetMeshGroups() const -> const std::vector<MeshGroup>&;
    [[nodiscard]] auto GetImportedMaterials() const -> const std::vector<ImportedPbrMaterial>&;
    [[nodiscard]] auto GetImportedMaterial(Uint32 MaterialSlot) const -> const PbrMetallicRoughnessMaterial*;
};

/// Policy for lowering a mesh asset into one reusable BLAS payload.
enum class BottomLevelAccelerationStructureGeometryPolicy : Uint8 {
    Unknown = 0,
    AllMeshSubMeshes,
};

/// Request options contributing to a reusable BLAS identity.
struct BottomLevelAccelerationStructureRequest {
    RHIAccelerationStructureBuildFlags                     BuildFlags = RHIAccelerationStructureBuildFlags::None;
    BottomLevelAccelerationStructureGeometryPolicy           GeometryPolicy =
        BottomLevelAccelerationStructureGeometryPolicy::AllMeshSubMeshes;
};

/// Independent resource payload for reusable object-space acceleration geometry.
class ResourceBottomLevelAccelerationStructure {
  public:
    ResourceBottomLevelAccelerationStructure() = default;

    ResourceBottomLevelAccelerationStructure(std::vector<ResourceRef<RHIVertexBuffer>> PositionBuffers,
                                     std::vector<ResourceRef<RHIIndexBuffer>> IndexBuffers,
                                     UPtr<RHIBottomLevelAccelerationStructure> Payload) {
        m_PositionBuffers = std::move(PositionBuffers);
        m_IndexBuffers = std::move(IndexBuffers);
        m_Payload = std::move(Payload);
    }

    ResourceBottomLevelAccelerationStructure(const ResourceBottomLevelAccelerationStructure&) = delete;
    auto operator=(const ResourceBottomLevelAccelerationStructure&) -> ResourceBottomLevelAccelerationStructure& = delete;
    ResourceBottomLevelAccelerationStructure(ResourceBottomLevelAccelerationStructure&&) = delete;
    auto operator=(ResourceBottomLevelAccelerationStructure&&) -> ResourceBottomLevelAccelerationStructure& = delete;
    ~ResourceBottomLevelAccelerationStructure() = default;

    [[nodiscard]] auto GetRhiPayload() const -> RHIBottomLevelAccelerationStructure* {
        return m_Payload.get();
    }

  private:
    std::vector<ResourceRef<RHIVertexBuffer>> m_PositionBuffers = {};
    std::vector<ResourceRef<RHIIndexBuffer>>  m_IndexBuffers = {};
    UPtr<RHIBottomLevelAccelerationStructure> m_Payload = nullptr;
};

/// Persistent renderer-scoped TLAS allocation. Per-frame instances remain command-driven.
class ResourceTopLevelAccelerationStructure {
  public:
    ResourceTopLevelAccelerationStructure() = default;

    explicit ResourceTopLevelAccelerationStructure(UPtr<RHITopLevelAccelerationStructure> Payload) {
        m_Payload = std::move(Payload);
    }

    ResourceTopLevelAccelerationStructure(const ResourceTopLevelAccelerationStructure&) = delete;
    auto operator=(const ResourceTopLevelAccelerationStructure&) -> ResourceTopLevelAccelerationStructure& = delete;
    ResourceTopLevelAccelerationStructure(ResourceTopLevelAccelerationStructure&&) = delete;
    auto operator=(ResourceTopLevelAccelerationStructure&&) -> ResourceTopLevelAccelerationStructure& = delete;
    ~ResourceTopLevelAccelerationStructure() = default;

    [[nodiscard]] auto GetRhiPayload() const -> RHITopLevelAccelerationStructure* {
        return m_Payload.get();
    }

  private:
    UPtr<RHITopLevelAccelerationStructure> m_Payload = nullptr;
};

} // namespace SoulEngine
