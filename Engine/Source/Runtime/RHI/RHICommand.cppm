module;

#include <imgui_threaded_rendering.h>

export module RHI:Command;

export import :Types;
export import :RayTracing;

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

/// @brief Set viewport to the full current pass render target.
struct RHISetFullViewportCmd {
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

/// @brief Set scissor to the full current pass render target.
struct RHISetFullScissorRectCmd {};

/// @brief Bind the graphics pipeline used by subsequent draw calls.
struct RHISetGraphicsPipelineCmd {
    /// Non-owning observer. Producer must keep the pipeline alive until Execute() completes.
    RHIGraphicsPipeline* PipelinePtr = nullptr;
};

/// @brief Bind the ray-tracing pipeline used by subsequent ray-tracing commands.
struct RHISetRayTracingPipelineCmd {
    /// Non-owning observer. Producer must keep the pipeline alive until Execute() completes.
    RHIRayTracingPipeline* PipelinePtr = nullptr;
};

/// @brief Push a CPU-side byte snapshot into the active graphics pipeline's push-constant range.
struct RHIPushConstantsCmd {
    /// RHIPipeline expected to be bound when the push happens. Backends use this for
    /// validation and pipeline-layout lookup.
    /// The common RHIPipeline base permits the same reflected push-constant contract for ray tracing.
    RHIPipeline*                PipelinePtr = nullptr;
    Uint32                   Offset      = 0;
    std::vector<std::byte>   Data        = {};
};

/// @brief Bind a reflection-derived shader parameter snapshot to the active pipeline.
struct RHIBindShaderParametersCmd {
    /// RHIPipeline expected to be bound when the parameter snapshot is bound.
    /// The common RHIPipeline base permits the same reflected parameter contract for ray tracing.
    RHIPipeline* PipelinePtr = nullptr;
    /// Value snapshot keeps per-frame constant data stable until the RHI thread consumes it.
    RHIShaderParameters  Parameters  = {};
};

/// @brief Draw indexed primitives.
struct RHIDrawIndexedCmd {
    /// RHIPipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    RHIGraphicsPipeline*                         PipelinePtr     = nullptr;
    std::array<RHIVertexBuffer*, kMaxVertexBufferBindings> VertexBuffers = {};
    RHIIndexBuffer*                              IndexBufferPtr  = nullptr;
};

/// @brief Draw non-indexed primitives.
struct RHIDrawCmd {
    /// RHIPipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    RHIGraphicsPipeline*                         PipelinePtr     = nullptr;
    std::array<RHIVertexBuffer*, kMaxVertexBufferBindings> VertexBuffers = {};
};

/// Resolve logical ray-tracing geometry sources into transient shader-storage buffers.
struct RHIWriteRayTracingGeometryDataCmd {
    RHITransientShaderStorageBuffer          InstanceBuffer = {};
    RHITransientShaderStorageBuffer          GeometryBuffer = {};
    std::vector<RHIRayTracingInstanceData>   Instances      = {};
    std::vector<RHIRayTracingGeometryDesc>   Geometries     = {};
};

/// @brief Upload a CPU snapshot into a RenderDevice-allocated transient constant buffer.
struct RHIWriteTransientConstantBufferCmd {
    RHITransientConstantBuffer Buffer = {};
    std::vector<std::byte>    Data   = {};
};

/// @brief Upload a CPU snapshot into a RenderDevice-allocated transient storage buffer.
struct RHIWriteTransientShaderStorageBufferCmd {
    RHITransientShaderStorageBuffer Buffer = {};
    std::vector<std::byte>          Data   = {};
};

/// @brief Build or update a persistent TLAS from renderer-provided logical instances.
struct RHIBuildOrUpdateTopLevelAccelerationStructureCmd {
    RHITopLevelAccelerationStructure*          TargetPtr = nullptr;
    std::vector<RHIAccelerationStructureInstance> Instances = {};
    RHITopLevelAccelerationStructureBuildMode   Mode      = RHITopLevelAccelerationStructureBuildMode::Auto;
};

/// @brief Dispatch hardware rays through the pipeline-owned shader binding table.
struct RHITraceRaysCmd {
    RHIRayTracingPipeline* PipelinePtr = nullptr;
    Uint32              Width       = 0;
    Uint32              Height      = 0;
    Uint32              Depth       = 1;
};

/// @brief All command types dispatched via std::visit.
using RHICommand = std::variant<RHISetViewportCmd,
                             RHISetFullViewportCmd,
                             RHISetScissorCmd,
                             RHISetFullScissorRectCmd,
                             RHISetGraphicsPipelineCmd,
                             RHISetRayTracingPipelineCmd,
                             RHIPushConstantsCmd,
                             RHIBindShaderParametersCmd,
                             RHIDrawIndexedCmd,
                             RHIDrawCmd,
                             RHIWriteRayTracingGeometryDataCmd,
                             RHIWriteTransientConstantBufferCmd,
                             RHIWriteTransientShaderStorageBufferCmd,
                             RHIBuildOrUpdateTopLevelAccelerationStructureCmd,
                             RHITraceRaysCmd>;

/// @brief One rendering pass with attachments and commands inside.
/// Backend automatically wraps each pass with begin/end rendering.
struct RHIPass {
    RHIRenderingDesc        Desc;
    std::vector<RHICommand> Commands;

