export module RHI:Types;

export import Core;
import Shader;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

// ── Buffer descriptor types ────────────────────────────────────────────────

struct VertexBufferDesc {
    const void* Data        = nullptr;
    Uint64      VertexCount = 0;
    Uint32      Stride      = 0;
};

struct IndexBufferDesc {
    const void* Data       = nullptr;
    Uint64      IndexCount = 0;
};

struct ConstantBufferDesc {
    Uint64 Size = 0;
};

enum class SamplerProfile : Uint8 {
    Unknown = 0,
    LinearRepeat,
    AnisotropicRepeat,
};

struct SamplerDesc {
    SamplerProfile Profile = SamplerProfile::LinearRepeat;
};

// ── GpuResource — base for GPU resources with usage tracking ────────────

struct GpuCompletionToken {
    Uint64 Id = 0;
};

/// Base class for GPU resources that tracks the last command-list usage token.
/// Resources inheriting this can participate in deferred deletion via
/// DeletionQueue: when the GPU completes all work up to the last usage token,
/// the resource is safe to destroy.
class GpuResource {
  public:
    GpuResource()                                      = default;
    GpuResource(const GpuResource&)                    = delete;
    auto operator=(const GpuResource&) -> GpuResource& = delete;
    GpuResource(GpuResource&&)                         = delete;
    auto operator=(GpuResource&&) -> GpuResource&      = delete;
    virtual ~GpuResource()                             = default;

    [[nodiscard]] auto GetLastUsageToken() const noexcept -> GpuCompletionToken {
        return m_LastUsage;
    }
    auto UpdateLastUsageToken(GpuCompletionToken Token) noexcept -> void {
        m_LastUsage = Token;
    }

  private:
    GpuCompletionToken m_LastUsage = {};
};

// ── Typed GPU buffer polymorphic bases ──────────────────────────────────

/// Empty polymorphic base for vertex buffer resources.
/// Backend concrete class (e.g. Vulkan::VertexBuffer) owns the GPU allocation.
/// Resource::Manager owns VertexBuffer instances; command lists only observe them.
class VertexBuffer : public GpuResource {
  public:
    VertexBuffer()                                       = default;
    VertexBuffer(const VertexBuffer&)                    = delete;
    auto operator=(const VertexBuffer&) -> VertexBuffer& = delete;
    VertexBuffer(VertexBuffer&&)                         = delete;
    auto operator=(VertexBuffer&&) -> VertexBuffer&      = delete;
    virtual ~VertexBuffer()                              = default;
};

/// Empty polymorphic base for index buffer resources.
/// Same role as VertexBuffer, for index data.
class IndexBuffer : public GpuResource {
  public:
    IndexBuffer()                                      = default;
    IndexBuffer(const IndexBuffer&)                    = delete;
    auto operator=(const IndexBuffer&) -> IndexBuffer& = delete;
    IndexBuffer(IndexBuffer&&)                         = delete;
    auto operator=(IndexBuffer&&) -> IndexBuffer&      = delete;
    virtual ~IndexBuffer()                             = default;
};

/// Logical shader-visible constant block identity.
///
/// This object declares the size and stable RHI identity of a constant block.
/// It does not imply a dedicated backend buffer allocation. Backends lower
/// command-list constant writes into their own in-flight-safe storage.
class ConstantBuffer {
  public:
    explicit ConstantBuffer(const ConstantBufferDesc& Desc) {
        m_Size = Desc.Size;
    }
    ConstantBuffer(const ConstantBuffer&)                    = delete;
    auto operator=(const ConstantBuffer&) -> ConstantBuffer& = delete;
    ConstantBuffer(ConstantBuffer&&)                         = delete;
    auto operator=(ConstantBuffer&&) -> ConstantBuffer&      = delete;
    virtual ~ConstantBuffer()                                = default;

    /// Return the declared size in bytes.
    [[nodiscard]] auto GetSize() const -> Uint64 {
        return m_Size;
    }

  private:
    Uint64 m_Size = 0;
};

/// Shader-visible sampling state object.
///
/// Backends own the native sampler handle. Resource::Manager owns Sampler
/// instances; command lists only observe them.
class Sampler : public GpuResource {
  public:
    explicit Sampler(const SamplerDesc& Desc) {
        m_Desc = Desc;
    }
    Sampler(const Sampler&)                    = delete;
    auto operator=(const Sampler&) -> Sampler& = delete;
    Sampler(Sampler&&)                         = delete;
    auto operator=(Sampler&&) -> Sampler&      = delete;
    virtual ~Sampler()                         = default;

