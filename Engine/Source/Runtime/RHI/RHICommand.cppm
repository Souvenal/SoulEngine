module;

#include <imgui_threaded_rendering.h>

export module RHI:Command;

export import :Types;
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
    RHIRef<RHIGraphicsPipeline> PipelineRef = nullptr;
};

/// @brief Bind the ray-tracing pipeline used by subsequent ray-tracing commands.
struct RHISetRayTracingPipelineCmd {
    RHIRef<RHIRayTracingPipeline> PipelineRef = nullptr;
};

struct RHIPipelineRef {
    RHIRef<RHIGraphicsPipeline>   Graphics   = nullptr;
    RHIRef<RHIRayTracingPipeline> RayTracing = nullptr;
};

/// @brief Push a CPU-side byte snapshot into the active graphics pipeline's push-constant range.
struct RHIPushConstantsCmd {
    /// RHIPipeline expected to be bound when the push happens. Backends use this for
    /// validation and pipeline-layout lookup.
    /// The common RHIPipeline base permits the same reflected push-constant contract for ray tracing.
    RHIPipelineRef         PipelineRef = {};
    Uint32                 Offset      = 0;
    std::vector<std::byte> Data        = {};
};

/// @brief Submission-owned resource snapshots used by shader parameter bindings.
struct RHIShaderParameterResources {
    std::vector<RHIRef<RHISampledTexture>> SampledTextures = {};
    std::vector<RHIRef<RHISampler>>        Samplers        = {};
    std::vector<RHIRef<RHIRenderTarget>>   RenderTargets   = {};
};

[[nodiscard]] inline auto AreShaderParameterResourcesReady(const RHIShaderParameterResources& Resources) -> bool {
    const auto IsReady = []<typename T>(const RHIRef<T>& Ref) { return static_cast<bool>(Ref); };
    return std::ranges::all_of(Resources.SampledTextures, IsReady) &&
           std::ranges::all_of(Resources.Samplers, IsReady) && std::ranges::all_of(Resources.RenderTargets, IsReady);
}

/// @brief Bind a reflection-derived shader parameter snapshot to the active pipeline.
struct RHIBindShaderParametersCmd {
    /// RHIPipeline expected to be bound when the parameter snapshot is bound.
    /// The common RHIPipeline base permits the same reflected parameter contract for ray tracing.
    RHIPipelineRef              PipelineRef = {};
    /// Value snapshot keeps per-frame constant data stable until the RHI thread consumes it.
    RHIShaderParameters         Parameters  = {};
    RHIShaderParameterResources Resources   = {};
};

/// @brief Draw indexed primitives.
struct RHIDrawIndexedCmd {
    /// RHIPipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    RHIRef<RHIGraphicsPipeline>                                   PipelineRef      = nullptr;
    std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings> VertexBufferRefs = {};
    RHIRef<RHIIndexBuffer>                                        IndexBufferRef   = nullptr;
};

/// @brief Draw non-indexed primitives.
struct RHIDrawCmd {
    /// RHIPipeline expected to be bound for this draw. Backends use this for
    /// validation and for lowering draw parameters that require pipeline
    /// layout information, such as Vulkan push constants.
    RHIRef<RHIGraphicsPipeline>                                   PipelineRef      = nullptr;
    std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings> VertexBufferRefs = {};
};

/// Resolve logical ray-tracing geometry sources into transient shader-storage buffers.
struct RHIWriteRayTracingGeometryDataCmd {
    RHITransientShaderStorageBuffer        InstanceBuffer = {};
    RHITransientShaderStorageBuffer        GeometryBuffer = {};
    std::vector<RHIRayTracingInstanceData> Instances      = {};
    std::vector<RHIRayTracingGeometryDesc> Geometries     = {};
};

/// @brief Upload a CPU snapshot into a RenderDevice-allocated transient constant buffer.
struct RHIWriteTransientConstantBufferCmd {
    RHITransientConstantBuffer Buffer = {};
    std::vector<std::byte>     Data   = {};
};

