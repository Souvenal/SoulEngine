module;

#include <hlsl++.h>

export module Resource:Types;

export import Core;
export import RHI;
export import ShaderCompiler;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Resource {

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

class Mesh;

template <>
struct ResourceTraits<RHI::SampledTexture> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::WaitForCompletion,
        "sampled texture",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHI::RenderTarget> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "render target",
        ResourceLifetimePolicy::Transient,
    };
};

template <>
struct ResourceTraits<RHI::GraphicsPipeline> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "graphics pipeline",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHI::VertexBuffer> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::WaitForCompletion,
        "vertex buffer",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHI::IndexBuffer> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::WaitForCompletion,
        "index buffer",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<RHI::ConstantBuffer> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "constant buffer",
        ResourceLifetimePolicy::Transient,
    };
};

template <>
struct ResourceTraits<RHI::Sampler> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "sampler",
        ResourceLifetimePolicy::CachedAsset,
    };
};

template <>
struct ResourceTraits<Mesh> {
    static constexpr ResourceTraitInfo Info{
        ResourceGpuPendingPolicy::None,
        "mesh",
        ResourceLifetimePolicy::CachedAsset,
    };
};

/// @brief Central list of RHI payload families managed by Resource.
///
/// A payload type must appear here and define `ResourceTraits<T>::Info` before
/// it satisfies `ManagedRHIResource`. Keeping both requirements in the concept
/// prevents a traits-only type from compiling without Context/FrameScope
/// storage.
using ManagedRHIResourceTypes = std::tuple<RHI::SampledTexture,
                                           RHI::RenderTarget,
                                           RHI::GraphicsPipeline,
                                           RHI::VertexBuffer,
                                           RHI::IndexBuffer,
                                           RHI::ConstantBuffer,
                                           RHI::Sampler>;

/// @brief Central list of high-level asset families managed by Resource.
using ManagedAssetResourceTypes = std::tuple<Mesh>;

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

static_assert(ManagedRHIResource<RHI::SampledTexture>);
static_assert(ManagedRHIResource<RHI::RenderTarget>);
static_assert(ManagedRHIResource<RHI::GraphicsPipeline>);
static_assert(ManagedRHIResource<RHI::VertexBuffer>);
static_assert(ManagedRHIResource<RHI::IndexBuffer>);
static_assert(ManagedRHIResource<RHI::ConstantBuffer>);
static_assert(ManagedRHIResource<RHI::Sampler>);
static_assert(ManagedAssetResource<Mesh>);
static_assert(ManagedResource<Mesh>);

/// @brief ResourceContext-owned RHI payload for supported resource types.
///
/// The primary template is constrained through `ManagedResource`, so trying
/// to instantiate Resource/Handle/Slot for an unsupported type fails at compile
/// time instead of silently creating an unmanaged resource family.
/// Mesh extends this storage to imported non-RHI asset payloads.
template <ManagedResource T>
struct Resource {
    UPtr<T> Object = nullptr;
};

/// @brief Async graphics pipeline request descriptor.
struct GraphicsPipelineRequest {
    ShaderCompiler::ShaderEntry VertEntry         = {};
    ShaderCompiler::ShaderEntry FragEntry         = {};
    RHI::VertexInputLayoutDesc  VertexInputLayout = {};
    RHI::PrimitiveTopology      Topology          = RHI::PrimitiveTopology::TriangleList;
    RHI::RasterizerState        Rasterizer        = {};
    RHI::BlendState             Blend             = {};
    RHI::DepthStencilState      DepthStencil      = {};
    RHI::Format                 ColorFormat       = RHI::Format::B8G8R8A8_UNORM;
    RHI::Format                 DepthFormat       = RHI::Format::Unknown;
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

/// @brief Single drawable submesh using structure-of-arrays vertex buffers.
struct SubMesh {
    std::vector<hlslpp::interop::float3> Positions = {};
    std::vector<hlslpp::interop::float3> Normals   = {};
    std::vector<hlslpp::interop::float4> Tangents  = {};
    std::vector<hlslpp::interop::float2> UVs       = {};
    std::vector<Uint32>                  Indices   = {};

    ResourceHandle<RHI::VertexBuffer> PositionVB = {};
    ResourceHandle<RHI::VertexBuffer> NormalVB   = {};
    ResourceHandle<RHI::VertexBuffer> TangentVB  = {};
    ResourceHandle<RHI::VertexBuffer> UVVB       = {};
    ResourceHandle<RHI::IndexBuffer>  IB         = {};

    Uint32 VertexCount  = 0;
    Uint32 MaterialSlot = 0;
};

struct MeshGroup {
    String               Name      = {};
    std::vector<SubMesh> SubMeshes = {};
};

/// @brief Snapshot-safe references for one indexed mesh draw.
struct DrawPacket {
    ResourceHandle<RHI::VertexBuffer> PositionVB = {};
    ResourceHandle<RHI::VertexBuffer> NormalVB   = {};
    ResourceHandle<RHI::VertexBuffer> TangentVB  = {};
    ResourceHandle<RHI::VertexBuffer> UVVB       = {};
    ResourceHandle<RHI::IndexBuffer>  IB         = {};
    Uint32                             IndexCount = 0;
};

/// @brief High-level imported mesh asset.
class Mesh {
  private:
    friend auto ParseAssimpMeshes(StringView, Mesh&) -> std::expected<void, ErrorMessage>;

    std::vector<MeshGroup> m_MeshGroups    = {};
    std::vector<String>    m_MaterialNames = {};
    String                 m_Name          = {};

  public:
    Mesh() = default;

    [[nodiscard]] auto GetName() const -> const String& {
        return m_Name;
    }

    auto PopulateDrawPackets(std::vector<DrawPacket>& Out) const -> void;

    [[nodiscard]] auto GetMeshGroups() -> std::vector<MeshGroup>&;
    [[nodiscard]] auto GetMeshGroups() const -> const std::vector<MeshGroup>&;
};

} // namespace SoulEngine::Resource