    [[nodiscard]] auto GetDesc() const -> const SamplerDesc& {
        return m_Desc;
    }

  private:
    SamplerDesc m_Desc = {};
};

/// @brief One reflected resource binding in a shader parameter-set layout.
struct ShaderParameterBindingLayout {
    String               ParameterPath = {};
    Uint32               Binding       = 0;
    Shader::ResourceType Type          = Shader::ResourceType::Unknown;
    Uint32               ArrayCount    = 1;
};

/// @brief Immutable reflected layout for one shader descriptor set.
class ShaderParameterSetLayout {
  public:
    ShaderParameterSetLayout() = default;

    [[nodiscard]] auto GetSetIndex() const -> Uint32 {
        return m_SetIndex;
    }

    [[nodiscard]] auto GetBindings() const -> std::span<const ShaderParameterBindingLayout> {
        return m_Bindings;
    }

    [[nodiscard]] auto FindBinding(StringView ParameterPath) const -> const ShaderParameterBindingLayout* {
        for (const auto& Binding : m_Bindings) {
            if (Binding.ParameterPath == ParameterPath)
                return &Binding;
        }
        return nullptr;
    }

  private:
    friend class ShaderParameterLayout;

    Uint32                                        m_SetIndex = 0;
    std::vector<ShaderParameterBindingLayout> m_Bindings = {};
};

/// @brief Immutable shader parameter interface derived from pipeline reflection.
class ShaderParameterLayout {
  public:
    [[nodiscard]] static auto Create(const Shader::Reflection& Reflection) -> ShaderParameterLayout {
        ShaderParameterLayout Result;
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
            Result.m_Sets[Binding.Set].m_Bindings.push_back(ShaderParameterBindingLayout{
                .ParameterPath = Binding.ParameterPath,
                .Binding       = Binding.Binding,
                .Type          = Binding.Type,
                .ArrayCount    = Binding.ArrayCount,
            });
        }

        for (auto& Set : Result.m_Sets)
            std::ranges::sort(Set.m_Bindings, {}, &ShaderParameterBindingLayout::Binding);

        return Result;
    }

    [[nodiscard]] auto GetId() const -> Uint64 {
        return m_Id;
    }

    [[nodiscard]] auto GetSets() const -> std::span<const ShaderParameterSetLayout> {
        return m_Sets;
    }

    [[nodiscard]] auto GetSetLayout(Uint32 SetIndex) const -> const ShaderParameterSetLayout* {
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
    std::vector<ShaderParameterSetLayout> m_Sets = {};
};

/// Empty polymorphic base for graphics pipeline resources.
/// Backend concrete class (e.g. Vulkan::GraphicsPipeline) owns the native
/// pipeline. Resource::Manager owns GraphicsPipeline instances.
class GraphicsPipeline : public GpuResource {
  public:
    GraphicsPipeline()                                           = default;
    GraphicsPipeline(const GraphicsPipeline&)                    = delete;
    auto operator=(const GraphicsPipeline&) -> GraphicsPipeline& = delete;
    GraphicsPipeline(GraphicsPipeline&&)                         = delete;
    auto operator=(GraphicsPipeline&&) -> GraphicsPipeline&      = delete;
    virtual ~GraphicsPipeline()                                  = default;

    [[nodiscard]] auto GetShaderParameterLayout() const -> const ShaderParameterLayout& {
        return m_ShaderParameterLayout;
    }

  protected:
    auto SetShaderParameterLayout(ShaderParameterLayout Layout) -> void {
        m_ShaderParameterLayout = std::move(Layout);
    }

  private:
    ShaderParameterLayout m_ShaderParameterLayout = {};
};

// ── Opaque handle types ───────────────────────────────────────────────────

/// Polymorphic base for shader-readable sampled texture resources.
/// Backend concrete class (e.g. Vulkan::SampledTexture) owns the GPU allocation.
/// Resource::Manager owns SampledTexture instances.
class SampledTexture : public GpuResource {
  public:
    SampledTexture()                                         = default;
    SampledTexture(const SampledTexture&)                    = delete;
    auto operator=(const SampledTexture&) -> SampledTexture& = delete;
    SampledTexture(SampledTexture&&)                         = delete;
    auto operator=(SampledTexture&&) -> SampledTexture&      = delete;
    virtual ~SampledTexture()                                = default;

