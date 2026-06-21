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

// ── Typed GPU buffer polymorphic bases ──────────────────────────────────

/// Empty polymorphic base for vertex buffer resources.
/// Backend concrete class (e.g. Vulkan::VertexBuffer) owns the GPU allocation.
/// Consumers hold SPtr<VertexBuffer> for type-safe API usage.
class VertexBuffer {
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
class IndexBuffer {
  public:
    IndexBuffer()                                      = default;
    IndexBuffer(const IndexBuffer&)                    = delete;
    auto operator=(const IndexBuffer&) -> IndexBuffer& = delete;
    IndexBuffer(IndexBuffer&&)                         = delete;
    auto operator=(IndexBuffer&&) -> IndexBuffer&      = delete;
    virtual ~IndexBuffer()                             = default;
};

/// Abstract base for constant (uniform) buffer resources.
/// Backend concrete class (e.g. Vulkan::UniformBuffer) holds per-frame
/// mappable buffer copies and auto-selects the current frame's copy on Write().
/// Consumers hold SPtr<ConstantBuffer> and call Write() each frame with
/// updated data — the FramesInFlight count is an internal backend detail.
class ConstantBuffer {
  public:
    ConstantBuffer()                                         = default;
    ConstantBuffer(const ConstantBuffer&)                    = delete;
    auto operator=(const ConstantBuffer&) -> ConstantBuffer& = delete;
    ConstantBuffer(ConstantBuffer&&)                         = delete;
    auto operator=(ConstantBuffer&&) -> ConstantBuffer&      = delete;
    virtual ~ConstantBuffer()                                = default;

    /// Upload new data to the current frame's buffer slot.
    /// Size must not exceed the buffer's capacity.
    [[nodiscard]] virtual auto Write(const void* Data, Uint64 Size) -> std::expected<void, ErrorMessage> = 0;

    /// Return the buffer size in bytes (same for all frame slots).
    [[nodiscard]] virtual auto GetSize() const -> Uint64 = 0;
};

/// Empty polymorphic base for graphics pipeline resources.
/// Backend concrete class (e.g. Vulkan::GraphicsPipeline) owns the native
/// pipeline. Consumers hold SPtr<GraphicsPipeline> for type-safe API usage.
class GraphicsPipeline {
  public:
    GraphicsPipeline()                                           = default;
    GraphicsPipeline(const GraphicsPipeline&)                    = delete;
    auto operator=(const GraphicsPipeline&) -> GraphicsPipeline& = delete;
    GraphicsPipeline(GraphicsPipeline&&)                         = delete;
    auto operator=(GraphicsPipeline&&) -> GraphicsPipeline&      = delete;
    virtual ~GraphicsPipeline()                                  = default;
};

// ── Opaque handle types ───────────────────────────────────────────────────

struct Texture {
    Uint64 Handle = 0;
};
struct Sampler {
    Uint64 Handle = 0;
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

enum class TextureUsage : Uint32 {
    None           = 0,
    RenderTarget   = 1u << 0,
    DepthStencil   = 1u << 1,
    ShaderResource = 1u << 2,
    TransferSrc    = 1u << 3,
    TransferDst    = 1u << 4,
    Present        = 1u << 5,
};

[[nodiscard]] inline auto operator|(TextureUsage a, TextureUsage b) -> TextureUsage {
    return static_cast<TextureUsage>(static_cast<Uint32>(a) | static_cast<Uint32>(b));
}

struct TextureDesc {
    Uint32       Width     = 1;
    Uint32       Height    = 1;
    Uint32       Depth     = 1;
    Uint32       MipLevels = 1;
    Format       Format    = Format::R8G8B8A8_UNORM;
    TextureUsage Usage     = TextureUsage::ShaderResource;
};

// ── Pipeline ─────────────────────────────────────────────────────────────────

enum class PrimitiveTopology : Uint8 {
    Unknown      = 0,
    TriangleList = 1,
};

// TODO: Validate ShaderPrograms and merged shader reflection at pipeline
// creation time:
//   - Code and Reflection must be non-null for every program
//   - Graphics pipelines must only contain graphics-stage programs
//     (Vertex, Fragment, Hull, Domain, Geometry, Mesh, Amplification)
//   - Duplicate stages within a single graphics pipeline are illegal
//   - Mesh shaders preclude Vertex/Hull/Domain/Geometry stages
//   - Reflected resource-layout merge errors surface during pipeline creation

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
    Shader::Program                VertexProgram   = {};
    std::optional<Shader::Program> FragmentProgram = std::nullopt;
    PrimitiveTopology              Topology        = PrimitiveTopology::TriangleList;
    RasterizerState                Rasterizer      = {};
    BlendState                     Blend           = {};
    DepthStencilState              DepthStencil    = {};
    Format                         ColorFormat     = Format::B8G8R8A8_UNORM;
    Format                         DepthFormat     = Format::Unknown;
};

// ── Sampler ──────────────────────────────────────────────────────────────────

struct SamplerDesc {
    bool MinFilterLinear  = true;
    bool MagFilterLinear  = true;
    bool AddressModeClamp = true; // else repeat
};

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
    Texture         Texture    = {};
    ClearColorValue ClearValue = {};
};

struct DepthAttachmentDesc {
    Texture                Texture    = {};
    ClearDepthStencilValue ClearValue = {};
};

struct RenderingDesc {
    ColorAttachmentDesc                ColorAttachment = {};
    std::optional<DepthAttachmentDesc> DepthAttachment = std::nullopt;
};

} // namespace SoulEngine::RHI
