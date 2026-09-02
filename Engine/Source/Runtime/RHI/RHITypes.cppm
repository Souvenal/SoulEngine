module;

export module RHI:Types;

export import Core;
import :Ref;
import Shader;
export import magic_enum;

export import std;

export namespace SoulEngine {

/// Common identity metadata for persistent RHI objects.
class RHIObject {
  public:
    RHIObject(const RHIObject&)                    = delete;
    auto operator=(const RHIObject&) -> RHIObject& = delete;
    virtual ~RHIObject()                           = default;

    [[nodiscard]] auto GetName() const -> StringView {
        return m_Name;
    }

    [[nodiscard]] auto GetId() const -> Uint64 {
        return m_Id;
    }

    [[nodiscard]] virtual auto GetShaderBindingType() const -> ShaderResourceType {
        return ShaderResourceType::Unknown;
    }

    /// Whether this object is a per-frame transient arena slice. Transient
    /// buffers bind through dynamic descriptors; persistent buffers never
    /// occupy descriptor bindings (they use vertex input or device addresses).
    [[nodiscard]] virtual auto IsTransient() const -> bool {
        return false;
    }

  protected:
    explicit RHIObject(String Name) : m_Name(std::move(Name)), m_Id(s_NextId.fetch_add(1, std::memory_order_relaxed)) {}
    RHIObject(RHIObject&&)                    = default;
    auto operator=(RHIObject&&) -> RHIObject& = default;

  private:
    inline static std::atomic<Uint64> s_NextId = 1;

    String m_Name = {};
    Uint64 m_Id   = 0;
};

class RHIRenderTarget;
class RHITopLevelAccelerationStructure;
class RHIShaderBindingSet;

// ── Buffer descriptor types ────────────────────────────────────────────────

struct RHIVertexBufferDesc {
    std::span<const std::byte> Data        = {};
    Uint64                     VertexCount = 0;
    Uint32                     Stride      = 0;
};

struct RHIIndexBufferDesc {
    std::span<const std::byte> Data       = {};
    Uint64                     IndexCount = 0;
};

enum class RHISamplerProfile : Uint8 {
    Unknown = 0,
    LinearRepeat,
    AnisotropicRepeat,
};

struct RHISamplerDesc {
    RHISamplerProfile Profile = RHISamplerProfile::LinearRepeat;
};

// ── Typed GPU buffer polymorphic bases ──────────────────────────────────

/// Polymorphic base for vertex buffer resources and their immutable metadata.
/// Backend concrete classes (e.g. VulkanVertexBuffer) own GPU allocations.
/// ResourceManager owns RHIVertexBuffer instances; command lists only observe them.
class RHIVertexBuffer : public RHIObject {
  public:
    RHIVertexBuffer(const RHIVertexBuffer&)                    = delete;
    auto operator=(const RHIVertexBuffer&) -> RHIVertexBuffer& = delete;
    RHIVertexBuffer(RHIVertexBuffer&&)                         = delete;
    auto operator=(RHIVertexBuffer&&) -> RHIVertexBuffer&      = delete;
    virtual ~RHIVertexBuffer()                                 = default;

    [[nodiscard]] auto GetVertexCount() const noexcept -> Uint64 {
        return m_VertexCount;
    }
    [[nodiscard]] auto GetStride() const noexcept -> Uint32 {
        return m_Stride;
    }
    [[nodiscard]] virtual auto GetDeviceAddress() const noexcept -> Uint64 {
        return 0;
    }

    /// Vertex buffers have no dedicated shader descriptor type; they reach
    /// shaders through vertex pulling as structured/storage buffers. The
    /// backend allocation carries the storage-buffer usage bit, so reporting
    /// StorageBuffer here is the valid descriptor-level capability.
    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        return ShaderResourceType::StorageBuffer;
    }

  protected:
    explicit RHIVertexBuffer(String Name, const RHIVertexBufferDesc& Desc)
        : RHIObject(std::move(Name)), m_VertexCount(Desc.VertexCount), m_Stride(Desc.Stride) {}

