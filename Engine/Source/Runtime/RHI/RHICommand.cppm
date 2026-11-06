module;

#include <imgui_threaded_rendering.h>

export module RHI.Command;

export import RHI.Types;
export import RHI.Pipeline;
import RHI.Ref;
import Core;

import std;

namespace SoulEngine {

/// @brief Set viewport rectangle.
export struct RHISetViewportCmd {
    Float32 X        = 0.0f;
    Float32 Y        = 0.0f;
    Float32 Width    = 0.0f;
    Float32 Height   = 0.0f;
    Float32 MinDepth = 0.0f;
    Float32 MaxDepth = 1.0f;
};

/// @brief Set scissor rectangle.
export struct RHISetScissorCmd {
    Int32  X      = 0;
    Int32  Y      = 0;
    Uint32 Width  = 0;
    Uint32 Height = 0;
};

/// @brief Draw non-indexed primitives with complete vertex, instance, and stream inputs.
export struct RHIDrawCmd {
    std::vector<RHIBufferView> VertexBuffers = {};
    Uint32                     VertexCount   = 0;
    Uint32                     InstanceCount = 1;
    Uint32                     FirstVertex   = 0;
    Uint32                     FirstInstance = 0;
};

/// @brief Draw non-indexed primitives from GPU-written indirect commands.
export struct RHIDrawIndirectCmd {
    std::vector<RHIBufferView> VertexBuffers = {};
    RHIBufferView              IndirectBuffer;
    Uint32                     DrawCount = 1;
};

/// @brief Draw indexed primitives with complete index, vertex, and instance inputs.
export struct RHIDrawIndexedCmd {
    std::vector<RHIBufferView> VertexBuffers = {};
    RHIBufferView              IndexBuffer;
    Uint32                     IndexCount    = 0;
    Uint32                     InstanceCount = 1;
    Uint32                     FirstIndex    = 0;
    Int32                      VertexOffset  = 0;
    Uint32                     FirstInstance = 0;
};

/// @brief Draw indexed primitives from GPU-written indexed indirect commands.
export struct RHIDrawIndexedIndirectCmd {
    std::vector<RHIBufferView> VertexBuffers = {};
    RHIBufferView              IndexBuffer;
    RHIBufferView              IndirectBuffer;
    Uint32                     DrawCount = 1;
};

/// @brief Dispatch compute workgroups.
export struct RHIDispatchCmd {
    Uint32 GroupCountX = 1;
    Uint32 GroupCountY = 1;
    Uint32 GroupCountZ = 1;
};

/// @brief Dispatch hardware rays through the pipeline-owned shader binding table.
export struct RHITraceRaysCmd {
    Uint32 Width  = 0;
    Uint32 Height = 0;
    Uint32 Depth  = 1;
};

/// @brief Read back a texel region from a render target into a readback. The backend picks an internal ring
/// position and stamps it with the submission's timeline value.
export struct RHIReadbackTextureCmd {
    RHITextureView      Source;
    Uint32              SrcX      = 0;
    Uint32              SrcY      = 0;
    Uint32              SrcWidth  = 1;
    Uint32              SrcHeight = 1;
    RHIRef<RHIReadback> Target    = nullptr;
};

/// @brief Blit one source mip into the acquired swapchain image.
///
/// This is the only present path: the backend resolves the current swapchain image, which stays
/// backend-private. Fixed nearest filtering.
export struct RHIBlitToSwapchainCmd {
    RHITextureView Source;
    Uint32         DstX      = 0;
    Uint32         DstY      = 0;
    Uint32         DstWidth  = 1;
    Uint32         DstHeight = 1;
};

/// @brief All command types dispatched via std::visit.
export using RHICommand = std::variant<RHISetViewportCmd,
                                       RHISetScissorCmd,
                                       RHIDrawCmd,
                                       RHIDrawIndexedCmd,
                                       RHIDrawIndirectCmd,
                                       RHIDrawIndexedIndirectCmd,
                                       RHIDispatchCmd,
                                       RHITraceRaysCmd,
                                       RHIReadbackTextureCmd,
                                       RHIBlitToSwapchainCmd>;

/// @brief One Dear ImGui overlay to record over the acquired presentation image.
///
/// All pointers are non-owning. The producer's frame slot retains the snapshot, texture queue, and mutex
/// until RHIRenderDevice::Execute() returns.
export struct RHIImGuiPresentationOverlayCmd {
    ImDrawDataSnapshot* Snapshot     = nullptr;
    ImTextureQueue*     TextureQueue = nullptr;
    std::mutex*         TextureMutex = nullptr;
};

} // namespace SoulEngine
