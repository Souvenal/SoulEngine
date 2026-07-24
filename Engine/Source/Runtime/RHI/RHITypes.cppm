module;

#include <magic_enum/magic_enum.hpp>

export module RHI:Types;

export import Core;
import Shader;

export import std;

export namespace SoulEngine {

class RHIRenderTarget;
class RHITopLevelAccelerationStructure;
class RHIRayTracingGeometryTable;

// ── Buffer descriptor types ────────────────────────────────────────────────

struct RHIVertexBufferDesc {
    const void* Data        = nullptr;
    Uint64      VertexCount = 0;
    Uint32      Stride      = 0;
};

struct RHIIndexBufferDesc {
    const void* Data       = nullptr;
    Uint64      IndexCount = 0;
};

struct RHIConstantBufferDesc {
    Uint64 Size = 0;
};

enum class RHISamplerProfile : Uint8 {
    Unknown = 0,
    LinearRepeat,
    AnisotropicRepeat,
};

struct RHISamplerDesc {
    RHISamplerProfile Profile = RHISamplerProfile::LinearRepeat;
};

// ── RHIGpuResource — base for GPU resources with usage tracking ────────────

struct RHIGpuCompletionToken {
    Uint64 Id = 0;
};

/// Base class for GPU resources that tracks the last command-list usage token.
/// Resources inheriting this can participate in deferred deletion via
/// DeletionQueue: when the GPU completes all work up to the last usage token,
/// the resource is safe to destroy.
class RHIGpuResource {
  public:
    RHIGpuResource()                                      = default;
    RHIGpuResource(const RHIGpuResource&)                    = delete;
    auto operator=(const RHIGpuResource&) -> RHIGpuResource& = delete;
    RHIGpuResource(RHIGpuResource&&)                         = delete;
    auto operator=(RHIGpuResource&&) -> RHIGpuResource&      = delete;
    virtual ~RHIGpuResource()                             = default;

    [[nodiscard]] auto GetLastUsageToken() const noexcept -> RHIGpuCompletionToken {
        return m_LastUsage;
    }
    auto UpdateLastUsageToken(RHIGpuCompletionToken Token) noexcept -> void {
        m_LastUsage = Token;
    }

  private:
    RHIGpuCompletionToken m_LastUsage = {};
};

// ── Typed GPU buffer polymorphic bases ──────────────────────────────────

/// Empty polymorphic base for vertex buffer resources.
/// Backend concrete class (e.g. VulkanVertexBuffer) owns the GPU allocation.
/// ResourceManager owns RHIVertexBuffer instances; command lists only observe them.
class RHIVertexBuffer : public RHIGpuResource {
  public:
    RHIVertexBuffer()                                       = default;
    RHIVertexBuffer(const RHIVertexBuffer&)                    = delete;
    auto operator=(const RHIVertexBuffer&) -> RHIVertexBuffer& = delete;
    RHIVertexBuffer(RHIVertexBuffer&&)                         = delete;
    auto operator=(RHIVertexBuffer&&) -> RHIVertexBuffer&      = delete;
    virtual ~RHIVertexBuffer()                              = default;
};

/// Empty polymorphic base for index buffer resources.
/// Same role as RHIVertexBuffer, for index data.
class RHIIndexBuffer : public RHIGpuResource {
  public:
    RHIIndexBuffer()                                      = default;
    RHIIndexBuffer(const RHIIndexBuffer&)                    = delete;
    auto operator=(const RHIIndexBuffer&) -> RHIIndexBuffer& = delete;
    RHIIndexBuffer(RHIIndexBuffer&&)                         = delete;
    auto operator=(RHIIndexBuffer&&) -> RHIIndexBuffer&      = delete;
    virtual ~RHIIndexBuffer()                             = default;
};

/// Logical shader-visible constant block identity.
///
/// This object declares the size and stable RHI identity of a constant block.
/// It does not imply a dedicated backend buffer allocation. Backends lower
/// command-list constant writes into their own in-flight-safe storage.
class RHIConstantBuffer {
  public:
    explicit RHIConstantBuffer(const RHIConstantBufferDesc& Desc) {
        m_Size = Desc.Size;
    }
    RHIConstantBuffer(const RHIConstantBuffer&)                    = delete;
    auto operator=(const RHIConstantBuffer&) -> RHIConstantBuffer& = delete;
    RHIConstantBuffer(RHIConstantBuffer&&)                         = delete;
    auto operator=(RHIConstantBuffer&&) -> RHIConstantBuffer&      = delete;
    virtual ~RHIConstantBuffer()                                = default;

