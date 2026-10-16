module;

#include <imgui_threaded_rendering.h>

export module RHI:Command;

export import :Types;
export import :Pipeline;
import :Ref;

import std;

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

/// @brief Draw non-indexed primitives. All geometry reaches shaders through
/// vertex pulling (BDA) or indirect draws; there is no vertex-buffer binding path. The fullscreen-triangle
/// users emit the default 3.
struct RHIDrawCmd {
    Uint32 VertexCount = 3;
};

/// @brief Draw non-indexed primitives from GPU-written indirect commands.
struct RHIDrawIndirectCmd {
    RHIRef<RHIBuffer> IndirectBuffer = nullptr;
    Uint64            Offset         = 0;
    Uint32            DrawCount      = 1;
    Uint32            Stride         = sizeof(Uint32) * 4;
};

/// @brief Dispatch compute workgroups.
struct RHIDispatchCmd {
    Uint32 GroupCountX = 1;
    Uint32 GroupCountY = 1;
    Uint32 GroupCountZ = 1;
};

/// @brief Dispatch hardware rays through the pipeline-owned shader binding table.
struct RHITraceRaysCmd {
    Uint32 Width  = 0;
    Uint32 Height = 0;
    Uint32 Depth  = 1;
};

/// @brief Read back a texel region from a render target into a readback. The backend picks an internal ring
/// position and stamps it with the submission's timeline value.
struct RHIReadbackTextureCmd {
    RHIRef<RHITexture>  Source    = nullptr;
    Uint32              SrcX      = 0;
    Uint32              SrcY      = 0;
    Uint32              SrcWidth  = 1;
    Uint32              SrcHeight = 1;
    RHIRef<RHIReadback> Target    = nullptr;
};

/// @brief Blit the complete render target into the acquired swapchain image.
///
/// This is the only present path: the backend resolves the current swapchain image, which stays
/// backend-private. Fixed nearest filtering.
struct RHIBlitToSwapchainCmd {
    RHIRef<RHITexture> Source    = nullptr;
    Uint32             DstX      = 0;
    Uint32             DstY      = 0;
    Uint32             DstWidth  = 1;
    Uint32             DstHeight = 1;
};

/// @brief All command types dispatched via std::visit.
using RHICommand = std::variant<RHISetViewportCmd,
                                RHISetScissorCmd,
                                RHIDrawCmd,
                                RHIDrawIndirectCmd,
                                RHIDispatchCmd,
                                RHITraceRaysCmd,
                                RHIReadbackTextureCmd,
                                RHIBlitToSwapchainCmd>;

/// @brief One Dear ImGui overlay to record over the acquired presentation image.
///
/// All pointers are non-owning. The producer's frame slot retains the snapshot, texture queue, and mutex
/// until RHIRenderDevice::Execute() returns.
struct RHIImGuiPresentationOverlayCmd {
    ImDrawDataSnapshot* Snapshot     = nullptr;
    ImTextureQueue*     TextureQueue = nullptr;
    std::mutex*         TextureMutex = nullptr;
};

} // namespace SoulEngine