  private:
    Uint64 m_VertexCount = 0;
    Uint32 m_Stride      = 0;
};

/// Polymorphic base for index buffer resources and their immutable metadata.
/// Same role as RHIVertexBuffer, for Uint32 index data.
class RHIIndexBuffer : public RHIObject {
  public:
    RHIIndexBuffer(const RHIIndexBuffer&)                    = delete;
    auto operator=(const RHIIndexBuffer&) -> RHIIndexBuffer& = delete;
    RHIIndexBuffer(RHIIndexBuffer&&)                         = delete;
    auto operator=(RHIIndexBuffer&&) -> RHIIndexBuffer&      = delete;
    virtual ~RHIIndexBuffer()                                = default;

    [[nodiscard]] auto GetIndexCount() const noexcept -> Uint64 {
        return m_IndexCount;
    }
    [[nodiscard]] virtual auto GetDeviceAddress() const noexcept -> Uint64 {
        return 0;
    }

    /// Same rationale as RHIVertexBuffer: no index-buffer descriptor type
    /// exists; shader access goes through storage-buffer vertex pulling, and
    /// the backend allocation carries the storage-buffer usage bit.
    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        return ShaderResourceType::StorageBuffer;
    }

  protected:
    explicit RHIIndexBuffer(String Name, const RHIIndexBufferDesc& Desc)
        : RHIObject(std::move(Name)), m_IndexCount(Desc.IndexCount) {}

  private:
    Uint64 m_IndexCount = 0;
};

/// @brief GPU access modes required by a transient buffer.
enum class RHITransientBufferUsage : Uint8 {
    Unknown             = 0,
    UniformRead         = 1 << 0,
    ShaderRead          = 1 << 1,
    IndirectCommandRead = 1 << 2,
};

} // namespace SoulEngine

export namespace magic_enum::customize {
template <>
struct enum_range<SoulEngine::RHITransientBufferUsage> {
    static constexpr bool is_flags = true;
};
} // namespace magic_enum::customize

export namespace SoulEngine {

struct RHITransientConstantBufferDesc {
    std::span<const std::byte> Data = {};
};

struct RHITransientShaderStorageBufferDesc {
    std::span<const std::byte> Data  = {};
    RHITransientBufferUsage    Usage = RHITransientBufferUsage::ShaderRead;
};

/// @brief Logical per-frame uniform buffer resolved by the active RHI backend.
class RHITransientConstantBuffer : public RHIObject {
  public:
    RHITransientConstantBuffer(const RHITransientConstantBuffer&)                    = delete;
    auto operator=(const RHITransientConstantBuffer&) -> RHITransientConstantBuffer& = delete;
    RHITransientConstantBuffer(RHITransientConstantBuffer&&)                         = delete;
    auto operator=(RHITransientConstantBuffer&&) -> RHITransientConstantBuffer&      = delete;
    virtual ~RHITransientConstantBuffer()                                            = default;

    [[nodiscard]] auto GetSize() const noexcept -> Uint64 {
        return m_Size;
    }

    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        return ShaderResourceType::ConstantBuffer;
    }

    [[nodiscard]] auto IsTransient() const -> bool override {
        return true;
    }

  protected:
    RHITransientConstantBuffer(String Name, Uint64 Size) : RHIObject(std::move(Name)), m_Size(Size) {}

  private:
    Uint64 m_Size = 0;
};

/// @brief Logical per-frame storage buffer resolved by the active RHI backend.
class RHITransientShaderStorageBuffer : public RHIObject {
  public:
    RHITransientShaderStorageBuffer(const RHITransientShaderStorageBuffer&)                    = delete;
    auto operator=(const RHITransientShaderStorageBuffer&) -> RHITransientShaderStorageBuffer& = delete;
    RHITransientShaderStorageBuffer(RHITransientShaderStorageBuffer&&)                         = delete;
    auto operator=(RHITransientShaderStorageBuffer&&) -> RHITransientShaderStorageBuffer&      = delete;
    virtual ~RHITransientShaderStorageBuffer()                                                 = default;

