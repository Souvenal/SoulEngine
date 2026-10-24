module;

#include <hlsl++.h>

export module RHI.Types;

import Core;
import RHI.Ref;
import magic_enum;

import std;

namespace SoulEngine {

/// Common identity metadata for persistent RHI objects.
export class RHIObject {
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

  protected:
    explicit RHIObject(String Name) : m_Name(std::move(Name)), m_Id(s_NextId.fetch_add(1, std::memory_order_relaxed)) {}
    RHIObject(RHIObject&&)                    = default;
    auto operator=(RHIObject&&) -> RHIObject& = default;

  private:
    inline static std::atomic<Uint64> s_NextId = 1;

    String m_Name = {};
    Uint64 m_Id   = 0;
};

export enum class RHISamplerProfile : Uint8 {
    Unknown = 0,
    LinearRepeat,
    AnisotropicRepeat,
};

export struct RHISamplerDesc {
    RHISamplerProfile Profile = RHISamplerProfile::LinearRepeat;
};

/// @brief Shader-visible sampling state object.
///
/// Backends own the native sampler handle. Higher-level owners keep RHIRef instances; command lists only
/// observe them.
export class RHISampler : public RHIObject {
  public:
    RHISampler(const RHISampler&)                    = delete;
    auto operator=(const RHISampler&) -> RHISampler& = delete;
    RHISampler(RHISampler&&)                         = delete;
    auto operator=(RHISampler&&) -> RHISampler&      = delete;
    virtual ~RHISampler()                            = default;

    [[nodiscard]] auto GetDesc() const -> const RHISamplerDesc& {
        return m_Desc;
    }

  protected:
    explicit RHISampler(String Name, const RHISamplerDesc& Desc) : RHIObject(std::move(Name)), m_Desc(Desc) {}

  private:
    RHISamplerDesc m_Desc = {};
};
// Texture

export enum class RHIFormat : Uint8 {
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

export struct RHIVertexInputAttributeDesc {
    Uint32    Location = 0;
    Uint32    Binding  = 0;
    RHIFormat Format   = RHIFormat::Unknown;
    Uint32    Offset   = 0;
};

export struct RHIVertexInputBindingDesc {
    Uint32 Binding = 0;
    Uint32 Stride  = 0;
};

export inline constexpr Uint32 kMaxVertexBufferBindings = 4;

export struct RHIVertexInputLayoutDesc {
    std::vector<RHIVertexInputBindingDesc>   Bindings   = {};
    std::vector<RHIVertexInputAttributeDesc> Attributes = {};
};

export enum class RHIPrimitiveTopology : Uint8 {
    Unknown      = 0,
    TriangleList = 1,
};

export struct RHIRasterizerState {
    bool    FillMode  = true; // true = solid, false = wireframe
    bool    CullMode  = true; // true = back cull
    Float32 LineWidth = 1.0f;
};

export enum class RHIBlendFactor : Uint8 {
    Zero             = 0,
    One              = 1,
    SrcColor         = 2,
    OneMinusSrcColor = 3,
    DstColor         = 4,
    OneMinusDstColor = 5,
    SrcAlpha         = 6,
    OneMinusSrcAlpha = 7,
    DstAlpha         = 8,
    OneMinusDstAlpha = 9,
};

export enum class RHIBlendOp : Uint8 {
    Add             = 0,
    Subtract        = 1,
    ReverseSubtract = 2,
    Min             = 3,
    Max             = 4,
};

export struct RHIBlendAttachment {
    bool           BlendEnable         = false;
    RHIBlendFactor SrcColorBlendFactor = RHIBlendFactor::One;
    RHIBlendFactor DstColorBlendFactor = RHIBlendFactor::Zero;
    RHIBlendOp     ColorBlendOp        = RHIBlendOp::Add;
    RHIBlendFactor SrcAlphaBlendFactor = RHIBlendFactor::One;
    RHIBlendFactor DstAlphaBlendFactor = RHIBlendFactor::Zero;
    RHIBlendOp     AlphaBlendOp        = RHIBlendOp::Add;

    bool operator==(const RHIBlendAttachment&) const = default;
};

/// @brief One color attachment's fixed-function pipeline state: output format plus its blend configuration.
export struct RHIColorAttachmentState {
    RHIFormat          Format = RHIFormat::Unknown;
    RHIBlendAttachment Blend  = {}; ///< Blending disabled.