/// @brief Upload a CPU snapshot into a RenderDevice-allocated transient storage buffer.
struct RHIWriteTransientShaderStorageBufferCmd {
    RHITransientShaderStorageBuffer Buffer = {};
    std::vector<std::byte>          Data   = {};
};

/// @brief Build or update a persistent TLAS from renderer-provided logical instances.
struct RHIBuildOrUpdateTopLevelAccelerationStructureCmd {
    RHIRef<RHITopLevelAccelerationStructure>      TargetRef = nullptr;
    std::vector<RHIAccelerationStructureInstance> Instances = {};
    RHITopLevelAccelerationStructureBuildMode     Mode      = RHITopLevelAccelerationStructureBuildMode::Auto;
};

/// @brief Dispatch hardware rays through the pipeline-owned shader binding table.
struct RHITraceRaysCmd {
    RHIRef<RHIRayTracingPipeline> PipelineRef = nullptr;
    Uint32                        Width       = 0;
    Uint32                        Height      = 0;
    Uint32                        Depth       = 1;
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
    RHIRenderingDesc        Desc     = {};
    std::vector<RHICommand> Commands = {};

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
    auto SetGraphicsPipeline(RHIRef<RHIGraphicsPipeline> PipelineRef) -> void {
        if (!PipelineRef)
            return;
        Commands.emplace_back(
            RHISetGraphicsPipelineCmd{.PipelineRef = std::move(PipelineRef)});
    }
    auto SetRayTracingPipeline(RHIRef<RHIRayTracingPipeline> PipelineRef) -> void {
        if (!PipelineRef)
            return;
        Commands.emplace_back(
            RHISetRayTracingPipelineCmd{.PipelineRef = std::move(PipelineRef)});
    }
    auto PushConstants(RHIRef<RHIGraphicsPipeline> PipelineRef, Uint32 Offset, const void* Data, Uint64 Size) -> void {
        if (Size == 0)
            return;
        if (!PipelineRef)
            return;
        RHIPushConstantsCmd Cmd{
            .PipelineRef = RHIPipelineRef{.Graphics = std::move(PipelineRef)},
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto PushConstants(RHIRef<RHIRayTracingPipeline> PipelineRef, Uint32 Offset, const void* Data, Uint64 Size)
        -> void {
        if (Size == 0)
            return;
        if (!PipelineRef)
            return;
        RHIPushConstantsCmd Cmd{
            .PipelineRef = RHIPipelineRef{.RayTracing = std::move(PipelineRef)},
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto BindShaderParameters(RHIRef<RHIGraphicsPipeline> PipelineRef,
                              RHIShaderParameters         Parameters,
                              RHIShaderParameterResources Resources) -> void {
        if (!PipelineRef || !AreShaderParameterResourcesReady(Resources))
            return;
        Commands.emplace_back(RHIBindShaderParametersCmd{
            .PipelineRef = RHIPipelineRef{.Graphics = std::move(PipelineRef)},
            .Parameters  = std::move(Parameters),
            .Resources   = std::move(Resources),
        });
    }
    auto BindShaderParameters(RHIRef<RHIRayTracingPipeline> PipelineRef,
                              RHIShaderParameters           Parameters,
                              RHIShaderParameterResources   Resources) -> void {
        if (!PipelineRef || !AreShaderParameterResourcesReady(Resources))
            return;
        Commands.emplace_back(RHIBindShaderParametersCmd{
            .PipelineRef = RHIPipelineRef{.RayTracing = std::move(PipelineRef)},
            .Parameters  = std::move(Parameters),
            .Resources   = std::move(Resources),
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
                                                         std::span<const std::byte>      Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient shader storage buffer is invalid"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(
                ErrorMessage("Transient shader storage buffer write size does not match allocation size"));
        Commands.emplace_back(RHIWriteTransientShaderStorageBufferCmd{
            .Buffer = Buffer,
            .Data   = std::vector<std::byte>{Data.begin(), Data.end()},
        });
        return {};
    }
    auto BuildOrUpdateTopLevelAccelerationStructure(
        RHIRef<RHITopLevelAccelerationStructure>          TargetRef,
        std::span<const RHIAccelerationStructureInstance> Instances,
        RHITopLevelAccelerationStructureBuildMode Mode = RHITopLevelAccelerationStructureBuildMode::Auto) -> void {
        if (!TargetRef)
            return;
        Commands.emplace_back(RHIBuildOrUpdateTopLevelAccelerationStructureCmd{
            .TargetRef = std::move(TargetRef),
            .Instances = std::vector<RHIAccelerationStructureInstance>{Instances.begin(), Instances.end()},
            .Mode      = Mode,
        });
    }
    auto TraceRays(RHIRef<RHIRayTracingPipeline> PipelineRef, Uint32 Width, Uint32 Height, Uint32 Depth = 1) -> void {
        if (!PipelineRef)
            return;
        Commands.emplace_back(RHITraceRaysCmd{
            .PipelineRef = std::move(PipelineRef),
            .Width       = Width,
            .Height      = Height,
            .Depth       = Depth,
        });
    }
    auto Draw(RHIRef<RHIGraphicsPipeline> PipelineRef) -> void {
        if (!PipelineRef)
            return;
        Commands.emplace_back(RHIDrawCmd{.PipelineRef = std::move(PipelineRef)});
    }
    auto DrawIndexed(RHIRef<RHIGraphicsPipeline>                                   PipelineRef,
                     std::array<RHIRef<RHIVertexBuffer>, kMaxVertexBufferBindings> VertexBufferRefs,
                     RHIRef<RHIIndexBuffer>                                        IndexBufferRef) -> void {
        if (!PipelineRef || !VertexBufferRefs[0] || !IndexBufferRef)
            return;
        for (Uint32 Index = 1; Index < VertexBufferRefs.size(); ++Index) {
            if (VertexBufferRefs[Index].GetState() != RHIRefState::Unknown && !VertexBufferRefs[Index])
                return;
        }
        Commands.emplace_back(RHIDrawIndexedCmd{
            .PipelineRef      = std::move(PipelineRef),
            .VertexBufferRefs = std::move(VertexBufferRefs),
            .IndexBufferRef   = std::move(IndexBufferRef),
        });
    }
};

/// @brief One non-rendering command scope recorded outside dynamic rendering.
///
/// Acceleration-structure builds and ray dispatch are not legal between
/// vkCmdBeginRendering and vkCmdEndRendering, so callers place them here.
struct RHINonRenderingPass {
    std::vector<RHICommand> Commands = {};

    auto SetRayTracingPipeline(RHIRef<RHIRayTracingPipeline> PipelineRef) -> void {
        if (!PipelineRef)
            return;
        Commands.emplace_back(
            RHISetRayTracingPipelineCmd{.PipelineRef = std::move(PipelineRef)});
    }
    auto PushConstants(RHIRef<RHIGraphicsPipeline> PipelineRef, Uint32 Offset, const void* Data, Uint64 Size) -> void {
        if (Size == 0)
            return;
        if (!PipelineRef)
            return;
        RHIPushConstantsCmd Cmd{
            .PipelineRef = RHIPipelineRef{.Graphics = std::move(PipelineRef)},
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto PushConstants(RHIRef<RHIRayTracingPipeline> PipelineRef, Uint32 Offset, const void* Data, Uint64 Size)
        -> void {
        if (Size == 0)
            return;
        if (!PipelineRef)
            return;
        RHIPushConstantsCmd Cmd{
            .PipelineRef = RHIPipelineRef{.RayTracing = std::move(PipelineRef)},
            .Offset      = Offset,
        };
        Cmd.Data.resize(Size);
        std::memcpy(Cmd.Data.data(), Data, Size);
        Commands.emplace_back(std::move(Cmd));
    }
    auto BindShaderParameters(RHIRef<RHIGraphicsPipeline> PipelineRef,
                              RHIShaderParameters         Parameters,
                              RHIShaderParameterResources Resources) -> void {
        if (!PipelineRef || !AreShaderParameterResourcesReady(Resources))
            return;
        Commands.emplace_back(RHIBindShaderParametersCmd{
            .PipelineRef = RHIPipelineRef{.Graphics = std::move(PipelineRef)},
            .Parameters  = std::move(Parameters),
            .Resources   = std::move(Resources),
        });
    }
    auto BindShaderParameters(RHIRef<RHIRayTracingPipeline> PipelineRef,
                              RHIShaderParameters           Parameters,
                              RHIShaderParameterResources   Resources) -> void {
        if (!PipelineRef || !AreShaderParameterResourcesReady(Resources))
            return;
        Commands.emplace_back(RHIBindShaderParametersCmd{
            .PipelineRef = RHIPipelineRef{.RayTracing = std::move(PipelineRef)},
            .Parameters  = std::move(Parameters),
            .Resources   = std::move(Resources),
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
                                                         std::span<const std::byte>      Data)
        -> std::expected<void, ErrorMessage> {
        if (!Buffer.IsValid())
            return std::unexpected(ErrorMessage("Transient shader storage buffer is invalid"));
        if (Data.size_bytes() != Buffer.GetSize())
            return std::unexpected(
                ErrorMessage("Transient shader storage buffer write size does not match allocation size"));
        Commands.emplace_back(RHIWriteTransientShaderStorageBufferCmd{
            .Buffer = Buffer,
            .Data   = std::vector<std::byte>{Data.begin(), Data.end()},
        });
        return {};
    }
    auto BuildOrUpdateTopLevelAccelerationStructure(
        RHIRef<RHITopLevelAccelerationStructure>          TargetRef,
        std::span<const RHIAccelerationStructureInstance> Instances,
        RHITopLevelAccelerationStructureBuildMode Mode = RHITopLevelAccelerationStructureBuildMode::Auto) -> void {
        if (!TargetRef)
            return;
        Commands.emplace_back(RHIBuildOrUpdateTopLevelAccelerationStructureCmd{
            .TargetRef = std::move(TargetRef),
            .Instances = std::vector<RHIAccelerationStructureInstance>{Instances.begin(), Instances.end()},
            .Mode      = Mode,
        });
    }
    auto TraceRays(RHIRef<RHIRayTracingPipeline> PipelineRef, Uint32 Width, Uint32 Height, Uint32 Depth = 1) -> void {
        if (!PipelineRef)
            return;
        Commands.emplace_back(RHITraceRaysCmd{
            .PipelineRef = std::move(PipelineRef),
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
    std::mutex*         TextureMutex = nullptr;
};

/// @brief Complete frame's worth of GPU commands, produced by RenderLoop,
/// consumed by RHIRenderDevice::Execute().
struct RHICommandList {
    RHICommandList()                                                                                      = default;
    RHICommandList(const RHICommandList&)                                                                 = delete;
    auto operator=(const RHICommandList&) -> RHICommandList&                                              = delete;
    RHICommandList(RHICommandList&&) noexcept                                                             = default;
    auto                                          operator=(RHICommandList&&) noexcept -> RHICommandList& = default;
    std::vector<RHICommandScope>                  Scopes                                                  = {};
    /// Final frame output. Backend presents this engine-owned RT to swapchain.
    RHIRef<RHIRenderTarget>                       PresentSourceRef                                        = nullptr;
    std::optional<RHIImGuiPresentationOverlayCmd> ImGuiPresentationOverlay = std::nullopt;
};

} // namespace SoulEngine