    [[nodiscard]] auto GetSize() const noexcept -> Uint64 {
        return m_Size;
    }

    [[nodiscard]] auto GetUsage() const noexcept -> RHITransientBufferUsage {
        return m_Usage;
    }

    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        return ShaderResourceType::StorageBuffer;
    }

    [[nodiscard]] auto IsTransient() const -> bool override {
        return true;
    }

  protected:
    RHITransientShaderStorageBuffer(String Name, Uint64 Size, RHITransientBufferUsage Usage)
        : RHIObject(std::move(Name)), m_Size(Size), m_Usage(Usage) {}

  private:
    Uint64                  m_Size  = 0;
    RHITransientBufferUsage m_Usage = RHITransientBufferUsage::Unknown;
};

/// Shader-visible sampling state object.
///
/// Backends own the native sampler handle. ResourceManager owns RHISampler
/// instances; command lists only observe them.
class RHISampler : public RHIObject {
  public:
    RHISampler(const RHISampler&)                    = delete;
    auto operator=(const RHISampler&) -> RHISampler& = delete;
    RHISampler(RHISampler&&)                         = delete;
    auto operator=(RHISampler&&) -> RHISampler&      = delete;
    virtual ~RHISampler()                            = default;

    [[nodiscard]] auto GetDesc() const -> const RHISamplerDesc& {
        return m_Desc;
    }

    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        return ShaderResourceType::Sampler;
    }

  protected:
    explicit RHISampler(String Name, const RHISamplerDesc& Desc) : RHIObject(std::move(Name)), m_Desc(Desc) {}

  private:
    RHISamplerDesc m_Desc = {};
};

/// Empty polymorphic base for graphics pipeline resources.
/// Backend concrete class (e.g. VulkanGraphicsPipeline) owns the native
/// pipeline. ResourceManager owns RHIGraphicsPipeline instances.
class RHIGraphicsPipeline : public RHIObject {
  public:
    RHIGraphicsPipeline(const RHIGraphicsPipeline&)                    = delete;
    auto operator=(const RHIGraphicsPipeline&) -> RHIGraphicsPipeline& = delete;
    RHIGraphicsPipeline(RHIGraphicsPipeline&&)                         = delete;
    auto operator=(RHIGraphicsPipeline&&) -> RHIGraphicsPipeline&      = delete;
    virtual ~RHIGraphicsPipeline()                                     = default;

    [[nodiscard]] auto GetShaderBindingSet() const -> const RHIRef<RHIShaderBindingSet>& {
        return m_ShaderBindingSet;
    }

  protected:
    explicit RHIGraphicsPipeline(String Name, RHIRef<RHIShaderBindingSet> BindingSet)
        : RHIObject(std::move(Name)), m_ShaderBindingSet(std::move(BindingSet)) {}

  private:
    RHIRef<RHIShaderBindingSet> m_ShaderBindingSet = nullptr;
};

/// @brief Descriptor for a persistent GPU-to-CPU readback buffer.
struct RHIReadbackBufferDesc {
    Uint32 Size = 0; ///< Bytes per readback payload; the payload must be single-copy atomic.
};

/// @brief Persistent GPU-to-CPU readback payload.
///
/// Backends may use a private multi-frame ring to avoid CPU/GPU read races.
/// The payload must stay single-copy atomic (e.g. one aligned Uint32); larger
/// payloads require a seqlock-style upgrade first.
class RHIReadbackBuffer : public RHIObject {
  public:
    RHIReadbackBuffer(const RHIReadbackBuffer&)                    = delete;
    auto operator=(const RHIReadbackBuffer&) -> RHIReadbackBuffer& = delete;
    RHIReadbackBuffer(RHIReadbackBuffer&&)                         = delete;
    auto operator=(RHIReadbackBuffer&&) -> RHIReadbackBuffer&      = delete;
    virtual ~RHIReadbackBuffer()                                   = default;