    bool operator==(const RHIColorAttachmentState&) const = default;
};

export struct RHIDepthStencilState {
    bool DepthTestEnable  = true;
    bool DepthWriteEnable = true;
};

export enum class RHITextureUsage : Uint32 {
    None         = 0,
    RenderTarget = 1u << 0,
    DepthStencil = 1u << 1,
    Sampled      = 1u << 2,
    Storage      = 1u << 4,
    TransferSrc  = 1u << 5,
    TransferDst  = 1u << 6,
};

/// @brief Allocation-legality usage flags for a unified buffer.
/// Mirrors VkBufferUsageFlagBits roles; the creation union must cover every future use.
/// TransferSrc/TransferDst double as the RenderGraph view/sync declaration axis.
export enum class RHIBufferUsage : Uint32 {
    Unknown     = 0,
    Vertex      = 1u << 0,
    Index       = 1u << 1,
    /// @brief Constant-buffer role (D3D/UE "constant buffer" = Vulkan "uniform buffer").
    ///
    /// Named Constant to match the shader-side vocabulary (ShaderResourceType::ConstantBuffer).
    Constant    = 1u << 2,
    Storage     = 1u << 3,
    Indirect    = 1u << 4,
    TransferSrc = 1u << 5,
    TransferDst = 1u << 6,
};

/// Resource lifetime axis
export enum class RHILifetime : Uint8 {
    Unknown,
    /// Individually owned; outlives frames.
    Persistent,
    /// Per-frame belt slice, valid only for the logical frame in which it was created.
    Transient,
};

/// Memory placement of a buffer allocation. Placement decides the upload mechanism.
export enum class RHIMemoryUsage : Uint8 {
    Unknown,
    /// Device-local transfer destination; upload rides a staging copy.
    GpuOnly,
    /// Host-visible; upload is an RHI-thread memory copy.
    CpuToGpu,
};

export struct RHIBufferDesc {
    Uint64         SizeBytes   = 0;
    RHIBufferUsage Usage       = RHIBufferUsage::Unknown;
    RHIMemoryUsage MemoryUsage = RHIMemoryUsage::Unknown;
    RHILifetime    Lifetime    = RHILifetime::Unknown;
};

/// @brief Unified buffer identity.
///
/// Vertex/index/uniform/storage/indirect are usages, not types — GPU→CPU readback is not a buffer at all, it
/// is the RHIReadback consumer object below. Lifetime is stored on the identity and discriminates the
/// backend's per-flavor state (persistent allocation vs transient belt slice), whose allocation strategy the
/// backend concrete class owns.
export class RHIBuffer : public RHIObject {
  public:
    RHIBuffer(const RHIBuffer&)                    = delete;
    auto operator=(const RHIBuffer&) -> RHIBuffer& = delete;
    RHIBuffer(RHIBuffer&&)                         = delete;
    auto operator=(RHIBuffer&&) -> RHIBuffer&      = delete;
    virtual ~RHIBuffer()                           = default;

    [[nodiscard]] auto GetSize() const noexcept -> Uint64 {
        return m_Size;
    }
    [[nodiscard]] auto GetUsage() const noexcept -> RHIBufferUsage {
        return m_Usage;
    }
    [[nodiscard]] auto GetMemoryUsage() const noexcept -> RHIMemoryUsage {
        return m_MemoryUsage;
    }
    /// @brief Lifetime of this buffer.
    ///
    /// Fixed at creation (validated Desc or a hardcoded backend flavor), so a constructed buffer never holds
    /// Unknown.
    [[nodiscard]] auto GetLifetime() const noexcept -> RHILifetime {
        return m_Lifetime;
    }
    /// @brief Buffer device address (Vulkan BDA).
    ///
    /// Every GpuOnly buffer carries one; host-visible and transient flavors return 0.
    [[nodiscard]] virtual auto GetDeviceAddress() const noexcept -> Uint64 {
        return 0;
    }

  protected:
    explicit RHIBuffer(String Name, Uint64 Size, RHIBufferUsage Usage, RHIMemoryUsage MemoryUsage, RHILifetime Lifetime)
        : RHIObject(std::move(Name)), m_Size(Size), m_Usage(Usage), m_MemoryUsage(MemoryUsage), m_Lifetime(Lifetime) {}

  private:
    Uint64         m_Size        = 0;
    RHIBufferUsage m_Usage       = RHIBufferUsage::Unknown;
    RHIMemoryUsage m_MemoryUsage = RHIMemoryUsage::Unknown;
    RHILifetime    m_Lifetime    = RHILifetime::Unknown;
};

/// @brief GPU→CPU readback identity — a consumer object, not a resource kind (RHISampler's category).
///
/// Flow: RHIRenderDevice::CreateReadback (synchronous, Ready at return) → a transfer-stage pass records
/// ReadbackTexture each frame → the game thread polls TryRead(). At most one write per frame per readback.
/// TryRead() is valid on the game thread while the device is alive.
export class RHIReadback : public RHIObject {
  public:
    RHIReadback(const RHIReadback&)                    = delete;
    auto operator=(const RHIReadback&) -> RHIReadback& = delete;
    RHIReadback(RHIReadback&&)                         = delete;
    auto operator=(RHIReadback&&) -> RHIReadback&      = delete;
    virtual ~RHIReadback()                             = default;

