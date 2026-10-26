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

/// @brief Draw non-indexed primitives. All geometry reaches shaders through
/// vertex pulling (BDA) or indirect draws; there is no vertex-buffer binding path. The fullscreen-triangle
/// users emit the default 3.
export struct RHIDrawCmd {
    Uint32 VertexCount = 3;
};

/// @brief Draw non-indexed primitives from GPU-written indirect commands.
export struct RHIDrawIndirectCmd {
    RHIRef<RHIBuffer> IndirectBuffer = nullptr;
    Uint64            Offset         = 0;
    Uint32            DrawCount      = 1;
    Uint32            Stride         = sizeof(Uint32) * 4;
};

/// @brief Bind a Uint32 index buffer at a byte offset.
export struct RHIBindIndexBufferCmd {
    RHIRef<RHIBuffer> IndexBuffer = nullptr;
    Uint64            Offset      = 0;

    /// @brief Validate the buffer reference, Uint32 alignment, and binding range.
    [[nodiscard]] auto Validate() const -> std::expected<void, ErrorMessage> {
        if (!IndexBuffer)
            return std::unexpected(ErrorMessage("Index binding requires a ready buffer"));
        if (Offset % sizeof(Uint32) != 0)
            return std::unexpected(ErrorMessage("Index binding offset must be aligned to Uint32"));
        if (Offset >= IndexBuffer->GetSize())
            return std::unexpected(ErrorMessage("Index binding offset exceeds the buffer"));
        return {};
    }
};

/// @brief Draw indexed primitives from GPU-written indexed indirect commands.
export struct RHIDrawIndexedIndirectCmd {
    RHIRef<RHIBuffer> IndirectBuffer = nullptr;
    Uint64            Offset         = 0;
    Uint32            DrawCount      = 1;
    Uint32            Stride         = sizeof(Uint32) * 5;

    /// @brief Validate the buffer reference, arguments, native draw-count limit, and command range.
    [[nodiscard]] auto Validate(Uint64 BufferSize, Uint32 MaxDrawCount) const -> std::expected<void, ErrorMessage> {
        if (!IndirectBuffer)
            return std::unexpected(ErrorMessage("Indexed indirect draw requires a ready buffer"));
        if (Offset % sizeof(Uint32) != 0)
            return std::unexpected(ErrorMessage("Indexed indirect draw offset must be aligned to Uint32"));
        if (Stride != sizeof(Uint32) * 5)
            return std::unexpected(ErrorMessage("Indexed indirect draw stride must be 20 bytes"));
        if (DrawCount == 0)
            return std::unexpected(ErrorMessage("Indexed indirect draw count must be greater than zero"));
        if (DrawCount > MaxDrawCount)
            return std::unexpected(ErrorMessage("Indexed indirect draw count exceeds maxDrawIndirectCount"));
        if (Offset > BufferSize || DrawCount > (BufferSize - Offset) / Stride)
            return std::unexpected(ErrorMessage("Indexed indirect draw range exceeds its buffer"));
        return {};
    }
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
export struct RHIBlitToSwapchainCmd {
    RHIRef<RHITexture> Source    = nullptr;
    Uint32             DstX      = 0;
    Uint32             DstY      = 0;
    Uint32             DstWidth  = 1;
    Uint32             DstHeight = 1;
};

/// @brief All command types dispatched via std::visit.
export using RHICommand = std::variant<RHISetViewportCmd,
                                       RHISetScissorCmd,
                                       RHIDrawCmd,
                                       RHIDrawIndirectCmd,
                                       RHIBindIndexBufferCmd,
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