    [[nodiscard]] virtual auto GetWidth() const -> Uint32  = 0;
    [[nodiscard]] virtual auto GetHeight() const -> Uint32 = 0;
};

/// @brief Mutable shader-visible array of resource observers.
///
/// Resource-layer arrays retain the corresponding ResourceRefs. This RHI value
/// contains only the resolved observers recorded into command lists.
template <typename T>
class ResourceArray {
  public:
    ResourceArray() = default;

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
struct ShaderParameterResourceTraits;

template <>
struct ShaderParameterResourceTraits<SampledTexture> {
    static constexpr Shader::ResourceType Type = Shader::ResourceType::SampledTexture;
};

template <typename T>
concept ShaderParameterArrayResource = requires {
    { ShaderParameterResourceTraits<T>::Type } -> std::convertible_to<Shader::ResourceType>;
};

/// @brief CPU-side snapshot for one reflected constant-buffer binding.
struct ShaderParameterConstant {
    ConstantBuffer*        Buffer = nullptr;
    std::vector<std::byte> Data   = {};
};

/// @brief One value assigned to a reflected shader parameter binding.
using ShaderParameterValue =
    std::variant<std::monostate, SampledTexture*, ResourceArray<SampledTexture>, Sampler*, ShaderParameterConstant>;

/// @brief Runtime values for one reflected shader descriptor set.
class ShaderParameterSet {
  public:
    ShaderParameterSet() = default;

    [[nodiscard]] auto GetLayout() const -> const ShaderParameterSetLayout& {
        return m_Layout;
    }

    [[nodiscard]] auto GetValues() const -> std::span<const ShaderParameterValue> {
        return m_Values;
    }

    [[nodiscard]] auto GetRevision() const -> Uint64 {
        return m_Revision;
    }

  private:
    friend class ShaderParameters;

    explicit ShaderParameterSet(ShaderParameterSetLayout Layout)
        : m_Layout(std::move(Layout)), m_Values(m_Layout.GetBindings().size()) {}

    [[nodiscard]] auto FindBindingIndex(StringView ParameterPath) const -> std::optional<Uint32> {
        const auto Bindings = m_Layout.GetBindings();
        for (Uint32 Index = 0; Index < Bindings.size(); ++Index) {
            if (Bindings[Index].ParameterPath == ParameterPath)
                return Index;
        }
        return std::nullopt;
    }

    auto SetValue(Uint32 Index, ShaderParameterValue Value) -> void {
        if (AreEquivalent(m_Values[Index], Value))
            return;

        m_Values[Index] = std::move(Value);
        ++m_Revision;
    }

    [[nodiscard]] static auto AreEquivalent(const ShaderParameterValue& Left, const ShaderParameterValue& Right)
        -> bool {
        return std::visit(
            [](const auto& LeftValue, const auto& RightValue) -> bool {
                using LeftType  = std::decay_t<decltype(LeftValue)>;
                using RightType = std::decay_t<decltype(RightValue)>;
                if constexpr (!std::same_as<LeftType, RightType>) {
                    return false;
                } else if constexpr (std::same_as<LeftType, std::monostate>) {
                    return true;
                } else if constexpr (std::same_as<LeftType, ResourceArray<SampledTexture>>) {
                    return std::ranges::equal(LeftValue.GetResources(), RightValue.GetResources());
                } else if constexpr (std::same_as<LeftType, ShaderParameterConstant>) {
                    return LeftValue.Buffer == RightValue.Buffer && LeftValue.Data == RightValue.Data;
                } else {
                    return LeftValue == RightValue;
                }
            },
            Left,
            Right);
    }

    ShaderParameterSetLayout           m_Layout   = {};
    std::vector<ShaderParameterValue>  m_Values   = {};
    Uint64                              m_Revision = 0;
};

/// @brief Renderer-facing shader binding values automatically partitioned by reflection.
class ShaderParameters {
  public:
    ShaderParameters() = default;

    [[nodiscard]] static auto Create(const GraphicsPipeline& Pipeline) -> ShaderParameters {
        return Create(Pipeline.GetShaderParameterLayout());
    }

    [[nodiscard]] static auto Create(const ShaderParameterLayout& Layout) -> ShaderParameters {
        ShaderParameters Result;
        Result.m_Id       = NextId();
        Result.m_LayoutId = Layout.GetId();
        for (const auto& SetLayout : Layout.GetSets())
            Result.m_Sets.push_back(ShaderParameterSet{SetLayout});
        return Result;
    }