    /// Return the declared size in bytes.
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }

  private:
    Uint64 m_Size = 0;
};

/// Shader-visible sampling state object.
///
/// Backends own the native sampler handle. ResourceManager owns RHISampler
/// instances; command lists only observe them.
class RHISampler : public RHIGpuResource {
  public:
    explicit RHISampler(const RHISamplerDesc& Desc) {
        m_Desc = Desc;
    }
    RHISampler(const RHISampler&)                    = delete;
    auto operator=(const RHISampler&) -> RHISampler& = delete;
    RHISampler(RHISampler&&)                         = delete;
    auto operator=(RHISampler&&) -> RHISampler&      = delete;
    virtual ~RHISampler()                         = default;

    [[nodiscard]] auto GetDesc() const -> const RHISamplerDesc& {
        return m_Desc;
    }

  private:
    RHISamplerDesc m_Desc = {};
};

/// @brief One reflected resource binding in a shader parameter-set layout.
struct RHIShaderParameterBindingLayout {
    String               ParameterPath = {};
    Uint32               Binding       = 0;
    ShaderResourceType Type          = ShaderResourceType::Unknown;
    Uint32               ArrayCount    = 1;
};

/// @brief Immutable reflected layout for one shader descriptor set.
class RHIShaderParameterSetLayout {
  public:
    RHIShaderParameterSetLayout() = default;

    [[nodiscard]] auto GetSetIndex() const -> Uint32 {
        return m_SetIndex;
    }

    [[nodiscard]] auto GetBindings() const -> std::span<const RHIShaderParameterBindingLayout> {
        return m_Bindings;
    }

    [[nodiscard]] auto FindBinding(StringView ParameterPath) const -> const RHIShaderParameterBindingLayout* {
        for (const auto& Binding : m_Bindings) {
            if (Binding.ParameterPath == ParameterPath)
                return &Binding;
        }
        return nullptr;
    }

  private:
    friend class RHIShaderParameterLayout;

    Uint32                                        m_SetIndex = 0;
    std::vector<RHIShaderParameterBindingLayout> m_Bindings = {};
};

/// @brief Immutable shader parameter interface derived from pipeline reflection.
class RHIShaderParameterLayout {
  public:
    [[nodiscard]] static auto Create(const ShaderReflection& Reflection) -> RHIShaderParameterLayout {
        RHIShaderParameterLayout Result;
        Result.m_Id = NextId();
        if (Reflection.Bindings.empty())
            return Result;

        Uint32 MaxSet = 0;
        for (const auto& Binding : Reflection.Bindings)
            MaxSet = (std::max)(MaxSet, Binding.Set);

        Result.m_Sets.resize(MaxSet + 1);
        for (Uint32 SetIndex = 0; SetIndex < Result.m_Sets.size(); ++SetIndex)
            Result.m_Sets[SetIndex].m_SetIndex = SetIndex;

        for (const auto& Binding : Reflection.Bindings) {
            Result.m_Sets[Binding.Set].m_Bindings.push_back(RHIShaderParameterBindingLayout{
                .ParameterPath = Binding.ParameterPath,
                .Binding       = Binding.BindingIndex,
                .Type          = Binding.Type,
                .ArrayCount    = Binding.ArrayCount,
            });
        }

        for (auto& Set : Result.m_Sets)
            std::ranges::sort(Set.m_Bindings, {}, &RHIShaderParameterBindingLayout::Binding);

        return Result;
    }

    [[nodiscard]] auto GetId() const -> Uint64 {
        return m_Id;
    }

    [[nodiscard]] auto GetSets() const -> std::span<const RHIShaderParameterSetLayout> {
        return m_Sets;
    }

    [[nodiscard]] auto GetSetLayout(Uint32 SetIndex) const -> const RHIShaderParameterSetLayout* {
        if (SetIndex >= m_Sets.size())
            return nullptr;
        return &m_Sets[SetIndex];
    }

  private:
    [[nodiscard]] static auto NextId() -> Uint64 {
        static std::atomic<Uint64> Next = 1;
        return Next.fetch_add(1, std::memory_order_relaxed);
    }