    [[nodiscard]] auto GetSize() const noexcept -> Uint32 {
        return m_Size;
    }

    /// @brief Try to copy out the newest GPU-completed payload.
    /// @return The payload, or std::nullopt when no completed write exists.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] auto TryRead() const -> std::optional<T> {
        if (sizeof(T) > m_Size)
            return std::nullopt;

        T Out{};
        return TryReadBytes(std::as_writable_bytes(std::span<T>{&Out, 1})) ? std::optional<T>{Out} : std::nullopt;
    }

  protected:
    explicit RHIReadbackBuffer(String Name, const RHIReadbackBufferDesc& Desc)
        : RHIObject(std::move(Name)), m_Size(Desc.Size) {}

    [[nodiscard]] virtual auto TryReadBytes(std::span<std::byte> Out) const -> bool = 0;

  private:
    Uint32 m_Size = 0;
};

// ── Opaque handle types ───────────────────────────────────────────────────

/// Polymorphic base for shader-readable sampled texture resources.
/// Backend concrete class (e.g. VulkanSampledTexture) owns the GPU allocation.
/// ResourceManager owns RHISampledTexture instances.
class RHISampledTexture : public RHIObject {
  public:
    RHISampledTexture(const RHISampledTexture&)                    = delete;
    auto operator=(const RHISampledTexture&) -> RHISampledTexture& = delete;
    RHISampledTexture(RHISampledTexture&&)                         = delete;
    auto operator=(RHISampledTexture&&) -> RHISampledTexture&      = delete;
    virtual ~RHISampledTexture()                                   = default;

    [[nodiscard]] virtual auto GetWidth() const -> Uint32  = 0;
    [[nodiscard]] virtual auto GetHeight() const -> Uint32 = 0;

    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        return ShaderResourceType::SampledTexture;
    }

  protected:
    explicit RHISampledTexture(String Name) : RHIObject(std::move(Name)) {}
};

/// @brief Mutable shader-visible array of resource observers.
///
/// Resource-layer arrays retain the corresponding ResourceRefs. This RHI value
/// contains only the resolved observers recorded into command lists.

// ── Texture ──────────────────────────────────────────────────────────────────

enum class RHIFormat : Uint8 {
    Unknown             = 0,
    R8_UNORM            = 1,
    R8G8_UNORM          = 2,
    R8G8B8A8_UNORM      = 3,
    B8G8R8A8_UNORM      = 4,
    R16G16B16A16_SFLOAT = 5,
    R32G32B32A32_SFLOAT = 6,
    R32G32B32_SFLOAT    = 7,
    R32G32_SFLOAT       = 8,
    R32_SFLOAT          = 9,
    R32_UINT            = 10,
    D32_SFLOAT          = 11,
    D24_UNORM_S8_UINT   = 12,
    D32_SFLOAT_S8_UINT  = 13,
};

struct RHIVertexInputAttributeDesc {
    Uint32    Location = 0;
    Uint32    Binding  = 0;
    RHIFormat Format   = RHIFormat::Unknown;
    Uint32    Offset   = 0;
};

struct RHIVertexInputBindingDesc {
    Uint32 Binding = 0;
    Uint32 Stride  = 0;
};

inline constexpr Uint32 kMaxVertexBufferBindings = 4;

struct RHIVertexInputLayoutDesc {
    std::vector<RHIVertexInputBindingDesc>   Bindings   = {};
    std::vector<RHIVertexInputAttributeDesc> Attributes = {};
};

enum class RHITextureUsage : Uint32 {
    None           = 0,
    RenderTarget   = 1u << 0,
    DepthStencil   = 1u << 1,
    ShaderResource = 1u << 2,
    FrameOutput    = 1u << 3,
    ShaderStorage  = 1u << 4,
    TransferSrc    = 1u << 5,
    TransferDst    = 1u << 6,
};

