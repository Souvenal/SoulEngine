/// @file   RHIRayTracing.cppm
/// @brief  Backend-agnostic hardware ray-tracing resource and command descriptors.

export module RHI:RayTracing;

export import :Types;

import Shader;

export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::RHI {

/// Build preferences for acceleration structures.
enum class AccelerationStructureBuildFlags : Uint32 {
    None            = 0,
    PreferFastTrace = 1u << 0,
    PreferFastBuild = 1u << 1,
    AllowUpdate     = 1u << 2,
};

[[nodiscard]] inline auto operator|(AccelerationStructureBuildFlags Left, AccelerationStructureBuildFlags Right)
    -> AccelerationStructureBuildFlags {
    return static_cast<AccelerationStructureBuildFlags>(static_cast<Uint32>(Left) | static_cast<Uint32>(Right));
}

/// Geometry policy used while building a triangle BLAS entry.
enum class AccelerationStructureGeometryFlags : Uint32 {
    None   = 0,
    Opaque = 1u << 0,
};

[[nodiscard]] inline auto operator|(AccelerationStructureGeometryFlags Left, AccelerationStructureGeometryFlags Right)
    -> AccelerationStructureGeometryFlags {
    return static_cast<AccelerationStructureGeometryFlags>(static_cast<Uint32>(Left) | static_cast<Uint32>(Right));
}

/// Index encoding understood by the first triangle BLAS implementation.
enum class AccelerationStructureIndexType : Uint8 {
    Unknown = 0,
    Uint32,
};

/// One triangle geometry entry in a bottom-level acceleration structure.
struct TriangleAccelerationStructureGeometryDesc {
    VertexBuffer*                      VertexBufferPtr = nullptr;
    Uint64                             VertexCount     = 0;
    Uint32                             VertexStride    = 0;
    Format                             VertexFormat    = Format::R32G32B32_SFLOAT;
    IndexBuffer*                       IndexBufferPtr  = nullptr;
    Uint64                             IndexCount      = 0;
    AccelerationStructureIndexType     IndexType       = AccelerationStructureIndexType::Uint32;
    AccelerationStructureGeometryFlags Flags           = AccelerationStructureGeometryFlags::Opaque;
};

/// Descriptor for immutable triangle geometry used to construct a BLAS.
struct BottomLevelAccelerationStructureDesc {
    std::vector<TriangleAccelerationStructureGeometryDesc> Geometries = {};
    AccelerationStructureBuildFlags BuildFlags = AccelerationStructureBuildFlags::PreferFastTrace;
};

/// Descriptor for a persistent, renderer-scoped TLAS allocation.
struct TopLevelAccelerationStructureDesc {
    Uint32                          InitialInstanceCapacity = 0;
    AccelerationStructureBuildFlags BuildFlags =
        AccelerationStructureBuildFlags::PreferFastTrace | AccelerationStructureBuildFlags::AllowUpdate;
};

/// Explicit row-major affine transform passed from Renderer to RHI.
struct RowMajorTransform3x4 {
    Float32 M00 = 1.0f;
    Float32 M01 = 0.0f;
    Float32 M02 = 0.0f;
    Float32 M03 = 0.0f;
    Float32 M10 = 0.0f;
    Float32 M11 = 1.0f;
    Float32 M12 = 0.0f;
    Float32 M13 = 0.0f;
    Float32 M20 = 0.0f;
    Float32 M21 = 0.0f;
    Float32 M22 = 1.0f;
    Float32 M23 = 0.0f;
};

/// Common GPU resource base for BLAS and TLAS payloads.
class AccelerationStructure : public GpuResource {
  public:
    AccelerationStructure()                                                = default;
    AccelerationStructure(const AccelerationStructure&)                    = delete;
    auto operator=(const AccelerationStructure&) -> AccelerationStructure& = delete;
    AccelerationStructure(AccelerationStructure&&)                         = delete;
    auto operator=(AccelerationStructure&&) -> AccelerationStructure&      = delete;
    virtual ~AccelerationStructure()                                       = default;
};

/// GPU payload containing reusable object-space triangle geometry.
class BottomLevelAccelerationStructure : public AccelerationStructure {
  public:
    BottomLevelAccelerationStructure()                                                           = default;
    BottomLevelAccelerationStructure(const BottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const BottomLevelAccelerationStructure&) -> BottomLevelAccelerationStructure& = delete;
    BottomLevelAccelerationStructure(BottomLevelAccelerationStructure&&)                         = delete;
    auto operator=(BottomLevelAccelerationStructure&&) -> BottomLevelAccelerationStructure&      = delete;
    virtual ~BottomLevelAccelerationStructure()                                                  = default;
};

/// Persistent GPU payload containing the current render-scene instance hierarchy.
class TopLevelAccelerationStructure : public AccelerationStructure {
  public:
    TopLevelAccelerationStructure()                                                        = default;
    TopLevelAccelerationStructure(const TopLevelAccelerationStructure&)                    = delete;
    auto operator=(const TopLevelAccelerationStructure&) -> TopLevelAccelerationStructure& = delete;
    TopLevelAccelerationStructure(TopLevelAccelerationStructure&&)                         = delete;
    auto operator=(TopLevelAccelerationStructure&&) -> TopLevelAccelerationStructure&      = delete;
    virtual ~TopLevelAccelerationStructure()                                               = default;
};

/// Backend-neutral instance flags for TLAS population.
enum class AccelerationStructureInstanceFlags : Uint8 {
    None = 0,
    DisableTriangleCulling,
};

/// One TLAS instance referencing a reusable BLAS.
struct AccelerationStructureInstance {
    BottomLevelAccelerationStructure*  BottomLevelPtr = nullptr;
    RowMajorTransform3x4               Transform      = {};
    Uint32                             CustomIndex    = 0;
    Uint32                             HitGroupIndex  = 0;
    Uint8                              Mask           = 0xFF;
    AccelerationStructureInstanceFlags Flags          = AccelerationStructureInstanceFlags::None;
};

/// Requested TLAS population operation. Backends select rebuild only when update is invalid.
enum class TopLevelAccelerationStructureBuildMode : Uint8 {
    Unknown = 0,
    Auto,
    Build,
    Update,
};

/// Logical shader-group category. Native Vulkan group indices remain backend-private.
enum class RayTracingShaderGroupType : Uint8 {
    Unknown = 0,
    General,
    TrianglesHit,
    ProceduralHit,
};

/// Logical shader-group descriptor populated by the later shader-program phase.
struct RayTracingShaderGroupDesc {
    RayTracingShaderGroupType Type = RayTracingShaderGroupType::Unknown;
};

/// Backend-agnostic RT pipeline policy. Shader program ownership is added by Phase 2.
struct RayTracingPipelineDesc {
    Shader::RayTracingProgram              Program           = {};
    std::vector<RayTracingShaderGroupDesc> ShaderGroups      = {};
    Uint32                                 MaxRecursionDepth = 1;
};

/// Empty polymorphic base for ray-tracing pipeline resources.
/// Backend concrete classes own native pipeline-layout and shader-binding-table state.
class RayTracingPipeline : public Pipeline {
  public:
    RayTracingPipeline()                                             = default;
    RayTracingPipeline(const RayTracingPipeline&)                    = delete;
    auto operator=(const RayTracingPipeline&) -> RayTracingPipeline& = delete;
    RayTracingPipeline(RayTracingPipeline&&)                         = delete;
    auto operator=(RayTracingPipeline&&) -> RayTracingPipeline&      = delete;
    virtual ~RayTracingPipeline()                                    = default;
};

} // namespace SoulEngine::RHI