    Uint64                                m_Id   = 0;
    std::vector<RHIShaderParameterSetLayout> m_Sets = {};
};

/// Common polymorphic base for pipelines that consume reflection-derived shader parameters.
class RHIPipeline : public RHIGpuResource {
  public:
    RHIPipeline()                                  = default;
    RHIPipeline(const RHIPipeline&)                    = delete;
    auto operator=(const RHIPipeline&) -> RHIPipeline& = delete;
    RHIPipeline(RHIPipeline&&)                         = delete;
    auto operator=(RHIPipeline&&) -> RHIPipeline&      = delete;
    virtual ~RHIPipeline()                          = default;

    [[nodiscard]] auto GetShaderParameterLayout() const -> const RHIShaderParameterLayout& {
        return m_ShaderParameterLayout;
    }

  protected:
    auto SetShaderParameterLayout(RHIShaderParameterLayout Layout) -> void {
        m_ShaderParameterLayout = std::move(Layout);
    }

  private:
    RHIShaderParameterLayout m_ShaderParameterLayout = {};
};

/// Empty polymorphic base for graphics pipeline resources.
/// Backend concrete class (e.g. VulkanGraphicsPipeline) owns the native
/// pipeline. ResourceManager owns RHIGraphicsPipeline instances.
class RHIGraphicsPipeline : public RHIPipeline {
  public:
    RHIGraphicsPipeline()                                           = default;
    RHIGraphicsPipeline(const RHIGraphicsPipeline&)                    = delete;
    auto operator=(const RHIGraphicsPipeline&) -> RHIGraphicsPipeline& = delete;
    RHIGraphicsPipeline(RHIGraphicsPipeline&&)                         = delete;
    auto operator=(RHIGraphicsPipeline&&) -> RHIGraphicsPipeline&      = delete;
    virtual ~RHIGraphicsPipeline()                                  = default;

    [[nodiscard]] auto GetShaderParameterLayout() const -> const RHIShaderParameterLayout& {
        return RHIPipeline::GetShaderParameterLayout();
    }

  protected:
    auto SetShaderParameterLayout(RHIShaderParameterLayout Layout) -> void {
        RHIPipeline::SetShaderParameterLayout(std::move(Layout));
    }
};

// ── Opaque handle types ───────────────────────────────────────────────────

/// Polymorphic base for shader-readable sampled texture resources.
/// Backend concrete class (e.g. VulkanSampledTexture) owns the GPU allocation.
/// ResourceManager owns RHISampledTexture instances.
class RHISampledTexture : public RHIGpuResource {
  public:
    RHISampledTexture()                                         = default;
    RHISampledTexture(const RHISampledTexture&)                    = delete;
    auto operator=(const RHISampledTexture&) -> RHISampledTexture& = delete;
    RHISampledTexture(RHISampledTexture&&)                         = delete;
    auto operator=(RHISampledTexture&&) -> RHISampledTexture&      = delete;
    virtual ~RHISampledTexture()                                = default;

    [[nodiscard]] virtual auto GetWidth() const -> Uint32  = 0;
    [[nodiscard]] virtual auto GetHeight() const -> Uint32 = 0;
};

/// @brief Mutable shader-visible array of resource observers.
///
/// Resource-layer arrays retain the corresponding ResourceRefs. This RHI value
/// contains only the resolved observers recorded into command lists.
template <typename T>
class RHIResourceArray {
  public:
    RHIResourceArray() = default;

    [[nodiscard]] auto GetSize() const -> Uint32 {
        return static_cast<Uint32>(m_Resources.size());
    }

    [[nodiscard]] auto GetResources() const -> std::span<T* const> {
        return m_Resources;
    }

    [[nodiscard]] auto GetRevision() const -> Uint64 {
        return m_Revision;
    }

    auto Set(Uint32 Slot, T* Resource) -> void {
        if (Slot >= m_Resources.size())
            m_Resources.resize(Slot + 1);
        if (m_Resources[Slot] == Resource)
            return;

        m_Resources[Slot] = Resource;
        ++m_Revision;
    }

  private:
    std::vector<T*> m_Resources = {};
    Uint64          m_Revision  = 0;
};

template <typename T>
struct RHIShaderParameterResourceTraits;