[[nodiscard]] inline auto operator|(RHITextureUsage a, RHITextureUsage b) -> RHITextureUsage {
    return static_cast<RHITextureUsage>(static_cast<Uint32>(a) | static_cast<Uint32>(b));
}

/// Polymorphic base for render-target images (color or depth/stencil).
/// Color vs depth is distinguished by GetFormat()/GetUsage(), not by type.
/// Backend concrete class owns GPU allocation. ResourceManager owns render targets.
class RHIRenderTarget : public RHIObject {
  public:
    RHIRenderTarget(const RHIRenderTarget&)                    = delete;
    auto operator=(const RHIRenderTarget&) -> RHIRenderTarget& = delete;
    RHIRenderTarget(RHIRenderTarget&&)                         = delete;
    auto operator=(RHIRenderTarget&&) -> RHIRenderTarget&      = delete;
    virtual ~RHIRenderTarget()                                 = default;

    [[nodiscard]] virtual auto GetWidth() const -> Uint32          = 0;
    [[nodiscard]] virtual auto GetHeight() const -> Uint32         = 0;
    [[nodiscard]] virtual auto GetFormat() const -> RHIFormat      = 0;
    [[nodiscard]] virtual auto GetUsage() const -> RHITextureUsage = 0;

    [[nodiscard]] auto GetShaderBindingType() const -> ShaderResourceType override {
        const auto Usage = static_cast<Uint32>(GetUsage());
        if ((Usage & static_cast<Uint32>(RHITextureUsage::ShaderStorage)) != 0)
            return ShaderResourceType::StorageTexture;
        if ((Usage & static_cast<Uint32>(RHITextureUsage::ShaderResource)) != 0)
            return ShaderResourceType::SampledTexture;
        return ShaderResourceType::Unknown;
    }

  protected:
    explicit RHIRenderTarget(String Name) : RHIObject(std::move(Name)) {}
};

/// @brief Type-erased ref-backed resource retained by a shader binding set.
/// @brief Reflected shader interface used to construct a binding set.
struct RHIShaderBindingSetDesc {
    ShaderReflection Reflection = {};
    /// Bitmask of every stage in the shader program (magic_enum flags).
    ShaderStage      Stages     = ShaderStage::Unknown;
};

struct RHIShaderBindingRequest {
    StringView         ParameterPath  = {};
    RHIObject*         Resource       = nullptr;
    ShaderResourceType Type           = ShaderResourceType::Unknown;
    // Sampled textures and render targets share SampledTexture at the shader
    // level; retain this concrete kind so Vulkan can dispatch the raw pointer safely.
    bool               IsRenderTarget = false;
    bool               IsReadOnly     = true;

    RHIShaderBindingRequest() = default;

    template <typename T>
    RHIShaderBindingRequest(StringView ParameterPathIn, RHIRef<T> Resource, bool IsReadOnlyIn)
        : ParameterPath(ParameterPathIn),
          Resource(Resource.TryGet()),
          Type(Resource ? Resource->GetShaderBindingType() : ShaderResourceType::Unknown),
          IsRenderTarget(std::same_as<T, RHIRenderTarget>),
          IsReadOnly(IsReadOnlyIn) {}
};

/// @brief Backend-owned descriptor resources for one reflected shader layout.
class RHIShaderBindingSet : public RHIObject {
  public:
    /// @brief One reflected binding slot: the shader-side contract plus the
    /// bound resource.
    struct Slot {
        ShaderBinding      Info           = {};
        RHIObject*         Resource       = nullptr;
        // The shader type alone cannot distinguish sampled textures from
        // render targets after binding resources are type-erased.
        bool               IsRenderTarget = false;
        bool               IsReadOnly     = true;
    };