    [[nodiscard]] auto GetId() const -> Uint64 {
        return m_Id;
    }

    [[nodiscard]] auto GetLayoutId() const -> Uint64 {
        return m_LayoutId;
    }

    [[nodiscard]] auto GetSets() const -> std::span<const ShaderParameterSet> {
        return m_Sets;
    }

    [[nodiscard]] auto SetSampledTexture(StringView ParameterPath, SampledTexture* Texture)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, Shader::ResourceType::SampledTexture, false, Texture);
    }

    template <ShaderParameterArrayResource T>
    [[nodiscard]] auto SetResourceArray(StringView ParameterPath, const ResourceArray<T>& Array)
        -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, ShaderParameterResourceTraits<T>::Type, true, Array);
    }

    [[nodiscard]] auto SetSampler(StringView ParameterPath, Sampler* SamplerPtr) -> std::expected<void, ErrorMessage> {
        return Set(ParameterPath, Shader::ResourceType::Sampler, false, SamplerPtr);
    }

    [[nodiscard]] auto SetConstantBuffer(StringView            ParameterPath,
                                         ConstantBuffer*       Buffer,
                                         const void*           Data,
                                         Uint64                Size)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer)
            return std::unexpected(ErrorMessage("Shader parameter constant buffer is null"));
        if (!Data || Size == 0)
            return std::unexpected(ErrorMessage("Shader parameter constant buffer data is empty"));
        if (Size > Buffer->GetSize()) {
            return std::unexpected(ErrorMessage(
                Format("Shader parameter '{}' exceeds declared ConstantBuffer size ({} bytes > {} bytes)",
                       ParameterPath,
                       Size,
                       Buffer->GetSize())));
        }

        ShaderParameterConstant Constant{
            .Buffer = Buffer,
        };
        Constant.Data.resize(Size);
        std::memcpy(Constant.Data.data(), Data, Size);
        return Set(ParameterPath, Shader::ResourceType::ConstantBuffer, false, std::move(Constant));
    }

  private:
    [[nodiscard]] static auto NextId() -> Uint64 {
        static std::atomic<Uint64> Next = 1;
        return Next.fetch_add(1, std::memory_order_relaxed);
    }

    template <typename T>
    [[nodiscard]] auto Set(StringView ParameterPath, Shader::ResourceType ExpectedType, bool bExpectArray, T Value)
        -> std::expected<void, ErrorMessage> {
        for (auto& Set : m_Sets) {
            auto Index = Set.FindBindingIndex(ParameterPath);
            if (!Index)
                continue;

            const auto& Binding = Set.m_Layout.GetBindings()[*Index];
            if (Binding.Type != ExpectedType) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' has incompatible reflected resource type", ParameterPath)));
            }
            if (!bExpectArray && Binding.ArrayCount != 1) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' is an array; bind a resource array instead", ParameterPath)));
            }
            if (bExpectArray && Binding.ArrayCount == 1) {
                return std::unexpected(ErrorMessage(
                    Format("Shader parameter '{}' is not an array; bind one sampled texture instead", ParameterPath)));
            }

            Set.SetValue(*Index, ShaderParameterValue{std::move(Value)});
            return {};
        }

        return std::unexpected(ErrorMessage(Format("Shader parameter '{}' is not present in the pipeline layout", ParameterPath)));
    }

    Uint64                           m_Id       = 0;
    Uint64                           m_LayoutId = 0;
    std::vector<ShaderParameterSet> m_Sets = {};
};

// ── Texture ──────────────────────────────────────────────────────────────────

enum class Format : Uint8 {
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

struct VertexInputAttributeDesc {
    Uint32 Location = 0;
    Format Format   = Format::Unknown;
    Uint32 Offset   = 0;
};

struct VertexInputLayoutDesc {
    Uint32                                Binding    = 0;
    Uint32                                Stride     = 0;
    std::vector<VertexInputAttributeDesc> Attributes = {};
};

enum class TextureUsage : Uint32 {
    None           = 0,
    RenderTarget   = 1u << 0,
    DepthStencil   = 1u << 1,
    ShaderResource = 1u << 2,
    FrameOutput    = 1u << 3,
};

[[nodiscard]] inline auto operator|(TextureUsage a, TextureUsage b) -> TextureUsage {
    return static_cast<TextureUsage>(static_cast<Uint32>(a) | static_cast<Uint32>(b));
}

/// Polymorphic base for render-target images (color or depth/stencil).
/// Color vs depth is distinguished by GetFormat()/GetUsage(), not by type.
/// Backend concrete class owns GPU allocation. Resource::Manager owns render targets.
class RenderTarget : public GpuResource {
  public:
    RenderTarget()                                       = default;
    RenderTarget(const RenderTarget&)                    = delete;
    auto operator=(const RenderTarget&) -> RenderTarget& = delete;
    RenderTarget(RenderTarget&&)                         = delete;
    auto operator=(RenderTarget&&) -> RenderTarget&      = delete;
    virtual ~RenderTarget()                              = default;