    /// Per-slot payload capacity in bytes, fixed at creation.
    [[nodiscard]] auto GetSize() const noexcept -> Uint64 {
        return m_Size;
    }

    /// @brief Poll the newest GPU-completed payload.
    ///
    /// Non-blocking latest-value read; an empty vector means no submission has completed a write yet. Bytes
    /// are the honest currency — the caller knows the payload layout it asked the pass to copy.
    [[nodiscard]] virtual auto TryRead() const -> std::vector<std::byte> = 0;

  protected:
    explicit RHIReadback(String Name, Uint64 Size) : RHIObject(std::move(Name)), m_Size(Size) {}

  private:
    Uint64 m_Size = 0;
};

export struct RHITextureDesc {
    Uint32          Width       = 1;
    Uint32          Height      = 1;
    Uint32          MipLevels   = 1;
    RHIFormat       Format      = RHIFormat::B8G8R8A8_UNORM;
    /// @brief Flags default to None: a role is always an explicit choice (the RenderGraph validates None at
    /// registration).
    ///
    /// The RenderGraph rule: creation usage is explicit here; a pass view's declared usage must stay a subset
    /// of it.
    RHITextureUsage Usage       = RHITextureUsage::None;
    /// @brief Allocation placement, same axis as RHIBufferDesc.
    ///
    /// GpuOnly derives transfer-dst capability at creation and uploads through the frame-start staging batch
    /// (UploadTexture); host-visible placements are not supported for images. Unknown fails validation — a
    /// Desc is explicit.
    RHIMemoryUsage  MemoryUsage = RHIMemoryUsage::Unknown;
    /// @brief Texel channel count (bytes per texel for the upload path).
    ///
    /// UploadTexture validates its byte count against Width × Height × Channels.
    Uint32          Channels    = 4;
};

/// @brief Unified texture identity covering both sampled and render-target roles.
///
/// Color vs depth vs sampled is distinguished by GetFormat()/GetUsage(), never by type.
export class RHITexture : public RHIObject {
  public:
    RHITexture(const RHITexture&)                    = delete;
    auto operator=(const RHITexture&) -> RHITexture& = delete;
    RHITexture(RHITexture&&)                         = delete;
    auto operator=(RHITexture&&) -> RHITexture&      = delete;
    virtual ~RHITexture()                            = default;

    [[nodiscard]] auto GetWidth() const noexcept -> Uint32 {
        return m_Width;
    }
    [[nodiscard]] auto GetHeight() const noexcept -> Uint32 {
        return m_Height;
    }
    [[nodiscard]] auto GetFormat() const noexcept -> RHIFormat {
        return m_Format;
    }
    [[nodiscard]] auto GetUsage() const noexcept -> RHITextureUsage {
        return m_Usage;
    }
    [[nodiscard]] auto GetMipLevelCount() const noexcept -> Uint32 {
        return m_MipLevels;
    }
    [[nodiscard]] auto GetMemoryUsage() const noexcept -> RHIMemoryUsage {
        return m_MemoryUsage;
    }
    [[nodiscard]] auto GetChannels() const noexcept -> Uint32 {
        return m_Channels;
    }

  protected:
    explicit RHITexture(String Name, const RHITextureDesc& Desc)
        : RHIObject(std::move(Name)),
          m_Width(Desc.Width),
          m_Height(Desc.Height),
          m_Format(Desc.Format),
          m_Usage(Desc.Usage),
          m_MipLevels(Desc.MipLevels),
          m_MemoryUsage(Desc.MemoryUsage),
          m_Channels(Desc.Channels) {}

