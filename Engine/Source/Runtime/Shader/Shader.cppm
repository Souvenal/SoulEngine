/// @file   Shader.cppm
/// @brief  Shader module — owns types describing compiled shader artifacts.
///
/// This module is the data contract between ShaderCompiler (produces)
/// and RHI (consumes).  It is deliberately independent of both:
/// the types here describe *what a compiled shader is*, not how it
/// is compiled or bound.
///
/// ShaderReflection model
/// ────────────────
/// ShaderReflection is normalized here into backend-agnostic data structures.
/// ShaderCompiler extracts that reflection from native compiler APIs,
/// while RHI/backends consume it to derive pipeline resource layouts,
/// vertex input requirements, and later binding interfaces.

export module Shader;

import Core;
import magic_enum;

import std;

export namespace SoulEngine {

/// Sentinel used by shader reflection for an unbounded/runtime-sized array.
inline constexpr Uint32 kShaderReflectionArrayUnboundedSize = std::numeric_limits<Uint32>::max();

/// @brief Canonical pipeline stage flags.
///
/// Used by the compiler to describe what stage to compile for, by the
/// RHI to select pipeline bind points, and by reflection to label entry
/// points.  Backends (Slang, Vulkan) map to/from their native stage
/// types.  Values are bit flags so multi-stage sets (e.g. a graphics
/// program's vertex+fragment) compose with operator|.
enum class ShaderStage : Uint16 {
    Unknown       = 0,
    Vertex        = 1 << 0,
    Fragment      = 1 << 1,
    Compute       = 1 << 2,
    Hull          = 1 << 3,
    Domain        = 1 << 4,
    Geometry      = 1 << 5,
    Mesh          = 1 << 6,
    Amplification = 1 << 7,
    RayGeneration = 1 << 8,
    Intersection  = 1 << 9,
    AnyHit        = 1 << 10,
    ClosestHit    = 1 << 11,
    Miss          = 1 << 12,
    Callable      = 1 << 13,
};

/// @brief Minimal scalar-type vocabulary needed by normalized reflection.
///
/// TODO: Extend this if/when reflection must describe additional scalar
/// categories such as 16-bit types or normalized integer encodings.
enum class ShaderScalarType : Uint8 {
    Unknown = 0,
    Float32 = 1,
    Int32   = 2,
    Uint32  = 3,
    Bool    = 4,
};

/// @brief Minimal resource categories supported by the first reflection pass.
///
/// The kind axis splits only where backend descriptor types differ: one texture kind serves both sampled and
/// storage bindings (the access axis below carries that direction), while constant and storage buffers stay
/// distinct because the backends type their descriptors differently.
///
/// TODO: Extend this to cover more native resource kinds once the engine needs them (texel buffers,
/// immutable samplers, combined image samplers, etc.).
enum class ShaderResourceType : Uint8 {
    Unknown        = 0,
    ConstantBuffer = 1,
    StorageBuffer  = 2,
    /// Texture image descriptor of any access direction (DX: SRV or UAV).
    Texture        = 3,
    Sampler        = 4,
    /// Top-level acceleration structure used for hardware ray traversal.
    AccelerationStructure = 5,
};

/// @brief Shader-declared access direction of one resource binding.
///
/// Access is an axis independent of ShaderResourceType: it comes from the shader's own declaration (Texture2D
/// vs RWTexture2D, StructuredBuffer vs RWStructuredBuffer) and travels with the binding so consumers can
/// dispatch or validate on it without re-reading the kind.
enum class ShaderBindingAccess : Uint8 {
    Unknown   = 0,
    /// The shader only reads through this binding.
    Read      = 1,
    /// The shader may read and write through this binding.
    ReadWrite = 2,
};

/// @brief Reflected scalar/vector/matrix shape for a shader-visible value.
struct ShaderValueType {
    ShaderScalarType ScalarType  = ShaderScalarType::Unknown;
    Uint32           RowCount    = 1;
    Uint32           ColumnCount = 1;
};

/// @brief Reflected shader-visible resource binding.
struct ShaderBinding {
    /// Shader parameter access path, e.g. "g_frame.cb"; not a resource key or file path.
    String              ParameterPath = {};
    Uint32              Set           = 0;
    Uint32              BindingIndex  = 0;
    ShaderResourceType  Type          = ShaderResourceType::Unknown;
    /// Shader-declared access direction; samplers and acceleration structures stay Unknown.
    ShaderBindingAccess Access        = ShaderBindingAccess::Unknown;
    Uint32              ArrayCount    = 1;
};

/// @brief Reflected push-constant byte range.
struct ShaderPushConstantRange {
    Uint32 Offset = 0;
    Uint32 Size   = 0;
};

/// @brief Reflected specialization constant (Slang [SpecializationConstant]).
///
/// Host code looks values up by Name; ConstantId only exists in reflection and the Vulkan layer. Defaults
/// come from Slang's integer getter: bool/int/uint are populated, float has no SDK getter and stays nullopt.
struct ShaderSpecializationConstant {
    String                Name         = {};
    Uint32                ConstantId   = 0;
    ShaderScalarType      ScalarType   = ShaderScalarType::Unknown;
    std::optional<Int64>  DefaultValue = std::nullopt;
};

/// @brief Reflected shader-side vertex attribute requirement.
///
/// This describes what the shader consumes.  It intentionally does not
/// describe how CPU-side vertex buffers are packed or streamed.
/// TODO: introduce a distinct vertex-buffer stream-layout abstraction later
/// if multi-stream, per-instance, or packed/custom vertex formats require it.
struct ShaderVertexInputAttribute {
    String                SemanticName  = {};
    Uint32                SemanticIndex = 0;
    std::optional<Uint32> Location      = std::nullopt;
    ShaderValueType       ValueType     = {};
};

/// @brief Normalized pipeline reflection data.
struct ShaderReflection {
    std::vector<ShaderBinding>                 Bindings                 = {};
    std::vector<ShaderPushConstantRange>       PushConstants            = {};
    std::vector<ShaderVertexInputAttribute>    VertexInputs             = {};
    std::vector<ShaderSpecializationConstant>  SpecializationConstants  = {};
    std::optional<Uint32>                      BindlessSpace            = std::nullopt;
};

/// @brief Compiled shader artifact for one graphics pipeline shader combination.
struct ShaderGraphicsProgram {
    /// SPIR-V binary is defined as 32-bit words, and Vulkan consumes shader
    /// module code through a uint32_t pointer rather than a byte buffer.
    std::vector<Uint32> Code                   = {};
    String              VertexEntryPointName   = {};
    String              FragmentEntryPointName = {};
    ShaderReflection    Reflection             = {};
};

/// @brief Compiled shader artifact for one compute pipeline.
struct ShaderComputeProgram {
    std::vector<Uint32> Code                 = {};
    String              ComputeEntryPointName = {};
    ShaderReflection    Reflection            = {};
};

/// @brief Logical category of one ray-tracing hit group.
enum class ShaderRayTracingHitGroupType : Uint8 {
    Unknown    = 0,
    Triangles  = 1,
    Procedural = 2,
};

/// @brief Canonical entry-point names that form one linked ray-tracing hit group.
struct ShaderRayTracingHitGroup {
    ShaderRayTracingHitGroupType Type                       = ShaderRayTracingHitGroupType::Triangles;
    std::optional<String>        ClosestHitEntryPointName   = std::nullopt;
    std::optional<String>        AnyHitEntryPointName       = std::nullopt;
    std::optional<String>        IntersectionEntryPointName = std::nullopt;
};

/// @brief Compiled shader artifact for one linked ray-tracing pipeline program.
struct ShaderRayTracingProgram {
    /// SPIR-V binary containing all selected ray-tracing entry points.
    std::vector<Uint32>                   Code                        = {};
    String                                RayGenerationEntryPointName = {};
    std::vector<String>                   MissEntryPointNames         = {};
    std::vector<ShaderRayTracingHitGroup> HitGroups                   = {};
    std::vector<String>                   CallableEntryPointNames     = {};
    ShaderReflection                      Reflection                  = {};
};

} // namespace SoulEngine

export namespace magic_enum::customize {
template <>
struct enum_range<SoulEngine::ShaderStage> {
    static constexpr bool is_flags = true;
};
} // namespace magic_enum::customize
