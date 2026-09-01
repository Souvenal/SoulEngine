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
    explicit RHIObject(String Name) : m_Name(std::move(Name)) {}
    RHIObject(RHIObject&&)                    = default;
    auto operator=(RHIObject&&) -> RHIObject& = default;

  private:
    String m_Name = {};
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
    std::span<const std::byte> Data   = {};
    RHITransientBufferUsage    Usage  = RHITransientBufferUsage::ShaderRead;
};

/// @brief Logical per-frame uniform buffer resolved by the active RHI backend.
class RHITransientConstantBuffer : public RHIObject {
  public:
    RHITransientConstantBuffer(const RHITransientConstantBuffer&)                    = delete;
    auto operator=(const RHITransientConstantBuffer&) -> RHITransientConstantBuffer& = delete;
    RHITransientConstantBuffer(RHITransientConstantBuffer&&)                         = delete;
    auto operator=(RHITransientConstantBuffer&&) -> RHITransientConstantBuffer&      = delete;
    virtual ~RHITransientConstantBuffer()                                             = default;

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
    Uint64                m_Size  = 0;
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
using RHIShaderBindingResource = std::variant<std::monostate,
                                              RHIRef<RHIVertexBuffer>,
                                              RHIRef<RHIIndexBuffer>,
                                              RHIRef<RHITransientConstantBuffer>,
                                              RHIRef<RHITransientShaderStorageBuffer>,
                                              RHIRef<RHISampledTexture>,
                                              RHIRef<RHISampler>,
                                              RHIRef<RHIRenderTarget>,
                                              RHIRef<RHITopLevelAccelerationStructure>>;

/// @brief Reflected shader interface used to construct a binding set.
struct RHIShaderBindingSetDesc {
    ShaderReflection Reflection = {};
    /// Bitmask of every stage in the shader program (magic_enum flags).
    ShaderStage      Stages     = ShaderStage::Unknown;
};

/// @brief Backend-owned descriptor resources for one reflected shader layout.
class RHIShaderBindingSet : public RHIObject {
  public:
    /// @brief One reflected binding slot: the shader-side contract plus the
    /// bound resource.
    struct Slot {
        ShaderBinding            Info     = {};
        RHIShaderBindingResource Resource = {};
        bool                     IsReadOnly = true;
    };

    RHIShaderBindingSet(const RHIShaderBindingSet&)                    = delete;
    auto operator=(const RHIShaderBindingSet&) -> RHIShaderBindingSet& = delete;
    RHIShaderBindingSet(RHIShaderBindingSet&&)                         = delete;
    auto operator=(RHIShaderBindingSet&&) -> RHIShaderBindingSet&      = delete;
    virtual ~RHIShaderBindingSet() = default;

    [[nodiscard]] auto GetBindings() const -> std::span<const Slot> {
        return m_Bindings;
    }

    [[nodiscard]] auto HasUnboundBindings() const -> bool {
        if (m_Reflection.BindlessSpace && !m_BindlessTextures)
            return true;
        return std::ranges::any_of(m_Bindings, [](const Slot& Binding) {
            return std::holds_alternative<std::monostate>(Binding.Resource);
        });
    }

    template <typename T>
    [[nodiscard]] auto BindResource(StringView ResourceNameInShader,
                                    RHIRef<T>   Resource,
                                    bool        IsReadOnly)
        -> std::expected<void, ErrorMessage> {
        for (Uint32 BindingIndex = 0; BindingIndex < m_Bindings.size(); ++BindingIndex) {
            auto& Slot = m_Bindings[BindingIndex];
            if (Slot.Info.ParameterPath != ResourceNameInShader)
                continue;

            if (Slot.Info.ArrayCount != 1) {
                return std::unexpected(
                    ErrorMessage(Format("Shader parameter '{}' is an array; bind an RHIRefArray reference instead",
                                        ResourceNameInShader)));
            }
            if (Resource.GetState() == RHIRefState::Unknown) {
                return std::unexpected(
                    ErrorMessage(Format("RHI resource for shader parameter '{}' is invalid", ResourceNameInShader)));
            }
            const auto* Object = Resource.TryGet();
            if (Object && Object->GetShaderBindingType() != Slot.Info.Type) {
                return std::unexpected(ErrorMessage(
                    Format("RHI resource type does not match shader parameter '{}'", ResourceNameInShader)));
            }

            // Buffer bindings are always transient arena slices bound through
            // dynamic descriptors; persistent buffers use vertex input or
            // device addresses instead of descriptor bindings.
            const bool IsBufferBinding = Slot.Info.Type == ShaderResourceType::ConstantBuffer ||
                                         Slot.Info.Type == ShaderResourceType::StorageBuffer;
            const bool ResourceIsTransient = Object && Object->IsTransient();
            if (IsBufferBinding && !ResourceIsTransient) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' expects a transient buffer", ResourceNameInShader)));
            }
            if (!IsBufferBinding && ResourceIsTransient) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' does not accept a transient buffer", ResourceNameInShader)));
            }