  private:
    Uint32          m_Width       = 0;
    Uint32          m_Height      = 0;
    RHIFormat       m_Format      = RHIFormat::Unknown;
    RHITextureUsage m_Usage       = RHITextureUsage::None;
    Uint32          m_MipLevels   = 1;
    RHIMemoryUsage  m_MemoryUsage = RHIMemoryUsage::Unknown;
    Uint32          m_Channels    = 4;
};
// Ray-tracing acceleration structures

/// @brief One position-only Float32x3 triangle geometry entry in a bottom-level acceleration structure.
///
/// BLAS triangle inputs. Geometry metadata (counts, stride) is mesh data, carried here by the caller —
/// buffers are unified identities and do not know vertex semantics.
export struct RHIBottomLevelAccelerationStructureDesc {
    RHIRef<RHIBuffer> VertexBufferRef = nullptr;
    RHIRef<RHIBuffer> IndexBufferRef  = nullptr;
    Uint64            VertexCount     = 0;
    Uint32            VertexStride    = 0;
    Uint64            IndexCount      = 0;
};

/// GPU payload containing reusable object-space triangle geometry.
export class RHIBottomLevelAccelerationStructure : public RHIObject {
  public:
    RHIBottomLevelAccelerationStructure(const RHIBottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const RHIBottomLevelAccelerationStructure&) -> RHIBottomLevelAccelerationStructure& = delete;
    RHIBottomLevelAccelerationStructure(RHIBottomLevelAccelerationStructure&&)                         = delete;
    auto operator=(RHIBottomLevelAccelerationStructure&&) -> RHIBottomLevelAccelerationStructure&      = delete;
    virtual ~RHIBottomLevelAccelerationStructure()                                                     = default;

  protected:
    explicit RHIBottomLevelAccelerationStructure(String Name) : RHIObject(std::move(Name)) {}
};

/// One TLAS instance: a BLAS plus its world transform for this frame.
export struct RHITopLevelAccelerationStructureInstance {
    RHIRef<RHIBottomLevelAccelerationStructure> Blas      = nullptr;
    hlslpp::float4x4                            Transform = hlslpp::float4x4::identity();
};

/// @brief Descriptor for a per-frame TLAS.
///
/// Instances slot order is the canonical TLAS instance order: the device packs them 1:1, so Slang
/// InstanceIndex() indexes the renderer's sorted instance table by the same slot.
export struct RHITopLevelAccelerationStructureDesc {
    std::vector<RHITopLevelAccelerationStructureInstance> Instances = {};
};

/// @brief Per-frame TLAS handle.
///
/// Instances are packed and storage is materialized at creation; the device builds it at frame start.
export class RHITopLevelAccelerationStructure : public RHIObject {
  public:
    RHITopLevelAccelerationStructure(const RHITopLevelAccelerationStructure&)                    = delete;
    auto operator=(const RHITopLevelAccelerationStructure&) -> RHITopLevelAccelerationStructure& = delete;
    RHITopLevelAccelerationStructure(RHITopLevelAccelerationStructure&&)                         = delete;
    auto operator=(RHITopLevelAccelerationStructure&&) -> RHITopLevelAccelerationStructure&      = delete;
    virtual ~RHITopLevelAccelerationStructure()                                                  = default;