    RHIShaderBindingSet(const RHIShaderBindingSet&)                    = delete;
    auto operator=(const RHIShaderBindingSet&) -> RHIShaderBindingSet& = delete;
    RHIShaderBindingSet(RHIShaderBindingSet&&)                         = delete;
    auto operator=(RHIShaderBindingSet&&) -> RHIShaderBindingSet&      = delete;
    virtual ~RHIShaderBindingSet()                                     = default;

    [[nodiscard]] auto GetBindingsBySet() const -> std::span<const std::vector<Slot>> {
        return m_BindingsBySet;
    }

    [[nodiscard]] auto HasUnboundBindings() const -> bool {
        if (m_BindlessSpace && !m_BindlessTextures)
            return true;
        return std::ranges::any_of(m_BindingsBySet, [](const auto& Bindings) {
            return std::ranges::any_of(Bindings, [](const Slot& Binding) { return Binding.Resource == nullptr; });
        });
    }

    [[nodiscard]] auto BindResources(std::span<const RHIShaderBindingRequest> Resources)
        -> std::expected<void, ErrorMessage> {
        if (Resources.empty())
            return std::unexpected(ErrorMessage("Shader binding resource batch must not be empty"));

        std::vector<Slot*> FlatBindings;
        for (auto& SetBindings : m_BindingsBySet)
            for (auto& Binding : SetBindings)
                FlatBindings.push_back(&Binding);

        // We first query each request's set and index
        std::vector<std::pair<Uint32, Uint32>> BindingLocations;
        BindingLocations.reserve(Resources.size());
        for (const auto& Request : Resources) {
            const auto It = std::ranges::find_if(
                FlatBindings, [&](const Slot* Slot) { return Slot->Info.ParameterPath == Request.ParameterPath; });
            if (It == FlatBindings.end())
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is not present in the shader binding layout", Request.ParameterPath)));
            const auto Location = std::pair{(*It)->Info.Set, (*It)->Info.BindingIndex};
            BindingLocations.push_back(Location);
            if (!Request.Resource)
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' has no ready resource", Request.ParameterPath)));
            if (Request.Type != (*It)->Info.Type)
                return std::unexpected(ErrorMessage(Format("Shader parameter '{}' expects {}, received {}",
                                                           Request.ParameterPath,
                                                           magic_enum::enum_name((*It)->Info.Type),
                                                           magic_enum::enum_name(Request.Type))));
            if ((*It)->Info.ArrayCount != 1)
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is an array and cannot use scalar batch binding", Request.ParameterPath)));
        }

        // Then we check if all set index are the same
        const auto SetIndex = BindingLocations.front().first;
        for (Uint32 Index = 0; Index < BindingLocations.size(); ++Index)
            for (Uint32 Previous = 0; Previous < Index; ++Previous)
                if (BindingLocations[Index] == BindingLocations[Previous])
                    return std::unexpected(ErrorMessage(
                        Format("Shader parameter '{}' appears more than once in a binding batch",
                               Resources[Index].ParameterPath)));
        if (std::ranges::any_of(BindingLocations, [SetIndex](const auto& Location) {
                return Location.first != SetIndex;
            })) {
            String Details;
            for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
                if (!Details.empty())
                    Details += ", ";
                Details += Format("'{}' (set {}, binding {})",
                                  Resources[Index].ParameterPath,
                                  BindingLocations[Index].first,
                                  BindingLocations[Index].second);
            }
            return std::unexpected(
                ErrorMessage(Format("Shader binding batch contains multiple descriptor sets: {}", Details)));
        }
        if (Resources.size() != m_BindingsBySet[SetIndex].size())
            return std::unexpected(ErrorMessage(Format("Shader binding set {} requires {} resources, received {}",
                                                       SetIndex,
                                                       m_BindingsBySet[SetIndex].size(),
                                                       Resources.size())));

