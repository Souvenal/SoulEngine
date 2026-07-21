/// @file   Shader.cppm
/// @brief  Shader module — owns types describing compiled shader artifacts.
///
/// This module is the data contract between ShaderCompiler (produces)
/// and RHI (consumes).  It is deliberately independent of both:
/// the types here describe *what a compiled shader is*, not how it
/// is compiled or bound.
///
/// Reflection model
/// ────────────────
/// Reflection is normalized here into backend-agnostic data structures.
/// ShaderCompiler extracts that reflection from native compiler APIs,
/// while RHI/backends consume it to derive pipeline resource layouts,
/// vertex input requirements, and later binding interfaces.

export module Shader;

import Core;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Shader {

/// @brief Canonical pipeline stage enum.
///
/// Used by the compiler to describe what stage to compile for, by the
/// RHI to select pipeline bind points, and by reflection to label entry
/// points.  Backends (Slang, Vulkan) map to/from their native stage
/// types.
enum class Stage : Uint8 {
    Unknown       = 0,
    Vertex        = 1,
    Fragment      = 2,
    Compute       = 3,
    Hull          = 4,
    Domain        = 5,
    Geometry      = 6,
    Mesh          = 7,
    Amplification = 8,
    RayGeneration = 9,
    Intersection  = 10,
    AnyHit        = 11,
    ClosestHit    = 12,
    Miss          = 13,
    Callable      = 14,
};

/// @brief Minimal scalar-type vocabulary needed by normalized reflection.
///
/// TODO: Extend this if/when reflection must describe additional scalar
/// categories such as 16-bit types or normalized integer encodings.
enum class ScalarType : Uint8 {
    Unknown = 0,
    Float32 = 1,
    Int32   = 2,
    Uint32  = 3,
};

/// @brief Minimal resource categories supported by the first reflection pass.
///
/// TODO: Extend this to cover more native resource kinds once the engine
/// needs them (texel buffers, immutable samplers, acceleration structures,
/// combined image samplers, etc.).
enum class ResourceType : Uint8 {
    Unknown        = 0,
    ConstantBuffer = 1,
    StorageBuffer  = 2,
    /// Shader-read-only texture descriptor (DX: SRV).
    SampledTexture = 3,
    /// Shader read-write texture/image descriptor (DX: UAV), not a render
    /// target attachment (DX: RTV/DSV).  Attachments usually use
    /// attachment-optimal layouts and may get hardware compression/tile-local
    /// paths; storage images commonly use general layouts for random access.
    StorageTexture          = 4,
    Sampler                 = 5,
    /// Top-level acceleration structure used for hardware ray traversal.
    AccelerationStructure   = 6,
};

/// @brief Reflected scalar/vector/matrix shape for a shader-visible value.
struct ValueType {
    ScalarType ScalarType  = ScalarType::Unknown;
    Uint32     RowCount    = 1;
    Uint32     ColumnCount = 1;
};

/// @brief Reflected shader-visible resource binding.
struct Binding {
    /// Shader parameter access path, e.g. "g_frame.cb"; not a resource key or file path.
    String       ParameterPath = {};
    Uint32       Set           = 0;
    Uint32       BindingIndex  = 0;
    ResourceType Type          = ResourceType::Unknown;
    Uint32       ArrayCount    = 1;
};

/// @brief Reflected push-constant byte range.
struct PushConstantRange {
    Uint32 Offset = 0;
    Uint32 Size   = 0;
};

/// @brief Reflected shader-side vertex attribute requirement.
///
/// This describes what the shader consumes.  It intentionally does not
/// describe how CPU-side vertex buffers are packed or streamed.
/// TODO: introduce a distinct vertex-buffer stream-layout abstraction later
/// if multi-stream, per-instance, or packed/custom vertex formats require it.
struct VertexInputAttribute {
    String                SemanticName  = {};
    Uint32                SemanticIndex = 0;
    std::optional<Uint32> Location      = std::nullopt;
    ValueType             ValueType     = {};
};

/// @brief Normalized pipeline reflection data.
///
/// TODO: Add specialization-constant reflection when pipeline specialization
/// is introduced.
struct Reflection {
    std::vector<Binding>              Bindings      = {};
    std::vector<PushConstantRange>    PushConstants = {};
    std::vector<VertexInputAttribute> VertexInputs  = {};
};

/// @brief Compiled shader artifact for one graphics pipeline shader combination.
struct GraphicsProgram {
    /// SPIR-V binary is defined as 32-bit words, and Vulkan consumes shader
    /// module code through a uint32_t pointer rather than a byte buffer.
    std::vector<Uint32> Code                   = {};
    String              VertexEntryPointName   = {};
    String              FragmentEntryPointName = {};
    Reflection          Reflection             = {};
};

/// @brief Logical category of one ray-tracing hit group.
enum class RayTracingHitGroupType : Uint8 {
    Unknown    = 0,
    Triangles  = 1,
    Procedural = 2,
};

/// @brief Canonical entry-point names that form one linked ray-tracing hit group.
struct RayTracingHitGroup {
    RayTracingHitGroupType    Type                       = RayTracingHitGroupType::Triangles;
    std::optional<String>     ClosestHitEntryPointName   = std::nullopt;
    std::optional<String>     AnyHitEntryPointName       = std::nullopt;
    std::optional<String>     IntersectionEntryPointName = std::nullopt;
};

/// @brief Compiled shader artifact for one linked ray-tracing pipeline program.
struct RayTracingProgram {
    /// SPIR-V binary containing all selected ray-tracing entry points.
    std::vector<Uint32>                Code                     = {};
    String                             RayGenerationEntryPointName = {};
    std::vector<String>                MissEntryPointNames      = {};
    std::vector<RayTracingHitGroup>    HitGroups                = {};
    std::vector<String>                CallableEntryPointNames  = {};
    Reflection                         Reflection               = {};
};

} // namespace SoulEngine::Shader