template <>
struct RHIShaderParameterResourceTraits<RHISampledTexture> {
    static constexpr ShaderResourceType Type = ShaderResourceType::SampledTexture;
};

template <typename T>
concept ShaderParameterArrayResource = requires {
    { RHIShaderParameterResourceTraits<T>::Type } -> std::convertible_to<ShaderResourceType>;
};

/// @brief CPU-side snapshot for one reflected constant-buffer binding.
struct RHIShaderParameterConstant {
    RHIConstantBuffer*        Buffer   = nullptr;
    std::vector<std::byte> Data     = {};
    bool                   bPerDraw = false;
};

/// @brief One value assigned to a reflected shader parameter binding.
using RHIShaderParameterValue = std::variant<std::monostate,
                                          RHISampledTexture*,
                                          RHIVertexBuffer*,
                                          RHIIndexBuffer*,
                                          RHIResourceArray<RHISampledTexture>,
                                          RHISampler*,
                                          RHITopLevelAccelerationStructure*,
                                          RHIRayTracingGeometryTable*,
                                          RHIRenderTarget*,
                                          RHIShaderParameterConstant>;

/// @brief Runtime values for one reflected shader descriptor set.
class RHIShaderParameterSet {
  public:
    RHIShaderParameterSet() = default;

    [[nodiscard]] auto GetLayout() const -> const RHIShaderParameterSetLayout& {
        return m_Layout;
    }

    [[nodiscard]] auto GetValues() const -> std::span<const RHIShaderParameterValue> {
        return m_Values;
    }

    [[nodiscard]] auto GetRevision() const -> Uint64 {
        return m_Revision;
    }

  private:
    friend class RHIShaderParameters;

    explicit RHIShaderParameterSet(RHIShaderParameterSetLayout Layout)
        : m_Layout(std::move(Layout)), m_Values(m_Layout.GetBindings().size()) {}

    [[nodiscard]] auto FindBindingIndex(StringView ParameterPath) const -> std::optional<Uint32> {
        const auto Bindings = m_Layout.GetBindings();
        for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
            if (Bindings[Index].ParameterPath == ParameterPath)
                return Index;
        }
        return std::nullopt;
    }

    auto SetValue(Uint32 Index, RHIShaderParameterValue Value) -> void {
        if (AreEquivalent(m_Values[Index], Value))
            return;

        m_Values[Index] = std::move(Value);
        ++m_Revision;
    }

    [[nodiscard]] static auto AreEquivalent(const RHIShaderParameterValue& Left, const RHIShaderParameterValue& Right)
        -> bool {
        return std::visit(
            [](const auto& LeftValue, const auto& RightValue) -> bool {
                using LeftType  = std::decay_t<decltype(LeftValue)>;
                using RightType = std::decay_t<decltype(RightValue)>;
                if constexpr (!std::same_as<LeftType, RightType>) {
                    return false;
                } else if constexpr (std::same_as<LeftType, std::monostate>) {
                    return true;
                } else if constexpr (std::same_as<LeftType, RHIResourceArray<RHISampledTexture>>) {
                    return std::ranges::equal(LeftValue.GetResources(), RightValue.GetResources());
                } else if constexpr (std::same_as<LeftType, RHIShaderParameterConstant>) {
                    return LeftValue.Buffer == RightValue.Buffer && LeftValue.Data == RightValue.Data;
                } else {
                    return LeftValue == RightValue;
                }
            },
            Left,
            Right);
    }

    RHIShaderParameterSetLayout           m_Layout   = {};
    std::vector<RHIShaderParameterValue>  m_Values   = {};
    Uint64                              m_Revision = 0;
};

/// @brief Renderer-facing shader binding values automatically partitioned by reflection.
class RHIShaderParameters {
  public:
    RHIShaderParameters() = default;

    [[nodiscard]] static auto Create(const RHIPipeline& PipelineValue) -> RHIShaderParameters {
        return Create(PipelineValue.GetShaderParameterLayout());
    }

    [[nodiscard]] static auto Create(const RHIGraphicsPipeline& PipelineValue) -> RHIShaderParameters {
        return Create(static_cast<const RHIPipeline&>(PipelineValue));
    }