            // Rebinding the identical resource is a no-op, so per-frame
            // rebuilds of an unchanged view stay legal on persistent slots.
            if (RHIShaderBindingResource NewResource{Resource}; Slot.Resource == NewResource)
                return {};

            // Persistent bindings are bind-once: the descriptor is written when
            // the binding set has not been referenced by any submission yet.
            // Transient slots rebind every frame with the frame's arena slice
            // without rewriting the descriptor.
            if (!std::holds_alternative<std::monostate>(Slot.Resource) && !ResourceIsTransient) {
                return std::unexpected(ErrorMessage(Format(
                    "Shader parameter '{}' is already bound; persistent bindings are bind-once",
                    ResourceNameInShader)));
            }

            Slot.Resource   = std::move(Resource);
            Slot.IsReadOnly = IsReadOnly;
            return OnResourceBound(BindingIndex);
        }

        return std::unexpected(
            ErrorMessage(Format("Shader parameter '{}' is not present in the shader binding layout",
                                ResourceNameInShader)));
    }

    template <typename T>
    [[nodiscard]] auto BindResource(RHIRefArray<T> Resource)
        -> std::expected<void, ErrorMessage> {
        if (!m_Reflection.BindlessSpace)
            return std::unexpected(ErrorMessage("Shader binding set has no reflected bindless space"));
        if constexpr (!std::same_as<T, RHISampledTexture>) {
            return std::unexpected(ErrorMessage("Only sampled-texture arrays support bindless shader bindings"));
        } else {
            if (m_BindlessTextures && *m_BindlessTextures != Resource)
                return std::unexpected(ErrorMessage("Shader binding set cannot use multiple bindless texture arrays"));
            m_BindlessTextures = std::move(Resource);
            return {};
        }
    }

    template <typename T>
    [[nodiscard]] auto PushConstants(Uint32 Offset, const T& Data)
        -> std::expected<void, ErrorMessage> {
        const auto Bytes = std::as_bytes(std::span<const T>{&Data, 1});
        const auto Size  = static_cast<Uint64>(Bytes.size_bytes());
        if (Size == 0)
            return std::unexpected(ErrorMessage("Push constant data must not be empty"));

        const auto End = static_cast<Uint64>(Offset) + Size;
        Uint64     ReflectedEnd = 0;
        for (const auto& Range : m_Reflection.PushConstants)
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

    [[nodiscard]] virtual auto Commit() -> std::expected<void, ErrorMessage> = 0;

  protected:
    /// @brief One pass-local push-constant write awaiting Commit().
    struct PushConstantWrite {
        Uint32              Offset = 0;
        std::vector<std::byte> Data = {};
    };

    RHIShaderBindingSet(String Name, const RHIShaderBindingSetDesc& Desc)
        : RHIObject(std::move(Name)), m_Reflection(Desc.Reflection), m_Stages(Desc.Stages) {
        m_Bindings.reserve(m_Reflection.Bindings.size());
        for (const auto& Reflected : m_Reflection.Bindings)
            m_Bindings.push_back(Slot{.Info = Reflected, .Resource = {}});
        std::ranges::sort(m_Bindings, [](const Slot& Left, const Slot& Right) {
            return std::pair{Left.Info.Set, Left.Info.BindingIndex} <
                   std::pair{Right.Info.Set, Right.Info.BindingIndex};
        });
    }

    [[nodiscard]] virtual auto OnResourceBound(Uint32 BindingIndex)
        -> std::expected<void, ErrorMessage> = 0;

    std::vector<PushConstantWrite> m_PendingPushConstants = {};
    std::optional<RHIRefArray<RHISampledTexture>> m_BindlessTextures = std::nullopt;
    ShaderReflection m_Reflection = {};
    ShaderStage m_Stages = ShaderStage::Unknown;

  private:
    std::vector<Slot>              m_Bindings            = {};
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
    ShaderGraphicsProgram    Program           = {};
    RHIRef<RHIShaderBindingSet> BindingSet      = nullptr;
    RHIVertexInputLayoutDesc VertexInputLayout = {};
    RHIPrimitiveTopology     Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState       Rasterizer        = {};
    RHIBlendState            Blend             = {};
    RHIDepthStencilState     DepthStencil      = {};
    std::vector<RHIFormat>   ColorFormats      = {RHIFormat::B8G8R8A8_UNORM};
    RHIFormat                DepthFormat       = RHIFormat::Unknown;
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
