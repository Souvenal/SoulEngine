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
                                          RHI::IndexBuffer>;

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
concept ManagedRHIResource = DefinedResourceTraits<T> && ListedManagedRHIResource<T>;

template <typename T>
concept GpuPendingManagedRHIResource = ManagedRHIResource<T> && ResourceTraits<T>::Info.HasGpuPending();

static_assert(ManagedRHIResource<RHI::SampledTexture>);
static_assert(ManagedRHIResource<RHI::RenderTarget>);
static_assert(ManagedRHIResource<RHI::GraphicsPipeline>);
static_assert(ManagedRHIResource<RHI::VertexBuffer>);
static_assert(ManagedRHIResource<RHI::IndexBuffer>);

/// @brief ResourceContext-owned RHI payload for supported resource types.
///
/// The primary template is constrained through `ManagedRHIResource`, so trying
/// to instantiate Resource/Handle/Slot for an unsupported type fails at compile
/// time instead of silently creating an unmanaged resource family.
template <ManagedRHIResource T>
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
/// `ResourceSlot` owns only the state needed to publish, pin, retire, and
/// release a ready RHI payload safely. It tracks the current generation,
/// public resource state, publish error, current ready payload, retired
/// still-pinned payloads, frame pin count, and deferred release flag.
///
/// It deliberately does not own logical request lifetime. Ref counts, lifetime
/// policy, cache eviction, request coalescing, and key lookup belong to
/// `ResourceContext` entries. Keep new behavior on this type limited to the
/// question "is this generation's payload ready, stale, failed, or safe to
/// reset while frame pins may still observe it?"
template <ManagedRHIResource T>
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
        // If the previous ready payload is still observed by frame pins, keep
        // owning it outside m_Value so this slot can publish the new generation
        // without invalidating old command-list raw pointers.
        RetireCurrentPayloadLocked();
        m_ReleaseWhenUnpinned = false;
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

    /// @brief Move-only RAII guard for a ready payload.
    ///
    /// External code does not construct this type directly.  Frame render code
    /// calls `FrameResourceScope::Acquire(ref)`, which resolves the ref's
    /// internal handle, creates a pin through Resource::Manager, stores it in
    /// the frame scope, and returns the borrowed RHI pointer for command-list
    /// use.
    ///
    /// The slot remains the sole owner of the payload. A successful pin
    /// increments the slot pin count and keeps the observed payload owned by
    /// the slot, either as the current ready payload or as a retired payload
    /// from an older generation. Moving a pin transfers that counted reference;
    /// destroying the last moved-to pin decrements the slot count and may
    /// release a payload that was previously marked stale.
    ///
    /// Typical low-level usage:
    ///
    /// ```cpp
    /// auto Pin = Slot.PinReady(Generation);
    /// if (Pin)
    ///     UseObserverPointer(Pin.Get());
    /// ```
    ///
    /// Renderer code should normally use `FrameResourceScope::Acquire()`
    /// instead. That API stores the pin immediately so command-list observer
    /// pointers cannot outlive their payload by accident.
    class Pin {
      public:
        Pin() = default;

        Pin(const Pin&)                    = delete;
        auto operator=(const Pin&) -> Pin& = delete;

        Pin(Pin&& Other) noexcept {
            m_Slot       = std::exchange(Other.m_Slot, nullptr);
            m_Generation = std::exchange(Other.m_Generation, 0);
            m_Value      = std::exchange(Other.m_Value, nullptr);
        }

        auto operator=(Pin&& Other) noexcept -> Pin& {
            if (this != &Other) {
                std::swap(m_Slot, Other.m_Slot);
                std::swap(m_Generation, Other.m_Generation);
                std::swap(m_Value, Other.m_Value);
            }
            return *this;
        }

        ~Pin() {
            if (!m_Slot)
                return;

            // Match the increment performed by PinReady().
            m_Slot->ReleasePin(m_Generation);
        }

        [[nodiscard]] explicit operator bool() const {
            return m_Value != nullptr;
        }

        /// @brief Borrow the pinned RHI object for observer-only use.
        ///
        /// The returned pointer is valid only while this pin, or a pin moved
        /// from it, remains alive.
        [[nodiscard]] auto Get() const -> T* {
            return m_Value;
        }

        [[nodiscard]] auto operator->() const -> T* {
            return m_Value;
        }

        [[nodiscard]] auto operator*() const -> T& {
            return *m_Value;
        }

      private:
        friend class ResourceSlot<T>;

        Pin(ResourceSlot<T>* Slot, ResourceGeneration Generation, T* Value) {
            m_Slot       = Slot;
            m_Generation = Generation;
            m_Value      = Value;
        }

        ResourceSlot<T>*     m_Slot       = nullptr;
        ResourceGeneration   m_Generation = 0;
        T*                   m_Value      = nullptr;
    };

    [[nodiscard]] auto PinReady(ResourceGeneration Generation) -> Pin {
        std::lock_guard Lock(m_Mutex);
        if (Generation != m_Generation || m_State != ResourceState::Ready || !m_Value.Object)
            return {};

        // The returned pin is now responsible for one counted reference.
        ++m_PinCount;
        return Pin(this, Generation, m_Value.Object.get());
    }

    /// @brief Request payload release while preserving pinned observer validity.
    auto RequestRelease() -> void {
        std::lock_guard Lock(m_Mutex);
        RequestReleaseLocked();
    }

    [[nodiscard]] auto HasPins() const -> bool {
        std::lock_guard Lock(m_Mutex);
        return m_PinCount > 0;
    }

    [[nodiscard]] auto IsReleased() const -> bool {
        std::lock_guard Lock(m_Mutex);
        return m_State == ResourceState::Stale && !m_Value.Object && !m_Error && !m_ReleaseWhenUnpinned &&
               m_RetiredPayloads.empty();
    }

  private:
    auto ReleasePin(ResourceGeneration Generation) -> void {
        std::lock_guard Lock(m_Mutex);
        if (m_PinCount > 0)
            --m_PinCount;
        // A release/clear request made while pins were alive is completed by
        // the last pin destructor, keeping observer pointers valid until then.
        if (m_PinCount == 0 && m_ReleaseWhenUnpinned)
            ReleasePayloadLocked();
        // Retired payloads exist only to preserve raw observer pointers for
        // older generations. Once the last frame pin drops, no command list can
        // still reference them through Resource, so they can be destroyed.
        if (m_PinCount == 0)
            m_RetiredPayloads.clear();
    }

    auto ReleasePayloadLocked() -> void {
        m_Value.Object.reset();
        m_RetiredPayloads.clear();
        m_Error.reset();
        m_ReleaseWhenUnpinned = false;
    }

    auto RetireCurrentPayloadLocked() -> void {
        if (!m_Value.Object)
            return;

        if (m_PinCount == 0) {
            m_Value.Object.reset();
            return;
        }

        // Pins store raw observer pointers.  When a new generation starts
        // before old frame pins drop, the old payload must stay owned here
        // until the last pin releases.
        m_RetiredPayloads.push_back(std::move(m_Value.Object));
    }

    auto RequestReleaseLocked() -> void {
        m_ReleaseWhenUnpinned = true;
        m_State = ResourceState::Stale;
        ++m_Generation;
        if (m_PinCount == 0)
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
    // Old ready payloads moved out of m_Value during Reset() while frame pins
    // still observe them. This is not a cache; it is temporary ownership to keep
    // stale-generation raw pointers valid until m_PinCount reaches zero.
    std::vector<UPtr<T>>        m_RetiredPayloads = {};
    std::optional<ErrorMessage> m_Error      = std::nullopt;
    Uint32                      m_PinCount   = 0;
    bool                        m_ReleaseWhenUnpinned = false;
};

class ResourceContext;

/// @brief Typed handle for an asynchronously published resource.
///
/// A handle is a stable ticket only: it carries key + generation and never keeps
/// the resource slot or GPU payload alive.  Resolve it through Resource::Manager
/// and keep a ResourceSlot<T>::Pin alive while passing observer pointers to RHI.
template <ManagedRHIResource T>
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

template <ManagedRHIResource T>
using ResourcePin = typename ResourceSlot<T>::Pin;

} // namespace SoulEngine::Resource