  protected:
    explicit RHITopLevelAccelerationStructure(String Name) : RHIObject(std::move(Name)) {}
};

export inline constexpr Uint32 kRHIRemainingSubresources = std::numeric_limits<Uint32>::max();
export inline constexpr Uint64 kRHIWholeBuffer           = std::numeric_limits<Uint64>::max();

/// @brief Read/Write direction of a view
export enum class RHIAccess : Uint8 {
    None  = 0,
    Read  = 1 << 0,
    Write = 1 << 1,
};

/// @brief Mip-level window into a texture; defaults to the whole chain.
export struct RHISubresourceRange {
    Uint32 BaseMipLevel  = 0;
    Uint32 MipLevelCount = kRHIRemainingSubresources;
};

/// @brief Value-semantic texture view: identity, role, extent, direction.
/// Plain data, not an RHIObject — lifetime comes from the embedded ref and the backend resolves/caches native
/// views at record time. One view, one role: Usage is a single-role selector, never the creation union.
export struct RHITextureView {
    RHIRef<RHITexture>  Texture = nullptr;
    RHIAccess           Access  = RHIAccess::Read;
    RHITextureUsage     Usage   = RHITextureUsage::None;
    RHISubresourceRange Range   = {};
};

/// @brief Buffer counterpart: (buffer, usage, offset, range, access).
export struct RHIBufferView {
    RHIRef<RHIBuffer> Buffer = nullptr;
    RHIAccess         Access = RHIAccess::Read;
    RHIBufferUsage    Usage  = RHIBufferUsage::Unknown;
    Uint64            Offset = 0;
    Uint64            Range  = kRHIWholeBuffer;
};

/// @brief Degenerate sampler view: identity only.
///
/// A sampler is a state object with no role, extent, or direction axis, so the view is a plain ref wrapper —
/// it exists so bindings retain the payload like every other view kind.
export struct RHISamplerView {
    RHIRef<RHISampler> Sampler = nullptr;
};

/// @brief Degenerate TLAS view: identity only.
///
/// An acceleration structure binds whole, so the view is a plain ref wrapper — it exists so bindings retain
/// the payload like every other view kind.
export struct RHITlasView {
    RHIRef<RHITopLevelAccelerationStructure> Tlas = nullptr;
};

export using RHIClearColorValue =
    std::variant<std::monostate, std::array<Float32, 4>, std::array<Int32, 4>, std::array<Uint32, 4>>;

export struct RHIClearDepthStencilValue {
    Float32 Depth   = 1.0f;
    Uint32  Stencil = 0;
};

/// @brief One color attachment: the RenderTarget view plus its clear policy.
export struct RHIColorAttachmentDesc {
    RHITextureView     View       = {}; ///< Range selects the rendered mip(s)
    RHIClearColorValue ClearValue = {};
    bool               Clear      = true;
};

/// @brief Depth/stencil counterpart of RHIColorAttachmentDesc.
export struct RHIDepthAttachmentDesc {
    RHITextureView            View       = {};
    RHIClearDepthStencilValue ClearValue = {};
    bool                      Clear      = true;
};

/// @brief Fixed rendering attachments owned by one graphics pass — the pass
/// execution context, traveling with IRHIPass rather than any RHICommand.
export struct RHIGraphicsAttachments {
    std::vector<RHIColorAttachmentDesc>   ColorAttachments = {};
    std::optional<RHIDepthAttachmentDesc> DepthAttachment  = std::nullopt;
};

// Synchronization vocabulary
// The RenderGraph produces dst-only prologue transitions from declared pass accesses; the backend's
// emission-side diff tracker materializes a native barrier only when tracked state differs. A transition is
// view + stage: the native access mask and the image layout are pure functions of the view's (Usage, Access),
// and that derivation lives in the backend's native mapping at emission, so this vocabulary carries no
// access-widening or layout axis. The stage schema is a fine-grained bit mask (D3D12 barrier-state shape) so
// precision upgrades are mapping-table data changes, not schema changes.

/// @brief Pipeline-stage mask of one endpoint of a resource state.
///
/// The initial mapping table only ever produces coarse values (AllGraphics for shader accesses in graphics
/// passes); precise stages (VertexShader vs FragmentShader) are reserved for future per-view stage hints.
export enum class RHISyncStage : Uint32 {
    None                  = 0,
    VertexShader          = 1 << 0,
    FragmentShader        = 1 << 1,
    ComputeShader         = 1 << 2,
    RayTracingShader      = 1 << 3,
    DrawIndirect          = 1 << 4,
    ColorAttachmentOutput = 1 << 5,
    EarlyFragmentTests    = 1 << 6,
    LateFragmentTests     = 1 << 7,
    Transfer              = 1 << 8,
    Host                  = 1 << 9,
    AllGraphics           = VertexShader | FragmentShader | DrawIndirect | ColorAttachmentOutput | EarlyFragmentTests |
        LateFragmentTests,
};

/// @brief A dst-only state requirement for one buffer: "the pass needs this
/// buffer accessed as View at Stage before its commands run". A transition is a declaration, not a barrier —
/// it carries no src/dst pair; the backend's diff tracker decides whether a barrier materializes. The View
/// travels by value: a pass declaring several views of one buffer merges them into one transition whose View
/// accumulates the usage and access union.
export struct RHIBufferTransition {
    RHIBufferView View  = {};
    RHISyncStage  Stage = RHISyncStage::None;
};

/// @brief Texture counterpart of RHIBufferTransition. The view's
/// subresource range defaults to the whole chain; the backend tracker resolves per-subresource state from it.
export struct RHITextureTransition {
    RHITextureView View  = {};
    RHISyncStage   Stage = RHISyncStage::None;
};

/// @brief One pass's prologue: the complete set of state requirements its
/// declared accesses map to, deduplicated per resource.
export struct RHIBarrierList {
    std::vector<RHIBufferTransition>  Buffers  = {};
    std::vector<RHITextureTransition> Textures = {};
};
} // namespace SoulEngine

namespace magic_enum::customize {

export template <>
struct enum_range<SoulEngine::RHITextureUsage> {
    static constexpr bool is_flags = true;
};
export template <>
struct enum_range<SoulEngine::RHIBufferUsage> {
    static constexpr bool is_flags = true;
};
export template <>
struct enum_range<SoulEngine::RHIAccess> {
    static constexpr bool is_flags = true;
};
export template <>
struct enum_range<SoulEngine::RHISyncStage> {
    static constexpr bool is_flags = true;
};
} // namespace magic_enum::customize