    // ── Builder helpers ──────────────────────────────────────────────

    auto SetViewport(Float32 X, Float32 Y, Float32 W, Float32 H, Float32 MinDepth = 0.0f, Float32 MaxDepth = 1.0f)
        -> void {
        Commands.emplace_back(RHISetViewportCmd{X, Y, W, H, MinDepth, MaxDepth});
    }
    auto SetFullViewport(Float32 MinDepth = 0.0f, Float32 MaxDepth = 1.0f) -> void {
        Commands.emplace_back(RHISetFullViewportCmd{.MinDepth = MinDepth, .MaxDepth = MaxDepth});
    }
    auto SetScissorRect(Int32 X, Int32 Y, Uint32 W, Uint32 H) -> void {
        Commands.emplace_back(RHISetScissorCmd{X, Y, W, H});
    }
    auto SetFullScissorRect() -> void {
        Commands.emplace_back(RHISetFullScissorRectCmd{});
    }
    auto SetGraphicsPipeline(RHIGraphicsPipeline* PipelinePtr) -> void {
        Commands.emplace_back(RHISetGraphicsPipelineCmd{.PipelinePtr = PipelinePtr});
    }
    auto SetRayTracingPipeline(RHIRayTracingPipeline* PipelinePtr) -> void {
        Commands.emplace_back(RHISetRayTracingPipelineCmd{.PipelinePtr = PipelinePtr});
    }
    auto PushConstants(RHIPipeline* PipelinePtr, Uint32 Offset, const void* Data, Uint64 Size) -> void {
        if (Size == 0)
            return;

        RHIPushConstantsCmd Cmd{
            .PipelinePtr = PipelinePtr,
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto BindShaderParameters(RHIPipeline* PipelinePtr, RHIShaderParameters Parameters) -> void {
        Commands.emplace_back(RHIBindShaderParametersCmd{
            .PipelinePtr = PipelinePtr,
            .Parameters  = std::move(Parameters),
        });
    }
    auto WriteRayTracingGeometryData(RHITransientShaderStorageBuffer        InstanceBuffer,
                                     RHITransientShaderStorageBuffer        GeometryBuffer,
                                     std::vector<RHIRayTracingInstanceData> Instances,
                                     std::vector<RHIRayTracingGeometryDesc> Geometries) -> void {
        Commands.emplace_back(RHIWriteRayTracingGeometryDataCmd{
            .InstanceBuffer = InstanceBuffer,
            .GeometryBuffer = GeometryBuffer,
            .Instances      = std::move(Instances),
            .Geometries     = std::move(Geometries),
        });
    }
    [[nodiscard]] auto WriteTransientConstantBuffer(RHITransientConstantBuffer Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient constant buffer is invalid"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(ErrorMessage("Transient constant buffer write size does not match allocation size"));
        Commands.emplace_back(RHIWriteTransientConstantBufferCmd{
            .Buffer = Buffer,
            .Data   = std::vector<std::byte>{Data.begin(), Data.end()},
        });
        return {};
    }
    [[nodiscard]] auto WriteTransientShaderStorageBuffer(RHITransientShaderStorageBuffer Buffer,
                                                          std::span<const std::byte>           Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient shader storage buffer is invalid"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(ErrorMessage("Transient shader storage buffer write size does not match allocation size"));
        Commands.emplace_back(RHIWriteTransientShaderStorageBufferCmd{
            .Buffer = Buffer,
            .Data   = std::vector<std::byte>{Data.begin(), Data.end()},
        });
        return {};
    }
    auto BuildOrUpdateTopLevelAccelerationStructure(RHITopLevelAccelerationStructure*               TargetPtr,
                                                     std::span<const RHIAccelerationStructureInstance> Instances,
                                                     RHITopLevelAccelerationStructureBuildMode        Mode =
                                                         RHITopLevelAccelerationStructureBuildMode::Auto) -> void {
        Commands.emplace_back(RHIBuildOrUpdateTopLevelAccelerationStructureCmd{
            .TargetPtr = TargetPtr,
            .Instances = std::vector<RHIAccelerationStructureInstance>{Instances.begin(), Instances.end()},
            .Mode      = Mode,
        });
    }
    auto TraceRays(RHIRayTracingPipeline* PipelinePtr, Uint32 Width, Uint32 Height, Uint32 Depth = 1) -> void {
        Commands.emplace_back(RHITraceRaysCmd{
            .PipelinePtr = PipelinePtr,
            .Width       = Width,
            .Height      = Height,
            .Depth       = Depth,
        });
    }
    auto DrawIndexed(RHIGraphicsPipeline* PipelinePtr,
                     RHIVertexBuffer*     VertexBufferPtr,
                     RHIIndexBuffer*      IndexBufferPtr) -> void {
        DrawIndexed(PipelinePtr, std::array<RHIVertexBuffer*, kMaxVertexBufferBindings>{VertexBufferPtr}, IndexBufferPtr);
    }
    auto DrawIndexed(RHIGraphicsPipeline*                                PipelinePtr,
                     std::array<RHIVertexBuffer*, kMaxVertexBufferBindings> VertexBuffers,
                     RHIIndexBuffer*                                     IndexBufferPtr) -> void {
        Commands.emplace_back(RHIDrawIndexedCmd{
            .PipelinePtr     = PipelinePtr,
            .VertexBuffers   = VertexBuffers,
            .IndexBufferPtr  = IndexBufferPtr,
        });
    }
    auto Draw(RHIGraphicsPipeline* PipelinePtr, RHIVertexBuffer* VertexBufferPtr) -> void {
        Draw(PipelinePtr, std::array<RHIVertexBuffer*, kMaxVertexBufferBindings>{VertexBufferPtr});
    }
    auto Draw(RHIGraphicsPipeline*                                PipelinePtr,
              std::array<RHIVertexBuffer*, kMaxVertexBufferBindings> VertexBuffers) -> void {
        Commands.emplace_back(RHIDrawCmd{
            .PipelinePtr   = PipelinePtr,
            .VertexBuffers = VertexBuffers,
        });
    }
};

/// @brief One non-rendering command scope recorded outside dynamic rendering.
///
/// Acceleration-structure builds and ray dispatch are not legal between
/// vkCmdBeginRendering and vkCmdEndRendering, so callers place them here.
struct RHINonRenderingPass {
    std::vector<RHICommand> Commands = {};

    auto SetRayTracingPipeline(RHIRayTracingPipeline* PipelinePtr) -> void {
        Commands.emplace_back(RHISetRayTracingPipelineCmd{.PipelinePtr = PipelinePtr});
    }
    auto PushConstants(RHIPipeline* PipelinePtr, Uint32 Offset, const void* Data, Uint64 Size) -> void {
        if (Size == 0)
            return;

        RHIPushConstantsCmd Cmd{
            .PipelinePtr = PipelinePtr,
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto BindShaderParameters(RHIPipeline* PipelinePtr, RHIShaderParameters Parameters) -> void {
        Commands.emplace_back(RHIBindShaderParametersCmd{
            .PipelinePtr = PipelinePtr,
            .Parameters  = std::move(Parameters),
        });
    }
    auto WriteRayTracingGeometryData(RHITransientShaderStorageBuffer        InstanceBuffer,
                                     RHITransientShaderStorageBuffer        GeometryBuffer,
                                     std::vector<RHIRayTracingInstanceData> Instances,
                                     std::vector<RHIRayTracingGeometryDesc> Geometries) -> void {
        Commands.emplace_back(RHIWriteRayTracingGeometryDataCmd{
            .InstanceBuffer = InstanceBuffer,
            .GeometryBuffer = GeometryBuffer,
            .Instances      = std::move(Instances),
            .Geometries     = std::move(Geometries),
        });
    }
    [[nodiscard]] auto WriteTransientConstantBuffer(RHITransientConstantBuffer Buffer, std::span<const std::byte> Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient constant buffer is invalid"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(ErrorMessage("Transient constant buffer write size does not match allocation size"));
        Commands.emplace_back(RHIWriteTransientConstantBufferCmd{
            .Buffer = Buffer,
            .Data   = std::vector<std::byte>{Data.begin(), Data.end()},
        });
        return {};
    }
    [[nodiscard]] auto WriteTransientShaderStorageBuffer(RHITransientShaderStorageBuffer Buffer,
                                                          std::span<const std::byte>           Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient shader storage buffer is invalid"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(ErrorMessage("Transient shader storage buffer write size does not match allocation size"));
        Commands.emplace_back(RHIWriteTransientShaderStorageBufferCmd{
            .Buffer = Buffer,
            .Data   = std::vector<std::byte>{Data.begin(), Data.end()},
        });
        return {};
    }
    auto BuildOrUpdateTopLevelAccelerationStructure(RHITopLevelAccelerationStructure*               TargetPtr,
                                                     std::span<const RHIAccelerationStructureInstance> Instances,
                                                     RHITopLevelAccelerationStructureBuildMode        Mode =
                                                         RHITopLevelAccelerationStructureBuildMode::Auto) -> void {
        Commands.emplace_back(RHIBuildOrUpdateTopLevelAccelerationStructureCmd{
            .TargetPtr = TargetPtr,
            .Instances = std::vector<RHIAccelerationStructureInstance>{Instances.begin(), Instances.end()},
            .Mode      = Mode,
        });
    }
    auto TraceRays(RHIRayTracingPipeline* PipelinePtr, Uint32 Width, Uint32 Height, Uint32 Depth = 1) -> void {
        Commands.emplace_back(RHITraceRaysCmd{
            .PipelinePtr = PipelinePtr,
            .Width       = Width,
            .Height      = Height,
            .Depth       = Depth,
        });
    }
};

/// Ordered command-list scope. Rendering and non-rendering work must retain
/// submission order because trace output can subsequently be rendered or presented.
using RHICommandScope = std::variant<RHIPass, RHINonRenderingPass>;

/// @brief One Dear ImGui overlay to record over the acquired presentation image.
///
/// All pointers are non-owning. The producer's frame slot retains the snapshot,
/// texture queue, and mutex until RHIRenderDevice::Execute() returns.
struct RHIImGuiPresentationOverlayCmd {
    ImDrawDataSnapshot* Snapshot     = nullptr;
    ImTextureQueue*     TextureQueue = nullptr;
    std::mutex*           TextureMutex = nullptr;
};

/// @brief Complete frame's worth of GPU commands, produced by RenderLoop,
/// consumed by RHIRenderDevice::Execute().
struct RHICommandList {
    std::vector<RHICommandScope> Scopes = {};
    /// Final frame output. Backend presents this engine-owned RT to swapchain.
    RHIRenderTarget*             PresentSource = nullptr;
    std::optional<RHIImGuiPresentationOverlayCmd> ImGuiPresentationOverlay = std::nullopt;
};

} // namespace SoulEngine