    [[nodiscard]] virtual auto GetWidth() const -> Uint32       = 0;
    [[nodiscard]] virtual auto GetHeight() const -> Uint32      = 0;
    [[nodiscard]] virtual auto GetFormat() const -> Format      = 0;
    [[nodiscard]] virtual auto GetUsage() const -> TextureUsage = 0;
};

struct SampledTextureDesc {
    const void*  Data     = nullptr;
    Uint32       Width    = 1;
    Uint32       Height   = 1;
    Uint32       Channels = 4;
    Format       Format   = Format::R8G8B8A8_UNORM;
    TextureUsage Usage    = TextureUsage::ShaderResource;
};

struct SampledTextureCreateResult {
    UPtr<SampledTexture> Texture          = nullptr;
    GpuCompletionToken   UploadCompletion = {};
};

struct RenderTargetDesc {
    Uint32       Width  = 1;
    Uint32       Height = 1;
    Format       Format = Format::B8G8R8A8_UNORM;
    TextureUsage Usage  = TextureUsage::RenderTarget;
};

struct RenderTargetCreateResult {
    UPtr<RenderTarget> Texture = nullptr;
};

struct VertexBufferCreateResult {
    UPtr<VertexBuffer> Buffer           = nullptr;
    GpuCompletionToken UploadCompletion = {};
};

struct IndexBufferCreateResult {
    UPtr<IndexBuffer>  Buffer           = nullptr;
    GpuCompletionToken UploadCompletion = {};
};

// ── Pipeline ─────────────────────────────────────────────────────────────────

enum class PrimitiveTopology : Uint8 {
    Unknown      = 0,
    TriangleList = 1,
};

// TODO: Validate GraphicsProgram and shader reflection at pipeline
// creation time:
//   - Code and Reflection must be non-null for every stage program
//   - Mesh shaders preclude Vertex/Hull/Domain/Geometry stages

struct RasterizerState {
    bool    FillMode  = true; // true = solid, false = wireframe
    bool    CullMode  = true; // true = back cull
    Float32 LineWidth = 1.0f;
};

struct BlendAttachment {
    bool BlendEnable = false;
};

struct BlendState {
    BlendAttachment Attachments[8] = {};
};

struct DepthStencilState {
    bool DepthTestEnable  = true;
    bool DepthWriteEnable = true;
};

struct GraphicsPipelineDesc {
    Shader::GraphicsProgram        Program           = {};
    VertexInputLayoutDesc          VertexInputLayout = {};
    PrimitiveTopology              Topology          = PrimitiveTopology::TriangleList;
    RasterizerState                Rasterizer        = {};
    BlendState                     Blend             = {};
    DepthStencilState              DepthStencil      = {};
    Format                         ColorFormat       = Format::B8G8R8A8_UNORM;
    Format                         DepthFormat       = Format::Unknown;
};

// ── Clear values ─────────────────────────────────────────────────────────────

struct ClearColorValue {
    Float32 R = 0.0f;
    Float32 G = 0.0f;
    Float32 B = 0.0f;
    Float32 A = 1.0f;
};

struct ClearDepthStencilValue {
    Float32 Depth   = 1.0f;
    Uint32  Stencil = 0;
};

struct ColorAttachmentDesc {
    RenderTarget*   TexturePtr = nullptr;
    ClearColorValue ClearValue = {};
};

struct DepthAttachmentDesc {
    RenderTarget*          TexturePtr = nullptr;
    ClearDepthStencilValue ClearValue = {};
};

struct RenderingDesc {
    ColorAttachmentDesc                ColorAttachment = {};
    std::optional<DepthAttachmentDesc> DepthAttachment = std::nullopt;
};

} // namespace SoulEngine::RHI
