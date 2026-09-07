module;

#include <imgui_threaded_rendering.h>

export module RHI:Command;

export import :Types;
export import :Pipeline;
export import :RayTracing;
import :Ref;

export import std;

export namespace SoulEngine {

/// @brief Set viewport rectangle.
struct RHISetViewportCmd {
    Float32 X        = 0.0f;
    Float32 Y        = 0.0f;
    Float32 Width    = 0.0f;
    Float32 Height   = 0.0f;
    Float32 MinDepth = 0.0f;
    Float32 MaxDepth = 1.0f;
};

/// @brief Set scissor rectangle.
struct RHISetScissorCmd {
    Int32  X      = 0;
    Int32  Y      = 0;
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

/// @brief Draw indexed primitives.
struct RHIDrawIndexedCmd {
    std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings> VertexBufferRefs = {};
    RHIRef<RHIIndexBuffer>                                        IndexBufferRef   = nullptr;
};

/// @brief Draw non-indexed primitives.
struct RHIDrawCmd {
    std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings> VertexBufferRefs = {};
};

/// @brief Draw non-indexed primitives from GPU-written indirect commands.
struct RHIDrawIndirectCmd {
    RHIRef<RHITransientShaderStorageBuffer> IndirectBuffer = nullptr;
    Uint64                          Offset         = 0;
    Uint32                          DrawCount      = 1;
    Uint32                          Stride         = sizeof(Uint32) * 4;
};

/// @brief Dispatch compute workgroups.
struct RHIDispatchCmd {
    Uint32 GroupCountX = 1;
    Uint32 GroupCountY = 1;
    Uint32 GroupCountZ = 1;
};

/// @brief Build or update a persistent TLAS from renderer-provided logical instances.
struct RHIBuildOrUpdateTopLevelAccelerationStructureCmd {
    RHIRef<RHITopLevelAccelerationStructure>      TargetRef = nullptr;
    std::vector<RHIAccelerationStructureInstance> Instances = {};
    RHITopLevelAccelerationStructureBuildMode     Mode      = RHITopLevelAccelerationStructureBuildMode::Auto;
};

/// @brief Dispatch hardware rays through the pipeline-owned shader binding table.
struct RHITraceRaysCmd {
    Uint32                        Width       = 0;
    Uint32                        Height      = 0;
    Uint32                        Depth       = 1;
};

/// @brief Copy a small texel region from a render target into one readback
/// buffer. The backend chooses an internal ring position and stamps it with
/// the submission's timeline value.
struct RHICopyTextureToBufferCmd {
    RHIRef<RHIRenderTarget>   Source    = nullptr;
    Uint32                    SrcX      = 0;
    Uint32                    SrcY      = 0;
    Uint32                    SrcWidth  = 1;
    Uint32                    SrcHeight = 1;
    RHIRef<RHIReadbackBuffer> Target    = nullptr;
};

/// @brief All command types dispatched via std::visit.
using RHICommand = std::variant<RHISetViewportCmd,
                                RHISetScissorCmd,
                                RHIDrawIndexedCmd,
                                RHIDrawCmd,
                                RHIDrawIndirectCmd,
                                RHIDispatchCmd,
                                RHIBuildOrUpdateTopLevelAccelerationStructureCmd,
                                RHITraceRaysCmd,
                                RHICopyTextureToBufferCmd>;

/// @brief One Dear ImGui overlay to record over the acquired presentation image.
///
/// All pointers are non-owning. The producer's frame slot retains the snapshot,
/// texture queue, and mutex until RHIRenderDevice::Execute() returns.
struct RHIImGuiPresentationOverlayCmd {
    ImDrawDataSnapshot* Snapshot     = nullptr;
    ImTextureQueue*     TextureQueue = nullptr;
    std::mutex*         TextureMutex = nullptr;
};

} // namespace SoulEngine
