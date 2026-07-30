/// @file   RHIRayTracing.cppm
/// @brief  Backend-agnostic hardware ray-tracing resource and command descriptors.

export module RHI:RayTracing;

export import :Types;

import Shader;

export import std;

export namespace SoulEngine {

/// One position-only Float32x3 triangle geometry entry in a bottom-level acceleration structure.
struct RHITriangleAccelerationStructureGeometryDesc {
    RHIVertexBuffer* VertexBufferPtr = nullptr;
    RHIIndexBuffer*  IndexBufferPtr  = nullptr;
};

/// Descriptor for immutable triangle geometry used to construct a BLAS.
struct RHIBottomLevelAccelerationStructureDesc {
    std::vector<RHITriangleAccelerationStructureGeometryDesc> Geometries = {};
};

/// Descriptor for a persistent, renderer-scoped TLAS allocation.
struct RHITopLevelAccelerationStructureDesc {
    Uint32 InitialInstanceCapacity = 0;
};

/// Explicit row-major affine transform passed from Renderer to RHI.
struct RHIRowMajorTransform3x4 {
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
class RHIAccelerationStructure : public RHIGpuResource {
  public:
    RHIAccelerationStructure()                                                   = default;
    RHIAccelerationStructure(const RHIAccelerationStructure&)                    = delete;
    auto operator=(const RHIAccelerationStructure&) -> RHIAccelerationStructure& = delete;
    RHIAccelerationStructure(RHIAccelerationStructure&&)                         = delete;
    auto operator=(RHIAccelerationStructure&&) -> RHIAccelerationStructure&      = delete;
    virtual ~RHIAccelerationStructure()                                          = default;
};

/// GPU payload containing reusable object-space triangle geometry.
class RHIBottomLevelAccelerationStructure : public RHIAccelerationStructure {
  public:
    RHIBottomLevelAccelerationStructure()                                                              = default;
    RHIBottomLevelAccelerationStructure(const RHIBottomLevelAccelerationStructure&)                    = delete;
    auto operator=(const RHIBottomLevelAccelerationStructure&) -> RHIBottomLevelAccelerationStructure& = delete;
    RHIBottomLevelAccelerationStructure(RHIBottomLevelAccelerationStructure&&)                         = delete;
    auto operator=(RHIBottomLevelAccelerationStructure&&) -> RHIBottomLevelAccelerationStructure&      = delete;
    virtual ~RHIBottomLevelAccelerationStructure()                                                     = default;
};

/// Persistent GPU payload containing the current render-scene instance hierarchy.
class RHITopLevelAccelerationStructure : public RHIAccelerationStructure {
  public:
    RHITopLevelAccelerationStructure()                                                           = default;
    RHITopLevelAccelerationStructure(const RHITopLevelAccelerationStructure&)                    = delete;
    auto operator=(const RHITopLevelAccelerationStructure&) -> RHITopLevelAccelerationStructure& = delete;
    RHITopLevelAccelerationStructure(RHITopLevelAccelerationStructure&&)                         = delete;
    auto operator=(RHITopLevelAccelerationStructure&&) -> RHITopLevelAccelerationStructure&      = delete;
    virtual ~RHITopLevelAccelerationStructure()                                                  = default;
};

/// One logical source geometry consumed by the device-owned BDA metadata table.
///
/// This record intentionally contains RHI resource observers and layout intent only.
/// The backend resolves native device addresses while recording the command list.
struct RHIRayTracingGeometryDesc {
    RHIVertexBuffer* PositionBuffer     = nullptr;
    RHIVertexBuffer* NormalBuffer       = nullptr;
    RHIVertexBuffer* TangentBuffer      = nullptr;
    RHIVertexBuffer* TexCoordBuffer     = nullptr;
    RHIIndexBuffer*  IndexBuffer        = nullptr;
    Uint32           PositionByteOffset = 0;
    Uint32           NormalByteOffset   = 0;
    Uint32           TangentByteOffset  = 0;
    Uint32           TexCoordByteOffset = 0;
    Uint32           IndexByteOffset    = 0;
    Uint32           PositionStride     = sizeof(Float32) * 3;
    Uint32           NormalStride       = sizeof(Float32) * 3;
    Uint32           TangentStride      = sizeof(Float32) * 4;
    Uint32           TexCoordStride     = sizeof(Float32) * 2;
    Uint32           IndexStride        = sizeof(Uint32);
    Uint32           VertexCount        = 0;
    Uint32           IndexCount         = 0;
    Uint32           MaterialIndex      = 0;
};

/// Shader-visible per-instance range into ray-tracing geometry data.
struct RHIRayTracingInstanceData {
    Uint32 FirstGeometry = 0;
    Uint32 GeometryCount = 0;
};

/// Shader-visible BDA and layout data for one BLAS geometry.
///
/// Renderer allocates storage for this ABI but never writes device addresses;
/// the backend resolves source buffers while executing the upload command.
struct alignas(16) RHIRayTracingGeometryData {
    Uint64 PositionAddress    = 0;
    Uint64 NormalAddress      = 0;
    Uint64 TangentAddress     = 0;
    Uint64 TexCoordAddress    = 0;
    Uint64 IndexAddress       = 0;
    Uint32 PositionByteOffset = 0;
    Uint32 NormalByteOffset   = 0;
    Uint32 TangentByteOffset  = 0;
    Uint32 TexCoordByteOffset = 0;
    Uint32 IndexByteOffset    = 0;
    Uint32 PositionStride     = 0;
    Uint32 NormalStride       = 0;
    Uint32 TangentStride      = 0;
    Uint32 TexCoordStride     = 0;
    Uint32 IndexStride        = 0;
    Uint32 MaterialIndex      = 0;
};
static_assert(sizeof(RHIRayTracingInstanceData) == 8);
static_assert(sizeof(RHIRayTracingGeometryData) == 96);
static_assert(alignof(RHIRayTracingGeometryData) == 16);

/// Backend-neutral instance flags for TLAS population.
enum class RHIAccelerationStructureInstanceFlags : Uint8 {
    None = 0,
    DisableTriangleCulling,
};

/// One TLAS instance referencing a reusable BLAS.
struct RHIAccelerationStructureInstance {
    RHIBottomLevelAccelerationStructure*  BottomLevelPtr = nullptr;
    RHIRowMajorTransform3x4               Transform      = {};
    Uint32                                CustomIndex    = 0;
    Uint32                                HitGroupIndex  = 0;
    Uint8                                 Mask           = 0xFF;
    RHIAccelerationStructureInstanceFlags Flags          = RHIAccelerationStructureInstanceFlags::None;
};

/// Requested TLAS population operation. Backends select rebuild only when update is invalid.
enum class RHITopLevelAccelerationStructureBuildMode : Uint8 {
    Unknown = 0,
    Auto,
    Build,
    Update,
};

/// Logical shader-group category. Native Vulkan group indices remain backend-private.
enum class RHIRayTracingShaderGroupType : Uint8 {
    Unknown = 0,
    General,
    TrianglesHit,
    ProceduralHit,
};

/// Logical shader-group descriptor populated by the later shader-program phase.
struct RHIRayTracingShaderGroupDesc {
    RHIRayTracingShaderGroupType Type = RHIRayTracingShaderGroupType::Unknown;
};

/// Backend-agnostic RT pipeline policy. Shader program ownership is added by Phase 2.
struct RHIRayTracingPipelineDesc {
    ShaderRayTracingProgram                   Program           = {};
    std::vector<RHIRayTracingShaderGroupDesc> ShaderGroups      = {};
    Uint32                                    MaxRecursionDepth = 1;
};

/// Empty polymorphic base for ray-tracing pipeline resources.
/// Backend concrete classes own native pipeline-layout and shader-binding-table state.
class RHIRayTracingPipeline : public RHIPipeline {
  public:
    RHIRayTracingPipeline()                                                = default;
    RHIRayTracingPipeline(const RHIRayTracingPipeline&)                    = delete;
    auto operator=(const RHIRayTracingPipeline&) -> RHIRayTracingPipeline& = delete;
    RHIRayTracingPipeline(RHIRayTracingPipeline&&)                         = delete;
    auto operator=(RHIRayTracingPipeline&&) -> RHIRayTracingPipeline&      = delete;
    virtual ~RHIRayTracingPipeline()                                       = default;
};

} // namespace SoulEngine