    [[nodiscard]] static auto Create(const RHIShaderParameterLayout& Layout) -> RHIShaderParameters {
        RHIShaderParameters Result;
        Result.m_Id       = NextId();
        Result.m_LayoutId = Layout.GetId();
        for (const auto& SetLayout : Layout.GetSets())
            Result.m_Sets.push_back(RHIShaderParameterSet{SetLayout});
        return Result;
    }

    [[nodiscard]] auto GetId() const -> Uint64 {
        return m_Id;
    }

    [[nodiscard]] auto GetLayoutId() const -> Uint64 {
        return m_LayoutId;
    }

    [[nodiscard]] auto GetSets() const -> std::span<const RHIShaderParameterSet> {
        return m_Sets;
    }

    [[nodiscard]] auto SetSampledTexture(StringView ParameterPath, RHISampledTexture* Texture)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::SampledTexture, false, Texture);
    }

    [[nodiscard]] auto SetStorageVertexBuffer(StringView ParameterPath, RHIVertexBuffer* Buffer)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::StorageBuffer, false, Buffer);
    }

    [[nodiscard]] auto SetStorageIndexBuffer(StringView ParameterPath, RHIIndexBuffer* Buffer)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::StorageBuffer, false, Buffer);
    }

    [[nodiscard]] auto SetTopLevelAccelerationStructure(StringView                         ParameterPath,
                                                         RHITopLevelAccelerationStructure* RHIAccelerationStructure)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::AccelerationStructure, false, RHIAccelerationStructure);
    }

    [[nodiscard]] auto SetRayTracingGeometryTable(StringView ParameterPath, RHIRayTracingGeometryTable* Table)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::StorageBuffer, false, Table);
    }

    [[nodiscard]] auto SetStorageRenderTarget(StringView ParameterPath, RHIRenderTarget* Target)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::StorageTexture, false, Target);
    }

    template <ShaderParameterArrayResource T>
    [[nodiscard]] auto SetResourceArray(StringView ParameterPath, const RHIResourceArray<T>& Array)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, RHIShaderParameterResourceTraits<T>::Type, true, Array);
    }

    [[nodiscard]] auto SetSampler(StringView ParameterPath, RHISampler* SamplerPtr) -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderResourceType::Sampler, false, SamplerPtr);
    }

    [[nodiscard]] auto SetConstantBuffer(StringView            ParameterPath,
                                         RHIConstantBuffer*       Buffer,
                                         const void*           Data,
                                         Uint64                Size,
                                         bool                  bPerDraw = false)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer)
            return std::unexpected(ErrorMessage("Shader parameter constant buffer is null"));
        if (!Data || Size == 0)
            return std::unexpected(ErrorMessage("Shader parameter constant buffer data is empty"));
        if (Size > Buffer->GetSize()) {
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' exceeds declared RHIConstantBuffer size ({} bytes > {} bytes)",
                       ParameterPath,
                       Size,
                       Buffer->GetSize())));
        }

        RHIShaderParameterConstant Constant{
            .Buffer   = Buffer,
            .bPerDraw = bPerDraw,
        };
        Constant.Data.resize(Size);
        std::memcpy(Constant.Data.data(), Data, Size);
        return Set(ParameterPath, ShaderResourceType::ConstantBuffer, false, std::move(Constant));
    }

  private:
    [[nodiscard]] static auto NextId() -> Uint64 {
        static std::atomic<Uint64> Next = 1;
        return Next.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename T>
    [[nodiscard]] auto Set(StringView ParameterPath, ShaderResourceType ExpectedType, bool bExpectArray, T Value)
        -> std::expected<void, ErrorMessage> {
        for (auto& Set : m_Sets) {
            auto Index = Set.FindBindingIndex(ParameterPath);
            if (!Index)
                continue;

            const auto& Binding = Set.m_Layout.GetBindings()[*Index];
            if (Binding.Type != ExpectedType) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' has incompatible reflected resource type (expected {}, reflected {})",
                           ParameterPath,
                           magic_enum::enum_name(ExpectedType),
                           magic_enum::enum_name(Binding.Type))));
            }
            if (!bExpectArray && Binding.ArrayCount != 1) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' is an array; bind a resource array instead", ParameterPath)));
            }
            if (bExpectArray && Binding.ArrayCount == 1) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' is not an array; bind one sampled texture instead", ParameterPath)));
            }

            Set.SetValue(*Index, RHIShaderParameterValue{std::move(Value)});
            return {};
        }

        return std::unexpected(ErrorMessage(Format("Shader parameter '{}' is not present in the pipeline layout", ParameterPath)));
    }

    Uint64                           m_Id       = 0;
    Uint64                           m_LayoutId = 0;
    std::vector<RHIShaderParameterSet> m_Sets = {};
};

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
    D32_SFLOAT          = 10,
    D24_UNORM_S8_UINT   = 11,
    D32_SFLOAT_S8_UINT  = 12,
};