        // After checking, we write the resource info into the slot
        for (Uint32 Index = 0; Index < Resources.size(); ++Index) {
            const auto [BindingSet, BindingIndex] = BindingLocations[Index];
            auto& SetBindings = m_BindingsBySet[BindingSet];
            const auto Slot = std::ranges::find_if(
                SetBindings, [BindingIndex](const RHIShaderBindingSet::Slot& Candidate) {
                    return Candidate.Info.BindingIndex == BindingIndex;
                });
            Slot->Resource = Resources[Index].Resource;
            Slot->IsRenderTarget = Resources[Index].IsRenderTarget;
            Slot->IsReadOnly = Resources[Index].IsReadOnly;
        }
        return OnResourcesBound(SetIndex);
    }

    /// Binds the sampled-image array used by the reflected bindless space.
    [[nodiscard]] auto BindBindlessResource(RHIRefArray<RHISampledTexture> Resource)
        -> std::expected<void, ErrorMessage> {
        if (!m_BindlessSpace)
            return std::unexpected(ErrorMessage("Shader binding set has no reflected bindless space"));
        if (!Resource)
            return std::unexpected(ErrorMessage("Bindless sampled-texture array is invalid"));
        if (m_BindlessTextures && *m_BindlessTextures != Resource)
            return std::unexpected(ErrorMessage("Shader binding set cannot use multiple bindless texture arrays"));
        m_BindlessTextures = std::move(Resource);
        return OnBindlessResourceBound();
    }

    template <typename T>
    [[nodiscard]] auto PushConstants(Uint32 Offset, const T& Data) -> std::expected<void, ErrorMessage> {
        const auto Bytes = std::as_bytes(std::span<const T>{&Data, 1});
        const auto Size  = static_cast<Uint64>(Bytes.size_bytes());
        if (Size == 0)
            return std::unexpected(ErrorMessage("Push constant data must not be empty"));

        const auto End          = static_cast<Uint64>(Offset) + Size;
        Uint64     ReflectedEnd = 0;
        for (const auto& Range : m_PushConstantRanges)
            ReflectedEnd = (std::max)(ReflectedEnd, static_cast<Uint64>(Range.Offset) + Range.Size);
        if (End > ReflectedEnd) {
            return std::unexpected(ErrorMessage(
                Format("Push constant range exceeds reflected size ({} + {} > {})", Offset, Size, ReflectedEnd)));
        }

        m_PendingPushConstants.push_back(PushConstantWrite{
            .Offset = Offset,
            .Data   = std::vector<std::byte>{Bytes.begin(), Bytes.end()},
        });
        return {};
    }

  protected:
    /// @brief One pass-local push-constant write awaiting state capture.
    struct PushConstantWrite {
        Uint32                 Offset = 0;
        std::vector<std::byte> Data   = {};
    };

    RHIShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc)
        : RHIObject(std::move(Name)),
          m_BindlessSpace(Desc.Reflection.BindlessSpace),
          m_PushConstantRanges(Desc.Reflection.PushConstants),
          m_Stages(Desc.Stages) {
        // Step 1: Resolve set count
        Uint32 SetCount = 0;
        // +1 because set index start with 0
        for (const auto& Reflected : Desc.Reflection.Bindings)
            SetCount = std::max(SetCount, Reflected.Set + 1);
        if (m_BindlessSpace)
            SetCount = std::max(SetCount, *m_BindlessSpace + 1);

        // Step 2: sort all bindings by set and binding index
        m_BindingsBySet.resize(SetCount);
        for (const auto& Reflected : Desc.Reflection.Bindings)
            m_BindingsBySet[Reflected.Set].push_back(Slot{.Info = Reflected});
        for (auto& SetBindings : m_BindingsBySet)
            std::ranges::sort(SetBindings, [](const auto& Left, const auto& Right) {
                return Left.Info.BindingIndex < Right.Info.BindingIndex;
            });
    }

    [[nodiscard]] virtual auto OnResourcesBound(Uint32) -> std::expected<void, ErrorMessage> = 0;
    /// Applies backend-specific updates after the bindless array is stored.
    [[nodiscard]] virtual auto OnBindlessResourceBound() -> std::expected<void, ErrorMessage> = 0;

    std::vector<std::vector<Slot>>                m_BindingsBySet        = {};
    std::vector<PushConstantWrite>                m_PendingPushConstants = {};
    std::optional<RHIRefArray<RHISampledTexture>> m_BindlessTextures     = std::nullopt;
    std::optional<Uint32>                         m_BindlessSpace        = std::nullopt;
    std::vector<ShaderPushConstantRange>          m_PushConstantRanges   = {};
    ShaderStage                                   m_Stages               = ShaderStage::Unknown;
};