struct RHIVertexInputAttributeDesc {
    Uint32 Location = 0;
    Uint32 Binding  = 0;
    RHIFormat Format   = RHIFormat::Unknown;
    Uint32 Offset   = 0;
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
class RHIRenderTarget : public RHIGpuResource {
  public:
    RHIRenderTarget()                                       = default;
    RHIRenderTarget(const RHIRenderTarget&)                    = delete;
    auto operator=(const RHIRenderTarget&) -> RHIRenderTarget& = delete;
    RHIRenderTarget(RHIRenderTarget&&)                         = delete;
    auto operator=(RHIRenderTarget&&) -> RHIRenderTarget&      = delete;
    virtual ~RHIRenderTarget()                              = default;

    [[nodiscard]] virtual auto GetWidth() const -> Uint32       = 0;
    [[nodiscard]] virtual auto GetHeight() const -> Uint32      = 0;
    [[nodiscard]] virtual auto GetFormat() const -> RHIFormat      = 0;
    [[nodiscard]] virtual auto GetUsage() const -> RHITextureUsage = 0;
};

struct RHISampledTextureDesc {
    const void*  Data     = nullptr;
    Uint32       Width    = 1;
    Uint32       Height   = 1;
    Uint32       Channels = 4;
    RHIFormat       Format   = RHIFormat::R8G8B8A8_UNORM;
    RHITextureUsage Usage    = RHITextureUsage::ShaderResource;
};

struct RHISampledTextureCreateResult {
    UPtr<RHISampledTexture> Texture          = nullptr;
    RHIGpuCompletionToken   UploadCompletion = {};
};

struct RHIRenderTargetDesc {
    Uint32       Width  = 1;
    Uint32       Height = 1;
    RHIFormat       Format = RHIFormat::B8G8R8A8_UNORM;
    RHITextureUsage Usage  = RHITextureUsage::RenderTarget;
};

struct RHIRenderTargetCreateResult {
    UPtr<RHIRenderTarget> Texture = nullptr;
};

struct RHIVertexBufferCreateResult {
    UPtr<RHIVertexBuffer> Buffer           = nullptr;
    RHIGpuCompletionToken UploadCompletion = {};
};

struct RHIIndexBufferCreateResult {
    UPtr<RHIIndexBuffer>  Buffer           = nullptr;
    RHIGpuCompletionToken UploadCompletion = {};
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
    ShaderGraphicsProgram        Program           = {};
    RHIVertexInputLayoutDesc          VertexInputLayout = {};
    RHIPrimitiveTopology              Topology          = RHIPrimitiveTopology::TriangleList;
    RHIRasterizerState                Rasterizer        = {};
    RHIBlendState                     Blend             = {};
    RHIDepthStencilState              DepthStencil      = {};
    RHIFormat                         ColorFormat       = RHIFormat::B8G8R8A8_UNORM;
    RHIFormat                         DepthFormat       = RHIFormat::Unknown;
};

// ── Clear values ─────────────────────────────────────────────────────────────

struct RHIClearColorValue {
    Float32 R = 0.0f;
    Float32 G = 0.0f;
    Float32 B = 0.0f;
    Float32 A = 1.0f;
};

struct RHIClearDepthStencilValue {
    Float32 Depth   = 1.0f;
    Uint32  Stencil = 0;
};

struct RHIColorAttachmentDesc {
    RHIRenderTarget*   TexturePtr = nullptr;
    RHIClearColorValue ClearValue = {};
};

struct RHIDepthAttachmentDesc {
    RHIRenderTarget*          TexturePtr = nullptr;
    RHIClearDepthStencilValue ClearValue = {};
};

struct RHIRenderingDesc {
    RHIColorAttachmentDesc                ColorAttachment = {};
    std::optional<RHIDepthAttachmentDesc> DepthAttachment = std::nullopt;
};

} // namespace SoulEngine