struct RHISampledTextureDesc {
    std::span<const std::byte> Data     = {};
    Uint32                     Width    = 1;
    Uint32                     Height   = 1;
    Uint32                     Channels = 4;
    RHIFormat                  Format   = RHIFormat::R8G8B8A8_UNORM;
    RHITextureUsage            Usage    = RHITextureUsage::ShaderResource;
};

struct RHIRenderTargetDesc {
    Uint32          Width  = 1;
    Uint32          Height = 1;
    RHIFormat       Format = RHIFormat::B8G8R8A8_UNORM;
    RHITextureUsage Usage  = RHITextureUsage::RenderTarget;
};

// ── RHIPipeline ─────────────────────────────────────────────────────────────────

enum class RHIPrimitiveTopology : Uint8 {
    Unknown      = 0,
    TriangleList = 1,
};

// TODO: Validate GraphicsProgram and shader reflection at pipeline
// creation time:
//   - Code and Reflection must be non-null for every stage program
//   - Mesh shaders preclude Vertex/Hull/Domain/Geometry stages

struct RHIRasterizerState {
    bool    FillMode  = true; // true = solid, false = wireframe
    bool    CullMode  = true; // true = back cull
    Float32 LineWidth = 1.0f;
};

struct RHIBlendAttachment {
    bool BlendEnable = false;
};

struct RHIBlendState {
    RHIBlendAttachment Attachments[8] = {};
};

struct RHIDepthStencilState {
    bool DepthTestEnable  = true;
    bool DepthWriteEnable = true;
};

struct RHIGraphicsPipelineDesc {
    ShaderGraphicsProgram       Program           = {};
    RHIRef<RHIShaderBindingSet> BindingSet        = nullptr;
    RHIVertexInputLayoutDesc    VertexInputLayout = {};
    RHIPrimitiveTopology        Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState          Rasterizer        = {};
    RHIBlendState               Blend             = {};
    RHIDepthStencilState        DepthStencil      = {};
    std::vector<RHIFormat>      ColorFormats      = {RHIFormat::B8G8R8A8_UNORM};
    RHIFormat                   DepthFormat       = RHIFormat::Unknown;
};

// ── Clear values ─────────────────────────────────────────────────────────────

using RHIClearColorValue =
    std::variant<std::monostate, std::array<Float32, 4>, std::array<Int32, 4>, std::array<Uint32, 4>>;

struct RHIClearDepthStencilValue {
    Float32 Depth   = 1.0f;
    Uint32  Stencil = 0;
};

struct RHIColorAttachmentDesc {
    RHIRef<RHIRenderTarget> TextureRef = nullptr;
    RHIClearColorValue      ClearValue = {};
    bool                    Clear      = true;
};

struct RHIDepthAttachmentDesc {
    RHIRef<RHIRenderTarget>   TextureRef = nullptr;
    RHIClearDepthStencilValue ClearValue = {};
    bool                      Clear      = true;
};

/// @brief Fixed rendering attachments owned by one graphics pass.
///
/// Attachments define the pass execution context and therefore travel with
/// IRHIPass instead of being encoded as an individual RHICommand.
struct RHIGraphicsAttachments {
    std::vector<RHIColorAttachmentDesc>   ColorAttachments = {};
    std::optional<RHIDepthAttachmentDesc> DepthAttachment  = std::nullopt;
};

} // namespace SoulEngine
